/* Copyright 2024 The OpenXLA Authors.

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

#include "xla/service/gpu/fusions/concatenate_mlir.h"

#include <cstdint>
#include <iterator>
#include <optional>

#include "absl/algorithm/container.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/Dialect/Tensor/IR/Tensor.h"  // from @llvm-project
#include "mlir/IR/ImplicitLocOpBuilder.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/IR/ValueRange.h"  // from @llvm-project
#include "mlir/Interfaces/DataLayoutInterfaces.h"  // from @llvm-project
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_instructions.h"
#include "xla/service/gpu/fusions/concatenate.h"
#include "xla/service/gpu/fusions/mlir/computation_partitioner.h"
#include "xla/service/gpu/fusions/mlir/elemental_hlo_to_mlir.h"
#include "xla/service/gpu/fusions/mlir/ir/xla_gpu_ops.h"
#include "xla/service/gpu/hlo_fusion_analysis.h"
#include "xla/service/gpu/launch_dimensions.h"
#include "xla/service/gpu/model/indexing_analysis.h"
#include "xla/service/gpu/model/indexing_map.h"
#include "tsl/platform/errors.h"

namespace xla {
namespace gpu {

using llvm::SmallVector;
using mlir::Value;
using mlir::ValueRange;

LaunchDimensions MlirConcatenateFusion::launch_dimensions() const {
  return CalculateLaunchDimensions(GetLargestConcatOperandShape(analysis_),
                                   analysis_.device_info());
}

std::optional<IndexingMap>
MlirConcatenateFusion::ComputeThreadIdToOutputIndexing(
    int64_t root_index, mlir::MLIRContext* ctx) const {
  return std::nullopt;
}

std::optional<IndexingMap>
MlirConcatenateFusion::ComputeThreadIdToInputIndexing(
    int64_t root_index, int64_t hero_operand_index,
    mlir::MLIRContext* ctx) const {
  // TODO(b/331356433): Add constraints depending on the `hero_operand_index`.
  return GetDefaultThreadIdToOutputIndexingMap(
      launch_dimensions(), /*unroll_factor=*/1,
      GetLargestConcatOperandShape(analysis_), ctx);
}

std::vector<const HloInstruction*>
MlirConcatenateFusion::GetInstructionsWithCustomCodegen(
    const HloFusionInstruction& fusion) const {
  return analysis_.fusion_heroes();
}

absl::Status MlirConcatenateFusion::EmitEntryFunction(
    const mlir_converter::PartitionedComputations& computations,
    const mlir_converter::CallTargetProvider& call_targets,
    mlir::func::FuncOp entry_function,
    const HloFusionInstruction& fusion) const {
  const auto& root_computation = computations.FindPartitionedComputation(
      fusion.fused_instructions_computation());
  const auto* concat = analysis_.fusion_heroes()[0];
  mlir::ImplicitLocOpBuilder builder(entry_function.getLoc(), entry_function);
  builder.setInsertionPointToStart(entry_function.addEntryBlock());
  auto* ctx = entry_function.getContext();

  int num_inputs = fusion.fused_instructions_computation()->num_parameters();
  SmallVector<Value> input_tensors(
      entry_function.getArguments().take_front(num_inputs));
  auto output_tensor_args =
      entry_function.getArguments().drop_front(num_inputs);

  SmallVector<Value> result_tensors{output_tensor_args.begin(),
                                    output_tensor_args.end()};

  auto thread_id_to_input_map =
      ComputeThreadIdToInputIndexing(
          /*root_index=*/0, /*hero_operand_index=*/0, ctx)
          .value();
  auto epilogue_indexing = ComputeEpilogueInputToOutputIndexing(concat, ctx);

  for (auto [operand_index, operand] : llvm::enumerate(concat->operands())) {
    auto input_to_output_map =
        *ComputeInputToOutputIndexing(concat, /*input_id=*/operand_index, ctx)
             .indexing_maps.front()
             .begin();
    auto thread_id_to_output_map = ComposeIndexingMaps(
        ComposeIndexingMaps(thread_id_to_input_map, input_to_output_map),
        epilogue_indexing);

    auto loop_nest_body_builder =
        [&, operand_index = operand_index](
            ValueRange output_tensors, ValueRange dim_values,
            ValueRange symbol_values) -> SmallVector<Value> {
      auto input_indices =
          mlir_converter::ApplyAffineMap(thread_id_to_input_map.GetAffineMap(),
                                         dim_values, symbol_values, builder);

      auto result_scalars = mlir_converter::ProvideParameter(
          root_computation.FindSubgraph(concat), concat, operand_index,
          input_indices, call_targets, entry_function, builder);
      auto output_indices =
          mlir_converter::ApplyAffineMap(thread_id_to_output_map.GetAffineMap(),
                                         dim_values, symbol_values, builder);
      result_scalars = EmitEpilogue(computations, entry_function,
                                    result_scalars, output_indices, builder);

      SmallVector<Value> result_tensors;
      result_tensors.reserve(output_tensor_args.size());
      for (auto [tensor, value] : llvm::zip(output_tensors, result_scalars)) {
        result_tensors.push_back(
            builder
                .create<mlir::tensor::InsertOp>(value, tensor, output_indices)
                .getResult());
      }

      return result_tensors;
    };

    result_tensors =
        EmitThreadLoopNest(builder, result_tensors, thread_id_to_output_map,
                           loop_nest_body_builder);
  }

  builder.create<mlir::func::ReturnOp>(result_tensors);

  return absl::OkStatus();
}

}  // namespace gpu
}  // namespace xla
