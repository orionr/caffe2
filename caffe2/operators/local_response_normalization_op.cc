#include "caffe2/operators/local_response_normalization_op.h"

namespace caffe2 {

template<>
bool LRNOp<float, CPUContext>::RunOnDeviceWithOrderNCHW() {
  // Note(Yangqing): this one is copied from my Caffe implementation.
  auto& X = Input(0);
  auto* Y = Output(0);
  auto* scale = Output(1);
  DCHECK_EQ(X.ndim(), 4);
  const int N = X.dim(0);
  const int C = X.dim(1);
  const int H = X.dim(2);
  const int W = X.dim(3);
  const int image_size = C * H * W;
  const float* Xdata = X.data();
  Y->ReshapeLike(X);
  scale->ReshapeLike(X);
  float* Ydata = Y->mutable_data();
  float* scale_data = scale->mutable_data();
  math::Set<float, CPUContext>(X.size(), bias_, scale_data, &device_context_);
  Tensor<float, CPUContext> padded_square(
      std::vector<int>{C + size_ - 1, H, W});
  float* padded_square_data = padded_square.mutable_data();
  math::Set<float, CPUContext>(padded_square.size(), 0., padded_square_data,
                               &device_context_);
  const float alpha_over_size = alpha_ / size_;
  // go through the images
  for (int n = 0; n < N; ++n) {
    // compute the padded square
    math::Sqr<float, CPUContext>(image_size, Xdata + image_size * n,
                                 padded_square_data + pre_pad_ * H * W,
                                 &device_context_);
    // Create the first channel scale
    for (int c = 0; c < size_; ++c) {
      math::Axpy<float, CPUContext>(
          H * W, &alpha_over_size, padded_square_data + c * H * W,
          scale_data + image_size * n, &device_context_);
    }
    for (int c = 1; c < C; ++c) {
      float* this_scale_slice = scale_data + n * image_size + c * H * W;
      // copy previous scale
      device_context_.Copy<float, CPUContext, CPUContext>(
          H * W, this_scale_slice - H * W, this_scale_slice);
      // add head
      math::Axpy<float, CPUContext>(
          H * W, &alpha_over_size, padded_square_data + (c + size_ - 1) * H * W,
          this_scale_slice, &device_context_);
      // subtract tail
      // negative_aos is in order to cope with math::Axpy's requirement.
      const float negative_aos = -alpha_over_size;
      math::Axpy<float, CPUContext>(
          H * W, &negative_aos, padded_square_data + (c - 1) * H * W,
          this_scale_slice, &device_context_);
    }
  }
  math::Powx<float, CPUContext>(
      X.size(), scale_data, -beta_, Ydata, &device_context_);
  math::Mul<float, CPUContext>(X.size(), Ydata, Xdata, Ydata, &device_context_);
  return true;
}

template<>
bool LRNOp<float, CPUContext>::RunOnDeviceWithOrderNHWC() {
  // Note(Yangqing): This one is copied from my Decaf implementation. How many
  // variants have I written...?
  auto& X = Input(0);
  auto* Y = Output(0);
  auto* scale = Output(1);
  DCHECK_EQ(X.ndim(), 4);
  const int N = X.dim(0);
  const int H = X.dim(1);
  const int W = X.dim(2);
  const int C = X.dim(3);
  const int num_rows = N * H * W;
  const float* Xdata = X.data();
  Y->ReshapeLike(X);
  scale->ReshapeLike(X);
  float* Ydata = Y->mutable_data();
  float* scale_data = scale->mutable_data();

  Tensor<float, CPUContext> padded_square(std::vector<int>(1, C + size_ - 1));
  float* padded_square_data = padded_square.mutable_data();
  math::Set<float, CPUContext>(padded_square.size(), 0., padded_square_data,
                               &device_context_);
  const float alpha_over_size = alpha_ / size_;

  for (int n = 0; n < num_rows; ++n) {
    for (int c = 0; c < C; ++c) {
      padded_square_data[c + pre_pad_] =
          Xdata[n * C + c] * Xdata[n * C + c] * alpha_over_size;
    }
    float accum_scale = 0.;
    for (int i = 0; i < size_ - 1; ++i) {
      accum_scale += padded_square_data[i];
    }
    for (int c = 0; c < C; ++c) {
      accum_scale += padded_square_data[c + size_ - 1];
      scale_data[n * C + c] = bias_ + accum_scale;
      accum_scale -= padded_square_data[c];
    }
  }
  math::Powx<float, CPUContext>(
      X.size(), scale_data, -beta_, Ydata, &device_context_);
  math::Mul<float, CPUContext>(X.size(), Ydata, Xdata, Ydata, &device_context_);
  return true;
}

template <>
bool LRNGradientOp<float, CPUContext>::RunOnDeviceWithOrderNCHW() {
  auto& X = Input(0);
  auto& Y = Input(1);
  auto& scale = Input(2);
  auto& dY = Input(3);
  auto* dX = Output(0);
  DCHECK_EQ(X.ndim(), 4);
  const int N = X.dim(0);
  const int C = X.dim(1);
  const int H = X.dim(2);
  const int W = X.dim(3);
  const int image_size = C * H * W;
  // Loosely checking the size, assuming that the shapes will be the same as
  // long as the sizes check out.
  DCHECK_EQ(X.size(), Y.size());
  DCHECK_EQ(X.size(), scale.size());
  DCHECK_EQ(X.size(), dY.size());
  dX->ReshapeLike(X);

  const float* Xdata = X.data();
  const float* Ydata = Y.data();
  const float* scale_data = scale.data();
  const float* dYdata = dY.data();
  float* dXdata = dX->mutable_data();

  Tensor<float, CPUContext> padded_ratio(
      std::vector<int>{C + size_ - 1, H, W});
  float* padded_ratio_data = padded_ratio.mutable_data();
  math::Set<float, CPUContext>(padded_ratio.size(), 0., padded_ratio_data,
                               &device_context_);
  Tensor<float, CPUContext> accum_ratio(std::vector<int>{H, W});
  float* accum_ratio_data = accum_ratio.mutable_data();


  const float cache_ratio = 2. * alpha_ * beta_ / size_;
  const int inverse_pre_pad = size_ - (size_ + 1) / 2;

  int offset = 0;
  for (int n = 0; n < N; ++n) {
    // first, compute diff_i * y_i / s_i
    math::Mul<float, CPUContext>(
        image_size, dYdata + offset, Ydata + offset,
        padded_ratio_data + inverse_pre_pad * H * W, &device_context_);
    math::Div<float, CPUContext>(
        image_size, padded_ratio_data + inverse_pre_pad * H * W,
        scale_data + offset,
        padded_ratio_data + inverse_pre_pad * H * W, &device_context_);
    // Now, compute the accumulated ratios and the bottom diff
    math::Set<float, CPUContext>(accum_ratio.size(), 0., accum_ratio_data,
                                 &device_context_);
    for (int c = 0; c < size_ - 1; ++c) {
      static const float kOne = 1.;
      math::Axpy<float, CPUContext>(H * W, &(kOne),
                                    padded_ratio_data + c * H * W,
                                    accum_ratio_data, &device_context_);
    }
    for (int c = 0; c < C; ++c) {
      for (int hw = 0; hw < H * W; ++hw) {
        accum_ratio_data[hw] += padded_ratio_data[(c + size_ - 1) * H * W + hw];
        dXdata[offset] =
            dYdata[offset] * pow(scale_data[offset], -beta_) -
            cache_ratio * accum_ratio_data[hw] * Xdata[offset];
        accum_ratio_data[hw] -= padded_ratio_data[c * H * W + hw];
        ++offset;
      }
    }
  }
  return true;
}

template <>
bool LRNGradientOp<float, CPUContext>::RunOnDeviceWithOrderNHWC() {
  auto& X = Input(0);
  auto& Y = Input(1);
  auto& scale = Input(2);
  auto& dY = Input(3);
  auto* dX = Output(0);
  DCHECK_EQ(X.ndim(), 4);
  const int N = X.dim(0);
  const int H = X.dim(1);
  const int W = X.dim(2);
  const int C = X.dim(3);
  // Loosely checking the size, assuming that the shapes will be the same as
  // long as the sizes check out.
  DCHECK_EQ(X.size(), Y.size());
  DCHECK_EQ(X.size(), scale.size());
  DCHECK_EQ(X.size(), dY.size());
  dX->ReshapeLike(X);
  Tensor<float, CPUContext> padded_ratio(std::vector<int>(1, C + size_ - 1));
  float* padded_ratio_data = padded_ratio.mutable_data();
  math::Set<float, CPUContext>(padded_ratio.size(), 0., padded_ratio_data,
                               &device_context_);
  // the ratio 2*alpha*beta/size
  const float cache_ratio = 2. * alpha_ * beta_ / size_;
  const int num_rows = N * H * W;
  const float* Xdata = X.data();
  const float* Ydata = Y.data();
  const float* scale_data = scale.data();
  const float* dYdata = dY.data();
  float* dXdata = dX->mutable_data();
  for (int n = 0; n < num_rows; ++n) {
    const int offset = n * C;
    for (int c = 0; c < C; ++c) {
      padded_ratio_data[c + pre_pad_] =
          Ydata[offset + c] * dYdata[offset + c] / scale_data[offset + c];
    }
    float accum_ratio = 0.;
    for (int c = 0; c < size_ - 1; ++c) {
      accum_ratio += padded_ratio_data[c];
    }
    for (int c = 0; c < C; ++c) {
      accum_ratio += padded_ratio_data[c + size_ - 1];
      dXdata[offset + c] =
          dYdata[offset + c] * pow(scale_data[offset + c], -beta_) -
          cache_ratio * Xdata[offset + c] * accum_ratio;
      accum_ratio -= padded_ratio_data[c];
    }
  }
  return true;
}

namespace {
REGISTER_CPU_OPERATOR(LRN, LRNOp<float, CPUContext>);
REGISTER_CPU_OPERATOR(LRNGradient, LRNGradientOp<float, CPUContext>);
}  // namespace
}  // namespace caffe2
