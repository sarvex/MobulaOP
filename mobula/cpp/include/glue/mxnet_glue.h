// Adapted from
// https://github.com/apache/incubator-mxnet/blob/master/src/nnvm/tvm_bridge.cc
#ifndef MOBULA_CPP_INCLUDE_GLUE_MXNET_GLUE_H_
#define MOBULA_CPP_INCLUDE_GLUE_MXNET_GLUE_H_

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "api.h"
#include "tvm_packed_func.h"

namespace tvm_bridge {
using namespace tvm;
using namespace tvm::runtime;

using TVMFunc = std::function<void(TVMArgs args, TVMRetValue* rv)>;

typedef DLManagedTensor* DLManagedTensorHandle;
/*! \brief handle to Context */
typedef const void* ContextHandle;
/*! \brief handle to Engine FnProperty */
typedef const void* EngineFnPropertyHandle;
/*! \brief handle to Engine VarHandle */
typedef void* EngineVarHandle;

/*! \brief Engine asynchronous operation */
typedef void (*EngineAsyncFunc)(void*, void*, void*);
/*! \brief Engine synchronous operation */
typedef void (*EngineSyncFunc)(void*, void*);
/*! \brief Callback to free the param for EngineAsyncFunc/EngineSyncFunc */
typedef void (*EngineFuncParamDeleter)(void*);

int (*MXShallowCopyNDArray)(NDArrayHandle src_handle, NDArrayHandle* out);
int (*MXNDArrayFree)(NDArrayHandle handle);
int (*MXNDArrayGetContext)(NDArrayHandle handle, int* out_dev_type,
                           int* out_dev_id);
int (*MXNDArrayToDLPack)(NDArrayHandle handle, DLManagedTensorHandle*);
int (*MXEnginePushSyncND)(
    EngineSyncFunc sync_func, void* func_param, EngineFuncParamDeleter deleter,
    ContextHandle ctx_handle, NDArrayHandle* const_nds_handle,
    int num_const_nds, NDArrayHandle* mutable_nds_handle, int num_mutable_nds,
    EngineFnPropertyHandle prop_handle, int priority, const char* opr_name);

struct Context {
  enum DeviceType { kCPU = 1 << 0, kGPU = 1 << 1, kCPUPinned = 3 };
  DeviceType dev_type;
  int32_t dev_id;
};

struct RunContext {
  Context ctx;
  void* stream;
};

thread_local int DEV_ID;
thread_local void* STRM;

void set_stream(TVMArgs args, TVMRetValue*) {
  DEV_ID = args.values[1].v_int64;
  STRM = args.values[2].v_handle;
}

/*!
 * \brief Async functor object
 *  calling argument of the function.
 */
class TVMFunctor {
 public:
  // constructor
  explicit TVMFunctor(PackedFunc func, PackedFunc fset_stream)
      : func_(func), fset_stream_(fset_stream) {}

  void Init(const TVMArgs& args, const std::vector<int>& const_loc,
            std::vector<NDArrayHandle>* const_nds,
            std::vector<NDArrayHandle>* mutate_nds) {
    values_.clear();
    type_codes_.clear();
    values_.insert(values_.end(), args.values, args.values + args.size());
    type_codes_.insert(type_codes_.end(), args.type_codes,
                       args.type_codes + args.size());

    size_t const_loc_ptr = 0;
    int dev_type, dev_id;
    ctx_.dev_id = -1;
    for (int i = 0; i < args.size(); ++i) {
      if (args.type_codes[i] == kTVMNDArrayTypeCode) {
        NDArrayHandle nd_handle =
            static_cast<NDArrayHandle>(args.values[i].v_handle);
        NDArrayHandle nd;
        MXShallowCopyNDArray(nd_handle, &nd);
        type_codes_[i] = kArrayHandle;
        array_handle_.push_back(nd);
        MXNDArrayGetContext(nd, &dev_type, &dev_id);
        if (ctx_.dev_id != -1) {
          if (dev_type != ctx_.dev_type || dev_id != ctx_.dev_id) {
            std::cout << "Inconsistent context: source(" << int(ctx_.dev_type)
                      << ":" << ctx_.dev_id << ") vs target: (" << dev_type
                      << ":" << dev_id << ")" << std::endl;
            exit(-1);
          }
        }
        ctx_ = {Context::DeviceType(dev_type), dev_id};
        array_loc_.push_back(i);
        // check if there is read or mutate
        // by default assume we mutate the array.
        if (const_loc_ptr < const_loc.size() && i == const_loc[const_loc_ptr]) {
          const_nds->push_back(nd);
          ++const_loc_ptr;
        } else {
          mutate_nds->push_back(nd);
        }
      } else {
        CHECK_LT(args.type_codes[i], int(kTVMType))
            << "Only allow POD type in mxnet async call";
      }
    }
  }

