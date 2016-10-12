#ifndef CAFFE2_OPERATORS_RECUDER_FUNCTORS_H_
#define CAFFE2_OPERATORS_RECUDER_FUNCTORS_H_

#include <array>

#include "caffe2/core/context.h"
#include "caffe2/utils/math.h"

namespace caffe2 {

////////////////////////////////////////////////////////////////////////////////
// Range reducers: can leverage that input segment is continuous and provide
// special implementation
////////////////////////////////////////////////////////////////////////////////

// Put forward and backward in the same template?
template <typename T, class Context>
class SumRangeReducer;
template <typename T, class Context>
class SumRangeReducerGradient;

template <typename T>
class SumRangeReducer<T, CPUContext> {
 public:
  void operator()(
      const TIndex block_size,
      const TIndex blocks,
      const T* in,
      T* out,
      CPUContext* context) {
    // do we need to go through wrapper in math.h?
    EigenVectorMap<T> out_vec(out, block_size);
    out_vec = ConstEigenMatrixMap<T>(in, block_size, blocks).rowwise().sum();
  }
};

template <typename T, class Context>
class SumRangeReducerGradient {
 public:
  void operator()(
      const TIndex block_size,
      const TIndex blocks,
      const T* segment_grad,
      T* data_grad,
      const T* data_in, // unused
      const T* data_out, // unused
      Context* context) {
    // do we have some op that does it smartly with minimum number of memcpy?
    for (TIndex i = 0; i < blocks; ++i) {
      context->template Copy<T, Context, Context>(
          block_size, segment_grad, data_grad + block_size * i);
    }
  }
};

struct SumRangeReducerDef {
  template <typename T, class Context>
  using Reducer = SumRangeReducer<T, Context>;
  template <typename T, class Context>
  using ReducerGradient = SumRangeReducerGradient<T, Context>;
  static constexpr const char* name = "Sum";
  static constexpr const char* doc =
      "Summation is done element-wise across slices of the input tensor and "
      "doesn't change the shape of the individual blocks.";
};

// Put forward and backward in the same template?
template <typename T, class Context>
class LogSumExpRangeReducer;
template <typename T, class Context>
class LogSumExpRangeReducerGradient;

template <typename T>
class LogSumExpRangeReducer<T, CPUContext> {
 public:
  void operator()(
      const TIndex block_size,
      const TIndex blocks,
      const T* in,
      T* out,
      CPUContext* context) {
    for (int j = 0; j < block_size; ++j) {
      T max_value = std::numeric_limits<T>::lowest();
      for (int i = 0; i < blocks; ++i) {
        max_value = std::max(max_value, in[i * block_size + j]);
      }
      T scaled_exp_sum = 0;
      for (int i = 0; i < blocks; ++i) {
        scaled_exp_sum += std::exp(in[i * block_size + j] - max_value);
      }
      *(out++) = std::log(scaled_exp_sum) + max_value;
    }
  }
  T r{1};
};

template <typename T, class Context>
class LogSumExpRangeReducerGradient {
 public:
  void operator()(
      const TIndex block_size,
      const TIndex blocks,
      const T* segment_grad, // GO
      T* data_grad, // GI
      const T* data_in, // I
      const T* data_out, // O
      Context* context) {
    for (int j = 0; j < block_size; ++j) {
      const T out_grad = *(segment_grad++);
      const T offset = *(data_out++);
      for (int i = 0; i < blocks; ++i) {
        auto idx = i * block_size + j;
        data_grad[idx] = out_grad * std::exp(data_in[idx] - offset);
      }
    }
  }
};

struct LogSumExpRangeReducerDef {
  template <typename T, class Context>
  using Reducer = LogSumExpRangeReducer<T, Context>;
  template <typename T, class Context>
  using ReducerGradient = LogSumExpRangeReducerGradient<T, Context>;
  static constexpr const char* name = "LogSumExp";
  static constexpr const char* doc =
      "LogSumExp computes the element-wise log of the sum of exponentials of "
      "input slices. Operation doesn't change the shape of individual blocks.";
};

template <typename T, class Context>
class LogMeanExpRangeReducer;
template <typename T, class Context>
class LogMeanExpRangeReducerGradient;

template <typename T>
class LogMeanExpRangeReducer<T, CPUContext> {
 public:
  void operator()(
      const TIndex block_size,
      const TIndex blocks,
      const T* in,
      T* out,
      CPUContext* context) {
    for (int j = 0; j < block_size; ++j) {
      T max_value = std::numeric_limits<T>::lowest();
      for (int i = 0; i < blocks; ++i) {
        max_value = std::max(max_value, in[i * block_size + j]);
      }
      T scaled_exp_sum = 0;
      for (int i = 0; i < blocks; ++i) {
        scaled_exp_sum += std::exp(in[i * block_size + j] - max_value);
      }
      scaled_exp_sum /= blocks;
      *(out++) = std::log(scaled_exp_sum) + max_value;
    }
  }
};

template <typename T, class Context>
class LogMeanExpRangeReducerGradient {
 public:
  void operator()(
      const TIndex block_size,
      const TIndex blocks,
      const T* segment_grad, // GO
      T* data_grad, // GI
      const T* data_in, // I
      const T* data_out, // O
      Context* context) {
    for (int j = 0; j < block_size; ++j) {
      const T out_grad = *(segment_grad++);
      const T offset = *(data_out++);
      for (int i = 0; i < blocks; ++i) {
        auto idx = i * block_size + j;
        data_grad[idx] = out_grad * std::exp(data_in[idx] - offset) / blocks;
      }
    }
  }
};

struct LogMeanExpRangeReducerDef {
  template <typename T, class Context>
  using Reducer = LogMeanExpRangeReducer<T, Context>;
  template <typename T, class Context>
  using ReducerGradient = LogMeanExpRangeReducerGradient<T, Context>;
  static constexpr const char* name = "LogMeanExp";
  static constexpr const char* doc =
      "LogMeanExp computes the element-wise log of the mean of exponentials of "
      "input slices. Operation doesn't change the shape of individual blocks.";
};

template <typename T, class Context>
class MeanRangeReducer;
template <typename T, class Context>
class MeanRangeReducerGradient;

template <typename T>
class MeanRangeReducer<T, CPUContext> {
 public:
  void operator()(
      const TIndex block_size,
      const TIndex blocks,
      const T* in,
      T* out,
      CPUContext* context) {
    for (int j = 0; j < block_size; ++j) {
      T avg_value = 0;
      for (int i = 0; i < blocks; ++i) {
        avg_value += in[i * block_size + j] / blocks;
      }
      *(out++) = avg_value;
    }
  }
};

template <typename T, class Context>
class MeanRangeReducerGradient {
 public:
  void operator()(
      const TIndex block_size,
      const TIndex blocks,
      const T* segment_grad, // GO
      T* data_grad, // GI
      const T* data_in, // I
      const T* data_out, // O
      Context* context) {
    const auto in_grad = 1.0 / blocks;
    for (int j = 0; j < block_size; ++j) {
      const T out_grad = *(segment_grad++);
      for (int i = 0; i < blocks; ++i) {
        auto idx = i * block_size + j;
        data_grad[idx] = out_grad * in_grad;
      }
    }
  }
};

struct MeanRangeReducerDef {
  template <typename T, class Context>
  using Reducer = MeanRangeReducer<T, Context>;
  template <typename T, class Context>
  using ReducerGradient = MeanRangeReducerGradient<T, Context>;
  static constexpr const char* name = "Mean";
  static constexpr const char* doc =
      "Mean computation is done element-wise, so that each element of the "
      "output slice corresponds to the average value of the respective "
      "elements in the input slices. Operation doesn't change the shape of "
      "individual blocks.";
};

template <typename T, class Context>
class MaxRangeReducer;
template <typename T, class Context>
class MaxRangeReducerGradient;

template <typename T>
class MaxRangeReducer<T, CPUContext> {
 public:
  void operator()(
      const TIndex block_size,
      const TIndex blocks,
      const T* in,
      T* out,
      CPUContext* context) {
    for (int j = 0; j < block_size; ++j) {
      T max_value = std::numeric_limits<T>::lowest();
      for (int i = 0; i < blocks; ++i) {
        max_value = std::max(max_value, in[i * block_size + j]);
      }
      *(out++) = max_value;
    }
  }
};

template <typename T, class Context>
class MaxRangeReducerGradient {
 public:
  void operator()(
      const TIndex block_size,
      const TIndex blocks,
      const T* segment_grad, // GO
      T* data_grad, // GI
      const T* data_in, // I
      const T* data_out, // O
      Context* context) {
    std::memset(
        static_cast<void*>(data_grad), 0, blocks * block_size * sizeof(T));
    for (int j = 0; j < block_size; ++j) {
      const T out_grad = *(segment_grad++);
      const T out = data_out[j];
      for (int i = 0; i < blocks; ++i) {
        auto idx = i * block_size + j;
        if (out == data_in[idx]) {
          data_grad[idx] = out_grad;
          break;
        }
      }
    }
  }
};

struct MaxRangeReducerDef {
  template <typename T, class Context>
  using Reducer = MaxRangeReducer<T, Context>;
  template <typename T, class Context>
  using ReducerGradient = MaxRangeReducerGradient<T, Context>;
  static constexpr const char* name = "Max";
  static constexpr const char* doc =
      "Max computation is done element-wise, so that each element of the "
      "output slice corresponds to the max value of the respective "
      "elements in the input slices. Operation doesn't change the shape of "
      "individual blocks. This implementation imitates torch nn.Max operator. "
      "If the maximum value occurs more than once, the operator will return "
      "the first occurence of value. When computing the gradient using the "
      "backward propagation, the gradient input corresponding to the first "
      "occurence of the maximum value will be used.";
};

////////////////////////////////////////////////////////////////////////////////
// Incremental reducers: consume elements one by one
////////////////////////////////////////////////////////////////////////////////

// Put forward and backward in the same template?
template <typename T, class Context>
class SumReducer;
template <typename T, class Context>
class SumReducerGradient;

template <typename T>
class SumReducer<T, CPUContext> {
 public:
  static constexpr int kInputCount = 1;

