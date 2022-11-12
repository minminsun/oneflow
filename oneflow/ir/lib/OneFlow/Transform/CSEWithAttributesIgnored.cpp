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
#include <iostream>
#include <string>
#include "OneFlow/OneFlowOps.h"
#include "OneFlow/Passes.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

using namespace mlir;

namespace mlir {

namespace oneflow {

namespace {

static auto MAGIC_OP_NAME = "ONEFLOW_ERASE_MAGIC";
static auto MAGIC_SCOPE_SYMBOL_ID = 77777;

struct EraseAttributes : public mlir::OpInterfaceRewritePattern<UserOpCompatible> {
  explicit EraseAttributes(mlir::MLIRContext* context)
      : OpInterfaceRewritePattern<UserOpCompatible>(context, /*benefit=*/1) {}
  mlir::LogicalResult matchAndRewrite(UserOpCompatible op,
                                      mlir::PatternRewriter& rewriter) const override {
    if (op->getAttrOfType<StringAttr>(OpTrait::IsOpConfCompatible<void>::getOpNameAttr())
            .getValue()
            .str()
        != MAGIC_OP_NAME) {
      op->setAttr(OpTrait::IsOpConfCompatible<void>::getOpNameAttr(),
                  rewriter.getStringAttr(MAGIC_OP_NAME));
      op->setAttr(OpTrait::IsOpConfCompatible<void>::getScopeSymbolIDAttr(),
                  rewriter.getI64IntegerAttr(MAGIC_SCOPE_SYMBOL_ID));
      return success();
    } else {
      return failure();
    }
  }
};

struct PutAttributes : public mlir::OpInterfaceRewritePattern<UserOpCompatible> {
  explicit PutAttributes(mlir::MLIRContext* context, std::shared_ptr<std::atomic<int64_t>> id)
      : OpInterfaceRewritePattern<UserOpCompatible>(context, /*benefit=*/1), id_{id} {}
  mlir::LogicalResult matchAndRewrite(UserOpCompatible op,
                                      mlir::PatternRewriter& rewriter) const override {
    if (op->getAttrOfType<StringAttr>(OpTrait::IsOpConfCompatible<void>::getOpNameAttr())
            .getValue()
            .str()
        == MAGIC_OP_NAME) {
      op->setAttr(OpTrait::IsOpConfCompatible<void>::getOpNameAttr(),
                  rewriter.getStringAttr("op" + std::to_string(*id_)));
      *id_ += 1;
      return success();
    } else {
      return failure();
    }
  }

 private:
  std::shared_ptr<std::atomic<int64_t>> id_;
};

class CSEWithAttributesIgnored : public CSEWithAttributesIgnoredBase<CSEWithAttributesIgnored> {
  void runOnOperation() override {
    Operation* op = getOperation();
    RewritePatternSet patterns(op->getContext());
    patterns.add<EraseAttributes>(op->getContext());
    (void)applyPatternsAndFoldGreedily(op, std::move(patterns));
  }
};

class CSEPutAttributes : public CSEWithAttributesIgnoredBase<CSEWithAttributesIgnored> {
  void runOnOperation() override {
    Operation* op = getOperation();
    RewritePatternSet patterns(op->getContext());
    std::shared_ptr<std::atomic<int64_t>> id = std::make_shared<std::atomic<int64_t>>(0);
    patterns.add<PutAttributes>(op->getContext(), id);
    (void)applyPatternsAndFoldGreedily(op, std::move(patterns));
  }
};

}  // namespace

std::unique_ptr<Pass> createCSEWithAttributesIgnored() {
  return std::make_unique<CSEWithAttributesIgnored>();
}

std::unique_ptr<Pass> createCSEPutAttributes() { return std::make_unique<CSEPutAttributes>(); }

}  // namespace oneflow

}  // namespace mlir
