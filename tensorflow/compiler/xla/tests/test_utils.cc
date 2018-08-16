/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/xla/tests/test_utils.h"
#include "tensorflow/compiler/xla/literal_util.h"
#include "tensorflow/compiler/xla/primitive_util.h"
#include "tensorflow/compiler/xla/service/hlo_dataflow_analysis.h"
#include "tensorflow/compiler/xla/service/hlo_verifier.h"
#include "tensorflow/compiler/xla/service/transfer_manager.h"

namespace xla {

namespace {

template <typename FloatT, typename GeneratorT>
void PopulateWithRandomFloatingPointDataImpl(Literal* literal,
                                             std::minstd_rand0* engine) {
  CHECK(engine != nullptr);
  CHECK_EQ(literal->shape().element_type(),
           primitive_util::NativeToPrimitiveType<FloatT>());
  // Create uniform numbers between 1 and 1.125 to avoid creating denormal
  // numbers.
  std::uniform_real_distribution<GeneratorT> generator(1.0f, 1.125f);
  const bool should_index_bias = ShapeUtil::ElementsIn(literal->shape()) > 1000;
  TF_CHECK_OK(literal->Populate<FloatT>(
      [&](tensorflow::gtl::ArraySlice<int64> indices) {
        // Generate a random uniform number from -0.0625 and 0.0625 and bias it
        // with a position dependent number with mean 0.037109375. These number
        // should allow for long chains of accumulation without being too close
        // to zero or too large to accumulate all numbers accurately. Only do
        // this for large literals where the number of elements is much greater
        // than 47 otherwise only negative values are produced.
        //
        // The value is positionally biased using a product of the indices. Add
        // one to each index value to avoid collapsing to zero if any of the
        // indices are zero.
        int64 index_product = 1;
        for (int64 i : indices) {
          index_product *= (1 + i);
        }
        const int64 negative_bias = should_index_bias ? 47 : 0;
        FloatT index_bias =
            static_cast<FloatT>(index_product % 113 - negative_bias) /
            static_cast<FloatT>(256.0f);
        return static_cast<FloatT>(generator(*engine) - 1.0625f) + index_bias;
      }));
}

template <typename FloatT>
void PopulateWithRandomFloatingPointData(Literal* literal,
                                         std::minstd_rand0* engine) {
  CHECK(engine != nullptr);
  PopulateWithRandomFloatingPointDataImpl<FloatT, FloatT>(literal, engine);
}

template <>
void PopulateWithRandomFloatingPointData<half>(Literal* literal,
                                               std::minstd_rand0* engine) {
  CHECK(engine != nullptr);
  PopulateWithRandomFloatingPointDataImpl<half, float>(literal, engine);
}

// The standard library does not have a case for bfloat16, unsurprisingly, so we
// handle that one specially.
template <>
void PopulateWithRandomFloatingPointData<bfloat16>(Literal* literal,
                                                   std::minstd_rand0* engine) {
  CHECK(engine != nullptr);
  CHECK_EQ(literal->shape().element_type(), BF16);
  std::uniform_real_distribution<float> generator(-0.9f, 1.0f);
  TF_CHECK_OK(literal->Populate<bfloat16>(
      [&](tensorflow::gtl::ArraySlice<int64> /*indices*/) {
        return static_cast<bfloat16>(generator(*engine));
      }));
}

template <typename IntT>
void PopulateWithRandomIntegralData(Literal* literal,
                                    std::minstd_rand0* engine) {
  CHECK(engine != nullptr);
  CHECK_EQ(literal->shape().element_type(),
           primitive_util::NativeToPrimitiveType<IntT>());
  std::uniform_int_distribution<IntT> generator(
      std::numeric_limits<IntT>::lowest(), std::numeric_limits<IntT>::max());
  TF_CHECK_OK(literal->Populate<IntT>(
      [&](tensorflow::gtl::ArraySlice<int64> /*indices*/) {
        return generator(*engine);
      }));
}

// Similar to MakeFakeLiteral but takes a random number generator engine to
// enable reusing the engine across randomly generated literals.
StatusOr<std::unique_ptr<Literal>> MakeFakeLiteralInternal(
    const Shape& shape, std::minstd_rand0* engine) {
  if (ShapeUtil::IsTuple(shape)) {
    std::vector<std::unique_ptr<Literal>> elements;
    for (const Shape& element_shape : shape.tuple_shapes()) {
      TF_ASSIGN_OR_RETURN(std::unique_ptr<Literal> element,
                          MakeFakeLiteralInternal(element_shape, engine));
      elements.push_back(std::move(element));
    }
    return LiteralUtil::MakeTupleOwned(std::move(elements));
  }
  if (engine == nullptr) {
    return Literal::CreateFromShape(shape);
  }
  auto literal = MakeUnique<Literal>(shape);
  switch (shape.element_type()) {
    case BF16:
      PopulateWithRandomFloatingPointData<bfloat16>(literal.get(), engine);
      break;
    case F16:
      PopulateWithRandomFloatingPointData<half>(literal.get(), engine);
      break;
    case F32:
      PopulateWithRandomFloatingPointData<float>(literal.get(), engine);
      break;
    case F64:
      PopulateWithRandomFloatingPointData<double>(literal.get(), engine);
      break;
    case S8:
      PopulateWithRandomIntegralData<int8>(literal.get(), engine);
      break;
    case U8:
      PopulateWithRandomIntegralData<uint8>(literal.get(), engine);
      break;
    case S16:
      PopulateWithRandomIntegralData<int16>(literal.get(), engine);
      break;
    case U16:
      PopulateWithRandomIntegralData<uint16>(literal.get(), engine);
      break;
    case S32:
      PopulateWithRandomIntegralData<int32>(literal.get(), engine);
      break;
    case U32:
      PopulateWithRandomIntegralData<uint32>(literal.get(), engine);
      break;
    case S64:
      PopulateWithRandomIntegralData<int64>(literal.get(), engine);
      break;
    case U64:
      PopulateWithRandomIntegralData<uint64>(literal.get(), engine);
      break;
    case PRED: {
      std::uniform_int_distribution<int> generator(0, 1);
      TF_CHECK_OK(literal->Populate<bool>(
          [&](tensorflow::gtl::ArraySlice<int64> /*indices*/) {
            return generator(*engine);
          }));
      break;
    }
    // Token requires no data.
    case TOKEN:
      break;
    default:
      return Unimplemented("Unsupported type for fake literal generation: %s",
                           ShapeUtil::HumanString(shape).c_str());
  }
  return std::move(literal);
}

enum class ConstantType { kUnknown, kZero, kOne };

// Return the constant type required by this computation, if known.
ConstantType GetInitValue(const HloComputation& computation) {
  const HloInstruction* const root = computation.root_instruction();
  if (computation.num_parameters() != 2 || root->operand_count() != 2 ||
      root->operand(0)->opcode() != HloOpcode::kParameter ||
      root->operand(1)->opcode() != HloOpcode::kParameter ||
      root->operand(0) == root->operand(1)) {
    return ConstantType::kUnknown;
  }

  switch (root->opcode()) {
    case HloOpcode::kAdd:
      return ConstantType::kZero;
    case HloOpcode::kMultiply:
      return ConstantType::kOne;
    default:
      return ConstantType::kUnknown;
  }
}

// Reduce, ReduceWindow, and SelectAndScatter ops may need a non-random
// initialization value.
bool NeedsInitValue(const HloUse& use) {
  const HloInstruction* const instruction = use.instruction;
  const HloOpcode opcode = instruction->opcode();
  const int64 op_num = use.operand_number;
  return (
      ((opcode == HloOpcode::kReduce || opcode == HloOpcode::kReduceWindow) &&
       op_num == 1) ||
      (opcode == HloOpcode::kSelectAndScatter && op_num == 2));
}

// Generate random values that are constrained to the input_shape minus the
// output_shape so as not to produce wrapping slices, for instance.
std::unique_ptr<Literal> MakeRandomIndex(
    tensorflow::gtl::ArraySlice<int64> index_space, std::minstd_rand0* engine) {
  std::vector<int32> start_indices(index_space.size());
  if (engine != nullptr) {
    for (int i = 0; i < index_space.size(); ++i) {
      std::uniform_int_distribution<int32> generator(0, index_space[i]);
      start_indices[i] = generator(*engine);
    }
  }
  return LiteralUtil::CreateR1<int32>(start_indices);
}

// Use dataflow analysis on each parameter to see if there are uses that would
// be problematic when generating input data.  Returns the list of instructions
// that correspond to their uses.
//
// Should be paired with the CreateLiteralForConstrainedUses() function below.
std::vector<HloInstruction*> FindConstrainedUses(
    const HloDataflowAnalysis& dataflow, const HloInstruction& param) {
  std::vector<HloInstruction*> constrained_uses;
  for (const auto& pair : dataflow.GetInstructionValueSet(&param)) {
    const HloValue& value = dataflow.GetUniqueValueAt(&param, pair.first);
    for (const HloUse& use : value.uses()) {
      HloInstruction* instruction = use.instruction;
      const HloOpcode opcode = instruction->opcode();
      const int64 op_num = use.operand_number;
      if ((opcode == HloOpcode::kDynamicSlice && op_num == 1) ||
          (opcode == HloOpcode::kDynamicUpdateSlice && op_num == 2)) {
        constrained_uses.push_back(instruction);
      } else if (opcode == HloOpcode::kFusion) {
        const HloInstruction* const to_analyze =
            instruction->fused_parameter(op_num);
        auto fused_uses = FindConstrainedUses(dataflow, *to_analyze);
        constrained_uses.insert(constrained_uses.end(), fused_uses.begin(),
                                fused_uses.end());
      } else if (NeedsInitValue(use)) {
        constrained_uses.push_back(instruction);
      } else if (opcode == HloOpcode::kConvert ||
                 opcode == HloOpcode::kReducePrecision) {
        auto converted_uses = FindConstrainedUses(dataflow, *instruction);
        constrained_uses.insert(constrained_uses.end(), converted_uses.begin(),
                                converted_uses.end());
      }
    }
  }
  return constrained_uses;
}

// Given a parameter, generate a random Literal to use as input if there exist
// no constrained uses in the dataflow graph.  If such constraints exist,
// generate a constrained literal (either bounded in the case of indices, or
// zero in the case of init_values for reductions).
StatusOr<std::unique_ptr<Literal>> CreateLiteralForConstrainedUses(
    const tensorflow::gtl::ArraySlice<HloInstruction*> constrained_uses,
    const HloInstruction& param, std::minstd_rand0* engine) {
  std::vector<int64> index_space;
  bool needs_constant = false;
  ConstantType constant_type = ConstantType::kUnknown;
  for (HloInstruction* use : constrained_uses) {
    switch (use->opcode()) {
      case HloOpcode::kDynamicSlice:
      case HloOpcode::kDynamicUpdateSlice: {
        const Shape& indexed_shape = use->operand(0)->shape();
        const Shape& slice_shape = use->opcode() == HloOpcode::kDynamicSlice
                                       ? use->shape()
                                       : use->operand(1)->shape();
        const int64 rank = ShapeUtil::Rank(indexed_shape);
        if (!index_space.empty()) {
          TF_RET_CHECK(rank == index_space.size());
          for (int64 i = 0; i < rank; ++i) {
            index_space[i] = std::min(
                index_space[i], ShapeUtil::GetDimension(indexed_shape, i) -
                                    ShapeUtil::GetDimension(slice_shape, i));
          }
        } else {
          index_space.resize(rank);
          for (int64 i = 0; i < rank; ++i) {
            index_space[i] = ShapeUtil::GetDimension(indexed_shape, i) -
                             ShapeUtil::GetDimension(slice_shape, i);
          }
        }
        break;
      }
      case HloOpcode::kReduce:
      case HloOpcode::kReduceWindow:
        needs_constant = true;
        constant_type = GetInitValue(*use->to_apply());
        break;

      case HloOpcode::kSelectAndScatter:
        needs_constant = true;
        constant_type = GetInitValue(*use->scatter());
        break;

      default:
        return Unimplemented(
            "Constrained operand generation not implemented for %s.",
            use->ToString().c_str());
    }
  }
  if (!index_space.empty() && needs_constant) {
    return Unimplemented(
        "Conflicting operand generation constraints. Dynamically indexes a "
        "shape and is the init value of a reduction.");
  }
  if (!index_space.empty()) {
    return MakeRandomIndex(index_space, engine);
  } else if (needs_constant) {
    switch (constant_type) {
      case ConstantType::kZero:
        return LiteralUtil::Zero(param.shape().element_type()).CloneToUnique();
      case ConstantType::kOne:
        return LiteralUtil::One(param.shape().element_type()).CloneToUnique();
      case ConstantType::kUnknown:
        // We want the identity element for the computation, but we don't really
        // know what it is - so any value we generate will be just as wrong.
        return MakeFakeLiteralInternal(param.shape(), engine);
    }
  } else {
    return MakeFakeLiteralInternal(param.shape(), engine);
  }
}

// Given a module entry parameter, use the dataflow analysis to see if a
// special case literal must be created, or if we can generate fake data.
StatusOr<std::unique_ptr<Literal>> MakeConstrainedArgument(
    const HloDataflowAnalysis& dataflow, const HloInstruction& param,
    std::minstd_rand0* engine) {
  const auto constrained_uses = FindConstrainedUses(dataflow, param);
  return CreateLiteralForConstrainedUses(constrained_uses, param, engine);
}

}  // namespace

StatusOr<std::unique_ptr<Literal>> MakeFakeLiteral(const Shape& shape,
                                                   bool pseudo_random) {
  auto engine = pseudo_random ? MakeUnique<std::minstd_rand0>() : nullptr;
  return MakeFakeLiteralInternal(shape, engine.get());
}

StatusOr<std::vector<std::unique_ptr<Literal>>> MakeFakeArguments(
    HloModule* const module, bool pseudo_random) {
  TF_ASSIGN_OR_RETURN(auto dataflow, HloDataflowAnalysis::Run(*module));
  const auto params = module->entry_computation()->parameter_instructions();
  auto engine = pseudo_random ? MakeUnique<std::minstd_rand0>() : nullptr;
  std::vector<std::unique_ptr<Literal>> arguments(params.size());
  for (int i = 0; i < params.size(); ++i) {
    arguments[i] = MakeConstrainedArgument(*dataflow, *params[i], engine.get())
                       .ValueOrDie();
  }
  return std::move(arguments);
}

Status VerifyHloModule(HloModule* const module, bool allow_mixed_precision) {
  return HloVerifier(allow_mixed_precision).Run(module).status();
}

}  // namespace xla
