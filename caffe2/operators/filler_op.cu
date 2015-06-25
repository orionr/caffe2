#include "caffe2/core/context_gpu.h"
#include "caffe2/operators/filler_op.h"

namespace caffe2 {

namespace {
__global__ void FillRangeKernel(const int n, float* data) {
  CUDA_1D_KERNEL_LOOP(index, n) {
    data[index] = index;
  }
}
}

template <>
bool RangeFillOp<float, CUDAContext>::Fill(
    Tensor<float, CUDAContext>* output) {
  int N = output->size();
  FillRangeKernel<<<CAFFE_GET_BLOCKS(N), CAFFE_CUDA_NUM_THREADS,
                    0, device_context_.cuda_stream()>>>(
      N, output->mutable_data());
  return true;
}

namespace {

REGISTER_CUDA_OPERATOR(UniformFill, UniformFillOp<float, CUDAContext>)
REGISTER_CUDA_OPERATOR(ConstantFill, ConstantFillOp<float, CUDAContext>)
REGISTER_CUDA_OPERATOR(GivenTensorFill, GivenTensorFillOp<float, CUDAContext>)
REGISTER_CUDA_OPERATOR(GaussianFill, GaussianFillOp<float, CUDAContext>)
REGISTER_CUDA_OPERATOR(XavierFill, XavierFillOp<float, CUDAContext>)
REGISTER_CUDA_OPERATOR(RangeFill, RangeFillOp<float, CUDAContext>)

}  // namespace
}  // namespace caffe2
