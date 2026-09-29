//===- IntegerABI.cpp - Scalar guest call layout and access --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/emulation/IntegerABI.h"

#include "neverd/emulation/CPU.h"

#include <limits>

namespace neverd::emulation {
namespace {
#define NEVERD_ABI_DIAGNOSTIC(Name, Text) constexpr char Name[] = Text;
#include "IntegerABIDiagnostics.def"
#undef NEVERD_ABI_DIAGNOSTIC

llvm::Error failure(const char *Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}

// Keep all register identities and ABI constants in the common inventory.
using enum CPURegister;
#define NEVERD_INTEGER_ARGUMENTS(Name, ...)                                    \
  constexpr CPURegister Name##Arguments[] = {__VA_ARGS__};
#include "neverd/emulation/IntegerABI.def"
#undef NEVERD_INTEGER_ARGUMENTS
#define NEVERD_INTEGER_ABI(Name, ISA, Bank, SP, Result, Link, Word, Alignment, \
                           Shadow, RedZone)                                    \
  constexpr IntegerABIInfo Name##Info{GuestArchitecture::ISA,                  \
                                      Bank##Arguments,                         \
                                      SP,                                      \
                                      Result,                                  \
                                      Link,                                    \
                                      Word,                                    \
                                      Alignment,                               \
                                      Shadow,                                  \
                                      RedZone};
#include "neverd/emulation/IntegerABI.def"
#undef NEVERD_INTEGER_ABI
constexpr uint64_t MaxAddress = std::numeric_limits<uint64_t>::max();
} // namespace

