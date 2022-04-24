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
#include "oneflow/core/job_rewriter/job_pass.h"
#include "oneflow/core/framework/framework.h"

namespace oneflow {

namespace {

std::function<bool(const OpNode* op_node)> MakePredicatorIsSafeToDelete(const OpGraph& op_graph) {
  HashSet<std::string> ctrl_in_op_names;
  op_graph.ForEachNode([&](const OpNode* op_node) {
    for (const std::string& ctrl_in_op_name : op_node->op().op_conf().ctrl_in_op_name()) {
      ctrl_in_op_names.insert(ctrl_in_op_name);
    }
  });
  return [=](const OpNode* op_node) {
    if (op_node->out_edges().size() > 1) { return false; }
    if (!op_node->op().op_conf().ctrl_in_op_name().empty()) { return false; }
    if (ctrl_in_op_names.find(op_node->op().op_conf().name()) != ctrl_in_op_names.end()) {
      return false;
    }
    return true;
  };
}

bool IsUserOpWithTypeName(const OperatorConf& op_conf, const std::string& op_type_name) {
  return op_conf.has_user_conf() && op_conf.user_conf().op_type_name() == op_type_name;
};

class FuseUpdateCastOpsPass final : public JobPass {
 public:
  FuseUpdateCastOpsPass() = default;
  ~FuseUpdateCastOpsPass() override = default;

  bool IsEnabled(const JobPassCtx& ctx) const {
    return ctx.job_desc().job_conf().enable_fuse_model_update_ops();
  }
  Maybe<void> Apply(const OpGraph& op_graph, JobBuilder* job_builder) const;

  Maybe<void> Apply(Job* job, JobPassCtx* ctx) const override {
    if (!IsEnabled(*ctx)) { return Maybe<void>::Ok(); }
    const OpGraph op_graph(*job);
    JobBuilder job_builder(job);
    return Apply(op_graph, &job_builder);
  }
};

Maybe<void> FuseUpdateCastOpsPass::Apply(const OpGraph& op_graph, JobBuilder* job_builder) const {
  const auto IsSafeToDelete = MakePredicatorIsSafeToDelete(op_graph);
  std::vector<OperatorConf> delete_ops;
  
  op_graph.ForEachNode([&](OpNode* op_node) {
    const auto& op_conf = op_node->op().op_conf();
    if (!op_conf.has_variable_conf()) { return; }
    int32_t out_node_size = op_node->out_edges().size();
    for (OpEdge* edge : node->out_edges()){
        bool if_find_cast = false; 
        OpNode* find_cast_node = edge->dst_node(); 
        if (!IsUserOpWithTypeName(find_cast_node.op().op_conf(), "cast")) { continue; }
        if_find_cast = true; 
        const user_op::UserOpConfWrapper cast_user_conf(find_cast_node->op().op_conf());
        if (find_cast_node->LogicalBlobDesc4Lbi(GenLogicalBlobId(cast_user_conf.input("in", 0))).data_type()
            != DataType::kFloat16) {
            continue;
        }
        if (find_cast_node->LogicalBlobDesc4Lbi(GenLogicalBlobId(cast_user_conf.output("out", 0))).data_type()
            != DataType::kFloat) {
            continue;
        }
        // Currently only support for cuda, maybe remove this limit. 
        if (find_cast_node->parallel_desc().device_type() != DeviceType::kCUDA) { continue; }

        user_op::UserOpConfWrapperBuilder fused_cast_op_builder(cast_user_conf.op_name());
        fused_cast_op_builder.OpTypeName("optim_fuse_cast")
            .Input("in", cast_user_conf.input("in", 0))
            .Attr<DataType>("dtype", cast_user_conf.attr<DataType>("dtype"))
            .Output("out");
        
        

        CHECK(cast_user_conf.op_conf().has_scope_symbol_id());
        fused_cast_op_builder.ScopeSymbolId(cast_user_conf.op_conf().scope_symbol_id());

        OperatorConf new_op_conf = cast_user_conf.op_conf();
        *new_cast_op_conf.mutable_user_conf() = fused_cast_op_builder.Build().op_conf().user_conf();
        job_builder->MutOpsOnlyOnce({new_cast_op_conf});

        LogicalBlobId model_half_lbi = GenLogicalBlobId(new_cast_op_conf.output("out", 0));

        if(if_find_cast){
            for (OpEdge* edge : node->out_edges()){
                OpNode* find_sgd_update_node = edge->dst_node(); 
                if (!IsUserOpWithTypeName(find_sgd_update_node.op().op_conf(), "sgd_update")) { continue; }
                const user_op::UserOpConfWrapper sgd_user_conf(find_sgd_update_node->op().op_conf());
                // Currently only support for cuda, maybe remove this limit. 
                if (op_node->parallel_desc().device_type() != DeviceType::kCUDA) { continue; }

                user_op::UserOpConfWrapperBuilder fused_sgd_op_builder(sgd_user_conf.op_name());
                fused_sgd_op_builder.OpTypeName("sgd_update")
                    .Input("model", sgd_op_conf.input("model", 0))
                    .Input("model_half", GenLogicalBlobName(model_half_lbi))
                    .Input("model_diff", sgd_op_conf.input("model_diff", 0))
                    .Input("learning_rate", sgd_op_conf.input("learning_rate", 0))
                    .Attr<double>("scale", sgd_op_conf.attr<double>("scale"))
                    .Attr<float>("l1", sgd_op_conf.attr<float>("l1"))
                    .Attr<float>("l2", sgd_op_conf.attr<float>("l2"))
                    .Attr<float>("weight_decay", user_op_conf.attr<float>("weight_decay"));
                
                CHECK(sgd_user_conf.op_conf().has_scope_symbol_id());
                fused_sgd_op_builder.ScopeSymbolId(sgd_user_conf.op_conf().scope_symbol_id());

                OperatorConf new_sgd_op_conf = sgd_user_conf.op_conf();
                *new_sgd_op_conf.mutable_user_conf() = fused_sgd_op_builder.Build().op_conf().user_conf();
                job_builder->MutOpsOnlyOnce({new_sgd_op_conf});
            }
        }
    }
  });

  return Maybe<void>::Ok();
}

}  // namespace

REGISTER_JOB_PASS("FuseUpdateCastOpsPass", FuseUpdateCastOpsPass);

}  // namespace oneflow
