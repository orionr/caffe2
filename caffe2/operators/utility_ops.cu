#include <math.h>
#include <cfloat>
// TODO(jamesreed): I would use <cmath> here but std::isnan
// and std::isinf are declared constexpr there and the nvidia
// compiler throws an error because of it

#include "caffe2/core/context_gpu.h"
#include "utility_ops.h"

namespace caffe2 {
CAFFE_KNOWN_TYPE(const float*);

__global__ void NanCheckKernel(int N, const float* X, bool* result) {
  bool has_nan = false;
  CUDA_1D_KERNEL_LOOP(i, N) {
    // Note: we have no need to do early return, since only if this fails
    // will we not need to inspect all elements. No need to optimize the
    // case that will fail.
    has_nan = has_nan || isnan(X[i]) || isinf(X[i]);
  }
  __syncthreads();
  if (has_nan) {
    result[0] = true;
  }
}

template <>
bool NanCheckOp<CUDAContext>::RunOnDevice() {
  auto& X = Input(0);
  auto* Y = Output(0);
  const size_t N = X.size();
  const float* data_ptr = X.data<float>();

  scratch_.Resize(1);
  math::Set<bool, CUDAContext>(
      1, false, scratch_.mutable_data<bool>(), &context_);
  NanCheckKernel<<<
      CAFFE_GET_BLOCKS(N),
      CAFFE_CUDA_NUM_THREADS,
      0,
      context_.cuda_stream()>>>(
      N, X.data<float>(), scratch_.mutable_data<bool>());

  bool result = false;
  {
    std::lock_guard<std::mutex> lock(CUDAContext::mutex());
    CUDA_ENFORCE(cudaMemcpyAsync(
        &result,
        scratch_.raw_data(),
        1,
        cudaMemcpyDefault,
        context_.cuda_stream()));
  }
  // Note: we must synchronize here so we can inspect the result
  context_.FinishDeviceComputation();

  // Print out diagnostic info if we have a NaN or inf
  if (result) {
    std::cerr << "Tensor contained NaN or inf: " << this->def().input(0)
              << std::endl;

    for (int j = 0; j < InputSize(); j++) {
      TensorCPU cpu_X;
      cpu_X.ResizeLike(Input(j));
      // Hack to cause allocaiton happen here, so it won't happen
      // when we do CopyFrom. We need the mutex then because host->gpu
      // copies seem to possibly lock with NCCL.
      cpu_X.mutable_data<float>();

      {
        std::lock_guard<std::mutex> lock(CUDAContext::mutex());
        cpu_X.CopyFrom(Input(j), &context_);
      }
      context_.FinishDeviceComputation();
      std::cerr << "Input tensor: " << j << ": [" << def().input(j) << "]"
                << std::endl;
      tensorPrinter_.Print<float>(cpu_X);

      if (j == 0) {
        std::cerr << "NaN idxs:" << std::endl;
        auto* cpu_X_data = cpu_X.data<float>();
        for (size_t i = 0; i < cpu_X.size(); ++i) {
          if (isnan(cpu_X_data[i]) || isinf(cpu_X_data[i])) {
            std::cerr << i << " ";
          }
        }
      }
      std::cerr << std::endl;
    }
    return false;
  }

  // This op should act as an identity matrix if we don't find any NaNs/infs.
  // Copy over the data if we are not doing this in-place.
  if (&X != Y) {
    Y->CopyFrom(X, &context_);
  }
  return true;
}

REGISTER_CUDA_OPERATOR(NanCheck, NanCheckOp<CUDAContext>);

__global__ void
ElwiseMaxKernel(const float* X, const float* Y, float* maxout, const int N) {
  CUDA_1D_KERNEL_LOOP(i, N) {
    maxout[i] = max(X[i], Y[i]);
  }
}

template <>
bool MaxOp<float, CUDAContext>::Compute() {
  float* output_data = Output(0)->mutable_data<float>();
  const int N = Input(0).size();

  // Run pairwise-maxes
  for (int i = 1; i < InputSize(); ++i) {
    ElwiseMaxKernel<<<
        CAFFE_GET_BLOCKS(N),
        CAFFE_CUDA_NUM_THREADS,
        0,
        context_.cuda_stream()>>>(
        (i == 0 ? Input(0).data<float>() : Output(0)->data<float>()),
        Input(i).data<float>(),
        output_data,
        N);
  }

  return true;
}

REGISTER_CUDA_OPERATOR(Max, MaxOp<float, CUDAContext>);

template<typename T_INDEX>
__global__ void
GatherKernel(const float* X, float* Y, const T_INDEX* indices, const int N, const int block_size) {
  for (int i = blockIdx.x; i < N; i += gridDim.x) {
    T_INDEX idx = indices[i];
    const float* src_offset = X + idx * block_size;
    float* dst_offset = Y + i   * block_size;
    for (int j = threadIdx.x; j < block_size; j += blockDim.x) {
      dst_offset[j] = src_offset[j];
    }
  }
}

template <>
bool GatherOp<CUDAContext>::RunOnDevice() {
  return DispatchHelper<TensorTypes<int32_t,int64_t>>::call(
      this, OperatorBase::Input<TensorCUDA>(INDICES));
}

template <>
template <typename Index>
bool GatherOp<CUDAContext>::DoRunWithType() {
  auto& data = Input(DATA);
  auto& indices = Input(INDICES);
  auto* output = Output(0);

  CAFFE_ENFORCE_GE(data.ndim(), 1, "DATA should be at least 1-D");
  auto shape = indices.dims();
  shape.insert(shape.end(), data.dims().begin() + 1, data.dims().end());
  output->Resize(shape);

  int block_size = data.size() / data.dim(0);
  auto block_bytesize = data.size_from_dim(1) * data.meta().itemsize();
  CAFFE_ENFORCE(
      block_bytesize == data.nbytes() / data.dim(0),
      "block_bytesize should be consistent with data dim");
  int N = indices.size();

  auto src_base = static_cast<const float*>(data.raw_data());
  const Index* idxs = indices.template data<Index>();
  auto out = static_cast<float*>(output->raw_mutable_data(data.meta()));

  GatherKernel<<<
      std::min(N, CAFFE_MAXIMUM_NUM_BLOCKS),
      CAFFE_CUDA_NUM_THREADS,
      0,
      context_.cuda_stream()>>>(
        src_base, out, idxs, N, block_size
      );
  return true;
}

REGISTER_CUDA_OPERATOR(Gather, GatherOp<CUDAContext>);

/**
 * @brief Update slices of Y in-place with a batch of weighted X's.
 * Y[idx] = alpha[b] * X[b][i] + Y[idx]
 * i=0,...,N-1
 * b=0,...,B-1
 * idx=Indices[i]
 */
template<typename T_INDEX>
__global__ void 
AxpySliceKernel(
             const TIndex N,
             const TIndex B,
             const TIndex slice_size,
             const float** alpha,
             const float** X,
             const T_INDEX* Indices, 
             float* Y,
             const TIndex M) {
  for (int i = blockIdx.x; i < N; i += gridDim.x) {
    T_INDEX idx = Indices[i];
    float* y_offset = Y + (idx * slice_size);
    for (int b = 0; b < B; b++) {
      const float* x_offset = X[b] + (i * slice_size);
      for (int j = threadIdx.x; j < slice_size; j += blockDim.x) {
        atomicAdd(&y_offset[j], (*alpha[b]) * x_offset[j]);
      }
    }
  }
}

template <>
bool ScatterWeightedSumOp<float,CUDAContext>::RunOnDevice() {
    return DispatchHelper<TensorTypes<int32_t, int64_t>>::call(this, Input(2));
}

template <>
template <typename Index>
bool ScatterWeightedSumOp<float,CUDAContext>::DoRunWithType() {
  DCHECK_EQ(InputSize() % 2, 1);
  auto& X0 = Input(0);
  auto& weight0 = Input(1);
  auto& indices = Input(2);
  auto* output = Output(0);

  CAFFE_ENFORCE_EQ(&X0, output, "In place operation is required");
  DCHECK_GT(X0.size(), 0);
  DCHECK_GT(X0.ndim(), 0) << "X0 has to be at least the vector";
  DCHECK_EQ(weight0.size(), 1);

  TIndex M = X0.size();
  TIndex N = X0.dim(0);
  TIndex K = indices.size();
  TIndex block_size = M / N;

  T* data = output->template mutable_data<T>();
  const Index* Indices = indices.template data<Index>();

  float w0;
  context_.Copy<float,CUDAContext,CPUContext>(1, weight0.template data<float>(), &w0);
  OPERATOR_NEEDS_FEATURE(
    w0 == 1.0,
    "ScatterWeightedSumOp only supports weight_0=1 on CUDAContext");

  const TIndex B = (InputSize()-3)/2;

  // In order to have all device pointers of x_i (and weight_i similarly) consecutively 
  // in device memory, copy pointers to a host vector and then copy back into a device array.
  x_data_host_.Resize(B);
  weights_host_.Resize(B);
  x_data_device_.Resize(B);
  weights_device_.Resize(B);
  CAFFE_ENFORCE(x_data_host_.size() == B); 
  CAFFE_ENFORCE(weights_host_.size() == B); 
  CAFFE_ENFORCE(x_data_device_.size() == B); 
  CAFFE_ENFORCE(weights_device_.size() == B); 
  const float** x_data_host = x_data_host_.mutable_data<const float*>();
  const float** weights_host = weights_host_.mutable_data<const float*>();
  const float** x_data_device = x_data_device_.mutable_data<const float*>();
  const float** weights_device = weights_device_.mutable_data<const float*>();
  for (int inp = 3; inp < InputSize(); inp += 2) {
    x_data_host [(inp-3)/2] = static_cast<const float*>(Input(inp).raw_data());
    weights_host[(inp-3)/2] = static_cast<const float*>(Input(inp+1).raw_data());
  }
  context_.Copy<const float*,CPUContext,CUDAContext>(B, x_data_host, x_data_device);
  context_.Copy<const float*,CPUContext,CUDAContext>(B, weights_host, weights_device);

  AxpySliceKernel<<<
    std::min<TIndex>(K, CAFFE_MAXIMUM_NUM_BLOCKS),
    CAFFE_CUDA_NUM_THREADS, 0, context_.cuda_stream()>>>
    (
      K, B, block_size, weights_device, x_data_device, Indices, data, M
    );

  return true;
}

REGISTER_CUDA_OPERATOR(ScatterWeightedSum, ScatterWeightedSumOp<float,CUDAContext>);

}  // namespace caffe2
