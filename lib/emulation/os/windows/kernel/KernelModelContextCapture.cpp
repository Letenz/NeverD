//===- KernelModelContextCapture.cpp - Bounded kernel caller capture -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Kernel-owned caller context publication and complete pre-effect admission.
//===----------------------------------------------------------------------===//
#include "../../../arch/x86_64/X64Machine.h"
#include "../exception/WindowsX64Context.h"
#include "KernelContextCapture.h"
#include "KernelModel.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <bit>

namespace neverd::emulation {
namespace {
constexpr llvm::StringLiteral API = "RtlCaptureContext";

uint64_t flagsAfterStackRestore(uint64_t EntrySP, uint64_t Flags) {
  // The bounded kernel routine restores its PUSHFQ slot with ADD RSP,8.
  // RET's later stack increment does not change flags.
  const uint64_t Left = EntrySP - 8, Right = 8, Result = EntrySP;
  uint64_t Arithmetic = 0;
  if (Result < Left)
    Arithmetic |= x64::CarryFlag;
  if (!(std::popcount(uint8_t(Result)) & 1))
    Arithmetic |= x64::ParityFlag;
  if ((Left ^ Right ^ Result) & x64::AuxiliaryFlag)
    Arithmetic |= x64::AuxiliaryFlag;
  if (!Result)
    Arithmetic |= x64::ZeroFlag;
  if (Result >> 63)
    Arithmetic |= x64::SignFlag;
  if ((~(Left ^ Right) & (Left ^ Result)) >> 63)
    Arithmetic |= x64::OverflowFlag;
  return (Flags & ~x64::ArithmeticFlags) | Arithmetic;
}
} // namespace

llvm::Expected<uint64_t>
KernelModel::captureCallerContext(uint64_t Address,
                                  const KernelContextCapture *Context) {
  if (Contract != ExecutionContract::CheckedX64)
    return llvm::createStringError(API + " requires checked-x64 execution");
  if (!Context || !Context->Read || !Context->SetFlags)
    return llvm::createStringError(API + " requires a stopped CPU context");
  if (Address % 16)
    return llvm::createStringError(API + " requires a 16-byte aligned context");
  auto State = Context->Read();
  if (!State)
    return State.takeError();
  using namespace llvm::support::endian;
  auto &Bytes = State->Bytes;
  const uint64_t SP = read64le(Bytes.data() + seh::ContextGPROffset + 4 * 8);
  const uint64_t CX = read64le(Bytes.data() + seh::ContextGPROffset + 8);
  const uint64_t Flags = read32le(Bytes.data() + seh::ContextEFlagsOffset);
  if (SP < 8 || SP > UINT64_MAX - 8 || SP % 16 != 8 || CX != Address ||
      (Flags & ~x64::AllowedFlags) || !(Flags & x64::ReservedFlag))
    return llvm::createStringError(API + " has an invalid caller CPU context");
  // This bounded contract requires the complete record to be readable and
  // writable; its publication footprint is deliberately much smaller.
  if (auto E = validateQueryBuffer(Address, seh::ContextSize, false, API))
    return E;
  if (auto E = validateQueryBuffer(Address, seh::ContextSize, true, API))
    return E;
  if (auto E = validateQueryBuffer(SP, 8, false, API))
    return E;
  // PUSHFQ writes eight bytes; the subsequent MOV EAX,[RSP] reads four.
  if (auto E = validateQueryBuffer(SP - 8, 8, false, API))
    return E;
  if (auto E = validateQueryBuffer(SP - 8, 8, true, API))
    return E;
  if (Address < SP + 8 && SP - 8 < Address + seh::ContextSize)
    return llvm::createStringError(API +
                                   " does not support context/stack overlap");
  auto PC = Memory.readInteger(SP, 8);
  if (!PC)
    return PC.takeError();
  write32le(Bytes.data() + seh::ContextFlagsOffset,
            windows_context::CapturedFlags);
  write64le(Bytes.data() + seh::ContextGPROffset + 4 * 8, SP + 8);
  write64le(Bytes.data() + seh::ContextPCOffset, *PC);
  std::fill_n(Bytes.begin() + seh::ContextFPOffset + 2, 4, 0);
  std::array<uint8_t, 8> Scratch;
  write64le(Scratch.data(), Flags);

  // All collection, admission and construction precede the only fallible
  // CPU mutation. A failed setter has no effects. Services cannot reenter or
  // change mappings, and the following stopped ordinary-RAM writes have no
  // guest/device callbacks. This is not rollback of arbitrary backend faults.
  if (auto E = Context->SetFlags(flagsAfterStackRestore(SP, Flags)))
    return E;
  if (auto E = Memory.write(SP - 8, Scratch))
    return E;
  constexpr std::pair<unsigned, unsigned> Ranges[] = {
      {48, 24}, {120, 142}, {280, 4}, {416, 256}};
  for (const auto &[Offset, Size] : Ranges)
    if (auto E = Memory.write(Address + Offset,
                              llvm::ArrayRef(Bytes).slice(Offset, Size)))
      return E;
  // The incoming RAX remains in the record. The physical return register gets
  // the zero-extended flags word; SP/PC remain the existing session's RET job.
  return Flags;
}
} // namespace neverd::emulation
