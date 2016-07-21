from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

import unittest
import hypothesis.strategies as st
from hypothesis import given, assume
import numpy as np
import time
import os
from caffe2.proto import caffe2_pb2
from caffe2.python import core, workspace, muji
import caffe2.python.hypothesis_test_util as hu

np.random.seed(1)


def gpu_device(i):
    device_option = caffe2_pb2.DeviceOption()
    device_option.device_type = caffe2_pb2.CUDA
    device_option.cuda_gpu_id = i
    return device_option


def benchmark(net, warmups=5, iters=100):
    for _ in range(warmups):
        workspace.RunNetOnce(net.Proto().SerializeToString())
    plan = core.Plan("plan")
    plan.AddStep(core.ExecutionStep("test-step", net, iters))
    before = time.time()
    workspace.RunPlan(plan.Proto().SerializeToString())
    after = time.time()
    print("Timing network, time taken per-iteration: {:.6f}ms".format((
        after - before) / float(iters) * 1000.0))
    return after - before


@unittest.skipIf(not workspace.has_gpu_support, "NCCL only on GPU")
class NCCLOpsTest(hu.HypothesisTestCase):
    @given(n=st.integers(min_value=2, max_value=workspace.NumCudaDevices()),
           m=st.integers(min_value=1, max_value=1000),
           in_place=st.booleans())
    def test_nccl_allreduce(self, n, m, in_place):
        xs = [np.random.randn(m).astype(np.float32) for i in range(n)]
        inputs = [str("x_{}".format(i)) for i in range(n)]
        prefix = "" if in_place else "o"
        outputs = [str("{}x_{}".format(prefix, i)) for i in range(n)]
        op = core.CreateOperator("NCCLAllreduce", inputs, outputs)
        input_device_options = {n: gpu_device(i) for i, n in enumerate(inputs)}

        def allreduce(*args):
            assert len(args) == n
            output = np.sum(args, axis=0)
            return [output for _ in range(n)]

        self.assertReferenceChecks(
            hu.gpu_do, op, [xs[i] for i, _ in enumerate(inputs)],
            allreduce, input_device_options)

    @given(n=st.integers(min_value=2, max_value=workspace.NumCudaDevices()),
           m=st.integers(min_value=1, max_value=1000),
           root=st.integers(min_value=0,
                            max_value=workspace.NumCudaDevices() - 1))
    def test_nccl_broadcast(self, n, m, root):
        assume(root < n)
        xs = [np.random.randn(m).astype(np.float32) for i in range(n)]
        inputs = [str("x_{}".format(i)) for i in range(n)]
        op = core.CreateOperator("NCCLBroadcast", inputs, inputs, root=root)
        input_device_options = {n: gpu_device(i) for i, n in enumerate(inputs)}

        def broadcast(*args):
            assert len(args) == n
            return [args[root] for _ in range(n)]

        self.assertReferenceChecks(
            hu.gpu_do, op, [xs[i] for i, _ in enumerate(inputs)],
            broadcast, input_device_options)

    @given(n=st.integers(min_value=2, max_value=workspace.NumCudaDevices()),
           m=st.integers(min_value=1, max_value=1000),
           # NCCL Reduce seems to deadlock for non-zero roots.
           root=st.integers(min_value=0, max_value=0),
           in_place=st.booleans())
    def test_nccl_reduce(self, n, m, root, in_place):
        assume(in_place is False or root == 0)
        xs = [np.random.randn(m).astype(np.float32) for i in range(n)]
        inputs = [str("x_{}".format(i)) for i in range(n)]
        op = core.CreateOperator(
            "NCCLReduce", inputs,
            inputs[root] if in_place else b"o", root=root)
        input_device_options = {n: gpu_device(i) for i, n in enumerate(inputs)}

        def reduce(*args):
            assert len(args) == n
            return [np.sum(args, axis=0)]

        self.assertReferenceChecks(
            hu.gpu_do, op, [xs[i] for i, _ in enumerate(inputs)],
            reduce, input_device_options)

    @given(n=st.integers(min_value=2, max_value=workspace.NumCudaDevices()),
           m=st.integers(min_value=1, max_value=1000))
    def test_nccl_allgather(self, n, m):
        xs = [np.random.randn(m).astype(np.float32) for i in range(n)]
        inputs = [str("x_{}".format(i)) for i in range(n)]
        outputs = [str("o_{}".format(i)) for i in range(n)]
        op = core.CreateOperator("NCCLAllGather", inputs, outputs)
        input_device_options = {n: gpu_device(i) for i, n in enumerate(inputs)}

        def allgather(*args):
            assert len(args) == n
            return [np.stack(args, axis=0) for _ in range(n)]

        self.assertReferenceChecks(
            hu.gpu_do, op, [xs[i] for i, _ in enumerate(inputs)],
            allgather, input_device_options)

    @given(n=st.integers(min_value=2, max_value=workspace.NumCudaDevices()),
           m=st.integers(min_value=100000, max_value=100000),
           iters=st.integers(min_value=1, max_value=100),
           net_type=st.sampled_from(["dag", "async_dag", "simple"]))
    def test_nccl_sync(self, n, m, iters, net_type):
        inputs = [str("x_{}".format(i)) for i in range(n)]
        extra_inputs = [str("xe_{}".format(i)) for i in range(n)]
        net = core.Net("asdf")
        net.Proto().type = net_type
        net.Proto().num_workers = n
        for i in range(n):
            net.ConstantFill([], inputs[i], shape=[m], value=0.0,
                             device_option=gpu_device(i))
            net.ConstantFill([], extra_inputs[i], shape=[m], value=1.0,
                             device_option=gpu_device(i))
            for _ in range(iters):
                net.Sum([inputs[i], extra_inputs[i]], [inputs[i]],
                        device_option=gpu_device(i))
        net.NCCLReduce(inputs, [inputs[0]], device_option=gpu_device(0))
        workspace.RunNetOnce(net)
        np.testing.assert_array_equal(
            workspace.FetchBlob(inputs[0]),
            np.full(shape=(m,), fill_value=iters * n, dtype=np.float32))



    @unittest.skipIf(not os.environ.get("CAFFE2_BENCHMARK"), "Benchmark")
    def test_timings(self):
        for n in range(2, workspace.NumCudaDevices()):
            for in_place in [False, True]:
                xs = [np.random.randn(1e7).astype(np.float32)
                      for i in range(n)]
                inputs = [str("x_{}".format(i)) for i in range(n)]
                prefix = "" if in_place else "o"
                outputs = [str("{}x_{}".format(prefix, i)) for i in range(n)]

                net = core.Net("test")
                net.NCCLAllreduce(inputs, outputs)
                net.RunAllOnGPU()
                for i in range(n):
                    workspace.FeedBlob(inputs[i], xs[i],
                                       gpu_device(i).SerializeToString())
                workspace.RunNetOnce(net.Proto().SerializeToString())
                net_time = benchmark(net)
                vanilla = core.Net("vanilla")
                muji.Allreduce(vanilla, inputs)
                vanilla_time = benchmark(vanilla)
                print("Speedup for NCCL: {:.2f}".format(
                    vanilla_time / net_time))