  using FixedDispatch = FixedValues<1>;

  struct Meta {
    TIndex block_size;
    vector<TIndex> block_shape;

    void
    observeInput(int input, const Tensor<CPUContext>& value, int skip_dims) {
      DCHECK_EQ(0, input);
      auto& dims = value.dims();
      block_shape.assign(dims.begin() + skip_dims, dims.end());
      block_size = value.size_from_dim(skip_dims);
    }

    void appendOutputShape(vector<TIndex>* output_shape) {
      output_shape->insert(
          output_shape->end(), block_shape.begin(), block_shape.end());
    }
  };

  SumReducer(const Meta& meta, T* out, CPUContext* context) : out_(out) {
    // add a wrapper in Context for it
    memset(out, 0, sizeof(T) * meta.block_size);
  }
  template <int FixedSize>
  void
  process(const Meta& meta, const T* in, TIndex offset, CPUContext* context) {
    math::Axpy<T, CPUContext, FixedSize>(meta.block_size, 1, in, out_, context);
  }

 private:
  T* out_;
};

template <typename T, class Context>
class SumReducerGradient {
 public:
  // which of the original inputs are required for gradient computation
  static constexpr std::array<int, 0> originalInputs() {
    return std::array<int, 0>();
  }

  using FixedDispatch = FixedValues<1>;

