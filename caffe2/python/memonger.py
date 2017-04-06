## @package memonger
# Module caffe2.python.memonger
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

import networkx as nx
import collections
import time
import copy
from caffe2.python import workspace
from caffe2.proto import caffe2_pb2

import logging

log = logging.getLogger("memonger")
log.setLevel(logging.INFO)
LiveRange = collections.namedtuple('LiveRange', ["defined", "used", "size"])


def share_grad_blobs(net, losses, param_grads, namescope):
    '''
    Implements similar optimization as Torch's shareGradInput():
    for the gradients that are passed between layers, share blobs between
    operators when possible. This yields significant memory savings with
    deep networks.

    Returns an optimized protobuf (assign to net._net)
    '''
    def is_grad_blob(b):
        name = str(b)
        # Note: need to look at _{namescope} pattern as it matches
        # to handle the auto-split gradients
        return "_grad" in name and (name.startswith(namescope) or
            name.startswith("_" + namescope)) and name not in param_grads

    def is_grad_op(op):
        # TODO: something smarter
        for inp in op.input:
            if is_grad_blob(inp):
                return True
        for out in op.output:
            if is_grad_blob(out):
                return True
        return False

    start_time = time.time()
    log.warn("NOTE: Executing *experimental* memonger to " +
             "optimize gradient memory")

    # Collect ops that have something to do with
    # gradients
    if not namescope.endswith("/"):
        namescope += "/"

    netproto = copy.deepcopy(net.Proto())
    grad_ops = [op for op in netproto.op if is_grad_op(op)]

    # Create mapping from blobs to ops
    blobs_to_ops = collections.defaultdict(lambda: [])
    blob_input_count = collections.defaultdict(lambda: 0)
    op_inputs = collections.defaultdict(lambda: 0)
    op_visit_count = collections.defaultdict(lambda: 0)
    for i, op in enumerate(grad_ops):
        for inp in op.input:
            if is_grad_blob(inp) or inp in losses:
                # Ignore in-place transformation ops (self cycles)
                if inp not in op.output:
                    blobs_to_ops[inp].append(i)
                    op_inputs[i] += 1

    # Traverse operators starting from the loss blobs.
    # Keep tabs on when blobs are seen first and last, and also
    # when operators have their input satisfied. Share blobs only
    # under same branch, avoiding problems with parallel workers.
    output_blobs = set()
    mapping = {}

    def descend(op_idx, free_blobs):
        cur_op = grad_ops[op_idx]
        new_free_blobs = set()
        for inp in cur_op.input:
            if is_grad_blob(inp):
                blob_input_count[inp] += 1
                if blob_input_count[inp] == len(blobs_to_ops[inp]):
                    actual_blob = inp if inp not in mapping else mapping[inp]
                    new_free_blobs.add(actual_blob)

        for outp in cur_op.output:
            if is_grad_blob(outp):
                if outp not in output_blobs:
                    # First seen this blob as output, can assign to a free blob
                    for freeb in free_blobs:
                        mapping[outp] = freeb
                        free_blobs.remove(freeb)
                        break

                output_blobs.add(outp)

        free_blobs.update(new_free_blobs)

        first_branch = True
        for outp in cur_op.output:
            for inp_op_idx in blobs_to_ops[outp]:
                op_visit_count[inp_op_idx] += 1

                # Descend only if we have satisfied all inputs
                if op_visit_count[inp_op_idx] == op_inputs[inp_op_idx]:
                    free_blobs_fwd = free_blobs if first_branch else set()
                    first_branch = False
                    descend(inp_op_idx, free_blobs_fwd)

    # Start DFS from the losses
    for loss in losses:
        for op_idx in blobs_to_ops[loss]:
            descend(op_idx, set())

    # Rename the shared blobs
    shared_blobs = set(mapping.values())
    renamed = {}
    for j, b in enumerate(shared_blobs):
        renamed[b] = namescope + "__m{}_".format(j)

    # Final mapping
    for k, v in mapping.items():
        mapping[k] = renamed[v]

    # Add the originators
    mapping.update(renamed)
    log.info("Remapping {} blobs, using {} shared".format(
        len(mapping), len(renamed),
    ))
    log.debug("Assignments: {}".format(mapping))

    apply_assignments(netproto, mapping)
    log.info("Gradient memory optimization took {} secs".format(
        time.time() - start_time),
    )
    return netproto


