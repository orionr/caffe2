"""
Defines the base interface for reading and writing operations.

Readers/Writers are objects that produce operations that read/write sequences
of data. Each operation reads or writes a list of BlobReferences.

Readers and Writers must be implemented such that read and write operations
are atomic and thread safe.

Examples of possible Readers and Writers:
    HiveReader, HiveWriter,
    QueueReader, QueueWriter,
    DatasetReader, DatasetWriter,
    DBReader, DBWriter,

See `dataset.py` for an example of implementation.
"""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals
from caffe2.python import core
from caffe2.python.schema import Field, Struct, from_blob_list
import numpy as np


class Reader(object):
    def __init__(self, schema=None):
        if schema is not None:
            assert isinstance(schema, Field)
        self._schema = schema

    def schema(self):
        """
        Return the schema associated with the Hive Reader
        """
        assert self._schema is not None, 'Schema not provided for this reader.'
        return self._schema

    def _set_schema(self, schema):
        self._schema = schema

    def setup_ex(self, init_net, finish_net):
        """Nets to be executed once at startup and finish.
           Experimental extension. Don't use yet"""
        pass

    def read_ex(self, local_init_net, local_finish_net):
        """Experimental extension to the interface. Don't use yet"""
        read_net = core.Net('reader_body')
        return ([read_net], ) + self.read(read_net)

    def read_record_ex(self, local_init_net, local_finish_net):
        """Experimental extension to the interface. Don't use yet"""
        nets, should_stop, fields = self.read_ex(
            local_init_net, local_finish_net)
        if self._schema:
            fields = from_blob_list(self._schema, fields)
        return nets, should_stop, fields

    """
    Reader is a abstract class to be implemented in order to provide
    operations capable of iterating through a dataset or stream of data.

    A Reader must implement at least one operation, `read`, which
    adds operations to a net that read the next batch of data. Readers can
    optionally support the `reset` operation, which is useful when multiple
    passes over the data are required.
    """
    def read(self, read_net):
        """
        Add operations to read_net that will read the read batch of data
        and return a list of BlobReference representing the blobs that will
        contain the batches produced.

        Operations added to `read_net` must be thread safe and atomic, that is,
        it should be possible to clone `read_net` and run multiple instances of
        it in parallel.

        Args:
            read_net: the net that will be appended with read operations

        Returns:
            A tuple (should_stop, fields), with:

                should_stop: BlobReference pointing to a boolean scalar
                             blob that indicates whether the read operation
                             was succesfull or whether the end of data has
                             been reached.
                fields: A tuple of BlobReference containing the latest batch
                        of data that was read.
        """
        raise NotImplementedError('Readers must implement `read`.')

    def reset(self, net):
        """Append operations to `net` that will reset the reader.

        This can be used to read the data multiple times.
        Not all readers support this operation.
        """
        raise NotImplementedError('This reader cannot be resetted.')

    def read_record(self, read_net):
        should_stop, fields = self.read(read_net)
        if self._schema:
            fields = from_blob_list(self._schema, fields)
        return should_stop, fields

    def execution_step(self, reader_net_name=None):
        """Create an execution step with a net containing read operators.

        The execution step will contain a `stop_blob` that knows how to stop
        the execution loop when end of data was reached.

        E.g.:

            read_step, fields = reader.execution_step()
            consume_net = core.Net('consume')
            consume_net.Print(fields[0], [])
            p = core.Plan('reader')
            p.AddStep(read_step.AddNet(consume_net))
            core.RunPlan(p)

        Args:

            reader_net_name: (optional) the name of the reader_net to be
                             created. The execution step will
                             be named accordingly.

        Returns:
            A tuple (read_step, fields), with:

                read_step: A newly created execution step containing a net with
                           read operations. The step will have `stop_blob` set,
                           in order to stop the loop on end of data.
                fields: A tuple of BlobReference containing the latest batch
                        of data that was read.
        """
        reader_net = core.Net(reader_net_name or 'reader')
        should_stop, fields = self.read_record(reader_net)
        read_step = core.execution_step(
            '{}_step'.format(reader_net_name),
            reader_net,
            should_stop_blob=should_stop)
        return (read_step, fields)


