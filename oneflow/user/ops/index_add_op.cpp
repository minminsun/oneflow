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
#include "oneflow/core/framework/framework.h"
#include "oneflow/core/framework/op_generated.h"

namespace oneflow {

/* static */ Maybe<void> IndexAddOp::InferLogicalTensorDesc(user_op::InferContext* ctx) {
  const Shape& input_shape = ctx->InputShape("input", 0);
  ctx->SetOutputShape("output", 0, input_shape);
  ctx->SetOutputStride("output", 0, ctx->InputStride("input", 0));
  return Maybe<void>::Ok();
}

/*static*/ Maybe<void> IndexAddOp::InferPhysicalTensorDesc(user_op::InferContext* ctx) {
  return InferLogicalTensorDesc(ctx);
}

/* static */ Maybe<void> IndexAddOp::GetSbp(user_op::SbpContext* ctx) {
  // TODO(yangzhimin): support more valid sbp signature.
  return Maybe<void>::Ok();
}

/* static */ Maybe<void> IndexAddOp::InferDataType(user_op::InferContext* ctx) {
  ctx->SetOutputDType("output", 0, ctx->InputDType("input", 0));
  return Maybe<void>::Ok();
}

}  // namespace oneflow
