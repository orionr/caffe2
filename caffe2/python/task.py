from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

from caffe2.python import core, context
from caffe2.python.schema import Field, from_blob_list
from collections import defaultdict


@context.define_context(allow_default=True)
class Cluster(object):
    """
    Context that keeps track of all the node names used.
    Users shouldn't have to use them directly, since a Cluster is automatically
    generated at the first usage of 'Node'.
    """

    def __init__(self):
        # list instead of set to keep order
        self._nodes = []

    def add_node(self, node):
        if str(node) not in self._nodes:
            self._nodes.append(str(node))

    def nodes(self):
        """
        Returns the list of unique node names used within this context.
        """
        return self._nodes


@context.define_context(allow_default=True)
class Node(object):
    """
    A Node context is used to indicate that all Tasks instantiated within will
    run on the given node name. (Only the name of the node actually counts.)
    Example:

        with TaskGroup() as tg:
            with Node('node1'):
                s1 = execution_step(...)
                Task(step=s1)
            with Node('node2'):
                s2 = execution_step(...)
            with Node('node1'):
                s3 = execution_step(...)

        In this example, all three execution steps will run in parallel.
        Moreover, s1 and s3 will run on the same node, and can see each
        others blobs.
    """

    def __init__(self, node='local'):
        self._name = str(node)
        Cluster.current().add_node(self)

    def __str__(self):
        return self._name


class WorkspaceType(object):
    """
    Determines whether tasks of a TaskGroup will run directly at the global
    workspace, which is kept alive across runs, or whether a new child
    workspace will be created for the run and destroyed afterwards.
    """
    PRIVATE = 'private'
    GLOBAL = 'global'


def get_setup_nets(key, steps, target):
    init_net = core.Net(key + '/init')
    exit_net = core.Net(key + '/exit')
    init_nets = []
    exit_nets = []
    objs = []
    for step in steps:
        if step is not None:
            objs += step.get_all_attributes(key)
    for obj in objs:
        # these are needed in order to allow nesting of TaskGroup, which
        # is a feature not yet implemented.
        if hasattr(obj, '_setup_used') and obj._setup_used:
            continue
        if hasattr(obj, '_setup_target') and obj._setup_target != target:
            continue
        if hasattr(obj, 'setup'):
            nets = obj.setup(init_net)
            if isinstance(nets, (list, tuple)):
                init_nets += nets
            elif isinstance(nets, core.Net):
                init_nets.append(nets)
            elif nets is not None:
                raise TypeError('Unsupported type for setup: %s' % type(nets))
            obj._setup_used = True
        if hasattr(obj, 'exit'):
            nets = obj.exit(exit_net)
            if isinstance(nets, (list, tuple)):
                exit_nets += nets
            elif isinstance(nets, core.Net):
                exit_nets.append(nets)
            elif nets is not None:
                raise TypeError('Unsupported type for setup: %s' % type(nets))
            obj._setup_used = True

    if len(init_net.Proto().op) > 0:
        init_nets.insert(0, init_net)
    if len(exit_net.Proto().op) > 0:
        exit_nets.insert(0, exit_net)
    return init_nets, exit_nets


