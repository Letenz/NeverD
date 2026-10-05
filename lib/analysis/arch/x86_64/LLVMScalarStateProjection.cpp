//===- LLVMScalarStateProjection.cpp - x64 entry domain -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/analysis/arch/x86_64/LLVMScalarStateProjection.h"

#include "X64UserFlags.h"

#include "neverd/analysis/arch/x86_64/InterpreterMachineState.h"

namespace neverd::analysis {
llvm::Expected<LLVMScalarStateContract> llvmScalarStateContractX64(
    InterpreterMachineStateProfile Profile,
    llvm::ArrayRef<LLVMScalarStateObservation> Observations,
    std::optional<InterpreterEntryAlignment> Alignment) {
  if (Profile != InterpreterMachineStateProfile::UserX64NoFaultV1)
    return llvm::createStringError("unsupported scalar state entry profile");
  if (Alignment && !Alignment->valid())
    return llvm::createStringError("invalid scalar state entry alignment");
  LLVMScalarStateContract Contract;
  Contract.StateBytes = sizeof(InterpreterMachineStateX64V1);
  Contract.CellBits = 32;
  constexpr auto Flags = offsetof(InterpreterMachineStateX64V1, RFlags) / 4;
  constexpr auto Mask = detail::X64UserFlags::EntryMask;
  Contract.Entry = {{Flags, uint32_t(Mask), 2}, {Flags + 1, Mask >> 32, 0}};
  if (Alignment) {
    constexpr auto Stack =
        (offsetof(InterpreterMachineStateX64V1, GPR) + 4 * sizeof(uint64_t)) /
        4;
    Contract.Entry.push_back(
        {Stack, ~uint32_t(Alignment->Alignment - 1), Alignment->Residue});
  }
  Contract.Observations.assign(Observations.begin(), Observations.end());
  return Contract;
}
} // namespace neverd::analysis
