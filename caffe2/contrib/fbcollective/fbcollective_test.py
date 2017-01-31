#!/usr/bin/env python

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

from hypothesis import given
import hypothesis.strategies as st
from multiprocessing import Process, Queue

import numpy as np
import os
import pickle
import tempfile
import shutil

from caffe2.python import core, workspace, dyndep
import caffe2.python.hypothesis_test_util as hu

dyndep.InitOpsLibrary("@/caffe2/caffe2/distributed:file_store_handler_ops")
dyndep.InitOpsLibrary("@/caffe2/caffe2/distributed:redis_store_handler_ops")
dyndep.InitOpsLibrary("@/caffe2/caffe2/distributed:store_ops")
dyndep.InitOpsLibrary("@/caffe2/caffe2/contrib/fbcollective:fbcollective_ops")

op_engine = 'FBCOLLECTIVE'


class TemporaryDirectory:
    def __enter__(self):
        self.tmpdir = tempfile.mkdtemp()
        return self.tmpdir

    def __exit__(self, type, value, traceback):
        shutil.rmtree(self.tmpdir)


class TestCase(hu.HypothesisTestCase):
    test_counter = 0
    sync_counter = 0

    def run_test_locally(self, fn, **kwargs):
        # Queue for assertion errors on subprocesses
        queue = Queue()

        # Capture any exception thrown by the subprocess
        def run_fn(*args, **kwargs):
            try:
                fn(*args, **kwargs)
                workspace.ResetWorkspace()
            except Exception as ex:
                queue.put(ex)

        # Start N processes in the background
        procs = []
        for i in range(kwargs['comm_size']):
            kwargs['comm_rank'] = i
            proc = Process(
                target=run_fn,
                kwargs=kwargs)
            proc.start()
            procs.append(proc)

        # Test complete, join background processes
        while len(procs) > 0:
            proc = procs.pop(0)
            while proc.is_alive():
                proc.join(1)

                # Raise exception if we find any.
                # Note that the following is executed ALSO after
                # the last process was joined, so if ANY exception
                # was raised, it will be re-raised here.
                if not queue.empty():
                    raise queue.get()

    def run_test_distributed(self, fn, **kwargs):
        comm_rank = os.getenv('COMM_RANK')
        self.assertIsNotNone(comm_rank)
        comm_size = os.getenv('COMM_SIZE')
        self.assertIsNotNone(comm_size)
        kwargs['comm_rank'] = int(comm_rank)
        kwargs['comm_size'] = int(comm_size)
        fn(**kwargs)
        workspace.ResetWorkspace()

    def create_common_world(self, comm_rank, comm_size, tmpdir=None):
        store_handler = "store_handler"

        # If REDIS_HOST is set, use RedisStoreHandler for rendezvous.
        redis_host = os.getenv("REDIS_HOST")
        redis_port = int(os.getenv("REDIS_PORT", 6379))
        if redis_host is not None:
            workspace.RunOperatorOnce(
                core.CreateOperator(
                    "RedisStoreHandlerCreate",
                    [],
                    [store_handler],
                    prefix=str(TestCase.test_counter) + "/",
                    host=redis_host,
                    port=redis_port))
        else:
            workspace.RunOperatorOnce(
                core.CreateOperator(
                    "FileStoreHandlerCreate",
                    [],
                    [store_handler],
                    path=tmpdir))

        common_world = "common_world"
        workspace.RunOperatorOnce(
            core.CreateOperator(
                "CreateCommonWorld",
                [store_handler],
                [common_world],
                size=comm_size,
                rank=comm_rank,
                engine=op_engine))
        return (store_handler, common_world)

    def synchronize(self, store_handler, value, comm_rank=None):
        TestCase.sync_counter += 1
        blob = "sync_{}".format(TestCase.sync_counter)
        if comm_rank == 0:
            workspace.FeedBlob(blob, pickle.dumps(value))
            workspace.RunOperatorOnce(
                core.CreateOperator(
                    "StoreSet",
                    [store_handler, blob],
                    []))
        else:
            workspace.RunOperatorOnce(
                core.CreateOperator(
                    "StoreGet",
                    [store_handler],
                    [blob]))
        return pickle.loads(workspace.FetchBlob(blob))

    def _test_broadcast(self,
                        comm_rank=None,
                        comm_size=None,
                        blob_size=None,
                        tmpdir=None,
                        ):
        store_handler, common_world = self.create_common_world(
            comm_rank=comm_rank,
            comm_size=comm_size,
            tmpdir=tmpdir)

        blob_size = self.synchronize(
            store_handler,
            blob_size,
            comm_rank=comm_rank)

        for i in range(comm_size):
            blob = "blob_{}".format(i)
            value = np.full(blob_size, comm_rank, np.float32)

            workspace.FeedBlob(blob, value)
            workspace.RunOperatorOnce(
                core.CreateOperator(
                    "Broadcast",
                    [common_world, blob],
                    [blob],
                    root=i,
                    engine=op_engine))
            np.testing.assert_array_equal(workspace.FetchBlob(blob), i)

    @given(comm_size=st.integers(min_value=2, max_value=8),
           blob_size=st.integers(min_value=1e3, max_value=1e6))
    def test_broadcast(self, comm_size, blob_size):
        TestCase.test_counter += 1
        if os.getenv('COMM_RANK') is not None:
            self.run_test_distributed(
                self._test_broadcast,
                blob_size=blob_size)
        else:
            with TemporaryDirectory() as tmpdir:
                self.run_test_locally(
                    self._test_broadcast,
                    comm_size=comm_size,
                    blob_size=blob_size,
                    tmpdir=tmpdir)

    def _test_allreduce(self,
                        comm_rank=None,
                        comm_size=None,
                        blob_size=None,
                        tmpdir=None,
                        ):
        store_handler, common_world = self.create_common_world(
            comm_rank=comm_rank,
            comm_size=comm_size,
            tmpdir=tmpdir)

        blob_size = self.synchronize(
            store_handler,
            blob_size,
            comm_rank=comm_rank)

        blob = "blob"
        value = np.full(blob_size, comm_rank, np.float32)

        workspace.FeedBlob(blob, value)
        workspace.RunOperatorOnce(
            core.CreateOperator(
                "Allreduce",
                [common_world, blob],
                [blob],
                engine=op_engine))

        np.testing.assert_array_equal(
            workspace.FetchBlob(blob),
            comm_size * (comm_size - 1) / 2)

    @given(comm_size=st.integers(min_value=2, max_value=8),
           blob_size=st.integers(min_value=1e3, max_value=1e6),
           **hu.gcs)
    def test_allreduce(self, comm_size, blob_size, gc, dc):
        TestCase.test_counter += 1
        if os.getenv('COMM_RANK') is not None:
            self.run_test_distributed(
                self._test_allreduce,
                blob_size=blob_size)
        else:
            with TemporaryDirectory() as tmpdir:
                self.run_test_locally(
                    self._test_allreduce,
                    comm_size=comm_size,
                    blob_size=blob_size,
                    tmpdir=tmpdir)


if __name__ == "__main__":
    import unittest
    unittest.main()