@context.define_context(allow_default=False)
class TaskGroup(object):
    """
    Context that gathers tasks which will run concurrently, potentially on
    multiple nodes. All tasks in the same node will share the same workspace
    and thus can share blobs, while tasks running in different nodes won't
    be able to directly share data.

    All tasks of the task group will start concurrently, and the task group
    will finish execution when the last task of the group finishes.

    Example:
        # supose that s1 ... s5 are execution steps or nets.
        with TaskGroup() as tg:
            # these tasks go to default node 'local'
            Task(step=s1)
            Task(step=s2)

            with Node('n2'):
                Task(step=s3)
            with Node('n1'):
                Task(step=s4)
            with Node('n2'):
                Task(step=s5)

        # this will run all steps in parallel.
        # s1 and s2 will run at default node 'local'
        # s3 and s5 will run at node 'n2'
        # s4 will run at node 'n1'
        session.run(tg)
    """
    LOCAL_SETUP = 'local_setup'

    def __init__(self, workspace_type=None):
        self._plan_cache = None
        self._tasks = []
        self._already_used = False
        self._prev_active = None
        self._tasks_to_add = []
        self._report_nets = {}
        self._workspace_type = workspace_type
        self._tasks_by_node = None

    def add(self, task):
        assert not self._already_used, (
            'Cannot add Task to an already used TaskGroup.')
        assert (
            self._workspace_type is None or
            task._workspace_type is None or
            self._workspace_type == task._workspace_type)
        if task._workspace_type is None:
            task._workspace_type = (
                self._workspace_type or WorkspaceType.PRIVATE)
        if self._workspace_type is None:
            self._workspace_type = task._workspace_type
        task._notify_used()
        self._tasks.append(task)

    def tasks(self):
        for task in self._tasks_to_add:
            self.add(task)
        self._tasks_to_add = []
        self._already_used = True
        return self._tasks

    def num_registered_tasks(self):
        return len(self._tasks_to_add) + len(self._tasks)

    def used_nodes(self):
        # use list to keep order
        used = []
        for task in self.tasks():
            if task.node not in used:
                used.append(task.node)
        return used

    def report_net(self, net=None, node=None, report_interval=5):
        """
        Get or set the `report_net`, which is a net that runs repeatedly every
        `report_interval` seconds for the duration of the TaskGroup execution
        on each of the nodes. Each node has it's own report net.

        Example:

            with TaskGroup() as tg:
                for i in range(0, 2):
                    with Node('trainer:%d' % i):
                        report_net = tg.report_net()
                        report_net.LogInfo('5s passed in trainer %d' % i)

        This will print '5s passed in trainer {}' every 5s on each one of the
        trainer nodes.
        """
        node = str(Node.current(node))
        assert net is None or node not in self._report_nets
        if node not in self._report_nets:
            self._report_nets[node] = (
                net if net else core.Net('%s/reporter' % node),
                report_interval)
        return self._report_nets[node][0]

    def tasks_by_node(self, node_remap=None):
        # tasks_by_node can't be called twice because the setup won't
        # work properly a second time.
        node_map = {}
        for task in self.tasks():
            node_map[task.node] =\
                node_remap(task.node) if node_remap else task.node
        if self._tasks_by_node is not None:
            tasks_by_node, prev_node_map = self._tasks_by_node
            assert prev_node_map == node_map, (
                'Cannot call tasks_by_node multiple times.')
            return tasks_by_node

        tasks_by_node = defaultdict(list)
        for task in self.tasks():
            tasks_by_node[node_map[task.node]].append(task)
        grouped_by_node = TaskGroup()
        for node, tasks in tasks_by_node.items():
            node_inits, node_exits = get_setup_nets(
                TaskGroup.LOCAL_SETUP, [t.get_step() for t in tasks], self)
            # shortcut for single task with no queue
            steps = []
            outputs = []
            workspace_type = tasks[0].workspace_type()
            for task in tasks:
                step = task.get_step()
                if step is not None:
                    steps.append(step)
                outputs += task.outputs()
                assert workspace_type == task.workspace_type(), (
                    'All tasks for a given node need same workspace type.')
            if len(steps) == 0:
                steps.append(core.execution_step('empty', []))
            if len(steps) == 1:
                step = steps[0]
            else:
                step = core.execution_step(
                    '%s:body' % node, steps, concurrent_substeps=True)
            if node in self._report_nets:
                net, interval = self._report_nets[node]
                step.SetReportNet(net, interval)
            if len(node_inits) > 0 or len(node_exits) > 0:
                steps = []
                if len(node_inits) > 0:
                    steps.append(
                        core.execution_step('%s:init' % node, node_inits))
                steps.append(step)
                if len(node_exits) > 0:
                    steps.append(
                        core.execution_step('%s:exit' % node, node_exits))
                step = core.execution_step(node, steps)
            Task(
                node=node, step=step, outputs=outputs,
                group=grouped_by_node, workspace_type=workspace_type)
        self._tasks_by_node = (grouped_by_node, node_map)
        return grouped_by_node

    def to_task(self, node='local'):
        tasks = self.tasks_by_node(lambda x: 'local').tasks()
        if len(tasks) == 0:
            return Task()
        return tasks[0]


class TaskOutput(object):
    """
    Represents the output of a task. An output can be a blob,
    a list of blob, or a record.
    """

    def __init__(self, names):
        self._schema = None
        self._is_scalar = False
        if isinstance(names, Field):
            self._schema = names
            names = self._schema.field_blobs()
        self._is_scalar = type(names) not in (tuple, list)
        if self._is_scalar:
            names = [names]
        self.names = names
        self._values = None

    def set(self, values, _fetch_func=None):
        assert len(values) == len(self.names)
        self._values = values
        self._fetch_func = _fetch_func

    def get(self):
        assert self._values is not None, 'Output value not set yet.'
        if self._is_scalar:
            return self._values[0]
        elif self._schema:
            return from_blob_list(self._schema, self._values)
        else:
            return self._values

    def fetch(self):
        assert self._fetch_func is not None, (
            'Cannot fetch value for this output.')
        fetched_vals = [self._fetch_func(v) for v in self._values]
        if self._is_scalar:
            return fetched_vals[0]
        elif self._schema:
            return from_blob_list(self._schema, fetched_vals)
        else:
            return fetched_vals


