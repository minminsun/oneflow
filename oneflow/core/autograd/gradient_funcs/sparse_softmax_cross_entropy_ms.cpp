/*
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
*/
#include "oneflow/core/framework/attr_map.h"
#include "oneflow/core/framework/op_expr_grad_function.h"
#include "oneflow/core/framework/op_builder.h"
#include "oneflow/core/framework/op_expr.h"
#include "oneflow/core/framework/op_interpreter/op_interpreter_util.h"
#include "oneflow/core/functional/functional.h"

namespace oneflow {
namespace one {

struct SparseSoftmaxCrossEntropyMsInterpState : public OpExprInterpState {
  bool requires_grad;
  int64_t depth;
};

class SparseSoftmaxCrossEntropyMs
    : public OpExprGradFunction<SparseSoftmaxCrossEntropyMsInterpState> {
 public:
  Maybe<void> Init(const OpExpr& op) override;
  Maybe<void> Capture(SparseSoftmaxCrossEntropyMsInterpState* ctx, const TensorTuple& inputs,
                      const TensorTuple& outputs, const AttrMap& attrs) const override;
  Maybe<void> Apply(const SparseSoftmaxCrossEntropyMsInterpState* ctx, const TensorTuple& out_grads,
                    TensorTuple* in_grads) const override;

 private:
  AttrMap base_attrs_;
};

Maybe<void> SparseSoftmaxCrossEntropyMs::Init(const OpExpr& op) {
  const auto* fw_op_expr = dynamic_cast<const UserOpExpr*>(&op);
  CHECK_NOTNULL_OR_RETURN(fw_op_expr);
  base_attrs_ = MakeAttrMapFromUserOpConf(fw_op_expr->proto());
  return Maybe<void>::Ok();
}

Maybe<void> SparseSoftmaxCrossEntropyMs::Capture(SparseSoftmaxCrossEntropyMsInterpState* ctx,
                                                 const TensorTuple& inputs,
                                                 const TensorTuple& outputs,
                                                 const AttrMap& attrs) const {
  ctx->requires_grad = inputs.at(0)->requires_grad();  // prediction
  if (!ctx->requires_grad) { return Maybe<void>::Ok(); }
  ComposedAttrMap composed_attrs(attrs, base_attrs_);
  ctx->depth = JUST(composed_attrs.GetAttr<int64_t>("depth"));
  CHECK_EQ_OR_RETURN(inputs.size(), 2);
  CHECK_EQ_OR_RETURN(outputs.size(), 2);
  ctx->SaveTensorForBackward(outputs.at(1));  // prob
  ctx->SaveTensorForBackward(inputs.at(1));   // label
  return Maybe<void>::Ok();
}

Maybe<void> SparseSoftmaxCrossEntropyMs::Apply(const SparseSoftmaxCrossEntropyMsInterpState* ctx,
                                               const TensorTuple& out_grads,
                                               TensorTuple* in_grads) const {
  CHECK_EQ_OR_RETURN(out_grads.size(), 2);
  const auto& dy = out_grads.at(0);
  const auto& prob = ctx->SavedTensors().at(0);
  const auto& label = ctx->SavedTensors().at(1);
  // SparseSoftmaxCrossEntropyMs has 2 inputs (prediction and label), and the second input does not
  // require gradient.
  in_grads->resize(2);
  in_grads->at(0) = JUST(functional::SparseSoftmaxCrossEntropyMsGrad(label, dy, prob, ctx->depth));
  return Maybe<void>::Ok();
}

REGISTER_OP_EXPR_GRAD_FUNCTION("sparse_softmax_cross_entropy_ms", SparseSoftmaxCrossEntropyMs);

}  // namespace one
}  // namespace oneflow