  void Run(const RunContext& rctx) {
    // setup DLTensor
    std::vector<DLManagedTensorHandle> dlms(array_loc_.size());
    for (size_t i = 0; i < array_loc_.size(); ++i) {
      DLManagedTensorHandle& dlm = dlms[i];
      MXNDArrayToDLPack(array_handle_[i], &dlm);
      values_[array_loc_[i]].v_handle = static_cast<void*>(&dlm->dl_tensor);
    }
    // run the packed function
    TVMRetValue rv;
    TVMArgs args(&values_[0], &type_codes_[0], values_.size());
    if (ctx_.dev_type == Context::kGPU) {
      // pass stream via last argument.
      void* strm = reinterpret_cast<void**>(rctx.stream)[0];
      int dev_type = kDLGPU;
      fset_stream_(dev_type, rctx.ctx.dev_id, strm);
      func_.CallPacked(args, &rv);
      fset_stream_(dev_type, rctx.ctx.dev_id, nullptr);
    } else {
      func_.CallPacked(args, &rv);
    }
    for (DLManagedTensorHandle dlm : dlms) {
      dlm->deleter(dlm);
    }
  }

  inline const Context& ctx() { return ctx_; }

  ~TVMFunctor() {
    for (NDArrayHandle handle : array_handle_) {
      MXNDArrayFree(handle);
    }
  }

