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
#include "mlir/Dialect/Tosa/Transforms/Passes.h"
#include "mlir/Dialect/LLVMIR/FunctionCallUtils.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMTypes.h"
#include "mlir/Dialect/LLVMIR/Transforms/RequestCWrappers.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/SymbolTable.h"
#include "oneflow/core/common/data_type.pb.h"
#include "oneflow/core/framework/dtype.h"
#include "oneflow/core/framework/variable_tensor_mgr.h"
#include "oneflow/core/operator/variable_op.h"
#include "oneflow/core/framework/sbp_context.h"
#include "oneflow/core/job/sbp_signature_builder.h"
#include "oneflow/core/framework/random_generator.h"
#include "OneFlow/SBP/SBPImporter.h"
#include "OneFlow/OneFlowOps.h"
#include "OneFlow/OneFlowDialect.h"
#include "OneFlow/OneFlowUtils.h"
#include "OneFlow/Passes.h"
#include "OneFlow/OneFlowUtils.h"
#include "OneFlow/OneFlowSupport.h"
#include "OneFlow/SBP/SBPAttributes.h"
#include "OneFlow/Transform/TransposeHelpers.h"
#include "mlir-c/BuiltinAttributes.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/OperationSupport.h"
#include "mlir/IR/MLIRContext.h"

#include "mlir/Conversion/LinalgToLLVM/LinalgToLLVM.h"
#include "mlir/Conversion/MemRefToLLVM/MemRefToLLVM.h"
#include "mlir/Conversion/ReconcileUnrealizedCasts/ReconcileUnrealizedCasts.h"
#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVMPass.h"
#include "mlir/Conversion/TosaToLinalg/TosaToLinalg.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Linalg/Passes.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/Transforms/Passes.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Func/Transforms/Passes.h"
#include "mlir/Dialect/Tensor/Transforms/Passes.h"
#include "mlir/Dialect/Tosa/IR/TosaOps.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/OperationSupport.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/BlockAndValueMapping.h"
#include "mlir/IR/Value.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Support/LogicalResult.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/Passes.h"
#include "mlir/Dialect/Bufferization/Transforms/Passes.h"
#include "mlir/Conversion/SCFToControlFlow/SCFToControlFlow.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/None.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <memory>
#include <vector>

#ifdef WITH_MLIR_CUDA_CODEGEN
#include "mlir/Conversion/AffineToStandard/AffineToStandard.h"
#include "mlir/Conversion/GPUCommon/GPUCommonPass.h"
#include "mlir/Conversion/GPUToNVVM/GPUToNVVMPass.h"
#include "mlir/Dialect/GPU/Transforms/Passes.h"
#include "mlir/Conversion/SCFToGPU/SCFToGPUPass.h"
#endif  // WITH_MLIR_CUDA_CODEGEN

#include "llvm/ADT/STLExtras.h"

#include <iostream>
#include <string>

