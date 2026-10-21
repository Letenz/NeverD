//===- RegistrationSEHControl.cpp - Physical PE32 finally control ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticates physical PE32 call, return and folded continuation bytes.

#include "RegistrationSEHControl.h"

#include "RegistrationABIPrivate.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/STLExtras.h"

#include <map>
#include <set>

namespace neverd {
namespace {
std::optional<RegistrationSEHControlContract> nearJump(const BinaryImage &Image,
                                                       va_t Address) {
  const auto First = readImmutablePE32CodeBytes(Image, Address, 1, {});
  if (!First || ((*First)[0] != 0xeb && (*First)[0] != 0xe9))
    return std::nullopt;
  const uint32_t Size = (*First)[0] == 0xeb ? 2 : 5;
  const auto Bytes = readImmutablePE32CodeBytes(Image, Address, Size, {});
  if (!Bytes)
    return std::nullopt;
  const int64_t Offset =
      Size == 2 ? int8_t((*Bytes)[1]) : readLE<int32_t>(Bytes->data() + 1);
  return RegistrationSEHControlContract{
      RegistrationSEHControlContract::Kind::NearJump, Address, Address + Size,
      uint32_t(Address + Size + Offset)};
}
} // namespace

std::optional<std::vector<RegistrationSEHControlContract>>
RegistrationCallCalleeIndex::sehControlContracts(const LowFunc &Function) {
  std::vector<RegistrationSEHControlContract> Result;
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || !Function.ExceptionMetadata ||
      !Function.ExceptionMetadata->Registration ||
      Function.ExceptionMetadata->Cxx)
    return Result;
  std::set<va_t> FinallyTargets;
  std::map<int, const LowBlock *> Blocks;
  std::map<va_t, RegistrationSEHControlContract> FoldedJumps;
  for (const auto &Block : Function.Blocks)
    if (!registration_abi::chargeCalleeWork(Work, 1) ||
        !Blocks.emplace(Block.Id, &Block).second)
      return std::nullopt;
  for (const auto &Scope : Function.ExceptionMetadata->Registration->Scopes)
    if (Scope.IsFinally)
      FinallyTargets.insert(Scope.HandlerVA);
  for (const auto &Block : Function.Blocks)
    for (const auto &Insn : Block.InstructionBoundaries) {
      if (!registration_abi::chargeCalleeWork(Work, 1 + Insn.Size))
        return std::nullopt;
      if (Insn.Address > UINT32_MAX || !Insn.Size ||
          Insn.Size > uint64_t(UINT32_MAX) + 1 - Insn.Address ||
          Insn.Mode != InstructionMode::Default ||
          Insn.TargetMode != LowInstructionTargetMode::Preserve ||
          hasLowInstructionControlFlag(
              Insn.ControlFlags, LowInstructionControlFlag::Conditional) ||
          hasLowInstructionControlFlag(
              Insn.ControlFlags, LowInstructionControlFlag::InstructionGuard))
        continue;
      if (Insn.Control != LowInstructionControl::Return &&
          (Insn.Control != LowInstructionControl::Call || !Insn.Immediate ||
           !FinallyTargets.count(*Insn.Immediate)))
        continue;
      const auto Bytes =
          readImmutablePE32CodeBytes(Image, Insn.Address, Insn.Size, {});
      if (!Bytes)
        continue;
      RegistrationSEHControlContract C;
      C.Address = Insn.Address;
      C.EndAddress = Insn.Address + Insn.Size;
      if (Insn.Control == LowInstructionControl::Call) {
        if (Bytes->size() != 5 || (*Bytes)[0] != 0xe8)
          continue;
        C.Target = uint32_t(C.EndAddress +
                            int64_t(readLE<int32_t>(Bytes->data() + 1)));
        if (C.Target != *Insn.Immediate)
          continue;
      } else {
        if (!((Bytes->size() == 1 && (*Bytes)[0] == 0xc3 && !Insn.Immediate) ||
              (Bytes->size() == 3 && (*Bytes)[0] == 0xc2 &&
               readLE<uint16_t>(Bytes->data() + 1) == 0 &&
               Insn.Immediate == 0)))
          continue;
        C.Control = RegistrationSEHControlContract::Kind::NearReturn;
      }
      Result.push_back(C);
      if (C.Control == RegistrationSEHControlContract::Kind::NearCall &&
          Block.Succs.size() == 1 && Blocks.count(Block.Succs.front())) {
        const va_t Resume = Blocks.at(Block.Succs.front())->StartAddr;
        va_t Next = C.EndAddress;
        std::set<va_t> Seen;
        std::vector<RegistrationSEHControlContract> Path;
        while (
            Next != Resume &&
            Seen.size() <
                ::neverd::limits::ir::
                    kRegistrationSEHControlRegistrationCallCalleeIndexSehControlContractsSeenSizeLimit &&
            Seen.insert(Next).second) {
          if (!registration_abi::chargeCalleeWork(Work, 6))
            return std::nullopt;
          if (!Function.ExceptionMetadata->CodeRange.contains(Next))
            break;
          const auto Jump = nearJump(Image, Next);
          if (!Jump ||
              Jump->EndAddress > Function.ExceptionMetadata->CodeRange.End)
            break;
          Path.push_back(*Jump);
          Next = Jump->Target;
        }
        // A folded successor needs the entire original jump path. A checked
        // prefix alone does not authenticate the physical return continuation.
        if (Next == Resume)
          for (const auto &Jump : Path) {
            const auto [It, New] = FoldedJumps.emplace(Jump.Address, Jump);
            if (!New && It->second != Jump)
              return std::nullopt;
          }
      }
    }
  for (const auto &[Address, Jump] : FoldedJumps)
    Result.push_back(Jump);
  // An ordinary parent epilogue is not evidence of a nonreturning finally.
  // Only the adjacent physical invocation needs its original caller values
  // threaded through the state solver. Seeding those values for every SEH
  // function needlessly multiplies the cost of unrelated runtime closures.
  if (llvm::none_of(Result, [](const auto &Control) {
        return Control.Control ==
                   RegistrationSEHControlContract::Kind::NearCall &&
               Control.EndAddress == Control.Target;
      }))
    return Result;
  for (const auto &Block : Function.Blocks) {
    if (!registration_abi::chargeCalleeWork(Work, 1))
      return std::nullopt;
    if (!Block.Succs.empty() || Block.StartAddr >= Block.EndAddr ||
        Block.EndAddr - Block.StartAddr < 4 ||
        Block.EndAddr - Block.StartAddr > 8)
      continue;
    if (!registration_abi::chargeCalleeWork(Work, 16))
      return std::nullopt;
    if (Function.ExceptionMetadata->ownsCode({Block.StartAddr, Block.EndAddr}))
      if (auto Epilogue = getRegistrationSEHParentEpilogue(Image, Block))
        Result.push_back(*Epilogue);
  }
  return Result;
}

} // namespace neverd