 private:
  /*! \brief The function */
  PackedFunc func_;
  /*! \brief Set stream */
  PackedFunc fset_stream_;
  /*! \brief Values field */
  std::vector<TVMValue> values_;
  /*! \brief type code field */
  std::vector<int> type_codes_;
  /*! \brief NDArrayHandles field */
  std::vector<NDArrayHandle> array_handle_;
  /*! \brief position of array in arguments */
  std::vector<int> array_loc_;
  /*! \brief context */
  Context ctx_;
};

inline void DeduplicateNDArrayHandle(std::vector<NDArrayHandle>* read_nds,
                                     std::vector<NDArrayHandle>* write_nds) {
  std::sort(write_nds->begin(), write_nds->end());
  write_nds->resize(std::unique(write_nds->begin(), write_nds->end()) -
                    write_nds->begin());
  std::sort(read_nds->begin(), read_nds->end());
  read_nds->resize(std::unique(read_nds->begin(), read_nds->end()) -
                   read_nds->begin());
  auto wit = write_nds->begin();
  auto rtop = read_nds->begin();
  for (auto rit = read_nds->begin(); rit != read_nds->end(); ++rit) {
    while (wit != write_nds->end() && *wit < *rit) ++wit;
    if (wit == write_nds->end() || *wit != *rit) {
      *rtop = *rit;
      ++rtop;
    }
  }
  read_nds->resize(rtop - read_nds->begin());
}

struct SyncFuncParams {
  Context ctx;
  std::shared_ptr<TVMFunctor> func;
};

void sync_func_inst(void* rctx, void* param) {
  SyncFuncParams* ps = static_cast<SyncFuncParams*>(param);
  const RunContext* pctx = static_cast<RunContext*>(rctx);
  ps->func->Run(*pctx);
}

void deleter_inst(void* param) {
  SyncFuncParams* ps = static_cast<SyncFuncParams*>(param);
  delete ps;
}

PackedFunc WrapAsyncCall(TVMFunc pfunc, decltype(set_stream) set_stream_func,
                         const int num_const, int* c_const_loc) {
  PackedFunc f(pfunc);
  PackedFunc fset_stream(set_stream_func);

  // sorted position of constant arguments
  std::vector<int> const_loc(c_const_loc, c_const_loc + num_const);
  std::sort(const_loc.begin(), const_loc.end());
  // wrapped function
  // This is the function that called by the user.
  auto wrapped = [f, fset_stream, const_loc](TVMArgs args,
                                             TVMRetValue* /*rv*/) {
    std::shared_ptr<TVMFunctor> func =
        std::make_shared<TVMFunctor>(f, fset_stream);
    std::vector<NDArrayHandle> const_nds, mutate_nds;
    func->Init(args, const_loc, &const_nds, &mutate_nds);
    DeduplicateNDArrayHandle(&const_nds, &mutate_nds);
    SyncFuncParams* ps = new SyncFuncParams();
    ps->ctx = func->ctx();
    ps->func = func;
    NDArrayHandle* const_nds_handle = const_nds.data();
    int num_const_nds = const_nds.size();
    NDArrayHandle* mutate_nds_handle = mutate_nds.data();
    int num_mutate_nds = mutate_nds.size();

    MXEnginePushSyncND(sync_func_inst, static_cast<void*>(ps), deleter_inst,
                       &ps->ctx, const_nds_handle, num_const_nds,
                       mutate_nds_handle, num_mutate_nds, nullptr, 0, nullptr);
  };
  return PackedFunc(wrapped);
}

class FunctionContainer {
 public:
  FunctionContainer() {}
  FunctionContainer(const PackedFunc& func) : func_(new PackedFunc(func)) {}
  FunctionContainer(PackedFunc&& func) : func_(new PackedFunc(func)) {}
  ~FunctionContainer() {}
  PackedFunc* get() { return func_.get(); }
  void reset(const PackedFunc& func) { func_.reset(new PackedFunc(func)); }

 private:
  std::shared_ptr<PackedFunc> func_;
};
std::unordered_map<std::string, FunctionContainer> FUNCTIONS;

}  // namespace tvm_bridge

extern "C" {
using namespace mobula;
using namespace tvm_bridge;

MOBULA_DLL PackedFunc* GetMXNetFunc(const char* cname, TVMFunc pfunc,
                                    const int num_const, int* const_loc) {
  std::string name(cname);
  auto p = FUNCTIONS.find(name);
  if (p != FUNCTIONS.end()) {
    return p->second.get();
  }
  FUNCTIONS[name].reset(WrapAsyncCall(pfunc, set_stream, num_const, const_loc));
  return FUNCTIONS[name].get();
}

MOBULA_DLL void RegisterMXAPI(
    decltype(MXShallowCopyNDArray) shallow_copy_ndarray,
    decltype(MXNDArrayFree) ndarray_free,
    decltype(MXNDArrayGetContext) ndarray_get_context,
    decltype(MXNDArrayToDLPack) ndarray_to_dlpack,
    decltype(MXEnginePushSyncND) engine_push_sync_nd) {
  MXShallowCopyNDArray = shallow_copy_ndarray;
  MXNDArrayFree = ndarray_free;
  MXNDArrayGetContext = ndarray_get_context;
  MXNDArrayToDLPack = ndarray_to_dlpack;
  MXEnginePushSyncND = engine_push_sync_nd;
}
}

#endif  // MOBULA_CPP_INCLUDE_GLUE_MXNET_GLUE_H_