namespace mlir {
namespace oneflow {

template<typename T>
LogicalResult DumpAssembly(::mlir::PatternRewriter& rewriter, T op) {
  // TODO: now we only need one JIT engine
  auto parent_func_op = op->template getParentOfType<oneflow::Job>();
  if (!parent_func_op) { return failure(); }
  auto parent_module_op = parent_func_op->template getParentOfType<ModuleOp>();
  if (!parent_module_op) { return failure(); }
  SymbolTable symbol_table(parent_module_op);
  std::string mlir;
  llvm::raw_string_ostream os_mlir(mlir);
  if (auto found = symbol_table.lookup(op.op_name())) {
    found->print(os_mlir);
  } else {
    parent_module_op->dump();
    return op.emitError("symbol of jit function not found: " + op.op_name());
  }
  op->setAttr("mlir_assembly", rewriter.getStringAttr(mlir));
  return success();
}

LLVM::LLVMFuncOp DeclareKernelLaunchCInterface(::mlir::PatternRewriter& rewriter,
                                               mlir::Location loc, ModuleOp* module,
                                               StringRef c_api_callee, Type llvm_ptr_type) {
  LLVM::LLVMFuncOp func;
  if (!(func = module->lookupSymbol<LLVM::LLVMFuncOp>(c_api_callee))) {
    OpBuilder::InsertionGuard guard(rewriter);
    rewriter.setInsertionPointToStart(module->getBody());
    auto void_type = LLVM::LLVMVoidType::get(rewriter.getContext());
    auto func_type = LLVM::LLVMFunctionType::get(void_type, {llvm_ptr_type, llvm_ptr_type}, false);
    func = rewriter.create<LLVM::LLVMFuncOp>(loc, c_api_callee, func_type, LLVM::Linkage::External);

    func->setAttr("llvm.emit_c_interface", mlir::UnitAttr::get(rewriter.getContext()));
  }
  return func;
}

LLVM::GlobalOp DeclareOrGetGlobalString(::mlir::PatternRewriter& rewriter, mlir::Location loc,
                                        ModuleOp* module, StringRef func_name) {
  LLVM::GlobalOp global;
  StringRef variable = rewriter.getStringAttr(func_name + "_var");
  if (!(global = module->lookupSymbol<LLVM::GlobalOp>(variable))) {
    OpBuilder::InsertionGuard insertGuard(rewriter);
    rewriter.setInsertionPointToStart(module->getBody());
    auto type =
        LLVM::LLVMArrayType::get(IntegerType::get(rewriter.getContext(), 8), func_name.size());
    global =
        rewriter.create<LLVM::GlobalOp>(loc, type, /*isConstant=*/true, LLVM::Linkage::Internal,
                                        variable, rewriter.getStringAttr(func_name),
                                        /*alignment=*/0);
  }
  return global;
}

ModuleOp GetModuleOpFromJobBodyOp(Operation* op) {
  auto parent_func_op = op->getParentOfType<oneflow::Job>();
  if (!parent_func_op) { return nullptr; }
  return parent_func_op->getParentOfType<ModuleOp>();
}

func::FuncOp GetOrInsertKernelOFFuncOp(::mlir::PatternRewriter& rewriter, Operation* op) {
  auto loc = op->getLoc();
  auto module = GetModuleOpFromJobBodyOp(op);
  if (!module) {
    emitError(loc) << "null ModuleOp " << *op;
    return nullptr;
  }

  BlockAndValueMapping mapping;
  OpBuilder::InsertionGuard guard(rewriter);
  rewriter.setInsertionPointToStart(module.getBody());

  auto func_name = op->getAttr("op_name").cast<StringAttr>().strref();
  auto func_type =
      rewriter.getFunctionType(TypeRange(op->getOperandTypes()), TypeRange(op->getResultTypes()));
  func::FuncOp func = rewriter.create<func::FuncOp>(loc, func_name, func_type);
  func.setSymVisibilityAttr(rewriter.getStringAttr("public"));
  func->setAttr("llvm.emit_c_interface", mlir::UnitAttr::get(rewriter.getContext()));
  func.getBody().emplaceBlock();
  for (auto& arg : func_type.getInputs()) { func.getBody().addArguments(arg, loc); }
  for (auto argument_pair :
       llvm::zip(ValueRange(op->getOperands()), func.getBody().getArguments())) {
    mapping.map(std::get<0>(argument_pair), std::get<1>(argument_pair));
  }
  rewriter.setInsertionPointToStart(&func.getBody().front());
  ImplicitLocOpBuilder new_block(loc, rewriter);
  new_block.clone(*op, mapping);
  SmallVector<::mlir::Value, 4> mapped_results;
  for (auto result : ValueRange(op->getResults())) {
    mapped_results.push_back(mapping.lookup(result));
  }
  rewriter.create<func::ReturnOp>(loc, mapped_results);
  return func;
}

LLVM::LLVMFuncOp GetOrInsertKernelLLVMFuncOp(::mlir::PatternRewriter& rewriter, func::FuncOp op) {
  auto loc = op->getLoc();
  auto llvm_ptr_type = LLVM::LLVMPointerType::get(IntegerType::get(rewriter.getContext(), 8));
  auto parent_module_op = op->getParentOfType<ModuleOp>();
  if (!parent_module_op) {
    emitError(loc) << "null ModuleOp " << *op;
    return nullptr;
  }
  StringRef c_api_callee = "kernel_launch";
  LLVM::LLVMFuncOp c_api_func =
      DeclareKernelLaunchCInterface(rewriter, loc, &parent_module_op, c_api_callee, llvm_ptr_type);

  auto func_name = std::string("_mlir_ciface_") + op.getSymName().str();
  auto void_type = LLVM::LLVMVoidType::get(rewriter.getContext());
  auto func_type = LLVM::LLVMFunctionType::get(void_type, {llvm_ptr_type, llvm_ptr_type}, false);
  OpBuilder::InsertionGuard guard_module(rewriter);
  rewriter.setInsertionPointToEnd(parent_module_op.getBody());

  auto func = rewriter.create<LLVM::LLVMFuncOp>(loc, func_name, func_type);
  func->setAttr("llvm.emit_c_interface", mlir::UnitAttr::get(rewriter.getContext()));
  func.getBody().emplaceBlock();
  func.getBody().addArgument(llvm_ptr_type, loc);
  func.getBody().addArgument(llvm_ptr_type, loc);
  OpBuilder::InsertionGuard guard_func(rewriter);
  rewriter.setInsertionPointToStart(&func.getBody().front());

  auto ctx_ptr = func.getBody().getArgument(0);
  auto kernel_ptr = func.getBody().getArgument(1);
  rewriter.create<LLVM::CallOp>(loc, c_api_func, ValueRange{ctx_ptr, kernel_ptr});
  rewriter.create<LLVM::ReturnOp>(loc, ValueRange());

  return func;
}
// TODO: cfg/multi block support
func::FuncOp GetOrInsertFuncOp(::mlir::PatternRewriter& rewriter, mlir::Location loc,
                               StringRef func_name, ValueRange operands, ValueRange results,
                               SmallVector<Operation*, 4> ops) {
  BlockAndValueMapping mapping;
  SmallVector<Type, 4> argument_types;
  argument_types.reserve(operands.size());
  SmallVector<Type, 4> result_types;
  argument_types.reserve(results.size());
  for (auto argument : operands) { argument_types.push_back(argument.getType()); }
  for (auto result : results) { result_types.push_back(result.getType()); }
  auto func_type = rewriter.getFunctionType(argument_types, result_types);
  auto first_op = *ops.begin();
  auto parent_func_op = first_op->getParentOfType<oneflow::Job>();
  if (!parent_func_op) {
    emitError(loc) << "null parent oneflow::Job " << *first_op;
    return nullptr;
  }
  auto parent_module_op = parent_func_op->getParentOfType<ModuleOp>();
  if (!parent_module_op) {
    emitError(loc) << "null ModuleOp " << *first_op;
    return nullptr;
  }
  SymbolTable symbol_table(parent_module_op);
  OpBuilder::InsertionGuard guard(rewriter);
  Block::iterator insertPt(parent_func_op->getNextNode());
  rewriter.setInsertionPointToStart(parent_module_op.getBody());
  if (parent_func_op->hasAttr("llvm.emit_c_interface")) {
    emitError(loc) << "parent should not has attr of llvm.emit_c_interface " << *parent_func_op;
    return nullptr;
  }
  auto function = rewriter.create<func::FuncOp>(loc, func_name, func_type);
  function->setAttr("llvm.emit_c_interface", mlir::UnitAttr::get(rewriter.getContext()));
  function.getBody().emplaceBlock();
  for (auto& arg : argument_types) { function.getBody().addArguments(arg, loc); }
  for (auto argument_pair : llvm::zip(operands, function.getBody().getArguments())) {
    mapping.map(std::get<0>(argument_pair), std::get<1>(argument_pair));
  }
  rewriter.setInsertionPointToStart(&function.getBody().front());
  ImplicitLocOpBuilder nb(loc, rewriter);
  for (auto op : ops) { nb.clone(*op, mapping); }
  SmallVector<::mlir::Value, 4> mapped_results;
  for (auto result : results) { mapped_results.push_back(mapping.lookup(result)); }
  rewriter.create<func::ReturnOp>(loc, mapped_results);
  if (symbol_table.lookup(func_name)) {
    emitError(loc) << func_name << " should not be at symbol table of ModuleOp";
    return nullptr;
  }
  return function;
}

NamedAttrList GetJitOpAttributes(::mlir::PatternRewriter& rewriter, StringRef op_name,
                                 int32_t input_size, int32_t output_size, Operation* op) {
  NamedAttrList attributes;
  attributes.set(OpTrait::IsOpConfCompatible<void>::getDeviceTagAttr(),
                 OpTrait::IsOpConfCompatible<void>::getDeviceTag(op));
  attributes.set(OpTrait::IsOpConfCompatible<void>::getDeviceNameAttr(),
                 OpTrait::IsOpConfCompatible<void>::getDeviceName(op));
  if (auto hierarchy = OpTrait::IsOpConfCompatible<void>::getHierarchy(op)) {
    attributes.set(OpTrait::IsOpConfCompatible<void>::getHierarchyAttr(), hierarchy);
  }
  attributes.set(OpTrait::IsOpConfCompatible<void>::getOpNameAttr(),
                 rewriter.getStringAttr(op_name));
  if (auto scope_symbol_id = OpTrait::IsOpConfCompatible<void>::getScopeSymbolID(op)) {
    attributes.set(OpTrait::IsOpConfCompatible<void>::getScopeSymbolIDAttr(), scope_symbol_id);
  }
  return attributes;
}

::llvm::SmallVector<::mlir::Value, 4> OutlineMulCast(::mlir::PatternRewriter& rewriter,
                                                     mlir::OpResult mul_res,
                                                     mlir::OpResult cast_res) {
  auto mul_op = mul_res.getDefiningOp();
  auto scale = mlir::Value();
  auto output = mlir::Value();
  if (auto scalar_mul_op = llvm::dyn_cast<ScalarMulByTensorOp>(mul_op)) {
    scale = scalar_mul_op.scalar();
    output = scalar_mul_op.y();
  } else if (auto broadcast_mul_op = llvm::dyn_cast<BroadcastMulOp>(mul_op)) {
    scale = broadcast_mul_op.y();
    output = broadcast_mul_op.z();
  } else {
    mul_res.getDefiningOp()->emitError("pattern mul(cast(x), scalar) doesn't support this op");
    exit(1);
  }
  if (!mul_op->hasTrait<OpTrait::IsOpConfCompatible>()) {
    mul_res.getDefiningOp()->emitError("not OpConf compatible");
    exit(1);
  }
  if (auto cast_op = llvm::dyn_cast<CastOp>(cast_res.getDefiningOp())) {
    // TODO: extract a function to generate op name for jit op from ops being fused
    SmallString<64> op_name_storage;
    auto op_name =
        (cast_op.op_name() + "__FUSE__"
         + mul_op->getAttrOfType<StringAttr>(OpTrait::IsOpConfCompatible<void>::getOpNameAttr())
               .getValue()
               .str())
            .toStringRef(op_name_storage);
    SmallString<16> tempBuffer;
    op_name = SanitizeIdentifier(op_name, tempBuffer);
    SmallVector<::mlir::Value, 2> operands;
    operands.push_back(cast_op.in());
    operands.push_back(scale);
    SmallVector<::mlir::Value, 1> results;
    results.push_back(output);
    NamedAttrList attributes =
        GetJitOpAttributes(rewriter, op_name, operands.size(), results.size(), mul_op);
    SmallVector<Operation*, 4> ops = {cast_op, mul_op};
    auto function = GetOrInsertFuncOp(rewriter, mul_op->getLoc(), op_name, operands, results, ops);
    auto created = rewriter.create<MlirJitOp>(mul_op->getLoc(), function, attributes, operands);
    if (failed(DumpAssembly(rewriter, created))) { exit(1); }
    cast_op->dropAllUses();
    cast_op.erase();
    return created->getResults();
  }
  return {};
}

::llvm::SmallVector<::mlir::Value, 4> CreateGPUMemcpyOpFromMemrefCopy(
    ::mlir::PatternRewriter& rewriter, ::mlir::memref::CopyOp copyOp) {
  // NOTE: to get lowered to LLVM, it has to be async
  ::mlir::ValueRange empty_async_dependencies{};
  auto token = rewriter.getType<gpu::AsyncTokenType>();
  auto t0 =
      rewriter.create<gpu::WaitOp>(copyOp->getLoc(), token, empty_async_dependencies).asyncToken();
  auto t2 = rewriter
                .create<gpu::MemcpyOp>(copyOp->getLoc(),
                                       /*optional asyncToken*/ token,
                                       /*asyncDependencies*/ llvm::SmallVector<Value, 1>({t0}),
                                       /*dst*/ copyOp.target(),
                                       /*src*/ copyOp.source())
                .getResults();
  rewriter.create<gpu::WaitOp>(copyOp->getLoc(), llvm::None, t2);
  return {};
}

bool IsScalarTensor(Value value) {
  if (auto tensor = value.getType().dyn_cast<RankedTensorType>()) {
    return tensor.getNumElements() == 1;
  }
  return false;
}

bool HasZeroPadding(mlir::ArrayAttr padding) {
  for (auto val : padding.getValue()) {
    if (val.cast<IntegerAttr>().getValue().getSExtValue() != 0) return false;
  }
  return true;
}

bool IsPaddingCouldBeAssimilatedIntoConv(::mlir::ArrayAttr padding_before,
                                         ::mlir::ArrayAttr padding_after,
                                         ::mlir::StringAttr data_format) {
  if (padding_before.size() == 4 && padding_after.size() == 4) {
    if (padding_before.getValue().equals(padding_after.getValue())) {
      if (data_format.str() == "channels_first") {
        return padding_before.getValue()[0].cast<IntegerAttr>().getValue().getSExtValue() == 0
               && padding_before.getValue()[1].cast<IntegerAttr>().getValue().getSExtValue() == 0;
      }
      if (data_format.str() == "channels_last") {
        return padding_before.getValue()[0].cast<IntegerAttr>().getValue().getSExtValue() == 0
               && padding_before.getValue()[3].cast<IntegerAttr>().getValue().getSExtValue() == 0;
      }
    }
  }
  return false;
}

IntegerAttr getSI64IntegerAttr(::mlir::PatternRewriter& rewriter, int64_t value) {
  return IntegerAttr::get(rewriter.getIntegerType(64, /*isSigned=*/true),
                          APInt(64, value, /*isSigned=*/true));
}

::llvm::SmallVector<::mlir::Value, 4> CreateConv2dAndErasePad(::mlir::PatternRewriter& rewriter,
                                                              OpResult conv_result,
                                                              OpResult pad_result) {
  if (auto conv_op = llvm::dyn_cast<oneflow::Conv2DOp>(conv_result.getDefiningOp())) {
    if (auto pad_op = llvm::dyn_cast<oneflow::PadOp>(pad_result.getDefiningOp())) {
      NamedAttrList attributes = conv_op->getAttrs();
      SmallVector<Value, 4> operands;
      operands.push_back(pad_op.x());
      operands.push_back(conv_op.weight());
      if (conv_op.bias()) operands.push_back(conv_op.bias());
      if (conv_op.bias_multiplier()) operands.push_back(conv_op.bias_multiplier());
      llvm::SmallVector<int32_t> padding_before_array;
      if (conv_op.data_formatAttr().getValue().str() == "channels_first") {
        for (auto val : pad_op.padding_before().getValue().take_back(2)) {
          padding_before_array.push_back(val.cast<IntegerAttr>().getValue().getSExtValue());
        }
      } else {
        padding_before_array.push_back(
            pad_op.padding_before().getValue()[1].cast<IntegerAttr>().getValue().getSExtValue());
        padding_before_array.push_back(
            pad_op.padding_before().getValue()[2].cast<IntegerAttr>().getValue().getSExtValue());
      }
      attributes.set(conv_op.padding_beforeAttrName(),
                     getSI32ArrayAttr(rewriter, padding_before_array));
      auto res = rewriter
                     .create<oneflow::Conv2DOp>(conv_op->getLoc(), conv_op->getResultTypes(),
                                                operands, attributes)
                     ->getResults();
      // pad op is expected to be erased if it is not used
      return res;
    }
  }
  return {};
}

NamedAttrList GetUserOpCommonAttrs(MLIRContext* ctx, const std::string& op_name) {
  NamedAttrList attrs;
  attrs.set(OpTrait::IsOpConfCompatible<void>::getOpNameAttr(), StringAttr::get(ctx, op_name));
  attrs.set(OpTrait::IsOpConfCompatible<void>::getDeviceTagAttr(), StringAttr::get(ctx, "cpu"));
  attrs.set(OpTrait::IsOpConfCompatible<void>::getDeviceNameAttr(),
            ArrayAttr::get(ctx, llvm::to_vector<8>(llvm::map_range(ArrayRef<StringRef>({"@0:0"}),
                                                                   [&](StringRef v) -> Attribute {
                                                                     return StringAttr::get(ctx, v);
                                                                   }))));
  return attrs;
}

::llvm::SmallVector<::mlir::Value, 4> FuseConv2DBatchNorm(::mlir::PatternRewriter& rewriter,
                                                          OpResult conv_result,
                                                          OpResult bn_result) {
  if (auto conv_op = llvm::dyn_cast<oneflow::Conv2DOp>(conv_result.getDefiningOp())) {
    if (auto bn_op = llvm::dyn_cast<oneflow::NormalizationInferenceOp>(bn_result.getDefiningOp())) {
      auto ctx = rewriter.getContext();
      SmallVector<Value, 4> final_results;
      NamedAttrList attributes = conv_op->getAttrs();

      attributes.set("operand_segment_sizes", rewriter.getI32VectorAttr({1, 1, 1, 0}));

      SmallVector<Value, 4> operands;
      operands.push_back(conv_op.in());

      // deal with weight
      auto add_op_attrs = GetUserOpCommonAttrs(ctx, "scalar_add");
      add_op_attrs.set("has_float_operand", BoolAttr::get(ctx, true));
      add_op_attrs.set("float_operand", bn_op.epsilonAttr());
      auto add_op = rewriter.create<oneflow::ScalarAddOp>(
          conv_op->getLoc(), conv_op->getResultTypes(),
          SmallVector<Value, 4>({bn_op.moving_variance()}), add_op_attrs);

      auto sqrt_op = rewriter.create<oneflow::SqrtOp>(conv_op->getLoc(), conv_op->getResultTypes(),
                                                      SmallVector<Value, 4>({add_op.out()}),
                                                      GetUserOpCommonAttrs(ctx, "sqrt"));

      auto div_op = rewriter.create<oneflow::BroadcastDivOp>(
          conv_op->getLoc(), conv_op->getResultTypes(),
          SmallVector<Value, 4>({bn_op.gamma(), sqrt_op.y()}), GetUserOpCommonAttrs(ctx, "div"));

      auto bn_gamma_variable_op =
          llvm::dyn_cast<oneflow::FrozenVariableOp>(bn_op.gamma().getDefiningOp());
      if (!bn_gamma_variable_op) {
        emitError(conv_op.getLoc()) << "Gamma of batchnorm should be a FrozenVariableOp.";
      }
      auto bn_gamma_shape =
          bn_gamma_variable_op.value().getType().cast<mlir::RankedTensorType>().getShape();

      auto conv_weight_variable_op =
          llvm::dyn_cast<oneflow::FrozenVariableOp>(conv_op.weight().getDefiningOp());
      if (!conv_weight_variable_op) {
        emitError(conv_op.getLoc()) << "Weight of conv2d should be a FrozenVariableOp.";
      }
      auto conv_weight_shape =
          conv_weight_variable_op.value().getType().cast<mlir::RankedTensorType>().getShape();

      std::vector<int64_t> bn_gamma_new_shape({bn_gamma_shape.front()});
      for (int i = 1; i < conv_weight_shape.size(); ++i) { bn_gamma_new_shape.emplace_back(1); }
      auto reshape_op_attrs = GetUserOpCommonAttrs(ctx, "reshape");
      reshape_op_attrs.set("shape", ArrayAttr::get(ctx, llvm::to_vector<8>(llvm::map_range(
                                                            ArrayRef<int64_t>(bn_gamma_new_shape),
                                                            [&](int64_t v) -> Attribute {
                                                              return rewriter.getI64IntegerAttr(v);
                                                            }))));
      auto reshape_op = rewriter.create<oneflow::ReshapeOp>(
          conv_op->getLoc(), conv_op->getResultTypes(), SmallVector<Value, 4>({div_op.z()}),
          reshape_op_attrs);

      auto mul_op = rewriter.create<oneflow::BroadcastMulOp>(
          conv_op->getLoc(), conv_op->getResultTypes(),
          SmallVector<Value, 4>({conv_op.weight(), reshape_op.out()}),
          GetUserOpCommonAttrs(ctx, "multiply"));
      operands.push_back(mul_op.z());

      // deal with bias
      if (!conv_op.bias()) {
        auto mul_op_bias = rewriter.create<oneflow::BroadcastMulOp>(
            conv_op->getLoc(), conv_op->getResultTypes(),
            SmallVector<Value, 4>({bn_op.moving_mean(), div_op.z()}),
            GetUserOpCommonAttrs(ctx, "multiply_bias"));
        auto sub_op_bias = rewriter.create<oneflow::BroadcastSubOp>(
            conv_op->getLoc(), conv_op->getResultTypes(),
            SmallVector<Value, 4>({bn_op.beta(), mul_op_bias.z()}),
            GetUserOpCommonAttrs(ctx, "sub_bias"));
        operands.push_back(sub_op_bias.z());
      } else {
        emitError(conv_op.getLoc())
            << "Fusing conv2d and batch_norm only supports conv2d without bias now.";
      }
      if (conv_op.bias_multiplier()) operands.push_back(conv_op.bias_multiplier());

      auto new_conv_op = rewriter.create<oneflow::Conv2DOp>(
          conv_op->getLoc(), conv_op->getResultTypes(), operands, attributes);

      final_results.push_back(new_conv_op.out());
      return final_results;
    }
  }
  return {};
}

::llvm::SmallVector<::mlir::Value, 4> CreateFusedBiasAddMaskScale(::mlir::PatternRewriter& rewriter,
                                                                  OpResult dropout_result,
                                                                  OpResult bias_add_result,
                                                                  Operation* mask) {
  if (auto dropout_op = llvm::dyn_cast<oneflow::DropoutOp>(dropout_result.getDefiningOp())) {
    if (auto bias_add_op = llvm::dyn_cast<oneflow::BiasAddOp>(bias_add_result.getDefiningOp())) {
      SmallVector<Value, 4> operands;
      operands.push_back(bias_add_op.a());
      operands.push_back(bias_add_op.b());
      operands.push_back(mask->getResults()[0]);
      NamedAttrList fused_bias_add_dropout_attributes = dropout_op->getAttrs();
      fused_bias_add_dropout_attributes.append(llvm::StringRef("axis"), bias_add_op.axisAttr());
      fused_bias_add_dropout_attributes.append(llvm::StringRef("scale"), dropout_op.rateAttr());
      fused_bias_add_dropout_attributes.erase(dropout_op.rateAttrName());
      auto res = rewriter
                     .create<oneflow::FusedBiasAddMaskScaleOp>(
                         dropout_op->getLoc(), dropout_op->getResultTypes().front(), operands,
                         fused_bias_add_dropout_attributes)
                     ->getResults();
      // bias_add and dropout op is expected to be erased if it is not used
      return res;
    }
  }
  return {};
}

struct ReplaceVariablePattern : public ::mlir::RewritePattern {
  explicit ReplaceVariablePattern(::mlir::MLIRContext* context)
      : ::mlir::RewritePattern("oneflow.variable", 1, context, {"oneflow.variable_ir"}) {}
  ::mlir::LogicalResult matchAndRewrite(::mlir::Operation* op0,
                                        ::mlir::PatternRewriter& rewriter) const override {
    auto op = ::llvm::dyn_cast<oneflow::VariableOp>(op0);
    if (!op) return failure();
    NamedAttrList attrs;
    if (op.op_name().str().find("FreeEagerTensor") != std::string::npos) { return failure(); }
    attrs.set(StringAttr::get(getContext(), "value"),
              support::TensorToDenseElementsAttr(
                  CHECK_JUST(::oneflow::Singleton<::oneflow::VariableTensorMgr>::Get()->Get(
                      op.op_name().str(), ::oneflow::DType::Float())),
                  rewriter.getContext()));
    attrs.set(op.op_nameAttrName(), op.op_nameAttr());
    attrs.set(op.data_typeAttrName(), op.data_typeAttr());
    attrs.set(op.device_tagAttrName(), op.device_tagAttr());
    attrs.set(op.device_nameAttrName(), op.device_nameAttr());
    attrs.set(op.scope_symbol_idAttrName(), op.scope_symbol_idAttr());
    attrs.set(op.hierarchyAttrName(), op.hierarchyAttr());
    auto name = FrozenVariableOp::nd_sbpAttrName(
        OperationName(FrozenVariableOp::getOperationName(), rewriter.getContext()));

    auto parallel_attr = op.parallelAttr();
    attrs.set(name, SBPTranslation::ConvertSBPToString(rewriter, parallel_attr));
    auto op_new = rewriter.create<oneflow::FrozenVariableOp>(op->getLoc(), op.output().getType(),
                                                             ValueRange(), attrs);
    rewriter.replaceOp(op0, op_new->getResults());
    return ::mlir::success();
  }
};

struct ReplaceVariableIrPattern : public ::mlir::RewritePattern {
  explicit ReplaceVariableIrPattern(::mlir::MLIRContext* context)
      : ::mlir::RewritePattern("oneflow.variable_ir", 1, context, {"oneflow.variable"}) {}
  ::mlir::LogicalResult matchAndRewrite(::mlir::Operation* op0,
                                        ::mlir::PatternRewriter& rewriter) const override {
    auto op = ::llvm::dyn_cast<oneflow::FrozenVariableOp>(op0);
    if (!op) return failure();
    NamedAttrList attrs;
    const auto tensor_attr = op.value();
    attrs.set(StringAttr::get(getContext(), "shape"),
              rewriter.getArrayAttr(llvm::to_vector<8>(llvm::map_range(
                  tensor_attr.getType().cast<mlir::RankedTensorType>().getShape(),
                  [&](int64_t v) -> Attribute {
                    return IntegerAttr::get(rewriter.getIntegerType(64, /*isSigned=*/true),
                                            APInt(64, v, /*isSigned=*/true));
                  }))));
    attrs.set(StringAttr::get(getContext(), "data_type"),
              oneflow::DataTypeAttr::get(getContext(), oneflow::DataType::DT_Float));
    auto output_lbns_attr = rewriter.getStrArrayAttr({op.op_name().str() + "/out"});
    attrs.set(OpTrait::IsImportCompatible<void>::getOutputLBNsAttr(), output_lbns_attr);
    attrs.set(op.op_nameAttrName(), op.op_nameAttr());
    attrs.set(op.data_typeAttrName(), op.data_typeAttr());
    attrs.set(op.device_tagAttrName(), op.device_tagAttr());
    attrs.set(op.device_nameAttrName(), op.device_nameAttr());
    attrs.set(op.scope_symbol_idAttrName(), op.scope_symbol_idAttr());
    attrs.set(op.hierarchyAttrName(), op.hierarchyAttr());
    auto name = VariableOp::parallelAttrName(
        OperationName(VariableOp::getOperationName(), rewriter.getContext()));

    auto nd_size = op.hierarchy()->size();
    ArrayAttr nd_sbp = op.nd_sbp();
    std::vector<std::string> nd_sbp_str;
    std::for_each(nd_sbp.begin(), nd_sbp.end(), [&](Attribute elem) {
      if (auto sbp_str_attr = elem.dyn_cast<StringAttr>()) {
        nd_sbp_str.push_back(sbp_str_attr.str());
      }
    });
    attrs.set(name, SBPTranslation::ConvertNdSbpToPsig(rewriter, nd_sbp_str, nd_size));
    auto op_new = rewriter.create<oneflow::VariableOp>(op->getLoc(), op.output().getType(),
                                                       ValueRange(), attrs);
    const std::string tensor_name = op.op_nameAttr().str();
    const auto data_type = support::FromMLIRAttrToOFDataType(op.data_typeAttr());
    if (failed(data_type)) {
      op0->emitError(::llvm::formatv("unsupported data type: {0}",
                                     ConvertToString(op.data_typeAttr().getValue())));
      return ::mlir::failure();
    }
    CHECK_JUST(::oneflow::Singleton<::oneflow::VariableTensorMgr>::Get()->Set(
        tensor_name,  // tensor_name can't be replaced by op.op_nameAttr().str() directly when
                      // compiling with gcc and I has no idea why.
                      // But it works when compiling with clang.
                      // Maybe temporary objects would be released earlier when using gcc.
        support::DenseElementsAttrToTensor(tensor_attr, op.device_tagAttr(), op.device_nameAttr()),
        CHECK_JUST(::oneflow::DType::Get(data_type.getValue()))));
    // replaceOp may deallocate `op0` (and also `op`), so we should not use `op` after this call.
    rewriter.replaceOp(op0, op_new->getResults());
    return ::mlir::success();
  }
};

mlir::IntegerAttr GetDefaultSeed(::mlir::PatternRewriter& rewriter) {
  const auto gen = CHECK_JUST(::oneflow::one::DefaultAutoGenerator());
  return getSI64IntegerAttr(rewriter, (int64_t)gen->current_seed());
}

LogicalResult InitTransposeAttributes(Operation* op, NamedAttrList& transpose_attributes,
                                      PatternRewriter& rewriter) {
  if (op->hasTrait<OpTrait::IsOpConfCompatible>()) {
    return OpTrait::IsOpConfCompatible<void>::saveToNamedAttrList(op, transpose_attributes);
  } else {
    op->emitError("must be a op of trait IsOpConfCompatible!");
    return failure();
  }
}

bool IsAddToOutputNone(ValueRange value) { return (int)value.size() > 0 ? false : true; }

llvm::SmallVector<int32_t> getChannelLastTransposePerm() { return {0, 2, 3, 1}; }

llvm::SmallVector<int32_t> getChannelFirstTransposePerm() { return {0, 3, 1, 2}; }

llvm::SmallVector<mlir::Value, 4> getInputOperandTransposeOp(NCHWCompatible op, Value val,
                                                             NamedAttrList transpose_attributes,
                                                             int num_transposed_operand,
                                                             PatternRewriter& rewriter) {
  std::string transpose_name = OpTrait::IsOpConfCompatible<void>::getOpName(op).str()
                               + "_transpose_input_" + std::to_string(num_transposed_operand);
  transpose_attributes.set(llvm::StringRef(OpTrait::IsOpConfCompatible<void>::getOpNameAttr()),
                           rewriter.getStringAttr(transpose_name));
  SmallVector<Value, 4> input_operands;
  input_operands.push_back(val);
  auto res = rewriter
                 .create<oneflow::TransposeOp>(op.getLoc(), getNHWCType(val.getType()),
                                               input_operands, transpose_attributes)
                 ->getResults();
  return res;
}

TransposeOp getResultTransposeOp(NCHWCompatible op, Value val, NamedAttrList transpose_attributes,
                                 int num_transposed_result, PatternRewriter& rewriter) {
  std::string transpose_name = OpTrait::IsOpConfCompatible<void>::getOpName(op).str()
                               + "_transpose_output_" + std::to_string(num_transposed_result);
  transpose_attributes.set(llvm::StringRef(OpTrait::IsOpConfCompatible<void>::getOpNameAttr()),
                           rewriter.getStringAttr(transpose_name));
  SmallVector<Value, 4> operands;
  operands.push_back(val);
  TransposeOp transpose_op = rewriter.create<oneflow::TransposeOp>(
      op.getLoc(), getNCHWType(val.getType()), operands, transpose_attributes);
  return transpose_op;
}

bool IsInsertTransposeOpBefore(NCHWCompatible op, PatternRewriter& rewriter) {
  bool insert_transpose_op_flag = false;
  for (mlir::Value operand : op->getOperands()) {
    TransposeOp transposeInputOp = operand.getDefiningOp<TransposeOp>();
    if (!transposeInputOp) continue;
    const auto perm = transposeInputOp.permAttr();
    if (perm.size() == 4 && perm[0] == rewriter.getSI32IntegerAttr(0)
        && perm[1] == rewriter.getSI32IntegerAttr(3) && perm[2] == rewriter.getSI32IntegerAttr(1)
        && perm[3] == rewriter.getSI32IntegerAttr(2)) {
      insert_transpose_op_flag = true;
      break;
    }
  }
  return insert_transpose_op_flag;
}

bool IsSameDtype(mlir::OpResult cast_result, mlir::Value input) {
  return cast_result.getType() == input.getType();
}

}  // namespace oneflow

}  // namespace mlir