class Writer(object):
    """
    Writer is a abstract class to be implemented in order to provide
    operations capable of feeding a data stream or a dataset.

    A Writer must implement 2 operations:
    `write`, which adds operations to a net that write the write batch of
    data, and `commit`, which adds operations to a net in order to indicate
    that no more data will be written.
    """

    _schema = None

    def schema(self):
        return self._schema

    def write(self, writer_net, fields):
        """Add operations to `writer_net` that write the next batch of data.

        Operations added to the net must be thread-safe and unique, that is:
        multiple writers must be able to write to the dataset in parallel.

        Args:
            fields: a tuple of BlobReference containing the batch of data to
                    write.
        """
        raise NotImplementedError('Writers must implement write.')

    def write_record(self, writer_net, fields):
        if isinstance(fields, Field):
            self._schema = fields
            fields = fields.field_blobs()
        self.write(writer_net, fields)

    def setup_ex(self, init_net, finish_net):
        """Experimental, don't use yet"""
        self.commit(finish_net)

    def write_ex(self, fields, local_init_net, local_finish_net, stop_blob):
        """Experimental extension to the interface. Don't use yet"""
        write_net = core.Net('write_net')
        self.write(write_net, fields)
        return [write_net]

    def write_record_ex(
            self, fields, local_init_net, local_finish_net, stop_blob=None):
        """Experimental extension to the interface. Don't use yet."""
        if isinstance(fields, Field):
            self._schema = fields
            fields = fields.field_blobs()
        if stop_blob is None:
            stop_blob = local_init_net.NextName("dequeue_status")
        write_nets = self.write_ex(
            fields, local_init_net, local_finish_net, stop_blob)
        return (write_nets, stop_blob)

    def commit(self, finish_net):
        """Add operations to `finish_net` that signal end of data.

        This must be implemented by all Writers, but may be no-op for some
        of them.
        """
        pass


class ReaderBuilder(object):
    """ Allow usage of a reader in distributed fashion. """
    def schema(self):
        raise NotImplementedError()

    def enqueue_splits(self, net, split_queue):
        raise NotImplementedError()

    def splits(self, net):
        raise NotImplementedError()

    def new_reader(self, split_queue):
        raise NotImplementedError()


class Pipe(object):
    def __init__(self, schema=None, obj_key=None):
        self._num_writers = 0
        self._num_readers = 0
        self._schema = schema
        self._obj_key = obj_key

    def schema(self):
        return self._schema

    def setup(self, global_init_net):
        pass

    def reader(self):
        raise NotImplementedError()

    def writer(self):
        raise NotImplementedError()

    def num_readers(self):
        return self._num_readers

    def num_writers(self):
        return self._num_writers

    def _new_writer(self, writer_schema, writer_init_net):
        if writer_schema is not None and self._schema is None:
            self._schema = writer_schema
        self._num_writers += 1
        if self._obj_key is not None:
            writer_init_net.add_attribute(self._obj_key, self)

    def _new_reader(self, reader_init_net):
        self._num_readers += 1
        if self._obj_key is not None:
            reader_init_net.add_attribute(self._obj_key, self)


class CounterReader(Reader):
    """ Reader that produces increasing integers. """
    def __init__(self):
        Reader.__init__(self, schema=Struct(('iter', np.int64)))
        self.counter = None
        self.should_stop = None

    def setup_ex(self, global_init_net, global_finish_net):
        if self.counter is None:
            self.counter = global_init_net.CreateCounter([], init_count=0)
            self.should_stop = global_init_net.ConstantFill(
                [], shape=[], dtype=core.DataType.BOOL, value=False)

    def read_ex(self, local_init_net, local_finish_net):
        count_net = core.Net('limited_reader_counter')
        value = count_net.CountUp([self.counter], 1)
        return [count_net], self.should_stop, [value]


class ReaderWithLimit(Reader):
    """ Reader that stops after `num_iter` calls. """
    def __init__(self, reader, num_iter=1):
        Reader.__init__(self, schema=reader._schema)
        self.reader = reader
        self.counter = None
        self.num_iter = num_iter
        self._data_finished = None

    def setup_ex(self, global_init_net, global_finish_net):
        if self._data_finished is None:
            self.counter = global_init_net.CreateCounter(
                [], init_count=int(self.num_iter))
            self.reader.setup_ex(global_init_net, global_finish_net)
            self._data_finished = global_init_net.ConstantFill(
                [], shape=[], value=False, dtype=core.DataType.BOOL)

    def read_ex(self, local_init_net, local_finish_net):
        """ 1. check if we reached number of iterations """
        count_net = core.Net('limited_reader_counter')
        should_stop = count_net.CountDown([self.counter], 1)

        """ 2. call original reader """
        nets, local_data_finished, fields = self.reader.read_ex(
            local_init_net, local_finish_net)
        self._set_schema(self.reader._schema)

        """ 3. check if original reader is done. """
        check_done_net = core.Net('limited_reader_post')
        check_done_net.Copy(local_data_finished, should_stop)
        check_done_net.Copy([local_data_finished], [self._data_finished])

        # this relies on `should_stop` being called after each net.
        return [count_net] + nets + [check_done_net], should_stop, fields

    def data_finished(self):
        """
        Return a blob that can be checked after the end of the reading task,
        which will contain a scalar float indicating whether the underlying
        reader has been exhausted (True) or whether we stopped because reached
        the limit of iterations (False).
        """
        assert self._data_finished is not None, (
            'read_record must be called before data_finished()')
        return self._data_finished


def CountUntil(num_iter):
    return ReaderWithLimit(CounterReader(), num_iter)
