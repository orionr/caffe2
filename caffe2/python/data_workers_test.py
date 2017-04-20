from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

import numpy as np
import unittest
import time

from caffe2.python import workspace, cnn
from caffe2.python import timeout_guard
import caffe2.python.data_workers as data_workers


def dummy_fetcher(fetcher_id, batch_size):
    # Create random amount of values
    n = np.random.randint(64) + 1
    data = np.zeros((n, 3))
    labels = []
    for j in range(n):
        data[j, :] *= (j + fetcher_id)
        labels.append(data[j, 0])

    return [np.array(data), np.array(labels)]


class DataWorkersTest(unittest.TestCase):

    def testNonParallelModel(self):
        model = cnn.CNNModelHelper(name="test")
        old_seq_id = data_workers.global_coordinator._fetcher_id_seq
        coordinator = data_workers.init_data_input_workers(
            model,
            ["data", "label"],
            dummy_fetcher,
            32,
            2,
        )
        new_seq_id = data_workers.global_coordinator._fetcher_id_seq
        self.assertEqual(new_seq_id, old_seq_id + 2)

        coordinator.start()

        workspace.RunNetOnce(model.param_init_net)
        workspace.CreateNet(model.net)

        for i in range(500):
            with timeout_guard.CompleteInTimeOrDie(5):
                workspace.RunNet(model.net.Proto().name)

            data = workspace.FetchBlob("data")
            labels = workspace.FetchBlob("label")

            self.assertEqual(data.shape[0], labels.shape[0])
            self.assertEqual(data.shape[0], 32)

            for j in range(32):
                self.assertEqual(labels[j], data[j, 0])
                self.assertEqual(labels[j], data[j, 1])
                self.assertEqual(labels[j], data[j, 2])

        coordinator.stop()

    def testGracefulShutdown(self):
        model = cnn.CNNModelHelper(name="test")
        old_seq_id = data_workers.global_coordinator._fetcher_id_seq
        coordinator = data_workers.init_data_input_workers(
            model,
            ["data", "label"],
            dummy_fetcher,
            32,
            2,
        )
        new_seq_id = data_workers.global_coordinator._fetcher_id_seq
        self.assertEqual(new_seq_id, old_seq_id + 2)

        coordinator.start()

        workspace.RunNetOnce(model.param_init_net)
        workspace.CreateNet(model.net)

        while coordinator._coordinators[0]._inputs < 100:
            time.sleep(0.01)

        # Run a couple of rounds
        workspace.RunNet(model.net.Proto().name)
        workspace.RunNet(model.net.Proto().name)

        # Wait for the enqueue thread to get blocked
        time.sleep(0.2)

        # We don't dequeue on caffe2 side (as we don't run the net)
        # so the enqueue thread should be blocked.
        # Let's now shutdown and see it succeeds.
        self.assertTrue(coordinator.stop())
