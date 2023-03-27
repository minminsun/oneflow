import re

import oneflow as flow


def parse_size(size):
    units = {"B": 1, "KB": 2**10, "MB": 2**20, "GB": 2**30, "TB": 2**40 ,
             "":  1, "KIB": 10**3, "MIB": 10**6, "GIB": 10**9, "TIB": 10**12}
    m = re.match(r'^([\d\.]+)\s*([a-zA-Z]{0,3})$', str(size).strip())
    assert m is not None
    number, unit = float(m.group(1)), m.group(2).upper()
    return int(number*units[unit])


def set_budget(budget: str):
    budget_in_bytes = parse_size(budget)
    flow._oneflow_internal.remat.set_budget_in_bytes(budget_in_bytes)


def get_budget():
    budget_in_bytes = flow._oneflow_internal.remat.budget_in_bytes()
    return budget_in_bytes


set_small_pieces_optimization = flow._oneflow_internal.remat.set_small_pieces_optimization
is_small_pieces_optimization_enabled = flow._oneflow_internal.remat.is_small_pieces_optimization_enabled