  struct Meta {
    TIndex block_size;
    vector<TIndex> block_shape;

    Meta(const Tensor<CPUContext>& out_grad, int skip_dims) {
      auto& dims = out_grad.dims();
      block_shape.assign(dims.begin() + skip_dims, dims.end());
      block_size = out_grad.size_from_dim(skip_dims);
    }

    void observeOriginalInput(
        int original_input,
        const Tensor<CPUContext>& value,
        int skip_dims) {}

    void appendGradShape(vector<TIndex>* output_shape) {
      output_shape->insert(
          output_shape->end(), block_shape.begin(), block_shape.end());
    }
  };

  SumReducerGradient(const Meta& meta, const T* s_grad, CPUContext* context)
      : s_grad_(s_grad) {}

  template <int FixedSize>
  void
  fillGrad(const Meta& meta, T* data_grad, TIndex offset, Context* context) {
    if (FixedSize == 1) { // static if
      *data_grad = *s_grad_;
    } else {
      context->template Copy<T, Context, Context>(
          meta.block_size, s_grad_, data_grad);
    }
  }

 private:
  const T* s_grad_;
};

struct SumReducerDef {
  template <typename T, class Context>
  using Reducer = SumReducer<T, Context>;
  template <typename T, class Context>
  using ReducerGradient = SumReducerGradient<T, Context>;
  static constexpr const char* name = "Sum";
  static constexpr const char* doc =
      "Summation is done element-wise across slices of the input tensor and "
      "doesn't change the shape of the individual blocks.";
  static void PopulateSchema(OpSchema& schema) {}
};

// Put forward and backward in the same template?
template <typename T, class Context>
class WeightedSumReducer;
template <typename T, class Context>
class WeightedSumReducerGradient;

template <typename T>
class WeightedSumReducer<T, CPUContext> {
 public:
  static constexpr int kInputCount = 2;

