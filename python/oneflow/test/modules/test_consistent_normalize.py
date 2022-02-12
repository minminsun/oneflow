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
from collections import OrderedDict

import numpy as np
from test_util import GenArgList

import oneflow as flow
import oneflow.unittest

from oneflow.test_utils.automated_test_util import *


@autotest(n=5, check_graph=False)
def test_functional_normalize(test_case, placement, sbp):
    ndim = random(low=2).to(int).value()
    shape = [random(low=2, high=4) * 8 for i in range(ndim)]
    x = random_tensor(len(shape), *shape).to_consistent(
        placement=placement, sbp=sbp
    )
    dim = random(low=0, high=ndim).to(int).value()
    y = torch.nn.functional.normalize(x, oneof(2, 3, 4), dim, 1e-12)
    return y


class TestModule(flow.unittest.TestCase):
    @consistent
    def test_normalize_with_random_data(test_case):
        for placement in all_placement():
            for sbp in all_sbp(placement, max_dim=2):
                test_functional_normalize(test_case, placement, sbp)

if __name__ == "__main__":
    unittest.main()


#两卡结果不正确。