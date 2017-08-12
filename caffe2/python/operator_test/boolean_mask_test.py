from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np
from hypothesis import given
import hypothesis.strategies as st
from caffe2.python import core
import caffe2.python.hypothesis_test_util as hu


class TestBooleanMaskOp(hu.HypothesisTestCase):

    @given(x=hu.tensor(min_dim=1,
                       max_dim=5,
                       elements=st.floats(min_value=0.5, max_value=1.0)),
           **hu.gcs)
    def test_boolean_mask(self, x, gc, dc):
        op = core.CreateOperator("BooleanMask",
                                 ["data", "mask"],
                                 "masked_data")
        mask = np.random.choice(a=[True, False], size=x.shape[0])

        def ref(x, mask):
            return (x[mask],)

        self.assertReferenceChecks(gc, op, [x, mask], ref)
        self.assertDeviceChecks(dc, op, [x, mask], [0])

    @given(x=hu.tensor(min_dim=1,
                       max_dim=5,
                       elements=st.floats(min_value=0.5, max_value=1.0)),
           **hu.gcs)
    def test_boolean_mask_indices(self, x, gc, dc):
        op = core.CreateOperator("BooleanMask",
                                 ["data", "mask"],
                                 ["masked_data", "masked_indices"])
        mask = np.random.choice(a=[True, False], size=x.shape[0])

        def ref(x, mask):
            return (x[mask], np.where(mask)[0])

        self.assertReferenceChecks(gc, op, [x, mask], ref)
        self.assertDeviceChecks(dc, op, [x, mask], [0])

    @given(x=hu.tensor(min_dim=2,
                       max_dim=5,
                       elements=st.floats(min_value=0.5, max_value=1.0)),
           **hu.gcs)
    def test_sequence_mask_with_lengths(self, x, gc, dc):
        op = core.CreateOperator("SequenceMask",
                                 ["data", "lengths"],
                                 ["masked_data"],
                                 mode="sequence",
                                 axis=len(x.shape) - 1)
        elem_dim = x.shape[-1]
        leading_dim = 1
        for dim in x.shape[:-1]:
            leading_dim *= dim
        lengths = np.random.randint(0, elem_dim, [leading_dim])\
            .astype(np.int32)
        print('lengths', lengths)

        def ref(x, lengths):
            ref = np.reshape(x, [leading_dim, elem_dim])
            for i in range(leading_dim):
                for j in range(elem_dim):
                    if j >= lengths[i]:
                        ref[i, j] = -np.inf
            return [ref.reshape(x.shape)]

        self.assertReferenceChecks(gc, op, [x, lengths], ref)
        self.assertDeviceChecks(dc, op, [x, lengths], [0])

    @given(x=hu.tensor(min_dim=2,
                       max_dim=5,
                       elements=st.floats(min_value=0.5, max_value=1.0)),
           mode=st.sampled_from(['upper', 'lower', 'upperdiag', 'lowerdiag']),
           **hu.gcs)
    def test_sequence_mask_triangle(self, x, mode, gc, dc):
        x = np.array([[0, 1], [2, 3]], dtype=np.float32)
        op = core.CreateOperator("SequenceMask",
                                 ["data"],
                                 ["masked_data"],
                                 mode=mode,
                                 axis=len(x.shape) - 1)
        elem_dim = x.shape[-1]
        leading_dim = 1
        for dim in x.shape[:-1]:
            leading_dim *= dim

        if mode == 'upper':
            def compare(i, j):
                return j > i
        elif mode == 'lower':
            def compare(i, j):
                return j < i
        elif mode == 'upperdiag':
            def compare(i, j):
                return j >= i
        elif mode == 'lowerdiag':
            def compare(i, j):
                return j <= i

        def ref(x):
            ref = np.reshape(x, [leading_dim, elem_dim])
            for i in range(leading_dim):
                for j in range(elem_dim):
                    if compare(i, j):
                        ref[i, j] = -np.inf
            return [ref.reshape(x.shape)]

        self.assertReferenceChecks(gc, op, [x], ref)
        self.assertDeviceChecks(dc, op, [x], [0])
