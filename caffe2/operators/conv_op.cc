#include "caffe2/operators/conv_op.h"
#include "caffe2/operators/conv_op_impl.h"

namespace caffe2 {
namespace {
REGISTER_CPU_OPERATOR(Conv, ConvOp<float, CPUContext>);
REGISTER_CPU_OPERATOR(ConvGradient, ConvGradientOp<float, CPUContext>);

OPERATOR_SCHEMA(Conv)
  .NumInputs(2,3)
  .NumOutputs(1)
  .SetDoc(R"DOC(
The convolution operator consumes an input vector, the filter blob and the bias
blob and computes the output. Note that other parameters, such as the stride and
kernel size, or the pads' sizes in each direction are not necessary for input
because they are provided by the ConvPoolOpBase operator. Various dimension
checks are done implicitly, and the sizes are specified in the Input docs for
this operator. As is expected, the filter is convolved with a subset of the
image and the bias is added; this is done throughout the image data and the
output is computed. As a side note on the implementation layout:
conv_op_impl.h is the templated implementation of the conv_op.h file, which is
why they are separate files.
  )DOC")
  .Input(0, "X", "Input data blob from previous layer; has size "
  "(N x C x H x W), where N is the batch size, C is the number of channels, and"
  " H and W are the height and width. Note that this is for the NCHW usage. On "
  "the other hand, the NHWC Op has a different set of dimension constraints.")
  .Input(1, "filter", "The filter blob that will be used in the convolutions; "
  "has size (M x C x kH x kW), where C is the number of channels, and kH and "
  "kW are the height and width of the kernel.")
  .Input(2, "bias", "The 1D bias blob that is added through the convolution; "
  "has size (M).")
  .Output(0, "Y", "Output data blob that contains the result of the "
  "convolution. The output dimensions are functions of the kernel size, "
  "stride size, and pad lengths."
  "");

OPERATOR_SCHEMA(ConvGradient).NumInputs(2,3).NumOutputs(2, 3);

class GetConvGradient : public GradientMakerBase {
  using GradientMakerBase::GradientMakerBase;
  vector<OperatorDef> GetGradientDefs() override {
    // todo(slayton) don't do this.
    // CAFFE_ENFORCE(3 == def_.input_size());

    vector<string> inputs, outputs;

    ArgumentHelper helper(def_);
    bool no_bias = static_cast<bool>(helper.GetSingleArgument<int>("no_bias", 0));

    if (no_bias) {
      // no bias - same inputs, only output dW, dY
      inputs = vector<string>{I(0), I(1), GO(0)};
      outputs = vector<string>{GI(1), GI(0)};
    } else {
      inputs = vector<string>{I(0), I(1), GO(0)};
      outputs = vector<string>{GI(1), GI(2), GI(0)};
    }

    return SingleGradientDef(
        "ConvGradient", "",
        inputs,
        outputs);
  }
};
REGISTER_GRADIENT(Conv, GetConvGradient);

}  // namespace
}  // namespace caffe2