def final_output(blob_or_record):
    """
    Create a dummy task that returns the given blob or record
    to the client. This will return the value of the blob or record when
    the last task of the TaskGroup for a given node finishes.
    """
    return Task(outputs=blob_or_record).outputs()[0]


class Task(object):
    """
    A Task is composed of an execution step and zero or more outputs.
    Tasks are executed in the context of a TaskGroup, which, in turn, can
    be run by a Session.

    Task outputs are fetched by the session at the end of the run.
    """

    TASK_SETUP = 'task_setup'

    def __init__(
            self, step=None, outputs=None,
            workspace_type=None, group=None, node=None):
        """
        Instantiate a Task and add it to the current TaskGroup and Node.
        """
        # register this node name with active context
        self.node = str(Node.current(None if node is None else Node(node)))
        group = TaskGroup.current(group, required=False)
        if group is not None:
            group._tasks_to_add.append(self)

        self._already_used = False
        self._step = None
        self._step_with_setup = None
        self._outputs = []
        if step is not None:
            self.set_step(step)
        if outputs is not None:
            self.add_outputs(outputs)

        self._pipeline = None
        self._is_pipeline_context = False
        self._workspace_type = workspace_type

    def workspace_type(self):
        return self._workspace_type

    def _assert_not_used(self):
        assert not self._already_used, (
            'Cannot modify task since it is already been used.')

    def add_output(self, output):
        self._assert_not_used()
        self._outputs.append(
            output if isinstance(output, TaskOutput) else TaskOutput(output))

    def add_outputs(self, outputs):
        self._assert_not_used()
        if type(outputs) not in (list, tuple):
            outputs = [outputs]
        for output in outputs:
            self.add_output(output)

    def set_step(self, step):
        self._assert_not_used()
        self._step = core.to_execution_step(step)

    def get_step(self):
        if self._step is not None and self._step_with_setup is None:
            init_nets, exit_nets = get_setup_nets(
                Task.TASK_SETUP, [self._step], self)
            if len(self._outputs) == 0:
                output_net = core.Net("output_net")
                self.add_output(output_net.ConstantFill(
                    [], 1, dtype=core.DataType.INT32, value=0))
                exit_nets.append(output_net)
            self._step_with_setup = core.execution_step(
                'task',
                [
                    core.execution_step('task_init', init_nets),
                    self._step,
                    core.execution_step('task_exit', exit_nets),
                ]
            )
        elif self._step_with_setup is None:
            self._step_with_setup = core.execution_step('task', [])
        return self._step_with_setup

    def outputs(self):
        return self._outputs

    def output_names(self):
        """
        Retrive the output names.
        TODO(azzolini): make this schema-based.
        """
        names = []
        for o in self._outputs:
            names += o.names
        return names

    def set_outputs(self, values, _fetch_func):
        """
        Set output values.
        TODO(azzolini): make this schema-based.
        """
        offset = 0
        for o in self._outputs:
            num = len(o.names)
            o.set(values[offset:offset + num], _fetch_func)
            offset += num
        assert offset == len(values), 'Wrong number of output values.'

    def resolved_outputs(self):
        return [output.get() for output in self._outputs]

    def _notify_used(self):
        self.get_step()
        self._already_used = True


class SetupNets(object):
    """
    Allow to register a list of nets to be run at initialization
    and finalization of Tasks or TaskGroups.
    For example, let's say you have the following:

        init_net = core.Net('init')
        my_val = init_net.ConstantFill([], 'my_val', value=0)

        net = core.Net('counter')
        net.Add([my_val, net.Const(1),], [my_val])

        with TaskGroup() as task_group:
            with Node('trainer'):
                my_task = Task(step=[net])

    In order to have `init_net` run once before `net` runs for the
    first time, you can do one of the following:

        net.add_object(Task.TASK_SETUP, SetupNets([init_net]))

    or

        net.add_object(TaskGroup.LOCAL_SETUP, SetupNets([init_net]))

    - With Task.TASK_SETUP, init_net will run once at my_task startup.
    - With TaskGroup.LOCAL_SETUP, init_net will run once on node 'trainer',
      before any task of the task group is run on that node.

    The same SetupNets object can be added to multiple nets. It will only
    run once per Task/TaskGroup run.
    """

    def __init__(self, init_nets=None, exit_nets=None):
        self.init_nets = init_nets
        self.exit_nets = exit_nets

    def setup(self, init_net):
        return self.init_nets

    def exit(self, exit_net):
        return self.exit_nets
