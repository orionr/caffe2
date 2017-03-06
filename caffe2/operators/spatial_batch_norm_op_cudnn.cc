#include <cfloat>

#include "caffe2/core/common_cudnn.h"
#include "caffe2/core/context_gpu.h"
#include "caffe2/operators/spatial_batch_norm_op.h"
#include "caffe2/utils/math.h"

// Note: Instead of directly failing, we will choose to not build this operator
// if cudnn version is not high enough.
static_assert(CUDNN_VERSION >= 5000,
             "CudnnSpatialBN requires cudnn version 5.0 or above.");

namespace caffe2 {

constexpr cudnnBatchNormMode_t kSpatialBNMode = CUDNN_BATCHNORM_SPATIAL;

template <typename T>
class CudnnSpatialBNOp final : public SpatialBNOp<CUDAContext> {
 public:
  USE_OPERATOR_FUNCTIONS(CUDAContext);
  CudnnSpatialBNOp(const OperatorDef& operator_def, Workspace* ws)
      : SpatialBNOp<CUDAContext>(operator_def, ws), cudnn_wrapper_(&context_) {
    CUDNN_ENFORCE(cudnnCreateTensorDescriptor(&data_desc_));
    CUDNN_ENFORCE(cudnnCreateTensorDescriptor(&bn_param_desc_));
    if (epsilon_ <= CUDNN_BN_MIN_EPSILON - FLT_EPSILON) {
      LOG(ERROR) << "Provided epsilon is smaller than "
                 << "CUDNN_BN_MIN_EPSILON. Setting it to "
                 << "CUDNN_BN_MIN_EPSILON instead.";
    }
    epsilon_ = std::max(epsilon_, CUDNN_BN_MIN_EPSILON);
  }

  ~CudnnSpatialBNOp() {
    CUDNN_ENFORCE(cudnnDestroyTensorDescriptor(data_desc_));
    CUDNN_ENFORCE(cudnnDestroyTensorDescriptor(bn_param_desc_));
  }

  bool RunOnDevice() override;

 protected:
  CuDNNWrapper cudnn_wrapper_;
  cudnnTensorDescriptor_t data_desc_;
  cudnnTensorDescriptor_t bn_param_desc_;
  vector<TIndex> cudnn_input_dims_;
};

template <typename T>
class CudnnSpatialBNGradientOp final : public SpatialBNGradientOp<CUDAContext> {
 public:
  USE_OPERATOR_FUNCTIONS(CUDAContext);
  CudnnSpatialBNGradientOp(const OperatorDef& operator_def, Workspace* ws)
      : SpatialBNGradientOp<CUDAContext>(operator_def, ws),
        cudnn_wrapper_(&context_) {
    CUDNN_ENFORCE(cudnnCreateTensorDescriptor(&data_desc_));
    CUDNN_ENFORCE(cudnnCreateTensorDescriptor(&bn_param_desc_));
    if (epsilon_ <= CUDNN_BN_MIN_EPSILON - FLT_EPSILON) {
      LOG(ERROR) << "Provided epsilon is smaller than "
                 << "CUDNN_BN_MIN_EPSILON. Setting it to "
                 << "CUDNN_BN_MIN_EPSILON instead.";
    }
    epsilon_ = std::max(epsilon_, CUDNN_BN_MIN_EPSILON);
  }

  ~CudnnSpatialBNGradientOp() {
    CUDNN_ENFORCE(cudnnDestroyTensorDescriptor(data_desc_));
    CUDNN_ENFORCE(cudnnDestroyTensorDescriptor(bn_param_desc_));
  }

  bool RunOnDevice() override;

