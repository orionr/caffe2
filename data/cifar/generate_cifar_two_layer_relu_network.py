from pycaffe2 import core
from pycaffe2 import core_gradients

init_net = core.Net("init")
W1 = init_net.UniformFill([], "W1", shape=[256, 3072], min=-0.05, max=0.05)
B1 = init_net.ConstantFill([], "B1", shape=[256], value=0.0)
W2 = init_net.UniformFill([], "W2", shape=[10, 256], min=-0.1, max=0.1)
B2 = init_net.ConstantFill([], "B2", shape=[10], value=0.0)
LR = init_net.ConstantFill([], "LR", shape=[1], value=-0.1)
ONE = init_net.ConstantFill([], "ONE", shape=[1], value=1.0)
DECAY = init_net.ConstantFill([], "DECAY", shape=[1], value=0.999)

train_net = core.Net("train")
data, label = train_net.TensorProtosDBInput(
    [], ["data", "label"], batch_size=64,
    db="gen/data/cifar/cifar-10-train-leveldb")
softmax = (data.Flatten([], "data_flatten")
               .FC([W1, B1], "hidden")
               .Relu([], "hidden_relu")
               .FC([W2, B2], 'pred')
               .Softmax([], "softmax"))
# Cross entropy, and accuracy
xent = softmax.LabelCrossEntropy([label], "xent")
accuracy = softmax.Accuracy([label], "accuracy")
# The loss function.
loss, xent_grad = xent.AveragedLoss([], ["loss", xent.Grad()])
# Get gradient, skipping the input and flatten layers.
train_net.AddGradientOperators(first=2)
# parameter update.
for param in [W1, B1, W2, B2]:
  train_net.WeightedSum([param, ONE, param.Grad(), LR], param)
LR = train_net.Mul([LR, DECAY], "LR")
train_net.Print([LR, accuracy])

# Run all on GPU.
init_net.RunAllOnGPU()
train_net.RunAllOnGPU()

plan = core.Plan("mnist_relu_network")
plan.AddNets([init_net, train_net])
plan.AddStep(core.ExecutionStep("init", init_net))
plan.AddStep(core.ExecutionStep("train", train_net, 10000))

with open('cifar_relu_network.pbtxt', 'w') as fid:
  fid.write(str(plan.Proto()))
