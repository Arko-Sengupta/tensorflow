/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <functional>
#include <stack>

#include "tensorflow/compiler/mlir/tensorflow/ir/tf_device.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_types.h"
#include "tensorflow/compiler/mlir/tensorflow/transforms/call_graph_util.h"
#include "tensorflow/compiler/mlir/tensorflow/transforms/passes.h"
#include "tensorflow/compiler/mlir/tensorflow/utils/attribute_utils.h"

inline constexpr absl::string_view kEntryFunction = "main";

namespace mlir {

namespace {

#define GEN_PASS_DEF_XLACLUSTERFORMATIONPASS
#include "tensorflow/compiler/mlir/tensorflow/transforms/tf_device_passes.h.inc"

// Outlines partitioned call ops with `_XlaMustCompile` to device clusters.
struct XlaClusterFormationPass
    : public impl::XlaClusterFormationPassBase<XlaClusterFormationPass> {
  void runOnOperation() override;
};

void EncapsulatePartitionedCall(Operation *call_op) {
  OpBuilder builder(call_op);

  auto cluster = builder.create<tf_device::ClusterOp>(
      call_op->getLoc(), call_op->getResultTypes());

  call_op->replaceAllUsesWith(cluster.getResults());

  cluster.getBody().push_back(new Block);

  call_op->moveBefore(&cluster.GetBody(), cluster.GetBody().end());

  builder.setInsertionPointToEnd(&cluster.GetBody());
  builder.create<tf_device::ReturnOp>(call_op->getLoc(), call_op->getResults());
}

void XlaClusterFormationPass::runOnOperation() {
  ModuleOp module = getOperation();
  SymbolTable symtab(module);

  func::FuncOp entry_func = symtab.lookup<func::FuncOp>(kEntryFunction);
  if (!entry_func) {
    module.emitError() << "entry function " << kEntryFunction
                       << " must be present";
    return signalPassFailure();
  }
  auto predicate = [](Operation *op) {
    if (op->hasAttr(tensorflow::kCompileDeviceTypeAttr)) return true;
    return false;
  };
  llvm::SmallVector<Operation *> outermost_call_ops;
  if (failed(GetOutermostOpsOfType<TF::StatefulPartitionedCallOp,
                                   TF::PartitionedCallOp>(
          entry_func, symtab, outermost_call_ops, predicate)))
    return signalPassFailure();
  // Cluster outermost partitioned calls with _xla_compile_device_type
  // attribute.
  for (auto &call_op : outermost_call_ops) {
    EncapsulatePartitionedCall(call_op);
  }
}

}  // namespace

namespace TFDevice {
std::unique_ptr<OperationPass<ModuleOp>> CreateXlaClusterFormationPass() {
  return std::make_unique<XlaClusterFormationPass>();
}
}  // namespace TFDevice

}  // namespace mlir
