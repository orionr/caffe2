from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

import contextlib
import threading
import os
import time
import logging


'''
Sometimes CUDA devices can get stuck, 'deadlock'. In this case it is often
better just the kill the process automatically. Use this guard to set a
maximum timespan for a python call, such as RunNet(). If it does not complete
in time, process is killed.

Example usage:
    with timeout_guard.CompleteInTimeOrDie(10.0):
        core.RunNet(...)
'''


class WatcherThread(threading.Thread):

    def __init__(self, timeout_secs):
        threading.Thread.__init__(self)
        self.timeout_secs = timeout_secs
        self.completed = False
        self.condition = threading.Condition()
        self.daemon = True

    def run(self):
        started = time.time()
        self.condition.acquire()
        while time.time() - started < self.timeout_secs and not self.completed:
            self.condition.wait(self.timeout_secs - (time.time() - started))
        self.condition.release()
        if not self.completed:
            log = logging.getLogger("timeout_guard")
            log.error("Call did not finish in time. Timeout:{}s".format(
                self.timeout_secs
            ))
            os._exit(1)


@contextlib.contextmanager
def CompleteInTimeOrDie(timeout_secs):
    watcher = WatcherThread(timeout_secs)
    watcher.start()
    yield
    watcher.completed = True
    watcher.condition.acquire()
    watcher.condition.notify()
    watcher.condition.release()