def _find_source_nodes(g):
    ''' Return nodes without predecessors '''
    ret = []
    for cn in g:
        cur_pred = g.predecessors(cn)
        if not cur_pred:
            ret.append(cn)
    return ret


def _find_target_nodes(g):
    ''' Return nodes without successors '''
    ret = []
    for cn in g:
        cur_succ = g.successors(cn)
        if not cur_succ:
            ret.append(cn)
    return ret


def _add_single_target_ifneeded(g):
    targets = _find_target_nodes(g)
    assert len(targets) >= 1
    if len(targets) == 1:
        return g
    ret = copy.deepcopy(g)

    def _next_available_idx(g):
        ret = -1
        for cn in g:
            if cn > ret:
                ret = cn
        ret += 1
        return ret

    target_node_idx = _next_available_idx(g)
    ret.add_node(target_node_idx)
    for cn in targets:
        ret.add_edge(cn, target_node_idx)

    return ret


def _get_path(pred_list, dist_list):
    ''' Get the path from nx.bellman_ford()'s output '''

    # distances are negative
    assert all(dist_list[x] <= 0 for x in dist_list)
    # node with longest distance to source is the target
    target = min(dist_list, key=lambda x: dist_list[x])

    ret = []
    cur = target
    while cur is not None:
        ret.append(cur)
        cur = pred_list[cur]
    return list(reversed(ret))


def _get_longest_paths(g, source_nodes):
    ''' Get the longest path for nodes in 'source_nodes'
        Find with bellman_ford() by setting weight = -1
    '''

    ng = copy.deepcopy(g)
    for u, v in ng.edges():
        ng[u][v]["weight"] = -1

    ret = {}
    for cn in source_nodes:
        pred, dist = nx.bellman_ford(ng, cn, weight="weight")
        path = _get_path(pred, dist)
        assert path[0] == cn
        assert len(path) - 1 == -dist[path[-1]]
        ret[cn] = path

    return ret


def _build_tree(paths):
    ''' Build a tree for given paths based on common elements.
        Last elements of all paths are the same, which is the root of the tree.
    '''
    assert all(cp[-1] == paths[0][-1] for cp in paths)
    g = nx.DiGraph()
    node_set = {y for x in paths for y in x}
    g.add_nodes_from(node_set)
    for cp in paths:
        for ce in zip(cp[0:-1], cp[1:]):
            g.add_edge(ce[1], ce[0])

    root = paths[0][-1]
    _compute_tree_height(g, root)

    return (g, root)


def _compute_tree_height(g, root):
    ''' Compute the heights of the tree for all nodes
        Height of leaves are 0
    '''
    def _get_height(root):
        children = g.successors(root)
        height = 0
        if children:
            child_heights = [_get_height(x) for x in children]
            height = max(child_heights) + 1
        g.node[root]["height"] = height
        return height

    _get_height(root)


def _sort_tree_leaves(g, root):
    ''' For each node, sort its child nodes based on the height of the nodes.
        Return the leaf nodes of the tree after sorting.
    '''
    def _get_height(root):
        return g.node[root]["height"]

    def _get_sorted_leaves(root):
        children = g.successors(root)
        if not children:
            return [root]
        child_heights = [_get_height(x) for x in children]
        order = sorted(range(len(children)), key=lambda x: child_heights[x])
        ret = []
        for co in order:
            cr = children[co]
            ret += _get_sorted_leaves(cr)

        return ret

    return _get_sorted_leaves(root)


def topological_sort_traversal_longest_path(g):
    ''' The graph 'g' may contain several source nodes (nodes without incoming
        edge), which could have be in any order and still being a valid
        topoligical sorting result. We would like to arrange these source nodes
        so that the average live spans of the computed blobs are shorter.
        The idea is to sort the source nodes based on the length of their path to
        the target node so that the one with longer path is used first.
        This is done by:
        - Add a single target node if there are multiple target nodes in 'g'.
        - Find the longest path between each source and the target node.
        - Convert the longest paths to a tree with the target node being the root
          and source nodes being the leaves.
        - Sort the nodes of the tree based on the height of the tree.
    '''
    gt = _add_single_target_ifneeded(g)
    source_nodes = _find_source_nodes(gt)
    lpaths = _get_longest_paths(gt, source_nodes)
    tree, root = _build_tree(lpaths.values())
    sorted_sources = _sort_tree_leaves(tree, root)
    assert(sorted(sorted_sources) == sorted(source_nodes))

    ret = nx.topological_sort(g, sorted_sources)
    assert(len(ret) == len(g.node))
    return ret


