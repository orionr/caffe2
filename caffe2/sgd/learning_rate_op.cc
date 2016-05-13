#include "caffe2/sgd/learning_rate_op.h"

namespace caffe2 {
namespace {
REGISTER_CPU_OPERATOR(LearningRate, LearningRateOp<float, CPUContext>);

OPERATOR_SCHEMA(LearningRate).NumInputs(1).NumOutputs(1);

NO_GRADIENT(LearningRate);
}  // namespace
}  // namespace caffe2


