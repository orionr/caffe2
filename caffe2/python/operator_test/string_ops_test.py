from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals
from caffe2.python import core
from hypothesis import given
import caffe2.python.hypothesis_test_util as hu
import hypothesis.strategies as st
import numpy as np


def _string_lists(alphabet=None):
    return st.lists(
        elements=st.text(alphabet=alphabet, average_size=3),
        min_size=0,
        max_size=3)


class TestStringOps(hu.HypothesisTestCase):
    @given(strings=_string_lists())
    def test_string_prefix(self, strings):
        length = 3
        # although we are utf-8 encoding below to avoid python exceptions,
        # StringPrefix op deals with byte-length prefixes, which may produce
        # an invalid utf-8 string. The goal here is just to avoid python
        # complaining about the unicode -> str conversion.
        strings = np.array(
            map(lambda a: a.encode('utf-8'), strings), dtype=np.object)

        def string_prefix_ref(strings):
            return (
                np.array(map(lambda a: a[:length], strings), dtype=object), )

        op = core.CreateOperator(
            'StringPrefix',
            ['strings'],
            ['stripped'],
            length=length)
        self.assertReferenceChecks(
            hu.cpu_do,
            op,
            [strings],
            string_prefix_ref)

    @given(strings=_string_lists())
    def test_string_suffix(self, strings):
        length = 3
        strings = np.array(
            map(lambda a: a.encode('utf-8'), strings), dtype=np.object)

        def string_suffix_ref(strings):
            return (
                np.array(map(lambda a: a[-length:], strings), dtype=object), )

        op = core.CreateOperator(
            'StringSuffix',
            ['strings'],
            ['stripped'],
            length=length)
        self.assertReferenceChecks(
            hu.cpu_do,
            op,
            [strings],
            string_suffix_ref)

    @given(strings=st.text(alphabet=['a', 'b'], average_size=3))
    def test_string_starts_with(self, strings):
        prefix = 'a'
        strings = np.array(
            map(lambda a: str(strings), strings), dtype=np.object)

        def string_starts_with_ref(strings):
            return (np.array(
                map(lambda a: a.startswith(prefix), strings), dtype=bool), )

        op = core.CreateOperator(
            'StringStartsWith',
            ['strings'],
            ['bools'],
            prefix=prefix)
        self.assertReferenceChecks(
            hu.cpu_do,
            op,
            [strings],
            string_starts_with_ref)

    @given(strings=st.text(alphabet=['a', 'b'], average_size=3))
    def test_string_ends_with(self, strings):
        suffix = 'a'
        strings = np.array(
            map(lambda a: str(strings), strings), dtype=np.object)

        def string_ends_with_ref(strings):
            return (np.array(
                map(lambda a: a.endswith(suffix), strings), dtype=bool), )

        op = core.CreateOperator(
            'StringEndsWith',
            ['strings'],
            ['bools'],
            suffix=suffix)
        self.assertReferenceChecks(
            hu.cpu_do,
            op,
            [strings],
            string_ends_with_ref)