namespace neverd::registration_state {

const ExceptionFunction *installedRegistration(const BinaryImage &Image,
                                               va_t Entry) {
  const ExceptionFunction *Exception =
      Image.ExceptionMetadata.findFunction(Entry);
  if (!Exception || !Exception->Registration)
    return nullptr;
  const va_t Installer = Exception->FunctionEntry != 0
                             ? Exception->FunctionEntry
                             : Exception->CodeRange.Begin;
  return Installer == Entry ? Exception : nullptr;
}

bool isSEHFinallyCallInstruction(const BinaryImage &Image,
                                 const RegistrationChainInfo &Chain,
                                 va_t Address, uint16_t Size,
                                 llvm::ArrayRef<LowOp> Ops) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || Size != 5 ||
      Address > UINT32_MAX - Size)
    return false;
  const LowOp *Call = nullptr;
  for (const auto &Op : Ops)
    if (Op.Opcode == NdOp::CALL) {
      if (Call || Op.Addr != Address || Op.NumInputs != 1 ||
          !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4)
        return false;
      Call = &Op;
    }
  if (!Call || !llvm::any_of(Chain.Scopes, [&](const auto &Scope) {
        return Scope.IsFinally && Scope.HandlerVA == Call->Inputs[0].Offset;
      }))
    return false;
  const auto Bytes = readImmutablePE32CodeBytes(Image, Address, Size, {});
  return Bytes && (*Bytes)[0] == 0xe8 &&
         uint32_t(Address + Size +
                  int64_t(readLE<int32_t>(Bytes->data() + 1))) ==
             Call->Inputs[0].Offset;
}

} // namespace neverd::registration_state
