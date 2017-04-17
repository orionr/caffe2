## @package train
# Module caffe2.python.helpers.train
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

from caffe2.python import core, scope
from caffe2.proto import caffe2_pb2


def Iter(model, blob_out, **kwargs):
    if 'device_option' in kwargs:
        del kwargs['device_option']
    model.param_init_net.ConstantFill(
        [], blob_out, shape=[1], value=0, dtype=core.DataType.INT64,
        device_option=core.DeviceOption(caffe2_pb2.CPU, 0),
        **kwargs)
    return model.net.Iter(blob_out, blob_out, **kwargs)


def Accuracy(model, blob_in, blob_out, **kwargs):
    dev = kwargs['device_option'] if 'device_option' in kwargs \
        else scope.CurrentDeviceScope()
    is_cpu = dev is None or dev.device_type == caffe2_pb2.CPU

    # We support top_k > 1 only on CPU
    if not is_cpu and 'top_k' in kwargs and kwargs['top_k'] > 1:
        pred_host = model.net.CopyGPUToCPU(blob_in[0], blob_in[0] + "_host")
        label_host = model.net.CopyGPUToCPU(blob_in[1], blob_in[1] + "_host")

        # Now use the Host version of the accuracy op
        model.net.Accuracy([pred_host, label_host],
                           blob_out,
                           device_option=core.DeviceOption(caffe2_pb2.CPU, 0),
                           **kwargs)
    else:
        model.net.Accuracy(blob_in, blob_out)
