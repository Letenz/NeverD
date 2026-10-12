//===- DriverEntryABI.cpp - Validate a checked DriverEntry return --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Win64 nonvolatile registers and control fields at the native entry boundary.
//===----------------------------------------------------------------------===//
#include "DriverEntryABI.h"

#include "llvm/Support/FormatVariadic.h"

namespace neverd::emulation {
namespace {
// Microsoft x64 calling convention: XMM6..15 retain all 128 bits; MXCSR's
// six status bits are volatile. FPCSR retains the complete control word.
// https://learn.microsoft.com/cpp/build/x64-calling-convention
struct Field {
  CPURegister Register;
  const char *Name;
  RegisterValue Mask;
};
constexpr Field Fields[] = {
    {CPURegister::X64BX, "rbx", {UINT64_MAX, 0}},
    {CPURegister::X64BP, "rbp", {UINT64_MAX, 0}},
    {CPURegister::X64SI, "rsi", {UINT64_MAX, 0}},
    {CPURegister::X64DI, "rdi", {UINT64_MAX, 0}},
    {CPURegister::X64R12, "r12", {UINT64_MAX, 0}},
    {CPURegister::X64R13, "r13", {UINT64_MAX, 0}},
    {CPURegister::X64R14, "r14", {UINT64_MAX, 0}},
    {CPURegister::X64R15, "r15", {UINT64_MAX, 0}},
    {CPURegister::X64V6, "xmm6", {UINT64_MAX, UINT64_MAX}},
    {CPURegister::X64V7, "xmm7", {UINT64_MAX, UINT64_MAX}},
    {CPURegister::X64V8, "xmm8", {UINT64_MAX, UINT64_MAX}},
    {CPURegister::X64V9, "xmm9", {UINT64_MAX, UINT64_MAX}},
    {CPURegister::X64V10, "xmm10", {UINT64_MAX, UINT64_MAX}},
    {CPURegister::X64V11, "xmm11", {UINT64_MAX, UINT64_MAX}},
    {CPURegister::X64V12, "xmm12", {UINT64_MAX, UINT64_MAX}},
    {CPURegister::X64V13, "xmm13", {UINT64_MAX, UINT64_MAX}},
    {CPURegister::X64V14, "xmm14", {UINT64_MAX, UINT64_MAX}},
    {CPURegister::X64V15, "xmm15", {UINT64_MAX, UINT64_MAX}},
    {CPURegister::X64MXCSR, "mxcsr", {0xffc0, 0}},
    {CPURegister::X64FPCW, "fpcw", {0xffff, 0}},
};
llvm::Error validateCPU(const ExecutionBackend &CPU) {
  if (CPU.architecture() != GuestArchitecture::X64)
    return llvm::createStringError("DriverEntry ABI requires an x64 CPU");
  return llvm::Error::success();
}
} // namespace

llvm::Expected<DriverEntryABI> DriverEntryABI::capture(ExecutionBackend &CPU) {
  if (auto E = validateCPU(CPU))
    return std::move(E);
  DriverEntryABI Out;
  static_assert(std::size(Fields) ==
                std::tuple_size_v<decltype(Out.Registers)>);
  for (size_t I = 0; I < std::size(Fields); ++I) {
    auto Value = CPU.readRegister(Fields[I].Register);
    if (!Value)
      return Value.takeError();
    Out.Registers[I] = *Value;
  }
  return Out;
}

llvm::Error DriverEntryABI::validateReturn(ExecutionBackend &CPU) const {
  if (auto E = validateCPU(CPU))
    return E;
  for (size_t I = 0; I < std::size(Fields); ++I) {
    const auto &F = Fields[I];
    auto Value = CPU.readRegister(F.Register);
    if (!Value)
      return Value.takeError();
    for (size_t Lane = 0; Lane < Value->size(); ++Lane)
      if (((*Value)[Lane] ^ Registers[I][Lane]) & F.Mask[Lane])
        return llvm::createStringError(llvm::formatv(
            "DriverEntry returned with changed nonvolatile {0}", F.Name));
  }
  auto Flags = CPU.readRegister(CPURegister::X64FLAGS);
  if (!Flags)
    return Flags.takeError();
  if ((*Flags)[0] & (1u << 10))
    return llvm::createStringError(
        "DriverEntry returned with the direction flag set");
  return llvm::Error::success();
}
} // namespace neverd::emulation