def topological_sort_traversal(g):
    return nx.topological_sort(g)


def compute_ranges(linearized_ops, blob_sizes=None):
    blobs = collections.defaultdict(
        lambda: LiveRange(defined=None, used=None, size=None))
    for i, op in enumerate(linearized_ops):
        for blob in op.input:
            used = blobs[blob].used
            if used is None:
                used = i
            else:
                used = max(used, i)
            blobs[blob] = blobs[blob]._replace(used=used)
            blob_size = blob_sizes[blob] if blob_sizes else None
            assert not blob_sizes or blob_size is not None
            blobs[blob] = blobs[blob]._replace(size=blob_size)
        for blob in op.output:
            defined = blobs[blob].defined
            if defined is None:
                defined = i
            else:
                defined = min(defined, i)
            blobs[blob] = blobs[blob]._replace(defined=defined)
            blob_size = blob_sizes[blob] if blob_sizes else None
            assert not blob_sizes or blob_size is not None
            blobs[blob] = blobs[blob]._replace(size=blob_size)

    return blobs


def is_compatible(candidate_range, assignment, static_blobs):
    (name, range_) = assignment[-1]
    if name in static_blobs:
        return False
    if candidate_range.defined is None or range_.defined is None \
      or range_.used is None:
        return False
    return candidate_range.defined > range_.used


def compute_blob_assignments(assignments):
    blob_assignments = {}
    for assignment in assignments:
        if len(assignment) == 1:
            continue
        last_blob, _ = assignment[-1]
        for (blob, _) in assignment:
            blob_assignments[blob] = last_blob
    return blob_assignments


def _get_max_size(assignment):
    if not assignment:
        return 0
    ret = max([x[1].size for x in assignment])
    ret = 0 if ret is None else ret
    return ret


def get_memory_usage(assignments):
    ret = 0
    for cur in assignments:
        ret += _get_max_size(cur)
    return ret


def compute_assignments_greedy(ranges_sorted, init_assignments=None):
    assignments = init_assignments or []
    visited = {y[0] for x in assignments for y in x}

    for (name, range_) in ranges_sorted:
        if name in visited:
            continue
        assigned = False
        best_assignment = 0
        min_dist = float("inf")
        candidate_size = range_.size or 0
        for idx, assignment in enumerate(assignments):
            if is_compatible(range_, assignment, []):
                assigned = True
                dist = abs(_get_max_size(assignment) - candidate_size)
                if dist < min_dist:
                    min_dist = dist
                    best_assignment = idx
        if assigned:
            assignment = assignments[best_assignment]
            assignment.append((name, range_))
        else:
            assignments.append([(name, range_)])
    return assignments


def get_updated_ranges(ranges, max_live=None):
    ''' Set LiveRange.defined = -1 if it is None
        Set LiveRange.used = max_live if it is None
        Set LiveRanee.size = 1 if it is None
    '''

    def _get_max_live(ranges):
        max_live = max(x[1].used for x in ranges if x[1].used) + 1
        return max_live

    def _update_range(x, max_live, size):
        cx = x
        if x[1].defined is None:
            cx = (cx[0], cx[1]._replace(defined=-1))
        if x[1].used is None:
            cx = (cx[0], cx[1]._replace(used=max_live))
        if x[1].size is None:
            cx = (cx[0], cx[1]._replace(size=size))
        return cx

    if max_live is None:
        max_live = _get_max_live(ranges)
    ranges = [_update_range(x, max_live, 1) for x in ranges]

    return ranges


def compute_assignments(ranges, static_blobs):
    # Sort the ranges based on when they are last used.
    # If LiveRange.used is None, then the blob is never used and could
    # be consumed externally. Sort these to the end of the list as opposed
    # to the beginning so that they can be shared as well.
    ranges = sorted(
        list(ranges.items()),
        key=lambda p: (p[1].used is None, p[1].used),
    )
    # Update None values
    ranges = get_updated_ranges(ranges)

    # sharable blobs
    ranges_sharable = [x for x in ranges if x[0] not in static_blobs]
    # static blobs, not sharable
    ranges_static = [x for x in ranges if x[0] in static_blobs]

    log.info("Total sharable blobs {}".format(len(ranges_sharable)))

    best_assignment = compute_assignments_greedy(ranges_sharable, [])
    best_assignment += [[x] for x in ranges_static]

    # verify_assignments(best_assignment)

    return best_assignment


