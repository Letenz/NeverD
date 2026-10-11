//===- DarwinSelect.cpp - Immediate Darwin bitmap readiness --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded immediate select for constructive virtual regular descriptions.
//===----------------------------------------------------------------------===//
// Independently authored projection of the contract in
// docs/darwin-emulation.md.
#include "DarwinFiles.h"
#include "DarwinUserMemory.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::darwin_model {
using namespace value;
namespace {
std::optional<ServiceResult> returned(uint64_t Value, bool Error = false) {
  return ServiceResult{Value, Error};
}
std::optional<ServiceResult> unsupported(ProcessResult &Result,
                                         const char *Reason) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = Reason;
  return std::nullopt;
}
bool overlaps(uint64_t A, uint64_t ASize, uint64_t B, uint64_t BSize) {
  if (!ASize || !BSize)
    return false;
  return A <= B ? B - A < ASize : A - B < BSize;
}
} // namespace

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::select(const ProcessServiceEvent &Event, ProcessResult &Result) {
  const auto &A = Event.Arguments;
  const int32_t Count = int32_t(A[0]);
  bool Immediate = false;
  if (A[4]) {
    std::array<uint8_t, 16> Time;
    auto Prefix = userMemoryPrefix(Memory, A[4], Time.size(), Read);
    if (!Prefix)
      return Prefix.takeError();
    if (*Prefix != Time.size())
      return returned(BadAddress, true);
    if (auto E = Memory.read(A[4], Time))
      return std::move(E);
    const int64_t Seconds =
        int64_t(llvm::support::endian::read64le(Time.data()));
    const int32_t Microseconds =
        int32_t(llvm::support::endian::read32le(Time.data() + 8));
    if (Seconds < 0 || uint64_t(Seconds) > SelectMaxSeconds ||
        Microseconds < 0 || Microseconds >= 1000000)
      return returned(InvalidArgument, true);
    Immediate = !Seconds && !Microseconds;
  }
  if (Count < 0)
    return returned(InvalidArgument, true);
  if (!Immediate)
    return unsupported(Result, diagnostic::SelectWait);
  if (!Count)
    return returned(0);

  // A live key proves a sufficient fd_nfiles lower bound, not an allocation
  // size or a capacity that shrinks on close. NOFILE and budgets are unrelated.
  if (Descriptors.empty() ||
      uint64_t(Count) > uint64_t(Descriptors.rbegin()->first) + 1)
    return unsupported(Result, diagnostic::SelectCapacity);
  const uint64_t Size = (uint64_t(Count) + 31) / 32 * 4;
  const std::array<uint64_t, 3> Address{A[1], A[2], A[3]};
  std::array<std::vector<uint8_t>, 3> Input, Output;
  for (size_t I = 0; I != Address.size(); ++I) {
    if (!Address[I])
      continue;
    auto Prefix = userMemoryPrefix(Memory, Address[I], Size, Read);
    if (!Prefix)
      return Prefix.takeError();
    if (*Prefix != Size)
      return returned(BadAddress, true);
    Input[I].resize(Size);
    if (auto E = Memory.read(Address[I], Input[I]))
      return std::move(E);
  }
  const auto Selected = [&](size_t I, uint32_t FD) {
    return !Input[I].empty() &&
           (llvm::support::endian::read32le(Input[I].data() + FD / 32 * 4) &
            (uint32_t{1} << (FD % 32)));
  };
  // All bitmap copyins precede all descriptor checks; all descriptor checks
  // precede provider interpretation, even for an earlier unknown description.
  for (size_t I = 0; I != Address.size(); ++I)
    for (uint32_t FD = 0; FD != uint32_t(Count); ++FD)
      if (Selected(I, FD) && !Descriptors.contains(FD))
        return returned(BadDescriptor, true);
  uint32_t Ready = 0;
  for (size_t I = 0; I != Address.size(); ++I) {
    if (!Address[I])
      continue;
    Output[I].assign(Size, 0);
    for (uint32_t FD = 0; FD != uint32_t(Count); ++FD) {
      if (!Selected(I, FD))
        continue;
      // This model constructs a pure, nonrefusing regular select provider
      // with nonzero results for read, write and exception. Native kind alone
      // does not establish that premise, nor does poll's filter mask.
      if (Descriptors.at(FD).Open->Type != Kind::File)
        return unsupported(Result, diagnostic::SelectObject);
      auto *Word = Output[I].data() + FD / 32 * 4;
      llvm::support::endian::write32le(Word,
                                       llvm::support::endian::read32le(Word) |
                                           (uint32_t{1} << (FD % 32)));
      ++Ready;
    }
  }
  for (size_t I = 0; I != Address.size(); ++I) {
    if (!Address[I])
      continue;
    auto Prefix = userMemoryPrefix(Memory, Address[I], Size, Write);
    if (!Prefix)
      return Prefix.takeError();
    if (*Prefix == Size)
      continue;
    if (*Prefix)
      return unsupported(Result, diagnostic::SelectPartialOutput);
    // A zero prefix says nothing about a writable tail. Inspect the entire
    // bounded range before admitting it as wholly unwritable. The descriptor
    // ceiling bounds each bitmap to at most 512 pure permission queries.
    for (uint64_t Offset = 0; Offset != Size; ++Offset) {
      auto Access =
          Memory.canAccess(Address[I] + Offset, 1, Write | UserAccessible);
      if (!Access)
        return Access.takeError();
      if (*Access)
        return unsupported(Result, diagnostic::SelectPartialOutput);
    }
  }
  for (size_t I = 0; I != Address.size(); ++I) {
    if (!Address[I])
      continue;
    if (overlaps(Address[I], Size, A[4], 16))
      return unsupported(Result, diagnostic::SelectOverlap);
    for (size_t J = I + 1; J != Address.size(); ++J)
      if (Address[J] && overlaps(Address[I], Size, Address[J], Size))
        return unsupported(Result, diagnostic::SelectOverlap);
  }
  std::optional<ServiceResult> CopyError;
  for (size_t I = 0; I != Address.size(); ++I) {
    if (!Address[I])
      continue;
    auto Copied = copyUserMemory(Memory, Address[I], Output[I],
                                 diagnostic::SelectPartialOutput, Result);
    if (!Copied || !*Copied)
      return Copied;
    if ((**Copied).Error && !CopyError)
      CopyError = **Copied;
  }
  if (CopyError)
    return CopyError;
  return returned(Ready);
}
} // namespace neverd::emulation::darwin_model
