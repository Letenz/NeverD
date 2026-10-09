//===- WindowsNativeServices.cpp - Explicit native service boundaries ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsProcess.h"

#include "neverd/emulation/CPU.h"

#include <algorithm>
#include <string_view>

namespace neverd::emulation::windows_process {
namespace {
#include "WindowsSyscallNumbers.inc"
static_assert([] {
  for (size_t I = 0; I < std::size(NativeServices); ++I) {
    if (NativeServices[I].Number != I)
      return false;
    if (I &&
        std::string_view(NativeServices[I - 1].Name) >= NativeServices[I].Name)
      return false;
  }
  return true;
}());
} // namespace

llvm::ArrayRef<NativeService> nativeServices() { return NativeServices; }
std::optional<uint32_t> nativeServiceNumber(llvm::StringRef Name) {
  std::string Alias;
  if (Name.starts_with("Zw")) {
    Alias = ("Nt" + Name.drop_front(2)).str();
    Name = Alias;
  }
  const auto *First = std::begin(NativeServices);
  const auto *Last = std::end(NativeServices);
  const auto *Found = std::lower_bound(
      First, Last, Name, [](const NativeService &Entry, llvm::StringRef Key) {
        return llvm::StringRef(Entry.Name) < Key;
      });
  if (Found == Last || llvm::StringRef(Found->Name) != Name)
    return std::nullopt;
  return Found->Number;
}
const NativeService *findNativeService(uint64_t Number) {
  return Number < std::size(NativeServices) ? &NativeServices[Number] : nullptr;
}
llvm::Expected<uint64_t>
readNativeServiceArgument(ExecutionBackend &CPU, uint64_t SP, unsigned Index) {
  if (CPU.architecture() != GuestArchitecture::X64)
    return failure(text::Service);
  auto ABI = IntegerABI::get(IntegerCallingConvention::Win64);
  if (!ABI)
    return ABI.takeError();
  if (auto E = ABI->validateStackPointer(SP))
    return std::move(E);
  if (Index)
    return ABI->readArgument(CPU, SP, Index);
  auto Value = CPU.readRegister(CPURegister::X64R10);
  if (!Value)
    return Value.takeError();
  return (*Value)[0];
}
llvm::Error returnNativeService(ExecutionBackend &CPU,
                                const ServiceRequest &Request,
                                uint64_t Result) {
  if (CPU.architecture() != GuestArchitecture::X64 ||
      Request.Kind != ServiceRequestKind::X64Syscall || Request.Immediate)
    return failure(text::Service);
  // The backend stopped before SYSCALL. Apply its user-visible clobbers and
  // resume the next instruction, leaving both RSP and its return slot intact.
  auto Flags = CPU.reg(X64Register::FLAGS);
  if (!Flags)
    return Flags.takeError();
  if (auto E = CPU.setReg(X64Register::CX, Request.NextPC))
    return E;
  if (auto E = CPU.setReg(X64Register::R11, *Flags))
    return E;
  if (auto E = CPU.setReg(X64Register::AX, Result))
    return E;
  return CPU.setReg(X64Register::PC, Request.NextPC);
}
} // namespace neverd::emulation::windows_process