 protected:
  CuDNNWrapper cudnn_wrapper_;
  cudnnTensorDescriptor_t data_desc_;
  cudnnTensorDescriptor_t bn_param_desc_;
  vector<TIndex> cudnn_input_dims_;
};


////////////////////////////////////////////////////////////////////////////////
// Implementations
////////////////////////////////////////////////////////////////////////////////

template <typename T>
bool CudnnSpatialBNOp<T>::RunOnDevice() {
  const auto& X = Input(INPUT);
  const auto& scale = Input(SCALE);
  const auto& bias = Input(BIAS);

  DCHECK_EQ(X.ndim(), 4);
  const int N = X.dim32(0);
  const int C = (order_ == StorageOrder::NCHW ? X.dim32(1) : X.dim32(3));
  const int H = (order_ == StorageOrder::NCHW ? X.dim32(2) : X.dim32(1));
  const int W = (order_ == StorageOrder::NCHW ? X.dim32(3) : X.dim32(2));
  DCHECK_EQ(scale.ndim(), 1);
  DCHECK_EQ(bias.ndim(), 1);
  DCHECK_EQ(scale.dim32(0), C);
  DCHECK_EQ(bias.dim32(0), C);
  // See if we need to reshape.
  if (X.dims() != cudnn_input_dims_) {
    VLOG(1) << "Setting descriptors.";
    cudnn_input_dims_ = X.dims();
    CUDNN_ENFORCE(cudnnSetTensor4dDescriptor(
        data_desc_,
        GetCudnnTensorFormat(order_),
        cudnnTypeWrapper<T>::type,
        N,
        C,
        H,
        W));
    CUDNN_ENFORCE(cudnnDeriveBNTensorDescriptor(
        bn_param_desc_, data_desc_, kSpatialBNMode));
  }

  // Now, depending on whether we are running test or not, we have two paths.
  if (is_test_) {
    // Run inference mode.
    const auto& est_mean = Input(EST_MEAN);
    const auto& est_var = Input(EST_VAR);
    DCHECK_EQ(est_mean.ndim(), 1);
    DCHECK_EQ(est_var.ndim(), 1);
    DCHECK_EQ(est_mean.dim32(0), C);
    DCHECK_EQ(est_var.dim32(0), C);

    auto* Y = Output(OUTPUT);
    Y->ResizeLike(X);
    CUDNN_ENFORCE(cudnnBatchNormalizationForwardInference(
        cudnn_wrapper_.inline_cudnn_handle(),
        kSpatialBNMode,
        cudnnTypeWrapper<T>::kOne(),
        cudnnTypeWrapper<T>::kZero(),
        data_desc_,
        X.template data<T>(),
        data_desc_,
        Y->template mutable_data<T>(),
        bn_param_desc_,
        scale.template data<T>(),
        bias.template data<T>(),
        est_mean.template data<T>(),
        est_var.template data<T>(),
        epsilon_));
  } else {
    // Run training mode.
    auto* Y = Output(OUTPUT);
    Y->ResizeLike(X);
    // obtain running mean and running inv var, and see if we need to
    // initialize them.
    auto* running_mean = Output(RUNNING_MEAN);
    auto* running_var = Output(RUNNING_VAR);
    double this_factor = 1. - momentum_;
    T* running_mean_data = nullptr;
    T* running_var_data = nullptr;
    if (!running_mean->size()) {
      // If the input mean and var are not initialized yet, this is the first
      // run and we will initialize the storage.
      VLOG(1) << "Initializing running mean and var.";
      // Need to do initialization
      running_mean->Resize(C);
      running_var->Resize(C);
      running_mean_data = running_mean->template mutable_data<T>();
      running_var_data = running_var->template mutable_data<T>();
      // In principle, setting this_momentum to 1 will wipe existing data.
      // This has a caveat that if cudnn does not deal with 0*NaN cases we
      // will be having an issue. Thus we choose a safe path by explicitly
      // setting zero.
      math::Set<T, CUDAContext>(C, 0, running_mean_data, &context_);
      math::Set<T, CUDAContext>(C, 0, running_var_data, &context_);
    } else {
      // Does not need to do initialization.
      DCHECK_EQ(running_mean->ndim(), 1);
      DCHECK_EQ(running_var->ndim(), 1);
      DCHECK_EQ(running_mean->dim32(0), C);
      DCHECK_EQ(running_var->dim32(0), C);
      running_mean_data = running_mean->template mutable_data<T>();
      running_var_data = running_var->template mutable_data<T>();
    }
    // Save the mean and inv var results.
    auto* save_mean = Output(SAVED_MEAN);
    auto* save_var = Output(SAVED_INV_VAR);
    save_mean->Resize(C);
    save_var->Resize(C);
    void* save_mean_data = save_mean->template mutable_data<T>();
    void* save_var_data = save_var->template mutable_data<T>();

    CUDNN_ENFORCE(cudnnBatchNormalizationForwardTraining(
        cudnn_wrapper_.inline_cudnn_handle(),
        kSpatialBNMode,
        cudnnTypeWrapper<T>::kOne(),
        cudnnTypeWrapper<T>::kZero(),
        data_desc_,
        X.template data<T>(),
        data_desc_,
        Y->template mutable_data<T>(),
        bn_param_desc_,
        scale.template data<T>(),
        bias.template data<T>(),
        this_factor,
        running_mean_data,
        running_var_data,
        epsilon_,
        save_mean_data,
        save_var_data));
  }
  return true;
}


template <typename T>
bool CudnnSpatialBNGradientOp<T>::RunOnDevice() {
  const auto& X = Input(INPUT);
  const auto& scale = Input(SCALE);
  const auto& dY = Input(OUTPUT_GRAD);

  DCHECK_EQ(X.ndim(), 4);
  const int N = X.dim32(0);
  const int C = (order_ == StorageOrder::NCHW ? X.dim32(1) : X.dim32(3));
  const int H = (order_ == StorageOrder::NCHW ? X.dim32(2) : X.dim32(1));
  const int W = (order_ == StorageOrder::NCHW ? X.dim32(3) : X.dim32(2));
  DCHECK_EQ(scale.ndim(), 1);
  DCHECK_EQ(scale.dim32(0), C);
  // See if we need to reshape.
  if (X.dims() != cudnn_input_dims_) {
    cudnn_input_dims_ = X.dims();
    CUDNN_ENFORCE(cudnnSetTensor4dDescriptor(
        data_desc_,
        GetCudnnTensorFormat(order_),
        cudnnTypeWrapper<T>::type,
        N,
        C,
        H,
        W));
    CUDNN_ENFORCE(cudnnDeriveBNTensorDescriptor(
        bn_param_desc_, data_desc_, kSpatialBNMode));
  }

  auto* dX = Output(INPUT_GRAD);
  auto* dScale = Output(SCALE_GRAD);
  auto* dBias = Output(BIAS_GRAD);
  dX->ResizeLike(X);
  dScale->ResizeLike(scale);
  dBias->ResizeLike(scale);

  const auto& saved_mean = Input(SAVED_MEAN);
  const auto& saved_var = Input(SAVED_INV_VAR);
  const void* saved_mean_data = saved_mean.template data<T>();
  const void* saved_var_data = saved_var.template data<T>();

  CUDNN_ENFORCE(cudnnBatchNormalizationBackward(
      cudnn_wrapper_.inline_cudnn_handle(),
      kSpatialBNMode,
      cudnnTypeWrapper<T>::kOne(),
      cudnnTypeWrapper<T>::kZero(),
      cudnnTypeWrapper<T>::kOne(),
      cudnnTypeWrapper<T>::kZero(),
      data_desc_,
      X.template data<T>(),
      data_desc_,
      dY.template data<T>(),
      data_desc_,
      dX->template mutable_data<T>(),
      bn_param_desc_,
      scale.template data<T>(),
      dScale->template mutable_data<T>(),
      dBias->template mutable_data<T>(),
      epsilon_,
      saved_mean_data,
      saved_var_data));
  return true;
}

namespace {
// Since there is no default implementation for spatial batch normalization,
// we will register the cudnn version as the default as well.
REGISTER_CUDA_OPERATOR(SpatialBN, CudnnSpatialBNOp<float>);
REGISTER_CUDA_OPERATOR(SpatialBNGradient, CudnnSpatialBNGradientOp<float>);

REGISTER_CUDNN_OPERATOR(SpatialBN, CudnnSpatialBNOp<float>);
REGISTER_CUDNN_OPERATOR(SpatialBNGradient, CudnnSpatialBNGradientOp<float>);
}  // namespace
}  // namespace caffe2