llvm::Expected<IntegerABI>
IntegerABI::get(IntegerCallingConvention Convention) {
  switch (Convention) {
#define NEVERD_INTEGER_ABI(Name, ...)                                          \
  case IntegerCallingConvention::Name:                                         \
    return IntegerABI(Name##Info);
#include "neverd/emulation/IntegerABI.def"
#undef NEVERD_INTEGER_ABI
  }
  return failure(UnknownConvention);
}

llvm::Error IntegerABI::validateCPU(const ExecutionBackend &CPU) const {
  if (CPU.architecture() != Info->Architecture)
    return failure(Architecture);
  return llvm::Error::success();
}

llvm::Error IntegerABI::validateStackPointer(uint64_t SP) const {
  if (SP % Info->StackAlignment !=
      (Info->StackAlignment - Info->returnAddressSize()) % Info->StackAlignment)
    return failure(StackAlignment);
  return llvm::Error::success();
}

llvm::Expected<IntegerCallLayout>
IntegerABI::layoutCall(uint64_t StackBase, uint64_t StackSize,
                       uint64_t ArgumentCount, uint64_t PayloadSize) const {
  if (!StackSize || StackSize > MaxAddress - StackBase)
    return failure(StackRange);
  const uint64_t Top = StackBase + StackSize;
  if (Top % Info->StackAlignment || PayloadSize % Info->StackAlignment)
    return failure(StackAlignment);
  const uint64_t StackCount = ArgumentCount > Info->Arguments.size()
                                  ? ArgumentCount - Info->Arguments.size()
                                  : 0;
  if (StackCount > MaxAddress / Info->WordSize)
    return failure(StackRange);
  const uint64_t StackBytes = StackCount * Info->WordSize;
  if (StackBytes > MaxAddress - (Info->StackAlignment - 1))
    return failure(StackRange);
  const uint64_t AlignedArguments =
      (StackBytes + Info->StackAlignment - 1) & ~(Info->StackAlignment - 1);
  // Subtraction keeps all additions below the supplied finite stack size.
  uint64_t Remaining = StackSize;
  for (uint64_t Size : {PayloadSize, AlignedArguments,
                        Info->stackArgumentOffset(), Info->RedZoneSize}) {
    if (Size > Remaining)
      return failure(StackCapacity);
    Remaining -= Size;
  }
  const uint64_t SP = StackBase + Remaining + Info->RedZoneSize;
  return IntegerCallLayout{SP, SP + Info->returnAddressSize(),
                           Top - PayloadSize};
}

llvm::Expected<IntegerArgumentLocation>
IntegerABI::argumentLocation(uint64_t SP, uint64_t Index) const {
  if (auto E = validateStackPointer(SP))
    return std::move(E);
  if (Index < Info->Arguments.size())
    return IntegerArgumentLocation{Info->Arguments[Index], 0};
  if (SP > MaxAddress - Info->stackArgumentOffset())
    return failure(StackRange);
  const uint64_t Base = SP + Info->stackArgumentOffset();
  const uint64_t Slot = Index - Info->Arguments.size();
  // Include the last byte of the slot, not just its start address.
  if (Base > MaxAddress - (Info->WordSize - 1) ||
      Slot > (MaxAddress - Base - (Info->WordSize - 1)) / Info->WordSize)
    return failure(StackRange);
  return IntegerArgumentLocation{CPURegister::Invalid,
                                 Base + Slot * Info->WordSize};
}

llvm::Expected<IntegerCallLayout>
IntegerABI::prepareCall(ExecutionBackend &CPU, uint64_t StackBase,
                        uint64_t StackSize, uint64_t ReturnPC,
                        llvm::ArrayRef<uint64_t> Arguments,
                        uint64_t PayloadSize) const {
  if (auto E = validateCPU(CPU))
    return std::move(E);
  auto Layout = layoutCall(StackBase, StackSize, Arguments.size(), PayloadSize);
  if (!Layout)
    return Layout.takeError();
  const uint64_t First = Layout->StackPointer - Info->RedZoneSize;
  const uint64_t Size = StackBase + StackSize - First;
  if (Size) {
    auto Accessible = CPU.canAccess(First, Size, Write);
    if (!Accessible)
      return Accessible.takeError();
    if (!*Accessible)
      return failure(StackAccess);
  }
  // Only the known scalar register bank is touched, never platform state
  // such as AArch64 X18, TLS, floating-point state or nonvolatile registers.
  if (Info->Link == CPURegister::Invalid) {
    if (auto E =
            CPU.writeInteger(Layout->StackPointer, ReturnPC, Info->WordSize))
      return std::move(E);
  } else if (auto E = CPU.writeRegister(Info->Link, {ReturnPC, 0})) {
    return std::move(E);
  }
  for (size_t I = Info->Arguments.size(); I < Arguments.size(); ++I) {
    auto Location = argumentLocation(Layout->StackPointer, I);
    if (!Location)
      return Location.takeError();
    if (auto E =
            CPU.writeInteger(Location->Address, Arguments[I], Info->WordSize))
      return std::move(E);
  }
  for (size_t I = 0; I < Info->Arguments.size(); ++I)
    if (auto E = CPU.writeRegister(
            Info->Arguments[I], {I < Arguments.size() ? Arguments[I] : 0, 0}))
      return std::move(E);
  if (auto E = CPU.writeRegister(Info->StackPointer, {Layout->StackPointer, 0}))
    return std::move(E);
  return *Layout;
}

llvm::Expected<uint64_t> IntegerABI::readArgument(ExecutionBackend &CPU,
                                                  uint64_t SP,
                                                  uint64_t Index) const {
  if (auto E = validateCPU(CPU))
    return std::move(E);
  auto Location = argumentLocation(SP, Index);
  if (!Location)
    return Location.takeError();
  if (Location->Register == CPURegister::Invalid)
    return CPU.readInteger(Location->Address, Info->WordSize);
  auto Value = CPU.readRegister(Location->Register);
  if (!Value)
    return Value.takeError();
  return (*Value)[0];
}

llvm::Expected<uint64_t> IntegerABI::returnStackPointer(uint64_t SP) const {
  if (auto E = validateStackPointer(SP))
    return std::move(E);
  if (SP > MaxAddress - Info->returnAddressSize())
    return failure(StackRange);
  return SP + Info->returnAddressSize();
}

llvm::Expected<uint64_t> IntegerABI::readReturnAddress(ExecutionBackend &CPU,
                                                       uint64_t SP) const {
  if (auto E = validateCPU(CPU))
    return std::move(E);
  auto ReturnSP = returnStackPointer(SP);
  if (!ReturnSP)
    return ReturnSP.takeError();
  if (Info->Link == CPURegister::Invalid)
    return CPU.readInteger(SP, Info->WordSize);
  auto Value = CPU.readRegister(Info->Link);
  if (!Value)
    return Value.takeError();
  return (*Value)[0];
}
} // namespace neverd::emulation
