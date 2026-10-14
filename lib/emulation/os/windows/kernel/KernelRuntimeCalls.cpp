//===- KernelRuntimeCalls.cpp - Unicode and debug calls -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "KernelModel.h"
#include "KernelModelRuntime.h"
#include "WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
using namespace windows;
llvm::Error modelError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}
// Every Windows ANSI code page and UTF-8 encode ASCII identically. Other text,
// including its converted length, depends on the unmodeled system code page.
constexpr uint8_t MaxASCIIUnit = 0x7f;
} // namespace

llvm::Expected<uint64_t>
KernelModel::initializeUnicodeString(llvm::ArrayRef<uint64_t> A) {
  if (auto E = runtime::checkBufferRange(A[0], 16))
    return E;
  uint64_t Length = 0;
  if (A[1]) {
    if (auto E = runtime::checkBufferRange(A[1], MaxUnicodeBytes + 2))
      return E;
    while (Length <= MaxUnicodeBytes) {
      if (auto E = validateGuestAccess(A[1] + Length, 2, false))
        return E;
      auto Unit = Memory.readInteger(A[1] + Length, 2);
      if (!Unit)
        return Unit.takeError();
      if (!*Unit)
        break;
      Length += 2;
    }
    if (Length > MaxUnicodeBytes)
      return modelError("RtlInitUnicodeString source exceeds model capacity");
  }
  if (auto E = validateGuestAccess(A[0], 4, true))
    return E;
  if (auto E = validateGuestAccess(A[0] + 8, 8, true))
    return E;
  if (auto E = Memory.writeInteger(A[0], Length, 2))
    return E;
  if (auto E = Memory.writeInteger(A[0] + 2, A[1] ? Length + 2 : 0, 2))
    return E;
  if (auto E = Memory.writeInteger(A[0] + 8, A[1], 8))
    return E;
  return 0;
}

llvm::Expected<uint64_t>
KernelModel::ansiToUnicodeString(llvm::ArrayRef<uint64_t> A) {
  // RtlAnsiStringToUnicodeString (RVA 0x6f6840) stores Length before checking
  // capacity and replaces Buffer when it allocates, even on failure.
  if (auto E = validateGuestAccess(A[1], UnicodeRecordSize, false))
    return E;
  auto Length = Memory.readInteger(A[1], 2);
  if (!Length)
    return Length.takeError();
  auto Source = Memory.readInteger(A[1] + UnicodeBufferOffset, 8);
  if (!Source)
    return Source.takeError();
  std::vector<uint8_t> Text(*Length);
  if (!Text.empty()) {
    if (auto E = validateGuestAccess(*Source, Text.size(), false))
      return E;
    if (auto E = Memory.read(*Source, Text))
      return E;
  }
  if (llvm::any_of(Text, [](uint8_t Byte) { return Byte > MaxASCIIUnit; }))
    return modelError("RtlAnsiStringToUnicodeString of non-ASCII text "
                      "requires an unmodeled ANSI code page");
  const uint64_t Size = Text.size() * 2 + 2;
  if (Size > runtime::MaxCountedUnicodeBytes)
    return StatusInvalidParameter2;
  if (auto E = validateGuestAccess(A[0], UnicodeRecordSize, true))
    return E;
  if (auto E = Memory.writeInteger(A[0], Size - 2, 2))
    return E;
  uint64_t Buffer = 0;
  if (uint8_t(A[2])) {
    auto Pointer = allocatePool({PoolPaged, Size, RuntimeStringPoolTag}, false);
    if (!Pointer)
      return Pointer.takeError();
    if (auto E = Memory.writeInteger(A[0] + UnicodeMaximumOffset, Size, 2))
      return E;
    if (auto E = Memory.writeInteger(A[0] + UnicodeBufferOffset, *Pointer, 8))
      return E;
    if (!*Pointer)
      return StatusNoMemory;
    Buffer = *Pointer;
  } else {
    auto Maximum = Memory.readInteger(A[0] + UnicodeMaximumOffset, 2);
    if (!Maximum)
      return Maximum.takeError();
    if (Size > *Maximum)
      return StatusBufferOverflow;
    auto Pointer = Memory.readInteger(A[0] + UnicodeBufferOffset, 8);
    if (!Pointer)
      return Pointer.takeError();
    Buffer = *Pointer;
  }
  std::vector<uint8_t> Wide(Size);
  for (size_t I = 0; I < Text.size(); ++I)
    Wide[I * 2] = Text[I];
  if (auto E = validateGuestAccess(Buffer, Wide.size(), true))
    return E;
  if (auto E = Memory.write(Buffer, Wide))
    return E;
  return StatusSuccess;
}

