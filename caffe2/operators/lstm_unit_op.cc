#include "lstm_unit_op.h"

namespace caffe2 {

namespace {
REGISTER_CPU_OPERATOR(LSTMUnit, LSTMUnitOp<float, CPUContext>);
OPERATOR_SCHEMA(LSTMUnit).NumInputs(4).NumOutputs(2).SetDoc(R"DOC(

LSTMUnit computes the activations of a standard LSTM (without peephole
connections), in a sequence-length aware fashion.

Concretely, given the (fused) inputs X (TxNxD), the previous cell
state (NxD), and the sequence lengths (N), computes the LSTM
activations, avoiding computation if the input is invalid (as in, the
value at X{t][n] >= seqLengths[n].

)DOC");
REGISTER_CPU_OPERATOR(LSTMUnitGradient, LSTMUnitGradientOp<float, CPUContext>);
OPERATOR_SCHEMA(LSTMUnitGradient).NumInputs(8).NumOutputs(2);

class GetLSTMUnitGradient : public GradientMakerBase {
  using GradientMakerBase::GradientMakerBase;
  vector<OperatorDef> GetGradientDefs() override {
    return SingleGradientDef(
        "LSTMUnitGradient",
        "",
        vector<string>{I(0), I(1), I(2), I(3), O(0), O(1), GO(0), GO(1)},
        vector<string>{GI(0), GI(1)});
  }
};
REGISTER_GRADIENT(LSTMUnit, GetLSTMUnitGradient);
}
}
