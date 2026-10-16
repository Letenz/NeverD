//===- RegistrationSEHRegisterInputs.cpp - Observable finally inputs -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A full register exchange preserves the callback ABI, but copying an unused
/// incoming register must not add an argument to the parent's physical ABI.

#include "neverd/Limits.h"
#include "neverd/ir/med/X86RegistrationCallback.h"
#include "neverd/ir/med/X86RegistrationSEHLocalUnwind.h"
#include "neverd/ir/med/X86RegistrationSEHRegisters.h"

#include "llvm/ADT/STLExtras.h"

#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace neverd::x86_registration {

void discardUnusedSEHCallInputs(MedFunc &Function) {
  const auto Owners = registrationSEHCallbackOwners(Function);
  if (!Owners || Owners->empty())
    return;
  const auto LocalUnwinds = getX86RegistrationSEHLocalUnwinds(Function);
  using ValueKey = std::tuple<MedVar::VarKind, int, int>;
  using RegisterKey = std::pair<va_t, uint64_t>;
  const auto Key = [](const MedVar &Value) {
    return ValueKey{Value.Kind, Value.Id, Value.SSAVer};
  };
  std::set<ValueKey> Uses;
  std::set<va_t> OpaqueCallbacks;
  std::map<RegisterKey, std::vector<ValueKey>> Reads;
  std::map<RegisterKey, std::vector<MedOp *>> Writes;
  size_t Work = 0;
  const auto Charge = [&] {
    return ++Work <= limits::kMaxRegistrationEHStateWork;
  };
  const auto Use = [&](const MedVar &Value) {
    if (!Value.isConst() && Value.Id >= 0 && Value.Size)
      Uses.insert(Key(Value));
  };
  for (auto &Block : Function.Blocks) {
    if (!Charge())
      return;
    const auto Owner = Owners->find(Block.Id);
    for (const auto &Phi : Block.Phis)
      for (const auto &[Predecessor, Value] : Phi.Args) {
        if (!Charge())
          return;
        Use(Value);
      }
    for (auto &Op : Block.Ops) {
      if (!Charge() || Op.NumInputs > Op.Inputs.size())
        return;
      for (unsigned I = 0; I != Op.NumInputs; ++I)
        Use(Op.Inputs[I]);
      if (!isSEHRegisterIntrinsic(Op)) {
        // A call or another intrinsic can consume physical registers through
        // an implicit ABI which has not yet been recovered. Keep all inputs
        // of such a callback, even if no explicit SSA use names the register.
        bool CheckedLocal = false;
        if (LocalUnwinds)
          if (const auto Local = LocalUnwinds->find(Op.Addr);
              Local != LocalUnwinds->end())
            CheckedLocal = Local->second.Operation == &Op;
        if (!CheckedLocal && Owner != Owners->end() &&
            (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
             Op.Opcode == NdOp::INTRINSIC))
          OpaqueCallbacks.insert(Owner->second.first);
        continue;
      }
      if (!hasValidSEHRegisterShape(Op))
        return;
      const RegisterKey Register{Op.Inputs[2].ConstVal, Op.Inputs[4].ConstVal};
      const auto Role = static_cast<SEHRegisterRole>(Op.Inputs[1].ConstVal);
      if (Role == SEHRegisterRole::CallbackInput) {
        if (Owner == Owners->end() || !Owner->second.second ||
            Owner->second.first != Register.first || Block.StartAddr != Op.Addr)
          return;
        Reads[Register].push_back(Key(Op.Output));
      } else if (Role == SEHRegisterRole::CallInput)
        Writes[Register].push_back(&Op);
    }
  }
  for (const auto &Clobber : Function.CallClobbers) {
    if (!Charge())
      return;
    if (Clobber.PreservedPrefixSize)
      Use(Clobber.PreservedInput);
  }
  for (const auto &Call : Function.CallInfos)
    for (const auto &Argument : Call.Args) {
      if (!Charge())
        return;
      Use(Argument);
    }
  // No mutation precedes the complete scan. Unknown/malformed transport and
  // budget exhaustion preserve the original conservative parameter recovery.
  for (const auto &[Register, Operations] : Writes) {
    const auto Found = Reads.find(Register);
    if (Found == Reads.end() || OpaqueCallbacks.count(Register.first) ||
        llvm::any_of(Found->second,
                     [&](const auto &Read) { return Uses.count(Read); }))
      continue;
    for (auto *Op : Operations)
      Op->Inputs[5] = MedVar::makeUnspecified(4, Arch::X86);
  }
}

} // namespace neverd::x86_registration
