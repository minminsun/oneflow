"""
Copyright 2020 The OneFlow Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
"""

import unittest

import numpy as np

import oneflow.compatible.single_client.unittest
from oneflow.compatible import single_client as flow
import oneflow.compatible.single_client.typing as oft

import xrt_util


def make_job(input_shape, permute, xrts=[]):
    config = flow.FunctionConfig()
    xrt_util.set_xrt(config, xrts=xrts)
    @flow.global_function(function_config=config)
    def transpose_job(x:oft.Numpy.Placeholder(input_shape)):
        return flow.transpose(x, perm=permute)

    return transpose_job


class TestTranspose(unittest.TestCase):
    def _test_body(self, x, permute):
        func = make_job(x.shape, permute)
        out = func(x).get()
        out_np = out.numpy()
        flow.clear_default_session()
        for xrt in xrt_util.xrt_backends:
            xrt_job = make_job(x.shape, permute, xrts=[xrt])
            xrt_out = xrt_job(x).get()
            self.assertTrue(np.allclose(out_np, xrt_out.numpy(), rtol=0.001, atol=1e-05))
            flow.clear_default_session()

    def _test_ones_body(self, shape, permute, dtype=flow.float32):
        np_dtype = flow.convert_oneflow_dtype_to_numpy_dtype(dtype)
        x = np.ones(shape, dtype=np_dtype)
        self._test_body(x, permute)

    def _test_random_body(self, shape, permute, dtype=flow.float32):
        np_dtype = flow.convert_oneflow_dtype_to_numpy_dtype(dtype)
        x = np.random.random(shape).astype(np_dtype)
        self._test_body(x, permute)

    def test_ones_input(self):
        self._test_ones_body((1, 2), (1, 0))
        self._test_ones_body((2, 2, 2), (0, 2, 1))
        self._test_ones_body((2, 2, 2), (1, 0, 2))
        self._test_ones_body((2, 2, 2), (1, 2, 0))

    def test_random_input(self):
        self._test_random_body((1, 2), (1, 0))
        self._test_random_body((2, 2, 2), (0, 2, 1))
        self._test_random_body((2, 2, 2), (1, 0, 2))
        self._test_random_body((2, 2, 2), (1, 2, 0))


if __name__ == "__main__":
    unittest.main()
