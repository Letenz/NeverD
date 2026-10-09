//===- WindowsNativeServices.cpp - Explicit native service boundaries ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsNativeServices.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>
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

std::array<uint8_t, value::NativeSyscallOffset>
nativeServicePrologue(uint32_t Number) {
  std::array<uint8_t, value::NativeSyscallOffset> Bytes{0x4c, 0x8b, 0xd1, 0xb8};
  llvm::support::endian::write32le(Bytes.data() + 4, Number);
  return Bytes;
}

NativeEntryEvidence::NativeEntryEvidence(llvm::ArrayRef<Import> Gates) {
  for (const auto &Gate : Gates)
    if (Gate.Module == text::NTDLL)
      if (auto Number = nativeServiceNumber(Gate.Name))
        Entries.emplace(Gate.Gate, *Number);
}
std::vector<ExecutionWatch> NativeEntryEvidence::watches() const {
  if (Entries.empty())
    return {};
  const uint64_t Begin = Entries.begin()->first;
  return {{Begin,
           Entries.rbegin()->first - Begin + value::NativeSyscallOffset + 1}};
}
void NativeEntryEvidence::invalidate() {
  Entry.reset();
  ExpectedPC.reset();
  VerifiedPC.reset();
}
llvm::Error NativeEntryEvidence::watched(ExecutionBackend &CPU, uint64_t PC) {
  if (ExpectedPC && PC == *ExpectedPC) {
    if (PC == *Entry + 3)
      ExpectedPC = *Entry + value::NativeSyscallOffset;
    else {
      VerifiedPC = PC;
      ExpectedPC.reset();
    }
    return llvm::Error::success();
  }
  invalidate();
  auto Found = Entries.find(PC);
  if (Found == Entries.end())
    return llvm::Error::success();
  std::array<uint8_t, value::NativeSyscallOffset> Bytes;
  if (auto E = CPU.read(PC, Bytes))
    return E;
  // Only this straight-line mov r10,rcx / mov eax,imm32 prologue supplies
  // evidence. Modified bytes and interior entries keep their numeric binding.
  if (Bytes == nativeServicePrologue(Found->second)) {
    Entry = PC;
    ExpectedPC = PC + 3;
  }
  return llvm::Error::success();
}
bool NativeEntryEvidence::take(uint64_t PC) {
  const bool Matched = VerifiedPC && *VerifiedPC == PC;
  invalidate();
  return Matched;
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