llvm::Expected<uint64_t>
KernelModel::unicodeToAnsiString(llvm::ArrayRef<uint64_t> A) {
  // RtlUnicodeStringToAnsiString (RVA 0x5edb00) stores Length first. Without
  // allocation it truncates to the capacity, still converts and terminates,
  // and reports STATUS_BUFFER_OVERFLOW.
  if (auto E = validateGuestAccess(A[1], UnicodeRecordSize, false))
    return E;
  auto Length = Memory.readInteger(A[1], 2);
  if (!Length)
    return Length.takeError();
  auto Source = Memory.readInteger(A[1] + UnicodeBufferOffset, 8);
  if (!Source)
    return Source.takeError();
  // A trailing odd byte is not a complete UTF-16 unit and is never converted.
  std::vector<uint8_t> Units(*Length & ~uint64_t(1));
  if (!Units.empty()) {
    if (auto E = validateGuestAccess(*Source, Units.size(), false))
      return E;
    if (auto E = Memory.read(*Source, Units))
      return E;
  }
  for (size_t I = 0; I < Units.size(); I += 2)
    if (Units[I + 1] || Units[I] > MaxASCIIUnit)
      return modelError("RtlUnicodeStringToAnsiString of non-ASCII text "
                        "requires an unmodeled ANSI code page");
  const uint64_t Count = Units.size() / 2;
  if (auto E = validateGuestAccess(A[0], UnicodeRecordSize, true))
    return E;
  if (auto E = Memory.writeInteger(A[0], Count, 2))
    return E;
  uint64_t Status = StatusSuccess;
  uint64_t Converted = Count;
  uint64_t Buffer = 0;
  if (uint8_t(A[2])) {
    auto Pointer =
        allocatePool({PoolPaged, Count + 1, RuntimeStringPoolTag}, false);
    if (!Pointer)
      return Pointer.takeError();
    if (auto E = Memory.writeInteger(A[0] + UnicodeMaximumOffset, Count + 1, 2))
      return E;
    if (auto E = Memory.writeInteger(A[0] + UnicodeBufferOffset, *Pointer, 8))
      return E;
    if (!*Pointer)
      return StatusNoMemory;
    Buffer = *Pointer;
  } else {
    auto Maximum = Memory.readInteger(A[0] + UnicodeMaximumOffset, 2);
    if (!Maximum)
      return Maximum.takeError();
    if (Count >= *Maximum) {
      if (!*Maximum)
        return StatusBufferOverflow;
      Status = StatusBufferOverflow;
      Converted = *Maximum - 1;
      if (auto E = Memory.writeInteger(A[0], Converted, 2))
        return E;
    }
    auto Pointer = Memory.readInteger(A[0] + UnicodeBufferOffset, 8);
    if (!Pointer)
      return Pointer.takeError();
    Buffer = *Pointer;
  }
  std::vector<uint8_t> Narrow(Converted + 1);
  for (uint64_t I = 0; I < Converted; ++I)
    Narrow[I] = Units[I * 2];
  if (auto E = validateGuestAccess(Buffer, Narrow.size(), true))
    return E;
  if (auto E = Memory.write(Buffer, Narrow))
    return E;
  return Status;
}

llvm::Expected<uint64_t>
KernelModel::freeCountedString(llvm::ArrayRef<uint64_t> A) {
  // RtlFreeUnicodeString and RtlFreeAnsiString (RVA 0x602cb0) free a non-null
  // Buffer without loading a pool tag, then clear the whole record.
  if (auto E = validateGuestAccess(A[0], UnicodeRecordSize, false))
    return E;
  auto Buffer = Memory.readInteger(A[0] + UnicodeBufferOffset, 8);
  if (!Buffer)
    return Buffer.takeError();
  if (!*Buffer)
    return StatusSuccess;
  if (auto E = validateGuestAccess(A[0], UnicodeRecordSize, true))
    return E;
  auto Freed = freePool({*Buffer}, false);
  if (!Freed)
    return Freed.takeError();
  if (auto E = Memory.writeInteger(A[0], 0, 8))
    return E;
  if (auto E = Memory.writeInteger(A[0] + UnicodeBufferOffset, 0, 8))
    return E;
  return StatusSuccess;
}

llvm::Expected<uint64_t> KernelModel::debugMessage(
    llvm::ArrayRef<uint64_t> A, unsigned FormatIndex,
    llvm::function_ref<llvm::Expected<uint64_t>(unsigned)> ReadArgument) {
  auto Text = runtime::formatDebugMessage(*this, Memory, A[FormatIndex],
                                          FormatIndex + 1, ReadArgument);
  if (!Text)
    return Text.takeError();
  // The observation profile enables all debugger component/level filters.
  Result.Messages.push_back(std::move(*Text));
  return StatusSuccess;
}

} // namespace neverd::emulation
