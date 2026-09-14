#include "neverd/ir/med/MedSourceParameterUses.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/med/MedIR.h"

#include <map>
#include <set>
#include <tuple>

namespace neverd {
namespace {
using ValueKey = std::tuple<MedVar::VarKind, int, int>;

ValueKey key(const MedVar &Value) {
  return {Value.Kind, Value.Id, Value.SSAVer};
}

enum Use : unsigned { Pointer = 1, Scalar = 2 };

struct ValueUses {
  unsigned Roles = 0;
  // Backward identity edges; narrow copies propagate only conflicting uses.
  std::vector<std::pair<ValueKey, bool>> Inputs;
};
} // namespace

std::vector<bool> inferMedSourcePointerParameters(const MedFunc &Function) {
  std::vector<bool> Result(Function.Params.size(), false);
  std::map<ValueKey, ValueUses> Values;
  std::set<ValueKey> Definitions;
  std::map<ValueKey, const MedVar *> Parameters;
  std::map<uint64_t, const MedVar *> EntryRegisters;
  for (const auto &Parameter : Function.Params) {
    if (Parameter.Id < 0)
      continue;
    if (Parameter.Kind != MedVar::Param ||
        !Parameters.emplace(key(Parameter), &Parameter).second ||
        (Parameter.RegOff != kNoParamReg &&
         !EntryRegisters.emplace(Parameter.RegOff, &Parameter).second))
      return Result;
  }

  bool Ambiguous = false;
  auto Observe = [&](const MedVar &Value) {
    if (Value.Kind == MedVar::Reg && Value.SSAVer == 0) {
      const auto Found = EntryRegisters.find(Value.RegOff);
      if (Found != EntryRegisters.end()) {
        // Generic ABI inference records Params separately from SSA live-ins.
        // Only the entry version at that exact physical location represents
        // the parameter; later versions of the register are unrelated.
        Values[key(Value)].Inputs.emplace_back(
            key(*Found->second), Value.Size == 8 && Found->second->Size == 8);
      }
    }
    if (Value.Kind != MedVar::Param)
      return;
    const auto Found = Parameters.find(key(Value));
    if (Found == Parameters.end() || Found->second->RegOff != Value.RegOff)
      Ambiguous = true;
  };
  std::vector<std::pair<ValueKey, unsigned>> Pending;
  auto Seed = [&](const MedVar &Value, unsigned Role) {
    Observe(Value);
    if (!Value.isConst())
      Pending.emplace_back(key(Value),
                           Role == Pointer && Value.Size != 8 ? Scalar : Role);
  };
  auto Connect = [&](const MedVar &Output, const MedVar &Input) {
    Observe(Input);
    if (!Input.isConst())
      Values[key(Output)].Inputs.emplace_back(key(Input), Output.Size == 8 &&
                                                              Input.Size == 8);
  };
  auto Define = [&](const MedVar &Output) {
    if (Output.Size && !Output.isConst() &&
        (Output.Kind == MedVar::Param ||
         (Output.Kind == MedVar::Reg && Output.SSAVer == 0 &&
          EntryRegisters.count(Output.RegOff)) ||
         !Definitions.insert(key(Output)).second))
      Ambiguous = true;
  };

  for (const auto &Block : Function.Blocks) {
    for (const auto &Phi : Block.Phis) {
      Define(Phi.Output);
      for (const auto &[Predecessor, Input] : Phi.Args)
        Connect(Phi.Output, Input);
    }
    for (const auto &Op : Block.Ops) {
      if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
          Op.Output == Op.Inputs[0] && Op.Output.Size == Op.Inputs[0].Size)
        continue;
      Define(Op.Output);
      if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1) {
        Connect(Op.Output, Op.Inputs[0]);
        // A narrowed view is a scalar use even if its result is dead.
        if (Op.Output.Size != 8 || Op.Inputs[0].Size != 8)
          Seed(Op.Inputs[0], Scalar);
        continue;
      }
      std::string Error;
      const bool BoundCall =
          (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
          Op.SourceCallHint &&
          validateSourceABI(Op.SourceCallHint->Signature, Error) &&
          Op.NumInputs == Op.SourceCallHint->Signature.Parameters.size() + 1;
      for (unsigned Index = 0; Index < Op.NumInputs; ++Index) {
        unsigned Role = Scalar;
        if (BoundCall && Index) {
          const auto &Type =
              Op.SourceCallHint->Signature.Parameters[Index - 1].Type;
          if (Type->Kind == NdTypeKind::Ptr && Type->Size == 8)
            Role = Pointer;
        }
        // Other operations may consume integer bits, compare addresses, or
        // change their identity. They never seed pointer evidence here.
        Seed(Op.Inputs[Index], Role);
      }
    }
  }
  if (Ambiguous)
    return Result;

  // Each role visits each SSA value once. Loops and shared PHI chains converge
  // without recursion, path enumeration, or dependence on block order.
  for (size_t Cursor = 0; Cursor < Pending.size(); ++Cursor) {
    const auto [Value, Roles] = Pending[Cursor];
    auto &Node = Values[Value];
    const unsigned NewRoles = Roles & ~Node.Roles;
    if (!NewRoles)
      continue;
    Node.Roles |= NewRoles;
    for (const auto &[Input, PreservesPointer] : Node.Inputs)
      Pending.emplace_back(Input, PreservesPointer ? NewRoles : Scalar);
  }
  for (size_t Index = 0; Index < Function.Params.size(); ++Index) {
    const auto &Parameter = Function.Params[Index];
    const auto Found = Values.find(key(Parameter));
    Result[Index] = Parameter.Id >= 0 && Parameter.Size == 8 &&
                    Found != Values.end() && Found->second.Roles == Pointer;
  }
  return Result;
}
} // namespace neverd
