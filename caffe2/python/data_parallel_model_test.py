from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np
import unittest
from caffe2.proto import caffe2_pb2
from caffe2.python import core, workspace, data_parallel_model, cnn
from caffe2.python.test_util import TestCase


@unittest.skipIf(not workspace.has_gpu_support, "No gpu support.")
@unittest.skipIf(workspace.NumCudaDevices() < 2, "Need at least 2 GPUs.")
class GPUDataParallelModelTest(TestCase):
    def test(self):
        gpu_devices = [0, 1]  # gpu ids
        perfect_model = np.array([2, 6, 5, 0, 1]).astype(np.float32)
        np.random.seed(123)
        data = np.random.randint(
            2, size=(50, perfect_model.size)
        ).astype(np.float32)
        label = np.dot(data, perfect_model)[:, np.newaxis]

        def input_builder_fun(model):
            return None

        def model_build_fun(model):
            fc = model.FC("data", "fc", perfect_model.size, 1,
                          ("ConstantFill", {}), ("ConstantFill", {}), axis=0)
            sq = model.SquaredL2Distance([fc, "label"], "sq")
            loss = model.AveragedLoss(sq, "loss")
            return [loss]

        def param_update_fun(model):
            ITER = model.Iter("ITER")
            LR = model.net.LearningRate(
                [ITER],
                "LR",
                base_lr=(-0.1 / len(gpu_devices)),
                policy="fixed",
            )
            ONE = model.param_init_net.ConstantFill(
                [], "ONE", shape=[1], value=1.0,
            )
            for param in model.GetParams():
                grad = model.param_to_grad[param]
                model.WeightedSum([param, ONE, grad, LR], param)

        # Create model
        model = cnn.CNNModelHelper(order="NHWC", name="fake")
        data_parallel_model.Parallelize_GPU(
            model,
            input_builder_fun=input_builder_fun,
            forward_pass_builder_fun=model_build_fun,
            param_update_builder_fun=param_update_fun,
            devices=gpu_devices,
        )

        # Feed some data
        for gpu_id in gpu_devices:
            with core.DeviceScope(core.DeviceOption(caffe2_pb2.CUDA, gpu_id)):
                workspace.FeedBlob(
                    "gpu_{}/data".format(gpu_id), data[0])
                workspace.FeedBlob(
                    "gpu_{}/label".format(gpu_id), label[0])


        workspace.RunNetOnce(model.param_init_net)
        workspace.CreateNet(model.net)

        for i in range(2000):
            idx = np.random.randint(data.shape[0])
            for gpu_id in gpu_devices:
                device = core.DeviceOption(caffe2_pb2.CUDA, gpu_id)
                with core.DeviceScope(device):
                    workspace.FeedBlob(
                        "gpu_{}/data".format(gpu_id), data[idx])
                    workspace.FeedBlob(
                        "gpu_{}/label".format(gpu_id), label[idx])
            workspace.RunNet(model.net)

        for gpu_id in gpu_devices:
            np.testing.assert_allclose(
                perfect_model[np.newaxis, :],
                workspace.FetchBlob("gpu_{}/fc_w".format(gpu_id)),
                atol=1e-2)
