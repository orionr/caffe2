
# Accumulate

Accumulate operator accumulates the input tensor to the output tensor. If the output tensor already has the right size, we add to it; otherwise, we first initialize the output tensor to all zeros, and then do accumulation. Any further calls to the operator, given that no one else fiddles with the output in the interim, will do simple accumulations.
Accumulation is done using Axpby operation as shown:  
````
  Y = 1*X + gamma*Y

````
 where X is the input tensor, Y is the output tensor and gamma is the multiplier argument.

### Interface
<table><tr><td>*Arguments*
</td><td>
</td></tr><tr><td>`gamma`
</td><td>(float, default 1.0) Accumulation multiplier
</td></tr><tr><td>*Inputs*
</td><td>
</td></tr><tr><td>`input`
</td><td>The input tensor that has to be accumulated to the output tensor. If the output size is not the same as input size, the output tensor is first reshaped and initialized to zero, and only then, accumulation is done.
</td></tr><tr><td>*Outputs*
</td><td>
</td></tr><tr><td>`output`
</td><td>Accumulated output tensor
</td></tr></table>
### Code
[caffe2/operators/accumulate_op.cc](https://github.com/caffe2/caffe2/blob/master/caffe2/operators/accumulate_op.cc)
### Devices

- *CPU* `caffe2::AccumulateOp<float, caffe2::CPUContext>`

- *GPU* `caffe2::AccumulateOp<float, caffe2::CUDAContext>`




---


# Accuracy

Accuracy takes two inputs- predictions and labels, and returns a float accuracy value for the batch. Predictions are expected in the form of 2-D tensor containing a batch of scores for various classes, and labels are expected in the  form of 1-D tensor containing true label indices of samples in the batch. If the score for the label index in the predictions is the highest among all classes, it is considered a correct prediction.

### Interface
<table><tr><td>*Inputs*
</td><td>
</td></tr><tr><td>`predictions`
</td><td>2-D tensor (Tensor<float>) of size (num_batches x num_classes) containing scores
</td></tr><tr><td>`labels`
</td><td>1-D tensor (Tensor<int>) of size (num_batches) having the indices of true labels
</td></tr><tr><td>*Outputs*
</td><td>
</td></tr><tr><td>`accuracy`
</td><td>1-D tensor (Tensor<float>) of size 1 containing accuracy
</td></tr></table>
### Code
[caffe2/operators/accuracy_op.cc](https://github.com/caffe2/caffe2/blob/master/caffe2/operators/accuracy_op.cc)
### Devices

- *CPU* `caffe2::AccuracyOp<float, caffe2::CPUContext>`

- *GPU* `caffe2::AccuracyOp<float, caffe2::CUDAContext>`




---


# Add

Performs element-wise binary addition (with limited broadcast support).
 If necessary the right-hand-side argument will be broadcasted to match the shape of left-hand-side argument. When broadcasting is specified, the second tensor can either be of size 1 (a scalar value), or having its shape as a contiguous subset of the first tensor's shape. The starting of the mutually equal shape is specified by the argument "axis", and if it is not set, suffix matching is assumed. 1-dim expansion doesn't work yet.
 For example, the following tensor shapes are supported (with broadcast=1):   
````
  shape(A) = (2, 3, 4, 5), shape(B) = (,), i.e. B is a scalar
  shape(A) = (2, 3, 4, 5), shape(B) = (5,)
  shape(A) = (2, 3, 4, 5), shape(B) = (4, 5)
  shape(A) = (2, 3, 4, 5), shape(B) = (3, 4), with axis=1
  shape(A) = (2, 3, 4, 5), shape(B) = (2), with axis=0


````
 Argument  `broadcast=1`  needs to be passed to enable broadcasting.

### Interface
<table><tr><td>*Arguments*
</td><td>
</td></tr><tr><td>`broadcast`
</td><td>Pass 1 to enable broadcasting
</td></tr><tr><td>`axis`
</td><td>If set, defines the broadcast dimensions. See doc for details.
</td></tr><tr><td>*Inputs*
</td><td>
</td></tr><tr><td>`A`
</td><td>First operand, should share the type with the second operand.
</td></tr><tr><td>`B`
</td><td>Second operand. With broadcasting can be of smaller size than A. If broadcasting is disabled it should be of the same size.
</td></tr><tr><td>*Outputs*
</td><td>
</td></tr><tr><td>`C`
</td><td>Result, has same dimensions and type as A
</td></tr></table>
### Code
[caffe2/operators/elementwise_op_schema.cc](https://github.com/caffe2/caffe2/blob/master/caffe2/operators/elementwise_op_schema.cc)
### Devices

- *CPU* `caffe2::BinaryElementwiseOp<caffe2::TensorTypes<int, long, float, double>, caffe2::CPUContext, caffe2::EigenAddFunctor, caffe2::SameTypeAsInput>`

- *GPU* `caffe2::BinaryElementwiseOp<caffe2::TensorTypes<int, long, float, double>, caffe2::CUDAContext, caffe2::CudaAddFunctor, caffe2::SameTypeAsInput>`




---


# AddPadding

Given a partitioned tensor T<N, D1..., Dn>, where the partitions are defined as ranges on its outer-most (slowest varying) dimension N, with given range lengths, return a tensor T<N + 2*pad_width, D1 ..., Dn> with paddings added to the start and end of each range.
Optionally, different paddings can be provided for beginning and end. Paddings provided must be a tensor T<D1..., Dn>.
 If no padding is provided, add zero padding.
If no lengths vector is provided, add padding only once, at the start and end of data.

### Interface
<table><tr><td>*Arguments*
</td><td>
</td></tr><tr><td>`pad_width`
</td><td>Number of copies of padding to add around each range.
</td></tr><tr><td>`end_pad_width`
</td><td>(Optional) Specifies a different end-padding width.
</td></tr><tr><td>*Inputs*
</td><td>
</td></tr><tr><td>`data_in`
</td><td>(T<N, D1..., Dn>) Input data
</td></tr><tr><td>`lengths`
</td><td>(i64) Num of elements in each range. sum(lengths) = N.
</td></tr><tr><td>`start_padding`
</td><td>T<D1..., Dn> Padding data for range start.
</td></tr><tr><td>`end_padding`
</td><td>T<D1..., Dn> (optional) Padding for range end. If not provided, start_padding is used as end_padding as well.
</td></tr><tr><td>*Outputs*
</td><td>
</td></tr><tr><td>`data_out`
</td><td>(T<N + 2*pad_width, D1..., Dn>) Padded data.
</td></tr><tr><td>`lengths_out`
</td><td>(i64, optional) Lengths for each padded range.
</td></tr></table>
### Code
[caffe2/operators/sequence_ops.cc](https://github.com/caffe2/caffe2/blob/master/caffe2/operators/sequence_ops.cc)
### Devices

- *CPU* `caffe2::(anonymous namespace)::AddPaddingOp`




---


# Alias

Makes the output and the input share the same underlying storage.
 WARNING: in general, in caffe2's operator interface different tensors should have different underlying storage, which is the assumption made by components such as the dependency engine and memory optimization. Thus, in normal situations you should not use the AliasOp, especially in a normal forward-backward pass.
 The Alias op is provided so one can achieve true asynchrony, such as Hogwild, in a graph. But make sure you understand all the implications similar to multi-thread computation before you use it explicitly.

### Interface
<table><tr><td>*Inputs*
</td><td>
</td></tr><tr><td>`input`
</td><td>Input tensor whose storage will be shared.
</td></tr><tr><td>*Outputs*
</td><td>
</td></tr><tr><td>`output`
</td><td>Tensor of same shape as input, sharing its storage.
</td></tr></table>
### Code
[caffe2/operators/utility_ops.cc](https://github.com/caffe2/caffe2/blob/master/caffe2/operators/utility_ops.cc)
### Devices

- *CPU* `caffe2::AliasOp<caffe2::CPUContext>`

- *GPU* `caffe2::AliasOp<caffe2::CUDAContext>`




---


# Allgather

Does an allgather operation among the nodes.

### Interface
<table><tr><td>*Inputs*
</td><td>
</td></tr><tr><td>`comm_world`
</td><td>The common world.
</td></tr><tr><td>`X`
</td><td>A tensor to be allgathered.
</td></tr><tr><td>*Outputs*
</td><td>
</td></tr><tr><td>`Y`
</td><td>The allgathered tensor, same on all nodes.
</td></tr></table>
### Code
[caffe2/operators/communicator_op.cc](https://github.com/caffe2/caffe2/blob/master/caffe2/operators/communicator_op.cc)
### Devices

- *CPU* `caffe2::NoDefaultEngineOp<caffe2::CPUContext>`

- *GPU* `caffe2::NoDefaultEngineOp<caffe2::CUDAContext>`




---


# Allreduce

Does an allreduce operation among the nodes. Currently only Sum is supported.

### Interface
<table><tr><td>*Inputs*
</td><td>
</td></tr><tr><td>`comm_world`
</td><td>The common world.
</td></tr><tr><td>`X`
</td><td>A tensor to be allreduced.
</td></tr><tr><td>*Outputs*
</td><td>
</td></tr><tr><td>`Y`
</td><td>The allreduced tensor, same on all nodes.
</td></tr></table>
### Code
[caffe2/operators/communicator_op.cc](https://github.com/caffe2/caffe2/blob/master/caffe2/operators/communicator_op.cc)
### Devices

- *CPU* `caffe2::NoDefaultEngineOp<caffe2::CPUContext>`

- *GPU* `caffe2::NoDefaultEngineOp<caffe2::CUDAContext>`




---


# And

Performs element-wise logical operation  `and`  (with limited broadcast support).
Both input operands should be of type  `bool` .
 If necessary the right-hand-side argument will be broadcasted to match the shape of left-hand-side argument. When broadcasting is specified, the second tensor can either be of size 1 (a scalar value), or having its shape as a contiguous subset of the first tensor's shape. The starting of the mutually equal shape is specified by the argument "axis", and if it is not set, suffix matching is assumed. 1-dim expansion doesn't work yet.
 For example, the following tensor shapes are supported (with broadcast=1):   
````
  shape(A) = (2, 3, 4, 5), shape(B) = (,), i.e. B is a scalar
  shape(A) = (2, 3, 4, 5), shape(B) = (5,)
  shape(A) = (2, 3, 4, 5), shape(B) = (4, 5)
  shape(A) = (2, 3, 4, 5), shape(B) = (3, 4), with axis=1
  shape(A) = (2, 3, 4, 5), shape(B) = (2), with axis=0


````
 Argument  `broadcast=1`  needs to be passed to enable broadcasting.

### Interface
<table><tr><td>*Arguments*
</td><td>
</td></tr><tr><td>`broadcast`
</td><td>Pass 1 to enable broadcasting
</td></tr><tr><td>`axis`
</td><td>If set, defines the broadcast dimensions. See doc for details.
</td></tr><tr><td>*Inputs*
</td><td>
</td></tr><tr><td>`A`
</td><td>First operand.
</td></tr><tr><td>`B`
</td><td>Second operand. With broadcasting can be of smaller size than A. If broadcasting is disabled it should be of the same size.
</td></tr><tr><td>*Outputs*
</td><td>
</td></tr><tr><td>`C`
</td><td>Result, has same dimensions and A and type `bool`
</td></tr></table>
### Code
[caffe2/operators/elementwise_op_schema.cc](https://github.com/caffe2/caffe2/blob/master/caffe2/operators/elementwise_op_schema.cc)
### Devices

- *CPU* `caffe2::BinaryElementwiseOp<caffe2::TensorTypes<bool>, caffe2::CPUContext, caffe2::NaiveAndFunctor, caffe2::FixedType<bool> >`

- *GPU* `caffe2::BinaryElementwiseOp<caffe2::TensorTypes<bool>, caffe2::CUDAContext, caffe2::CudaAndFunctor, caffe2::FixedType<bool> >`




---


# Append

Append input 2 to the end of input 1.
Input 1 must be the same as output, that is, it is required to be in-place.
Input 1 may have to be re-allocated in order for accommodate to the new size.
Currently, an exponential growth ratio is used in order to ensure amortized constant time complexity.
All except the outer-most dimension must be the same between input 1 and 2.

### Interface
<table><tr><td>*Inputs*
</td><td>
</td></tr><tr><td>`dataset`
</td><td>The tensor to be appended to.
</td></tr><tr><td>`new_data`
</td><td>Tensor to append to the end of dataset.
</td></tr><tr><td>*Outputs*
</td><td>
</td></tr><tr><td>`dataset`
</td><td>Same as input 0, representing the mutated tensor.
</td></tr></table>
### Code
[caffe2/operators/dataset_ops.cc](https://github.com/caffe2/caffe2/blob/master/caffe2/operators/dataset_ops.cc)
### Devices

- *CPU* `caffe2::(anonymous namespace)::AppendOp<caffe2::CPUContext>`




---


# AtomicAppend
No documentation yet.

### Code
[caffe2/operators/dataset_ops.cc](https://github.com/caffe2/caffe2/blob/master/caffe2/operators/dataset_ops.cc)
### Devices

- *CPU* `caffe2::(anonymous namespace)::AtomicAppendOp<caffe2::CPUContext>`




---


# AtomicFetchAdd

Given a mutex and two int32 scalar tensors, performs an atomic fetch add by mutating the first argument and adding it to the second input argument. Returns the updated integer and the value prior to the update.

### Interface
<table><tr><td>*Inputs*
</td><td>
</td></tr><tr><td>`mutex_ptr`
</td><td>Blob containing to a unique_ptr<mutex>
</td></tr><tr><td>`mut_value`
</td><td>Value to be mutated after the sum.
</td></tr><tr><td>`increment`
</td><td>Value to add to the first operand.
</td></tr><tr><td>*Outputs*
</td><td>
</td></tr><tr><td>`mut_value`
</td><td>Mutated value after sum. Usually same as input 1.
</td></tr><tr><td>`fetched_value`
</td><td>Value of the first operand before sum.
</td></tr></table>
### Code
[caffe2/operators/atomic_ops.cc](https://github.com/caffe2/caffe2/blob/master/caffe2/operators/atomic_ops.cc)
### Devices

- *CPU* `caffe2::fb::(anonymous namespace)::AtomicFetchAddOp`




---


# AveragePool

AveragePool consumes an input blob X and applies average pooling across the the blob according to kernel sizes, stride sizes, and pad lengths defined by the ConvPoolOpBase operator. Average pooling consisting of averaging all values of a subset of the input tensor according to the kernel size and downsampling the data into the output blob Y for further processing.
  
### Interface
<table><tr><td>*Inputs*
</td><td>
</td></tr><tr><td>`X`
</td><td>Input data tensor from the previous operator; dimensions depend on whether the NCHW or NHWC operators are being used. For example, in the former, the input has size (N x C x H x W), where N is the batch size, C is the number of channels, and H and W are the height and the width of the data. The corresponding permutation of dimensions is used in the latter case. 
</td></tr><tr><td>*Outputs*
</td><td>
</td></tr><tr><td>`Y`
</td><td>Output data tensor from average pooling across the input tensor. Dimensions will vary based on various kernel, stride, and pad sizes.
</td></tr></table>
### Code
[caffe2/operators/pool_op.cc](https://github.com/caffe2/caffe2/blob/master/caffe2/operators/pool_op.cc)
### Devices

- *CPU* `caffe2::PoolOp<float, caffe2::CPUContext, caffe2::(anonymous namespace)::AveragePool>`

- *GPU* `caffe2::PoolOp<float, caffe2::CUDAContext, caffe2::(anonymous namespace)::AveragePool>`



### Engines
`CUDNN` on *CUDA*

---


# AveragePoolGradient
No documentation yet.

### Code
[caffe2/operators/pool_op.cc](https://github.com/caffe2/caffe2/blob/master/caffe2/operators/pool_op.cc)
### Devices

- *CPU* `caffe2::PoolGradientOp<float, caffe2::CPUContext, caffe2::(anonymous namespace)::AveragePool>`

- *GPU* `caffe2::PoolGradientOp<float, caffe2::CUDAContext, caffe2::(anonymous namespace)::AveragePool>`



### Engines
`CUDNN` on *CUDA*

---


# AveragedLoss

AveragedLoss takes in a 1-D tensor as input and returns a single output float value which represents the average of input data (average of the losses).

### Interface
<table><tr><td>*Inputs*
</td><td>
</td></tr><tr><td>`input`
</td><td>The input data as Tensor
</td></tr><tr><td>*Outputs*
</td><td>
</td></tr><tr><td>`output`
</td><td>The output tensor of size 1 containing the averaged value.
</td></tr></table>
### Code
[caffe2/operators/loss_op.cc](https://github.com/caffe2/caffe2/blob/master/caffe2/operators/loss_op.cc)
### Devices

- *CPU* `caffe2::AveragedLoss<float, caffe2::CPUContext>`

- *GPU* `caffe2::AveragedLoss<float, caffe2::CUDAContext>`




---


# AveragedLossGradient
No documentation yet.

### Code
[caffe2/operators/loss_op.cc](https://github.com/caffe2/caffe2/blob/master/caffe2/operators/loss_op.cc)
### Devices

- *CPU* `caffe2::AveragedLossGradient<float, caffe2::CPUContext>`

- *GPU* `caffe2::AveragedLossGradientGPUSpecialization`




---


# BatchMatMul

Batch Matrix multiplication Yi = Ai * Bi, where A has size (C x M x K), B has size (C x K x N) where C is the batch size and i ranges from 0 to C-1.

### Interface
<table><tr><td>*Arguments*
</td><td>
</td></tr><tr><td>`trans_a`
</td><td>Pass 1 to transpose A before multiplication
</td></tr><tr><td>`trans_b`
</td><td>Pass 1 to transpose B before multiplication
</td></tr><tr><td>*Inputs*
</td><td>
</td></tr><tr><td>`A`
</td><td>3D matrix of size (C x M x K)
</td></tr><tr><td>`B`
</td><td>3D matrix of size (C x K x N)
</td></tr><tr><td>*Outputs*
</td><td>
</td></tr><tr><td>`Y`
</td><td>3D matrix of size (C x M x N)
</td></tr></table>
### Code
[caffe2/operators/batch_matmul_op.cc](https://github.com/caffe2/caffe2/blob/master/caffe2/operators/batch_matmul_op.cc)
### Devices

- *CPU* `caffe2::BatchMatMulOp<float, caffe2::CPUContext, caffe2::DefaultEngine>`

- *GPU* `caffe2::BatchMatMulOp<float, caffe2::CUDAContext, caffe2::DefaultEngine>`




---


# BatchToSpace

 BatchToSpace for 4-D tensors of type T.
 Rearranges (permutes) data from batch into blocks of spatial data, followed by cropping. This is the reverse transformation of SpaceToBatch. More specifically, this op outputs a copy of the input tensor where values from the batch dimension are moved in spatial blocks to the height and width dimensions, followed by cropping along the height and width dimensions.
 
### Code
[caffe2/operators/space_batch_op.cc](https://github.com/caffe2/caffe2/blob/master/caffe2/operators/space_batch_op.cc)
### Devices

- *CPU* `caffe2::BatchToSpaceOp<caffe2::CPUContext>`

- *GPU* `caffe2::BatchToSpaceOp<caffe2::CUDAContext>`




---


# BooleanMask

Given a data 1D tensor and a mask (boolean) tensor of same shape, returns a tensor containing only the elements corresponding to positions where the mask is true.

### Interface
<table><tr><td>*Inputs*
</td><td>
</td></tr><tr><td>`data`
</td><td>The 1D, original data tensor.
</td></tr><tr><td>`mask`
</td><td>A tensor of bools of same shape as `data`.
</td></tr><tr><td>*Outputs*
</td><td>
</td></tr><tr><td>`masked_data`
</td><td>A tensor of same type as `data`.
</td></tr></table>
### Code
[caffe2/operators/boolean_mask_ops.cc](https://github.com/caffe2/caffe2/blob/master/caffe2/operators/boolean_mask_ops.cc)
### Devices

- *CPU* `caffe2::(anonymous namespace)::BooleanMaskOp<caffe2::CPUContext>`




---


# BooleanMaskLengths

Given a tensor of int32 segment lengths and a mask (boolean) tensor, return the segment lengths of a corresponding segmented tensor after BooleanMask is applied.

### Interface
<table><tr><td>*Inputs*
</td><td>
</td></tr><tr><td>`lengths`
</td><td>A 1D int32 tensor representing segment lengths.
</td></tr><tr><td>`mask`
</td><td>A 1D bool tensor of values to keep.
</td></tr><tr><td>*Outputs*
</td><td>
</td></tr><tr><td>`masked_lengths`
</td><td>Segment lengths of a masked tensor.
</td></tr></table>
### Code
[caffe2/operators/boolean_mask_ops.cc](https://github.com/caffe2/caffe2/blob/master/caffe2/operators/boolean_mask_ops.cc)
### Devices

- *CPU* `caffe2::(anonymous namespace)::BooleanMaskLengthsOp<caffe2::CPUContext>`




---


# Broadcast

Does a broadcast operation from the root node to every other node. The tensor on each node should have been pre-created with the same shape and data type.

### Interface
<table><tr><td>*Arguments*
</td><td>
</td></tr><tr><td>`root`
</td><td>(int, default 0) the root to run broadcast from.
</td></tr><tr><td>*Inputs*
</td><td>
</td></tr><tr><td>`comm_world`
</td><td>The common world.
</td></tr><tr><td>`X`
</td><td>A tensor to be broadcasted.
</td></tr><tr><td>*Outputs*
</td><td>
</td></tr><tr><td>`X`
</td><td>In-place as input 1.
</td></tr></table>
### Code
[caffe2/operators/communicator_op.cc](https://github.com/caffe2/caffe2/blob/master/caffe2/operators/communicator_op.cc)
### Devices

- *CPU* `caffe2::NoDefaultEngineOp<caffe2::CPUContext>`

- *GPU* `caffe2::NoDefaultEngineOp<caffe2::CUDAContext>`




---