#include "OneFlow/OneFlowPatterns.cpp.inc"

namespace mlir {

namespace oneflow {

template<typename Op>
struct FusedConsecutiveAddPattern : public OpRewritePattern<Op> {
  explicit FusedConsecutiveAddPattern(mlir::MLIRContext* context)
      : OpRewritePattern<Op>(context, /*benefit=*/1) {}

 public:
  LogicalResult matchAndRewrite(Op op, PatternRewriter& rewriter) const override;
};

template<typename Op>
LogicalResult TryFusedConsecutiveAdd(Op op, const SmallVector<mlir::Value, 4>& opOperands,
                                     PatternRewriter& rewriter) {
  for (mlir::Value operand : opOperands) {
    if (!operand.getDefiningOp<AddNOp>() && !operand.getDefiningOp<Add2Op>()) { continue; }
    // check if the operand has only one user
    LogicalResult checkResult = [&]() {
      for (const auto& use : operand.getUses()) {
        if (use.getOwner() != op) { return failure(); }
      }
      return success();
    }();
    if (failed(checkResult)) { continue; }

    SmallVector<mlir::Value, 4> operands;
    SmallVector<mlir::Value, 4> inputOpOperands;
    mlir::Value inputOpResult;
    if (AddNOp addInputOp = operand.getDefiningOp<AddNOp>()) {
      inputOpOperands = addInputOp.in();
      inputOpResult = addInputOp.out();
    } else if (Add2Op addInputOp = operand.getDefiningOp<Add2Op>()) {
      inputOpOperands = {addInputOp.in0(), addInputOp.in1()};
      inputOpResult = addInputOp.out();
    }
    for (mlir::Value operand : opOperands) {
      if (operand != inputOpResult) {
        operands.push_back(operand);
      } else {
        operands.insert(operands.end(), inputOpOperands.begin(), inputOpOperands.end());
      }
    }
    auto new_op =
        rewriter.create<AddNOp>(op->getLoc(), op->getResultTypes(), operands, op->getAttrs());
    rewriter.replaceOp(op, new_op.out());
    return success();
  }
  return failure();
}

template<>
LogicalResult FusedConsecutiveAddPattern<AddNOp>::matchAndRewrite(AddNOp op,
                                                                  PatternRewriter& rewriter) const {
  return TryFusedConsecutiveAdd<AddNOp>(op, op.in(), rewriter);
}

template<>
LogicalResult FusedConsecutiveAddPattern<Add2Op>::matchAndRewrite(Add2Op op,
                                                                  PatternRewriter& rewriter) const {
  return TryFusedConsecutiveAdd<Add2Op>(op, {op.in0(), op.in1()}, rewriter);
}

struct AutoNhwcPattern : public OpInterfaceRewritePattern<NCHWCompatible> {
  explicit AutoNhwcPattern(mlir::MLIRContext* context)
      : OpInterfaceRewritePattern<NCHWCompatible>(context, /*benefit=*/1) {}

