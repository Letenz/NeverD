//===- LinuxMemory.cpp - Linux anonymous memory services -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxMemory.h"

#include "llvm/Support/FormatVariadic.h"

#include <algorithm>

namespace neverd::emulation::linux_model {
namespace {
uint64_t errorValue(uint64_t Number) { return uint64_t(0) - Number; }
bool freeRange(llvm::ArrayRef<AddressMapping> Ranges, uint64_t Address,
               uint64_t Size) {
  for (const auto &Range : Ranges)
    if (Address <= Range.Address ? Range.Address - Address < Size
                                 : Address - Range.Address < Range.Size)
      return false;
  return true;
}
unsigned permissions(uint64_t Protection) {
  return UserAccessible | (Protection & ProtRead ? Read : 0u) |
         (Protection & ProtWrite ? Write : 0u) |
         (Protection & ProtExecute ? Execute : 0u);
}
bool supportedProtection(uint64_t Protection) {
  // Write-only and execute-only differ across hardware/personality/pkey
  // policies. Admit only the explicit common readable combinations or NONE.
  return !(Protection & ~(ProtRead | ProtWrite | ProtExecute)) &&
         (!Protection || (Protection & ProtRead));
}
} // namespace

LinuxMemory::LinuxMemory(AddressSpace &Space, const MemoryLayout &Layout,
                         uint64_t InitialBreak, const ProcessOptions &Options)
    : Space(Space), PageSize(Layout.PageSize), UserLimit(Layout.UserLimit),
      Limit(Options.MemoryLimit), MinimumBreak(InitialBreak),
      GuardBase(StackTop - Options.StackSize - PageSize),
      GuardEnd(StackTop + PageSize), ProgramBreak(InitialBreak) {}

std::optional<uint64_t> LinuxMemory::roundSize(uint64_t Size) const {
  if (Size > UINT64_MAX - (PageSize - 1))
    return std::nullopt;
  return (Size + PageSize - 1) & ~(PageSize - 1);
}
bool LinuxMemory::validRange(uint64_t Address, uint64_t Size) const {
  return Address < UserLimit && Size <= UserLimit - Address;
}
llvm::Expected<std::vector<AddressMapping>>
LinuxMemory::reservedRanges() const {
  auto Ranges = Space.mappings();
  if (!Ranges)
    return Ranges.takeError();
  // Stack guard policy is an OS reservation, not a second page table.
  Ranges->push_back({GuardBase, GuardEnd - GuardBase, 0, false});
  std::sort(Ranges->begin(), Ranges->end(),
            [](const auto &A, const auto &B) { return A.Address < B.Address; });
  return Ranges;
}
std::optional<uint64_t>
LinuxMemory::findGap(llvm::ArrayRef<AddressMapping> Ranges, uint64_t Start,
                     uint64_t Size) const {
  uint64_t Candidate = std::max(Start, MinimumAddress);
  for (const auto &Range : Ranges) {
    if (!validRange(Candidate, Size))
      return std::nullopt;
    if (Range.Address > Candidate && Size <= Range.Address - Candidate)
      return Candidate;
    if (Range.Address <= Candidate && Candidate - Range.Address >= Range.Size)
      continue;
    if (!validRange(Range.Address, Range.Size))
      return std::nullopt;
    Candidate = Range.Address + Range.Size;
  }
  return validRange(Candidate, Size) ? std::optional<uint64_t>(Candidate)
                                     : std::nullopt;
}

llvm::Expected<bool> LinuxMemory::mapPages(uint64_t Address, uint64_t Size,
                                           unsigned Permissions) {
  auto RAM = Space.physicalMemory();
  if (Space.mappedBytes() > Limit || Size > Limit - Space.mappedBytes() ||
      RAM->allocatedBytes() > Limit || Size > Limit - RAM->allocatedBytes())
    return false;
  // Each anonymous page has an independent allocation lifetime. A partial
  // munmap must not retain the entire original mmap allocation. Allocate all
  // owners before publishing mappings; predictable shortage has no effects.
  std::vector<std::shared_ptr<MemoryRegion>> Pages;
  Pages.reserve(Size / PageSize);
  for (uint64_t Offset = 0; Offset < Size; Offset += PageSize) {
    auto Region = RAM->allocate(PageSize);
    if (!Region) {
      auto E = Region.takeError();
      if (!E.isA<GuestMemoryLimitError>())
        return std::move(E);
      llvm::consumeError(std::move(E));
      return false;
    }
    Pages.push_back(std::move(*Region));
  }
  uint64_t Mapped = 0;
  for (auto &Page : Pages) {
    if (auto E =
            Space.mapRegion(Address + Mapped, Page, 0, PageSize, Permissions)) {
      if (Mapped)
        E = llvm::joinErrors(std::move(E), Space.unmap(Address, Mapped));
      return std::move(E);
    }
    Mapped += PageSize;
  }
  return true;
}
llvm::Expected<bool> LinuxMemory::unmapPages(uint64_t Address, uint64_t Size) {
  auto Ranges = Space.mappings();
  if (!Ranges)
    return Ranges.takeError();
  bool Removed = false;
  for (const auto &Range : *Ranges) {
    const uint64_t Begin = std::max(Address, Range.Address);
    const uint64_t End = std::min(Address + Size, Range.Address + Range.Size);
    if (Begin >= End)
      continue;
    if (Range.Device)
      return failure(MemoryState);
    if (auto E = Space.unmap(Begin, End - Begin))
      return std::move(E);
    Removed = true;
  }
  return Removed;
}

llvm::Expected<uint64_t> LinuxMemory::map(const ProcessServiceEvent &Event) {
  const auto [Hint, Length, Protection, Flags, FD, Offset] = Event.Arguments;
  if (!Length || Offset % PageSize)
    return errorValue(InvalidArgument);
  auto Size = roundSize(Length);
  if (!Size || *Size > Limit)
    return errorValue(NoMemory);
  auto Ranges = reservedRanges();
  if (!Ranges)
    return Ranges.takeError();
  uint64_t Address = Hint & ~(PageSize - 1);
  if (Address < MinimumAddress || !validRange(Address, *Size) ||
      !freeRange(*Ranges, Address, *Size)) {
    auto Gap = findGap(*Ranges, MmapBase, *Size);
    if (!Gap)
      Gap = findGap(*Ranges, MinimumAddress, *Size);
    if (!Gap)
      return errorValue(NoMemory);
    Address = *Gap;
  }
  auto Mapped = mapPages(Address, *Size, permissions(Protection));
  if (!Mapped)
    return Mapped.takeError();
  return *Mapped ? Address : errorValue(NoMemory);
}

llvm::Expected<uint64_t>
LinuxMemory::protect(const ProcessServiceEvent &Event) {
  const uint64_t Address = Event.Arguments[0], Length = Event.Arguments[1];
  if (Address % PageSize)
    return errorValue(InvalidArgument);
  if (!Length)
    return 0;
  auto Size = roundSize(Length);
  if (!Size || !validRange(Address, *Size))
    return errorValue(NoMemory);
  auto Ranges = Space.mappings();
  if (!Ranges)
    return Ranges.takeError();
  uint64_t Cursor = Address;
  const uint64_t End = Address + *Size;
  for (const auto &Range : *Ranges) {
    if (Range.Address > Cursor)
      break;
    if (Cursor - Range.Address >= Range.Size)
      continue;
    if (Range.Device)
      return failure(MemoryState);
    const uint64_t Count =
        std::min(End - Cursor, Range.Size - (Cursor - Range.Address));
    if (auto E = Space.protect(Cursor, Count, permissions(Event.Arguments[2])))
      return std::move(E);
    Cursor += Count;
    if (Cursor == End)
      return 0;
  }
  // Linux applies each preceding VMA change before encountering a gap.
  // Do not roll back that visible prefix when returning ENOMEM.
  return errorValue(NoMemory);
}
llvm::Expected<uint64_t> LinuxMemory::unmap(const ProcessServiceEvent &Event) {
  const uint64_t Address = Event.Arguments[0], Length = Event.Arguments[1];
  auto Size = roundSize(Length);
  if (Address % PageSize || !Length || !Size || !validRange(Address, *Size))
    return errorValue(InvalidArgument);
  auto Removed = unmapPages(Address, *Size);
  if (!Removed)
    return Removed.takeError();
  return 0;
}
llvm::Expected<uint64_t> LinuxMemory::setBreak(uint64_t Address) {
  if (Address < MinimumBreak || Address >= UserLimit)
    return ProgramBreak;
  const uint64_t Before = *roundSize(ProgramBreak), After = *roundSize(Address);
  if (Before == After) {
    ProgramBreak = Address;
    return ProgramBreak;
  }
  if (After < Before) {
    auto Removed = unmapPages(After, Before - After);
    if (!Removed)
      return Removed.takeError();
    if (!*Removed)
      return ProgramBreak;
  } else {
    auto Ranges = reservedRanges();
    if (!Ranges)
      return Ranges.takeError();
    // Linux requires a page of separation from the next unrelated mapping.
    if (!validRange(Before, After - Before + PageSize) ||
        !freeRange(*Ranges, Before, After - Before + PageSize))
      return ProgramBreak;
    auto Mapped =
        mapPages(Before, After - Before, Read | Write | UserAccessible);
    if (!Mapped)
      return Mapped.takeError();
    if (!*Mapped)
      return ProgramBreak;
  }
  ProgramBreak = Address;
  return ProgramBreak;
}

llvm::Expected<std::optional<uint64_t>>
LinuxMemory::handle(ServiceKind Kind, const ProcessServiceEvent &Event,
                    ProcessResult &Result) {
  if (((Kind == ServiceKind::Mmap || Kind == ServiceKind::Mprotect) &&
       !supportedProtection(Event.Arguments[2])) ||
      (Kind == ServiceKind::Mmap &&
       Event.Arguments[3] != (MapPrivate | MapAnonymous))) {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = llvm::formatv(MemoryOperation, Event.Number,
                                      Event.Arguments[2], Event.Arguments[3])
                            .str();
    return std::optional<uint64_t>();
  }
  auto Value = [&]() -> llvm::Expected<uint64_t> {
    switch (Kind) {
    case ServiceKind::Mmap:
      return map(Event);
    case ServiceKind::Mprotect:
      return protect(Event);
    case ServiceKind::Munmap:
      return unmap(Event);
    case ServiceKind::Brk:
      return setBreak(Event.Arguments[0]);
    default:
      return failure(MemoryState);
    }
  }();
  if (!Value)
    return Value.takeError();
  return std::optional<uint64_t>(*Value);
}
} // namespace neverd::emulation::linux_model
