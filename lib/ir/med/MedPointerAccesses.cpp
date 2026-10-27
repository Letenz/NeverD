//===- MedPointerAccesses.cpp - What code does through a pointer ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedPointerAccesses.h"

#include "neverd/ir/Limits.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <set>
#include <tuple>

namespace neverd {
namespace {

/// \p Var, a constant, as a signed value of its width.
int64_t signedValue(const MedVar &Var) {
  const unsigned Bits = Var.Size ? std::min(Var.Size * 8u, 64u) : 64u;
  uint64_t Value = Var.ConstVal;
  if (Bits < 64) {
    const uint64_t Mask = (uint64_t(1) << Bits) - 1;
    Value &= Mask;
    if (Value >> (Bits - 1))
      Value |= ~Mask;
  }
  return static_cast<int64_t>(Value);
}

/// An operation that only moves an address or adds to it.
bool movesAddress(const MedOp &Op) {
  return Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ADD ||
         Op.Opcode == NdOp::INT_SUB;
}

} // namespace

MedPointerAccesses::MedPointerAccesses(const MedFunc &Func) {
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (!Op.Dead && !Op.Output.isConst())
        Definitions.emplace(keyOf(Op.Output), &Op);

  auto Note = [&](const MedVar &Address, MedPointerAccess Access) {
    std::optional<std::pair<MedVar, int64_t>> Base = baseOf(Address);
    if (!Base)
      return;
    Access.Offset = Base->second;
    const Key BaseKey = keyOf(Base->first);
    Bases.emplace(BaseKey, Base->first);
    ByBase[BaseKey].push_back(Access);
    ByInstruction.emplace_back(Base->first, Access);
  };

  // The values the code uses otherwise than as an address or to compute
  // one: an address among them is passed on.
  std::set<Key> PassedOn;
  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis)
      for (const auto &[Pred, Arg] : Phi.Args) {
        (void)Pred;
        PassedOn.insert(keyOf(Arg));
      }
    for (const MedOp &Op : Block.Ops) {
      if (Op.Dead || movesAddress(Op))
        continue;
      for (unsigned I = 0; I < Op.NumInputs; ++I) {
        const bool Address =
            I == 0 && (Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE);
        if (!Address && !Op.Inputs[I].isConst())
          PassedOn.insert(keyOf(Op.Inputs[I]));
      }
    }
  }

  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops) {
      // A segment's bytes are no pointer's.
      if (Op.Dead || Op.NumInputs == 0 ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        continue;
      if (Op.Opcode == NdOp::LOAD)
        Note(Op.Inputs[0],
             {Op.Addr, 0, Op.Output.Size, MedPointerAccess::Use::Read});
      else if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2)
        Note(Op.Inputs[0],
             {Op.Addr, 0, Op.Inputs[1].Size, MedPointerAccess::Use::Write});
    }
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops) {
      if (Op.Dead ||
          (Op.Opcode != NdOp::INT_ADD && Op.Opcode != NdOp::INT_SUB) ||
          !PassedOn.count(keyOf(Op.Output)))
        continue;
      std::optional<std::pair<MedVar, int64_t>> Base = baseOf(Op.Output);
      // The pointer itself passed on takes no member's address.
      if (Base && Base->second != 0)
        Note(Op.Output, {Op.Addr, 0, 0, MedPointerAccess::Use::Address});
    }
  for (auto &[BaseKey, Accesses] : ByBase)
    llvm::stable_sort(
        Accesses, [](const MedPointerAccess &L, const MedPointerAccess &R) {
          return std::tie(L.Addr, L.Offset) < std::tie(R.Addr, R.Offset);
        });
}

std::optional<std::pair<MedVar, int64_t>>
MedPointerAccesses::baseOf(const MedVar &Address) const {
  MedVar Var = Address;
  int64_t Offset = 0;
  for (unsigned Step = 0;
       Step < ::neverd::limits::ir::kMedPointerAccessesMaxChain &&
       !Var.isConst();
       ++Step) {
    const auto Found = Definitions.find(keyOf(Var));
    if (Found == Definitions.end())
      break;
    const MedOp &Op = *Found->second;
    if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1) {
      const MedVar &From = Op.Inputs[0];
      if (From.isConst() || From.Size != Var.Size || keyOf(From) == keyOf(Var))
        break;
      Var = From;
      continue;
    }
    if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
        Op.NumInputs == 2) {
      const MedVar &A = Op.Inputs[0], &B = Op.Inputs[1];
      if (B.isConst() && !A.isConst()) {
        Offset += Op.Opcode == NdOp::INT_ADD ? signedValue(B) : -signedValue(B);
        Var = A;
        continue;
      }
      if (Op.Opcode == NdOp::INT_ADD && A.isConst() && !B.isConst()) {
        Offset += signedValue(A);
        Var = B;
        continue;
      }
    }
    break;
  }
  if (Var.isConst())
    return std::nullopt;
  return std::make_pair(Var, Offset);
}

std::optional<std::pair<MedVar, MedPointerAccess>>
MedPointerAccesses::at(va_t Addr) const {
  const std::pair<MedVar, MedPointerAccess> *Address = nullptr;
  for (const auto &Entry : ByInstruction) {
    if (Entry.second.Addr != Addr)
      continue;
    if (Entry.second.Kind != MedPointerAccess::Use::Address)
      return Entry;
    if (!Address)
      Address = &Entry;
  }
  if (Address)
    return *Address;
  return std::nullopt;
}

std::vector<MedPointerAccess>
MedPointerAccesses::through(const MedVar &Base) const {
  const auto Found = ByBase.find(keyOf(Base));
  return Found == ByBase.end() ? std::vector<MedPointerAccess>()
                               : Found->second;
}

std::vector<MedVar> MedPointerAccesses::bases() const {
  std::vector<MedVar> Result;
  Result.reserve(Bases.size());
  for (const auto &[BaseKey, Var] : Bases)
    Result.push_back(Var);
  return Result;
}

} // namespace neverd