 public:
  LogicalResult matchAndRewrite(NCHWCompatible op, PatternRewriter& rewriter) const override {
    if (op->hasTrait<OpTrait::IsOpConfCompatible>()) {
      for (mlir::Value operand : op.OperandsToTranspose()) {
        if (operand.getType().cast<mlir::RankedTensorType>().getShape().size() != 4) {
          return failure();
        }
      }
      const auto device_name = OpTrait::IsOpConfCompatible<void>::getDeviceTag(op)
                                   .cast<mlir::StringAttr>()
                                   .getValue()
                                   .str();
      if (device_name == "cpu") { return failure(); }
    }
    llvm::SmallVector<int32_t> perm = getChannelLastTransposePerm();
    llvm::SmallVector<int32_t> result_perm = getChannelFirstTransposePerm();

    NamedAttrList transpose_attributes;
    if (InitTransposeAttributes(op, transpose_attributes, rewriter).succeeded()) {
      transpose_attributes.append(llvm::StringRef("perm"), getSI32ArrayAttr(rewriter, perm));
    } else {
      return failure();
    }
    // when op op has no sense of data_format and pre op is transpose, we greedily insert transpose
    // into this op, seeking more opportunities to eliminate transpose pattern.
    const bool greedily_transpose_flag = !op.IsNCHW() && IsInsertTransposeOpBefore(op, rewriter);

    if (op.IsNCHW() || greedily_transpose_flag) {
      // create transpose op for input operand
      SmallVector<Value, 4> tranposed_operands;
      llvm::DenseSet<Value> operand_transpose = op.OperandsToTranspose();
      int num_transposed_operand = 0;
      for (Value operand : op->getOperands()) {
        if (operand_transpose.find(operand) != operand_transpose.end()) {
          SmallVector<Value, 4> input_res = getInputOperandTransposeOp(
              op, operand, transpose_attributes, num_transposed_operand, rewriter);
          tranposed_operands.push_back(input_res[0]);
          num_transposed_operand += 1;
        }
      }
      // create NHWC op
      SmallVector<Value, 4> created_results = op.NchwToNhwc(tranposed_operands, rewriter);
      // create transpose op for results
      int num_transposed_result = 0;
      transpose_attributes.set(llvm::StringRef("perm"), getSI32ArrayAttr(rewriter, result_perm));
      llvm::DenseSet<Value> transpose_result = op.ResultsToTranspose();

      for (Value result : op->getOpResults()) {
        if (transpose_result.find(result) != transpose_result.end()) {
          if (auto result_transpose_op =
                  getResultTransposeOp(op, created_results[num_transposed_result],
                                       transpose_attributes, num_transposed_result, rewriter)) {
            result.replaceAllUsesWith(result_transpose_op);
            num_transposed_result += 1;
          } else {
            return failure();
          }
        }
      }
    }
    return success();
  }
};

bool IsRedundantTransposeMatch(ArrayAttr pre, ArrayAttr afe, mlir::PatternRewriter& rewriter) {
  const auto prePerm = pre.getValue().vec();
  const auto afePerm = afe.getValue().vec();
  if (prePerm.size() == 4 && afePerm.size() == 4) {
    // handle nchw->nhwc->nchw: (0, 2, 3, 1) -> (0, 3, 1, 2)
    if (prePerm[0] == afePerm[0] && prePerm[1] == afePerm[3] && prePerm[2] == afePerm[1]
        && prePerm[3] == afePerm[2] && prePerm[0] == rewriter.getSI32IntegerAttr(0)
        && prePerm[1] == rewriter.getSI32IntegerAttr(2)
        && prePerm[2] == rewriter.getSI32IntegerAttr(3)
        && prePerm[3] == rewriter.getSI32IntegerAttr(1))
      return true;
    // handle nhwc->nchw->nhwc: (0, 3, 1, 2) -> (0, 2, 3, 1)
    if (prePerm[0] == afePerm[0] && prePerm[1] == afePerm[2] && prePerm[2] == afePerm[3]
        && prePerm[3] == afePerm[1] && prePerm[0] == rewriter.getSI32IntegerAttr(0)
        && prePerm[1] == rewriter.getSI32IntegerAttr(3)
        && prePerm[2] == rewriter.getSI32IntegerAttr(1)
        && prePerm[3] == rewriter.getSI32IntegerAttr(2))
      return true;
  }
  return false;
}

struct AutoNhwcEliminateRedundantTransposePattern : public mlir::OpRewritePattern<TransposeOp> {
  explicit AutoNhwcEliminateRedundantTransposePattern(mlir::MLIRContext* context)
      : OpRewritePattern<TransposeOp>(context, /*benefit=*/1) {}
  mlir::LogicalResult matchAndRewrite(TransposeOp op,
                                      mlir::PatternRewriter& rewriter) const override {
    mlir::Value transposeInput = op.getOperand();
    TransposeOp transposeInputOp = transposeInput.getDefiningOp<TransposeOp>();

    if (!transposeInputOp
        || !IsRedundantTransposeMatch(op.permAttr(), transposeInputOp.permAttr(), rewriter)) {
      return failure();
    }
    rewriter.replaceOp(op, {transposeInputOp.getOperand()});
    return success();
  }
};

void BroadcastMulOp::getCanonicalizationPatterns(RewritePatternSet& results, MLIRContext* context) {
  results.insert<BroadcastMulToScalarMulPattern>(context);
}

struct ConvertOFKLCalleeToLLVMPattern : public mlir::OpRewritePattern<func::FuncOp> {
  explicit ConvertOFKLCalleeToLLVMPattern(mlir::MLIRContext* context)
      : OpRewritePattern<func::FuncOp>(context, /*benefit=*/0) {}
  mlir::LogicalResult matchAndRewrite(func::FuncOp op,
                                      mlir::PatternRewriter& rewriter) const override {
    GetOrInsertKernelLLVMFuncOp(rewriter, op);
    op->remove();
    return success();
  }
};

struct KernelLaunchPattern : public RewritePattern {
  explicit KernelLaunchPattern(::mlir::MLIRContext* context)
      : RewritePattern(MatchAnyOpTypeTag(), 0, context){};

