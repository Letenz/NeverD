//===- DarwinTLS.cpp - Authenticated main-thread TLV continuation ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Preserve TLV ABI state and instantiate live templates once per image.
#include "DarwinTLS.h"

#include "../../../runtime/RuntimeValues.h"

#include "llvm/Support/Endian.h"
namespace neverd::emulation::darwin_model {
llvm::Expected<bool> DarwinTLS::invoke(ExecutionBackend &CPU,
                                       ExecutionBudget &Budget,
                                       const ServiceRequest &Request,
                                       ProcessResult &Result) {
  const bool X64 = Image.Architecture == GuestArchitecture::X64;
  auto ABI = IntegerABI::get(X64 ? IntegerCallingConvention::SysVAMD64
                                 : IntegerCallingConvention::AAPCS64);
  if (!ABI)
    return ABI.takeError();
  auto SP = CPU.readRegister(ABI->info().StackPointer);
  if (!SP)
    return SP.takeError();
  auto ReturnSP = ABI->returnStackPointer((*SP)[0]);
  if (!ReturnSP)
    return ReturnSP.takeError();
  auto Descriptor = ABI->readArgument(CPU, (*SP)[0], 0);
  if (!Descriptor)
    return Descriptor.takeError();
  auto Found = Image.TLSDescriptors.find(*Descriptor);
  if (Found == Image.TLSDescriptors.end())
    return failure("Darwin TLV resolver received an unknown descriptor");
  const auto &D = Found->second;
  uint8_t Bytes[24];
  if (auto E = CPU.read(*Descriptor, Bytes))
    return std::move(E);
  const auto Gate =
      value::LibSystemGate +
      unsigned(LibSystemGateKind::TLVBootstrap) * value::LibSystemGateStride;
  if (llvm::support::endian::read64le(Bytes) != Gate ||
      llvm::support::endian::read64le(Bytes + 8) != D.Image + 1 ||
      llvm::support::endian::read64le(Bytes + 16) != D.Offset)
    return failure("Darwin TLV descriptor was modified");
  auto ReturnPC = ABI->readReturnAddress(CPU, (*SP)[0]);
  if (!ReturnPC)
    return ReturnPC.takeError();
  auto Executable =
      CPU.canAccess(*ReturnPC, X64 ? 1 : 4, Execute | UserAccessible);
  if (!Executable)
    return Executable.takeError();
  if (!*Executable || (!X64 && *ReturnPC % 4))
    return failure("invalid Darwin TLV return frame");
  NativeCallEvent Call;
  Call.PC = Request.PC;
  Call.Name = "__tlv_get_addr";
  Call.Module = value::LibSystemPath;
  Call.ArgumentCount = 1;
  Call.Arguments[0] = *Descriptor;
  Call.ReturnAddress = *ReturnPC;
  Result.NativeCalls.push_back(std::move(Call));
  const auto &T = Image.TLS[D.Image];
  if (!Instantiated[D.Image]) {
    std::array<uint8_t, 4096> Chunk;
    // The image mapping owns BSS and file-page-tail semantics. Copy the live
    // contiguous span, including guest changes made before the first access.
    for (uint64_t Offset = 0; Offset < T.Size;) {
      if (!Budget.remainingMicroseconds()) {
        Result.Stop = ProcessStopReason::Timeout;
        Result.Diagnostic = runtime::Timeout;
        return false;
      }
      const auto Count = std::min<uint64_t>(Chunk.size(), T.Size - Offset);
      auto Data = llvm::MutableArrayRef(Chunk).take_front(Count);
      if (auto E = CPU.read(T.Template + Offset, Data))
        return std::move(E);
      if (auto E = CPU.write(T.Storage + Offset, Data))
        return std::move(E);
      Offset += Count;
    }
    Instantiated[D.Image] = true;
  }
  const uint64_t Value = T.Storage + D.Offset;
  if (auto E = CPU.writeRegister(ABI->info().Result, {Value, 0}))
    return std::move(E);
  if (auto E = CPU.writeRegister(ABI->info().StackPointer, {*ReturnSP, 0}))
    return std::move(E);
  if (auto E = CPU.writeRegister(
          X64 ? CPURegister::X64PC : CPURegister::AArch64PC, {*ReturnPC, 0}))
    return std::move(E);
  Result.NativeCalls.back().Result = Value;
  Result.PC = *ReturnPC;
  return true;
}
} // namespace neverd::emulation::darwin_model