  using FixedDispatch = FixedValues<1>;

  struct Meta {
    TIndex block_size;
    vector<TIndex> block_shape;
    const T* scalars;

    void
    observeInput(int input, const Tensor<CPUContext>& value, int skip_dims) {
      if (input == 1) {
        CHECK_EQ(skip_dims, value.ndim())
            << "SCALARS mustn't have extra dimensions";
        scalars = value.data<T>();
        return;
      }
      DCHECK_EQ(0, input);
      auto& dims = value.dims();
      block_shape.assign(dims.begin() + skip_dims, dims.end());
      block_size = value.size_from_dim(skip_dims);
    }

    void appendOutputShape(vector<TIndex>* output_shape) {
      output_shape->insert(
          output_shape->end(), block_shape.begin(), block_shape.end());
    }
  };

  WeightedSumReducer(const Meta& meta, T* out, CPUContext* context)
      : out_(out) {
    // do we have a wrapper for it?
    memset(out, 0, sizeof(T) * meta.block_size);
  }
  template <int FixedSize>
  void
  process(const Meta& meta, const T* in, TIndex offset, CPUContext* context) {
    math::Axpy<T, CPUContext, FixedSize>(
        meta.block_size, meta.scalars[offset], in, out_, context);
  }

 private:
  T* out_;
};

template <typename T, class Context>
class WeightedSumReducerGradient {
 public:
  // which of the original inputs are required for gradient computation
  static constexpr std::array<int, 1> originalInputs() {
    return {1};
  }

  using FixedDispatch = FixedValues<1>;

  struct Meta {
    TIndex block_size;
    vector<TIndex> block_shape;
    const T* scalars;

    Meta(const Tensor<CPUContext>& out_grad, int skip_dims) {
      auto& dims = out_grad.dims();
      block_shape.assign(dims.begin() + skip_dims, dims.end());
      block_size = out_grad.size_from_dim(skip_dims);
    }

    void observeOriginalInput(
        int original_input,
        const Tensor<CPUContext>& value,
        int skip_dims) {
      CHECK_EQ(1, original_input);
      scalars = value.data<T>();
    }

    void appendGradShape(vector<TIndex>* output_shape) {
      output_shape->insert(
          output_shape->end(), block_shape.begin(), block_shape.end());
    }
  };

  WeightedSumReducerGradient(
      const Meta& meta,
      const T* s_grad,
      CPUContext* context)
      : s_grad_(s_grad) {}

  template <int FixedSize>
  void
  fillGrad(const Meta& meta, T* data_grad, TIndex offset, Context* context) {
    math::Scale<T, CPUContext, FixedSize>(
        meta.block_size, meta.scalars[offset], s_grad_, data_grad, context);
  }

 private:
  const T* s_grad_;
};

struct WeightedSumReducerDef {
  template <typename T, class Context>
  using Reducer = WeightedSumReducer<T, Context>;
  template <typename T, class Context>
  using ReducerGradient = WeightedSumReducerGradient<T, Context>;
  static constexpr const char* name = "WeightedSum";
  static constexpr const char* doc =
      "Input slices are first scaled by SCALARS and then summed element-wise. "
      "It doesn't change the shape of the individual blocks.";
  static void PopulateSchema(OpSchema& schema) {
    schema.Input(0, "DATA", "Input tensor for the summation");
    schema.Input(
        1,
        "SCALARS",
        "Scalar multipliers for the input slices. Must be a vector with the "
        "length matching the first dimension of DATA");
  }
};

} // namespace caffe2

#endif // CAFFE2_OPERATORS_RECUDER_FUNCTORS_H_