  LogicalResult matchAndRewrite(Operation* op, ::mlir::PatternRewriter& rewriter) const override {
    auto op_name = op->getName().getStringRef();
    std::vector<StringRef> white_list{
        KernelLaunchOp::getOperationName(),
        OutputOp::getOperationName(),
        InputOp::getOperationName(),
        VariableOp::getOperationName(),
    };
    if (std::count(white_list.begin(), white_list.end(), op_name) || !op->getAttr("op_name")
        || !GetModuleOpFromJobBodyOp(op)) {
      return success();
    }
    SmallString<16> tempBuffer;
    op_name = SanitizeIdentifier(op->getAttr("op_name").cast<StringAttr>(), tempBuffer);
    op->setAttr("op_name", rewriter.getStringAttr(op_name));

    ValueRange in = op->getOperands();
    NamedAttrList attrs = op->getAttrs();
    auto func = GetOrInsertKernelOFFuncOp(rewriter, op);
    auto new_op = rewriter.replaceOpWithNewOp<KernelLaunchOp>(op, func, attrs, in);
    if (failed(DumpAssembly(rewriter, new_op))) { exit(1); }
    return success();
  }
};

void AddLowerToLinalgMemRefPasses(PassManager& pm) {
  pm.addPass(createConvertToSignlessForTosaPass());  // convert-to-signless-for-tosa
  pm.addNestedPass<func::FuncOp>(LLVM::createRequestCWrappersPass());  // llvm-request-c-wrappers
  pm.addPass(createConvertToSignlessForTosaPass());  // convert-to-signless-for-tosa
  pm.addPass(createLowerOneFlowToTosaPass());        // lower-oneflow-to-tosa
  pm.addNestedPass<func::FuncOp>(
      tosa::createTosaMakeBroadcastablePass());                // tosa-make-broadcastable
  pm.addPass(createCSEPass());                                 // cse
  pm.addNestedPass<func::FuncOp>(tosa::createTosaToLinalg());  // tosa-to-linalg-on-tensors
  pm.addNestedPass<func::FuncOp>(
      createLinalgElementwiseOpFusionPass());                       // linalg-fuse-elementwise-ops
  pm.addNestedPass<func::FuncOp>(createLinalgBufferizePass());      // linalg-bufferize
  pm.addNestedPass<func::FuncOp>(createTensorBufferizePass());      // tensor-bufferize
  pm.addPass(func::createFuncBufferizePass());                      // func-bufferize
  pm.addPass(bufferization::createBufferResultsToOutParamsPass());  // buffer-results-to-out-params
  pm.addPass(createCanonicalizerPass());                            // canonicalize
  pm.addNestedPass<func::FuncOp>(
      mlir::bufferization::createFinalizingBufferizePass());  // finalizing-bufferize
}

LogicalResult LowerKernelLaunchModuleToLLVM(ModuleOp module) {
  mlir::PassManager pm(module->getContext());
  pm.addPass(createConvertOFKLCalleeToLLVMPass());   // convert-ofkl-callee-to-llvm
  pm.addPass(createConvertFuncToLLVMPass());         // convert-func-to-llvm
  pm.addPass(createReconcileUnrealizedCastsPass());  // reconcile-unrealized-casts
  return pm.run(module);
}

LogicalResult LowerModuleToLLVM(mlir::MLIRContext* context, ModuleOp module) {
  mlir::PassManager pm(context);
  AddLowerToLinalgMemRefPasses(pm);
  pm.addNestedPass<func::FuncOp>(createConvertLinalgToLoopsPass());  // convert-linalg-to-loops
  pm.addNestedPass<func::FuncOp>(createConvertSCFToCFPass());        // convert-scf-to-cf
  pm.addPass(createConvertLinalgToLLVMPass());                       // convert-linalg-to-llvm
  pm.addPass(createMemRefToLLVMPass());                              // convert-memref-to-llvm
  pm.addPass(createConvertFuncToLLVMPass());                         // convert-func-to-llvm
  pm.addPass(createReconcileUnrealizedCastsPass());                  // reconcile-unrealized-casts
  return pm.run(module);
}

#ifdef WITH_MLIR_CUDA_CODEGEN

LogicalResult LowerModuleToCUDALLVM(mlir::MLIRContext* context, ModuleOp module) {
  InitializeLLVMNVPTXBackend();
  mlir::PassManager pm(context);
  mlir::oneflow::CheckEnableIRPrinting(pm);
  AddLowerToLinalgMemRefPasses(pm);
  pm.addNestedPass<func::FuncOp>(
      createConvertLinalgToParallelLoopsPass());  // convert-linalg-to-parallel-loops
  pm.addNestedPass<func::FuncOp>(createGpuMapParallelLoopsPass());  // gpu-map-parallel-loops
  pm.addPass(createParallelLoopToGpuPass());                        // convert-parallel-loops-to-gpu
  pm.addPass(createGpuLauchSinkIndexComputationsPass());
  pm.addPass(createGpuKernelOutliningPass());                      // gpu-kernel-outlining
  pm.addNestedPass<func::FuncOp>(createBufferHostRegisterPass());  // buffer-host-register
  pm.addPass(createCanonicalizerPass());                           // canonicalize
  // -pass-pipeline='gpu.module([PASS1][PASS2]...)'
  pm.addNestedPass<gpu::GPUModuleOp>(createStripDebugInfoPass());        // strip-debuginfo
  pm.addNestedPass<gpu::GPUModuleOp>(createLowerAffinePass());           // lower-affine
  pm.addNestedPass<gpu::GPUModuleOp>(createLowerGpuOpsToNVVMOpsPass());  // convert-gpu-to-nvvm
  pm.addNestedPass<gpu::GPUModuleOp>(createSerializeToCubinPass());      // out-of-tree-gpu-to-cubin
  pm.addNestedPass<func::FuncOp>(createGpuCopyArgPass());                // buffer-host-register
  pm.addPass(createGpuToLLVMConversionPass());
  pm.addPass(createReconcileUnrealizedCastsPass());  // reconcile-unrealized-casts
  return pm.run(module);
}

#endif  // WITH_MLIR_CUDA_CODEGEN

void populateFuserPasses(::mlir::RewritePatternSet& patterns) {
  patterns.add<MulCastPattern>(patterns.getContext());
}
void populateConvertOFKLCalleeToLLVMPasses(::mlir::RewritePatternSet& patterns) {
  patterns.add<ConvertOFKLCalleeToLLVMPattern>(patterns.getContext());
}

void populateKernelWrapperPasses(::mlir::RewritePatternSet& patterns) {
  patterns.add<KernelLaunchPattern>(patterns.getContext());
}
void populateFuserForExistingOp(::mlir::RewritePatternSet& patterns) {
  patterns.add<FusedBiasAddGeluPattern>(patterns.getContext());
  patterns.add<FusedScaleTrilPattern>(patterns.getContext());
  patterns.add<FusedScaleTrilPattern2>(patterns.getContext());
  patterns.add<FusedPadConv2DPattern>(patterns.getContext());
  patterns.add<FusedBiasAddDropoutPattern>(patterns.getContext());
  patterns.add<NormalizationAddReluPattern>(patterns.getContext());
  patterns.add<DeleteSameDtypeCastOpPattern>(patterns.getContext());
  patterns.add<FusedConsecutiveAddPattern<Add2Op>>(patterns.getContext());
  patterns.add<FusedConsecutiveAddPattern<AddNOp>>(patterns.getContext());
}

void populateAutoNhwcPatterns(::mlir::RewritePatternSet& patterns) {
  bool enable_nhwc = ::oneflow::ParseBooleanFromEnv("ONEFLOW_MLIR_PREFER_NHWC", false);
  if (enable_nhwc) {
    patterns.add<AutoNhwcPattern>(patterns.getContext());
    patterns.add<AutoNhwcEliminateRedundantTransposePattern>(patterns.getContext());
  }
}

void populateGpuHelperPatterns(::mlir::RewritePatternSet& patterns) {
  patterns.add<ReplaceCopyWithGPUPattern>(patterns.getContext());
}

void populatePreConvertInferenceOp(::mlir::RewritePatternSet& patterns) {
  patterns.add<ReplaceVariablePattern>(patterns.getContext());
}

void populateConvertInferenceOp(::mlir::RewritePatternSet& patterns) {
  patterns.add<FuseConv2DBatchNormPattern>(patterns.getContext());
}

void populatePostConvertInferenceOp(::mlir::RewritePatternSet& patterns) {
  patterns.add<ReplaceVariableIrPattern>(patterns.getContext());
}

}  // namespace oneflow

}  // namespace mlir