def verify_assignments(assignments):
    for cur in assignments:
        for x, y in zip(cur[0:-1], cur[1:]):
            assert x[1].used < y[1].defined


def compute_interference_graph(ops):
    g = nx.DiGraph()
    for i, op in enumerate(ops):
        g.add_node(i, op=op)
    for i, parent_op in enumerate(ops):
        for j, child_op in enumerate(ops):
            if i == j:
                continue
            if any(output in child_op.input for output in parent_op.output):
                deps = set(child_op.input).intersection(parent_op.output)
                g.add_edge(i, j, deps=deps)
                assert nx.is_directed_acyclic_graph(g), child_op
    return g


Optimization = collections.namedtuple(
    'Optimization', ['net', 'assignments', 'blob_assignments'])


def apply_assignments(net, blob_assignments):
    def canonical_name(blob):
        if blob not in blob_assignments:
            return blob
        return blob_assignments[blob]

    for op in net.op:
        # Descend into subnets of the recurrent network
        if op.type.startswith('RecurrentNetwork'):
            apply_recurrent_blob_assignments(op, blob_assignments, canonical_name)

        for i, input_ in enumerate(op.input):
            op.input[i] = canonical_name(input_)
        for i, output in enumerate(op.output):
            op.output[i] = canonical_name(output)


def apply_recurrent_blob_assignments(op, blob_assignments, canonical_name):
    log.debug("Applying assignments to recurrent op: {}".format(op.type))
    import google.protobuf.text_format as protobuftx
    step_args = [a for a in op.arg if a.name.endswith("step_net")]
    for step_arg in step_args:
        step_proto = caffe2_pb2.NetDef()
        protobuftx.Merge(step_arg.s, step_proto)
        apply_assignments(step_proto, blob_assignments)
        for i, einp in enumerate(step_proto.external_input):
            if einp in blob_assignments:
                step_proto.external_input[i] = canonical_name(einp)
        step_arg.s = str(step_proto)
    # Store renamings
    for blob, renamed in blob_assignments.items():
        if blob in list(op.input) + list(op.output):
            a = caffe2_pb2.Argument()
            a.name = blob + ".rename"
            a.s = str(renamed)
            op.arg.extend([a])


def optimize_interference(net, static_blobs,
                          ordering_function=topological_sort_traversal,
                          blob_sizes=None):
    """
    1) Use a BFS traversal of the execution graph to generate an
       ordering of the node executions.
    2) Generate use-def ranges for each `blob` in the BFS traversal
       order.
    3) Assign blobs to `canonical blobs`
    4) Rename blobs to canonical blobs
    """
    net = copy.deepcopy(net)
    g = compute_interference_graph(net.op)
    ordering = ordering_function(g)
    linearized_ops = [net.op[i] for i in ordering]

    # Reorder ops in net based on the computed linearlized order.
    # If the graph has multiple topological orderings and if the NetDef's
    # ordering differs from the order used to compute ranges, then the
    # runtime might end up overwriting blobs before they are used.
    del net.op[:]
    net.op.extend(linearized_ops)

    ranges = compute_ranges(linearized_ops, blob_sizes)
    assignments = compute_assignments(ranges, static_blobs)
    blob_assignments = compute_blob_assignments(assignments)
    apply_assignments(net, blob_assignments)
    return Optimization(
        net=net,
        blob_assignments=blob_assignments,
        assignments=assignments)


Statistics = collections.namedtuple(
    'Statistics', ['baseline_nbytes', 'optimized_nbytes'])


def compute_statistics(assignments):
    def blob_nbytes(blob):
        return workspace.FetchBlob(blob).nbytes
    blob_bytes = {
        blob: blob_nbytes(blob) for assignment in assignments
        for (blob, _) in assignment}
    baseline_nbytes = sum(v for _, v in blob_bytes.items())
    optimized_nbytes = sum(
        max(blob_bytes[blob] for (blob, _) in assignment)
        for assignment in assignments)
    return Statistics(
        baseline_nbytes=baseline_nbytes,
        optimized_nbytes=optimized_nbytes)


def collect_blob_sizes(net):
    ''' College blob sizes from worksapce '''
    def blob_nbytes(blob):
        return workspace.FetchBlob(blob).nbytes

    blobs = {}
    for op in net.op:
        for blob in op.input:
            blobs[blob] = blob_nbytes(blob)
        for blob in op.output:
            blobs[blob] = blob_nbytes(blob)

    return blobs
