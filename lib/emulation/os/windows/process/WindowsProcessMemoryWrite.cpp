//===- WindowsProcessMemoryWrite.cpp - Bounded current-process writes -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsProcess.h"

#include "neverd/emulation/CPU.h"

#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <optional>
#include <vector>

namespace neverd::emulation::windows_process {
namespace {
using namespace value;
enum class WritePolicy { Blocked, Direct, Temporary };
std::optional<WritePolicy> policy(uint32_t Protection) {
  switch (Protection) {
#define NEVERD_WINDOWS_MEMORY_WRITE_POLICY(Protection, Policy)                 \
  case Protection:                                                             \
    return WritePolicy::Policy;
#include "WindowsProcessMemory.def"
#undef NEVERD_WINDOWS_MEMORY_WRITE_POLICY
  default:
    return std::nullopt;
  }
}
struct Span {
  uint64_t Address, Size;
  uint32_t Protection;
  WritePolicy Policy;
};
} // namespace

llvm::Expected<std::optional<uint64_t>>
Services::writeProcessMemory(const Service &S, const NativeCallEvent &Event) {
  const auto &A = Event.Arguments;
  auto WinError =
      [&](uint32_t Code) -> llvm::Expected<std::optional<uint64_t>> {
    auto V = error(Code);
    if (!V)
      return V.takeError();
    return std::optional<uint64_t>(*V);
  };
  if (A[0] != CurrentProcess)
    return WinError(ErrorInvalidHandle);
  if (A[3] > MemoryWriteLimit)
    return unsupported(S);
  if (A[4]) {
    auto Out = access(A[4], PointerSize, Write);
    if (!Out)
      return Out.takeError();
    if (!*Out)
      return WinError(ErrorNoAccess);
  }

  std::vector<uint8_t> Bytes(A[3]);
  if (A[3]) {
    auto Readable = access(A[2], A[3], Read);
    if (!Readable)
      return Readable.takeError();
    if (!*Readable)
      return WinError(ErrorNoAccess);
    if (auto E = CPU.read(A[2], Bytes))
      return std::move(E);
  }

  // Plan from the OS reservation and live AddressSpace rights while the guest
  // is stopped. An unmodeled span must not acquire invented Windows semantics.
  llvm::SmallVector<Span, MemoryWriteMaxPages> Spans;
  if (A[3] &&
      (A[1] < ImageAlignment || A[1] >= UserLimit || A[3] > UserLimit - A[1]))
    return unsupported(S);
  for (uint64_t Offset = 0; Offset < A[3];) {
    auto Info = Virtual.query(A[1] + Offset);
    if (!Info)
      return Info.takeError();
    if (!*Info || (**Info).State != MemCommit)
      return unsupported(S);
    const auto &I = **Info;
    auto Policy = policy(I.Protection);
    if (!Policy)
      return unsupported(S);
    // A blocked first page fails before publishing a byte count. A later
    // blocked page has a different contract after an actual writable prefix.
    if (!Offset && *Policy == WritePolicy::Blocked)
      return WinError(ErrorNoAccess);
    const uint64_t Skip = A[1] + Offset - I.Base;
    if (Skip >= I.Size || Spans.size() == MemoryWriteMaxPages)
      return failure(text::MemoryState);
    const uint64_t Count = std::min(A[3] - Offset, I.Size - Skip);
    Spans.push_back({A[1] + Offset, Count, I.Protection, *Policy});
    Offset += Count;
  }

  uint64_t Written = 0;
  bool PartialFailure = false;
  const bool Temporary =
      !Spans.empty() && Spans.front().Policy == WritePolicy::Temporary;
  for (const auto &Part : Spans) {
    if (Part.Policy == WritePolicy::Blocked ||
        (!Temporary && Part.Policy == WritePolicy::Temporary)) {
      // Original Windows x64/ARM64 preserves ERROR_PARTIAL_COPY after a
      // writable start. Its RX wrapper instead returns its preceding success
      // with a short count when the next region cannot be written.
      PartialFailure = !Temporary;
      break;
    }
    const bool Changed = Part.Policy == WritePolicy::Temporary;
    if (Changed) {
      auto Protected =
          Virtual.protect(Part.Address, Part.Size, PageExecuteReadWrite);
      if (!Protected)
        return Protected.takeError();
      if (Protected->Unsupported)
        return unsupported(S);
      if (Protected->Error)
        return WinError(Protected->Error);
    }
    auto Result = CPU.write(Part.Address,
                            llvm::ArrayRef(Bytes).slice(Written, Part.Size));
    if (Changed) {
      auto Restored = Virtual.protect(Part.Address, Part.Size, Part.Protection);
      llvm::Error RestoreError = llvm::Error::success();
      if (!Restored)
        RestoreError = Restored.takeError();
      else if (Restored->Error || Restored->Unsupported)
        RestoreError = failure(text::MemoryWriteRestore);
      if (RestoreError)
        return llvm::joinErrors(std::move(Result), std::move(RestoreError));
    }
    if (Result)
      return std::move(Result);
    Written += Part.Size;
  }
  if (A[4])
    if (auto E = CPU.writeInteger(A[4], Written, PointerSize))
      return std::move(E);
  if (PartialFailure)
    return WinError(ErrorPartialCopy);
  return std::optional<uint64_t>(1);
}
} // namespace neverd::emulation::windows_process
