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
from typing import Any, Optional, TypeVar

import oneflow as flow
from oneflow.nn.functional import normalize
from oneflow.nn.modules.module import Module
from oneflow.framework.tensor import Tensor


class SpectralNorm:
    _version: int = 1
    name: str
    dim: int
    n_power_iterations: int
    eps: float

    def __init__(
        self,
        name: str = "weight",
        n_power_iterations: int = 1,
        dim: int = 0,
        eps: float = 1e-12,
    ) -> None:
        self.name = name
        self.dim = dim
        if n_power_iterations <= 0:
            raise ValueError(
                "Expected n_power_iterations to be positive, but "
                "got n_power_iterations={}".format(n_power_iterations)
            )
        self.n_power_iterations = n_power_iterations
        self.eps = eps

    def reshape_weight_to_matrix(self, weight: Tensor) -> Tensor:
        weight_mat = weight
        if self.dim != 0:
            # permute dim to front
            weight_mat = weight_mat.permute(
                self.dim, *[d for d in range(weight_mat.dim()) if d != self.dim]
            )
        height = weight_mat.size(0)
        return weight_mat.reshape(height, -1)

    def compute_weight(self, module: Module, do_power_iteration: bool) -> Tensor:
        weight = getattr(module, self.name + "_orig")
        u = getattr(module, self.name + "_u")
        v = getattr(module, self.name + "_v")
        weight_mat = self.reshape_weight_to_matrix(weight)

        if do_power_iteration:
            with flow.no_grad():
                for _ in range(self.n_power_iterations):
                    # Spectral norm of weight equals to `u^T W v`, where `u` and `v`
                    # are the first left and right singular vectors.
                    # This power iteration produces approximations of `u` and `v`.
                    v = normalize(flow.mv(weight_mat.t(), u), dim=0, eps=self.eps)
                    u = normalize(flow.mv(weight_mat, v), dim=0, eps=self.eps)
                if self.n_power_iterations > 0:
                    u = u.clone()
                    v = v.clone()
        setattr(module, self.name + "_u", u)
        setattr(module, self.name + "_v", v)
        sigma = flow.dot(u, flow.mv(weight_mat, v))
        weight = weight / sigma
        return weight

    def remove(self, module: Module) -> None:
        with flow.no_grad():
            weight = self.compute_weight(module, do_power_iteration=False)
        delattr(module, self.name)
        delattr(module, self.name + "_u")
        delattr(module, self.name + "_v")
        delattr(module, self.name + "_orig")
        module.register_parameter(self.name, flow.nn.Parameter(weight.detach()))

    def __call__(self, module: Module, inputs: Any) -> None:
        setattr(
            module,
            self.name,
            self.compute_weight(module, do_power_iteration=module.training),
        )

    @staticmethod
    def apply(
        module: Module, name: str, n_power_iterations: int, dim: int, eps: float
    ) -> "SpectralNorm":
        for k, hook in module._forward_pre_hooks.items():
            if isinstance(hook, SpectralNorm) and hook.name == name:
                raise RuntimeError(
                    "Cannot register two spectral_norm hooks on "
                    "the same parameter {}".format(name)
                )

        fn = SpectralNorm(name, n_power_iterations, dim, eps)
        weight = module._parameters[name]
        if weight is None:
            raise ValueError(
                f"`SpectralNorm` cannot be applied as parameter `{name}` is None"
            )

        with flow.no_grad():
            weight_mat = fn.reshape_weight_to_matrix(weight)

            h, w = weight_mat.size()
            # randomly initialize `u` and `v`
            u = normalize(weight.new_empty(h).normal_(0, 1), dim=0, eps=fn.eps)
            v = normalize(weight.new_empty(w).normal_(0, 1), dim=0, eps=fn.eps)

        delattr(module, fn.name)
        module.register_parameter(fn.name + "_orig", weight)
        setattr(module, fn.name, weight.data)
        module.register_buffer(fn.name + "_u", u)
        module.register_buffer(fn.name + "_v", v)

        module.register_forward_pre_hook(fn)
        return fn


T_module = TypeVar("T_module", bound=Module)

def spectral_norm(
    module: T_module,
    name: str = "weight",
    n_power_iterations: int = 1,
    eps: float = 1e-12,
    dim: Optional[int] = None,
) -> T_module:
    r"""Applies spectral normalization to a parameter in the given module.

    .. math::
        \mathbf{W}_{SN} = \dfrac{\mathbf{W}}{\sigma(\mathbf{W})},
        \sigma(\mathbf{W}) = \max_{\mathbf{h}: \mathbf{h} \ne 0} \dfrac{\|\mathbf{W} \mathbf{h}\|_2}{\|\mathbf{h}\|_2}

    Spectral normalization stabilizes the training of discriminators (critics)
    in Generative Adversarial Networks (GANs) by rescaling the weight tensor
    with spectral norm :math:`\sigma` of the weight matrix calculated using
    power iteration method. If the dimension of the weight tensor is greater
    than 2, it is reshaped to 2D in power iteration method to get spectral
    norm. This is implemented via a hook that calculates spectral norm and
    rescales weight before every :meth:`~Module.forward` call.

    See `Spectral Normalization for Generative Adversarial Networks`_ .

    .. _`Spectral Normalization for Generative Adversarial Networks`: https://arxiv.org/abs/1802.05957

    Args:
        module (nn.Module): containing module
        name (str, optional): name of weight parameter
        n_power_iterations (int, optional): number of power iterations to
            calculate spectral norm
        eps (float, optional): epsilon for numerical stability in
            calculating norms
        dim (int, optional): dimension corresponding to number of outputs,
            the default is ``0``, except for modules that are instances of
            ConvTranspose{1,2,3}d, when it is ``1``

    Returns:
        The original module with the spectral norm hook

    For Example::

    .. code-block:: python

        >>> import oneflow as flow
        >>> m = flow.nn.utils.spectral_norm(flow.nn.Linear(20, 40))
        >>> m
        Linear(in_features=20, out_features=40, bias=True)
        >>> m.weight_u.size()
        oneflow.Size([40])

    """
    if dim is None:
        if isinstance(
            module,
            (flow.nn.ConvTranspose1d, flow.nn.ConvTranspose2d, flow.nn.ConvTranspose3d),
        ):
            dim = 1
        else:
            dim = 0
    SpectralNorm.apply(module, name, n_power_iterations, dim, eps)
    return module


def remove_spectral_norm(module: T_module, name: str = "weight") -> T_module:
    r"""Removes the spectral normalization reparameterization from a module.

    Args:
        module (Module): containing module
        name (str, optional): name of weight parameter

    For Example:

    .. code-block:: python

        >>> import oneflow as flow
        >>> m = flow.nn.utils.spectral_norm(nn.Linear(40, 10))
        >>> flow.nn.utils.remove_spectral_norm(m)

    """
    for k, hook in module._forward_pre_hooks.items():
        if isinstance(hook, SpectralNorm) and hook.name == name:
            hook.remove(module)
            del module._forward_pre_hooks[k]
            break
    else:
        raise ValueError("spectral_norm of '{}' not found in {}".format(name, module))

    for k, hook in module._state_dict_hooks.items():
        if isinstance(hook, SpectralNormStateDictHook) and hook.fn.name == name:
            del module._state_dict_hooks[k]
            break

    for k, hook in module._load_state_dict_pre_hooks.items():
        if isinstance(hook, SpectralNormLoadStateDictPreHook) and hook.fn.name == name:
            del module._load_state_dict_pre_hooks[k]
            break

    return module


if __name__ == "__main__":
    import doctest

    doctest.testmod(raise_on_error=True)
