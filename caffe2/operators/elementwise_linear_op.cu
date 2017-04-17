#include <assert.h>

#include "elementwise_linear_op.h"

#include "caffe2/core/context_gpu.h"
#include "caffe2/operators/operator_fallback_gpu.h"


namespace caffe2 {

namespace {
__global__ void ElementwiseLinearKernel(const int N, const int D,
  const float* X_data, const float* a_data, const float* b_data,
  float* Y_data) {
    CUDA_1D_KERNEL_LOOP(i, N * D) {
      int d = i % D;
      Y_data[i] = X_data[i] * a_data[d] + b_data[d];
    }
}

__global__ void ElementwiseLinearGradientKernel(const int N, const int D,
  const float* g_o_data, const float* X_data, const float* a_data,
  float* g_X_data, float* g_a_data, float* g_b_data) {
  CUDA_1D_KERNEL_LOOP(d, D) {
    for (int n = 0; n < N; ++n) {
      g_X_data[n * D + d] = g_o_data[n * D + d] * a_data[d];
      g_a_data[d] += g_o_data[n * D + d] * X_data[n * D + d];
      g_b_data[d] += g_o_data[n * D + d];
    }
  }
}

}  // namespace


template<>
bool ElementwiseLinearOp<float, CUDAContext>::RunOnDevice(){
  const auto& X = Input(0);
  const auto& a = Input(1);
  const auto& b = Input(2);
  auto* Y = Output(0);
  CAFFE_ENFORCE(X.ndim() == 2, X.ndim());
  CAFFE_ENFORCE(a.ndim() == 1, a.ndim());
  CAFFE_ENFORCE(X.dim32(1) == a.dim32(0));
  CAFFE_ENFORCE(a.dims() == b.dims());
  Y->ResizeLike(X);

  const int N = X.dim32(0);
  const int D = X.dim32(1);

  ElementwiseLinearKernel<<<CAFFE_GET_BLOCKS(N * D), CAFFE_CUDA_NUM_THREADS,
                          0, context_.cuda_stream()>>>(
    N, D, X.data<float>(), a.data<float>(), b.data<float>(),
    Y->mutable_data<float>());
  return true;
}


template<>
bool ElementwiseLinearGradientOp<float, CUDAContext>::RunOnDevice(){
  const auto& g_o = Input(0);
  const auto& X = Input(1);
  const auto& a = Input(2);
  CAFFE_ENFORCE(X.ndim() == 2, X.ndim());
  CAFFE_ENFORCE(a.ndim() == 1, a.ndim());
  CAFFE_ENFORCE(X.dim32(1) == a.dim32(0));

  auto *g_X = Output(0);
  auto *g_a = Output(1);
  auto *g_b = Output(2);
  g_X->ResizeLike(X);
  g_a->ResizeLike(a);
  g_b->ResizeLike(a);

  const int N = X.dim32(0);
  const int D = X.dim32(1);

  float* g_a_data = g_a->mutable_data<float>();
  float* g_b_data = g_b->mutable_data<float>();
  math::Set<float, CUDAContext>(g_a->size(), 0.f, g_a_data, &context_);
  math::Set<float, CUDAContext>(g_b->size(), 0.f, g_b_data, &context_);

  ElementwiseLinearGradientKernel<<<CAFFE_GET_BLOCKS(D), CAFFE_CUDA_NUM_THREADS,
                                  0, context_.cuda_stream()>>>(
    N, D, g_o.data<float>(), X.data<float>(), a.data<float>(),
    g_X->mutable_data<float>(), g_a_data, g_b_data);
  return true;
}

namespace {

REGISTER_CUDA_OPERATOR(ElementwiseLinear,
                       ElementwiseLinearOp<float, CUDAContext>);
REGISTER_CUDA_OPERATOR(ElementwiseLinearGradient,
                       ElementwiseLinearGradientOp<float, CUDAContext>);

}  // namespace

}  // namespace caffe2
