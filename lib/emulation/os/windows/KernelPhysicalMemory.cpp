//===- KernelPhysicalMemory.cpp - RAM pages and allocation ownership ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Stable simulated physical pages and exact live allocation/pin authority.
/// Data stays in GuestMemory; device access never changes CPU permissions.
///
//===----------------------------------------------------------------------===//

#include "KernelPhysicalMemory.h"

#include "neverd/emulation/AddressSpace.h"

#include "llvm/Support/ErrorHandling.h"

#include <algorithm>

namespace neverd::emulation {
char PhysicalMemoryLimitError::ID;
void PhysicalMemoryLimitError::log(llvm::raw_ostream &OS) const {
  OS << Message;
}
std::error_code PhysicalMemoryLimitError::convertToErrorCode() const {
  return llvm::inconvertibleErrorCode();
}
namespace {
namespace physical_diagnostic {
#define NEVERD_PHYSICAL_DIAGNOSTIC(Name, Message)                              \
  constexpr char Name[] = Message;
#include "KernelPhysicalMemoryDiagnostics.def"
#undef NEVERD_PHYSICAL_DIAGNOSTIC
} // namespace physical_diagnostic
llvm::Error physicalError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}
uint64_t pageBase(uint64_t Address) {
  return Address & ~(physical::PageSize - 1);
}

bool validCacheType(KernelPhysicalMemory::CacheType Cache) {
  switch (Cache) {
  case KernelPhysicalMemory::CacheType::Cached:
  case KernelPhysicalMemory::CacheType::NonCached:
  case KernelPhysicalMemory::CacheType::WriteCombined:
    return true;
  }
  return false;
}
} // namespace

const KernelPhysicalMemory::Owners *KernelPhysicalMemory::activeOwners() const {
  auto Space = Memory.addressSpace();
  if (!Space)
    return nullptr;
  auto I = OwnersBySpace.find(Space->identity());
  return I == OwnersBySpace.end() ? nullptr : &I->second;
}
llvm::Expected<KernelPhysicalMemory::PageKey>
KernelPhysicalMemory::pageKey(uint64_t Address) const {
  auto Space = Memory.addressSpace();
  if (!Space)
    return physicalError(physical_diagnostic::AddressSpaceRequired);
  auto View = Space->pinBacking(Address, 1);
  if (!View)
    return View.takeError();
  const auto &Slice = View->slices().front();
  return PageKey{Slice.Region, pageBase(Slice.Offset)};
}
KernelPhysicalMemory::PageKey
KernelPhysicalMemory::pageKey(const Region &Region, uint64_t Address) const {
  uint64_t Offset = Address <= Region.Backing ? 0 : Address - Region.Backing;
  for (const auto &Slice : Region.Storage.slices()) {
    if (Offset < Slice.Size)
      return {Slice.Region, pageBase(Slice.Offset + Offset)};
    Offset -= Slice.Size;
  }
  llvm_unreachable(physical_diagnostic::PageOutsideOwner);
}
bool KernelPhysicalMemory::containsPage(const MemoryView &View,
                                        const PageKey &Key) const {
  return std::any_of(View.slices().begin(), View.slices().end(),
                     [&](const MemorySlice &Slice) {
                       return !Key.first.owner_before(Slice.Region) &&
                              !Slice.Region.owner_before(Key.first) &&
                              Key.second >= pageBase(Slice.Offset) &&
                              Key.second <=
                                  pageBase(Slice.Offset + Slice.Size - 1);
                     });
}

const KernelPhysicalMemory::Region *
KernelPhysicalMemory::find(uint64_t Owner) const {
  const auto I = Regions.find(Owner);
  return I == Regions.end() ? nullptr : &I->second;
}

llvm::Expected<std::vector<uint64_t>>
KernelPhysicalMemory::planRegion(uint64_t Owner, uint64_t Backing,
                                 uint64_t Size, CacheType Cache) const {
  if (!validCacheType(Cache))
    return physicalError(physical_diagnostic::InvalidCache);
  if (!Owner || UsedOwners.count(Owner))
    return physicalError(physical_diagnostic::InvalidOwner);
  if (!Size || Size > UINT64_MAX - Backing)
    return physicalError(physical_diagnostic::InvalidRegion);
  if (UsedOwners.size() >= physical::RegionLimit)
    return llvm::make_error<PhysicalMemoryLimitError>(
        physical_diagnostic::OwnerLimit);
  auto Storage = Memory.pinBacking(Backing, Size);
  if (!Storage)
    return Storage.takeError();
  if (const auto *Active = activeOwners()) {
    for (auto I = Active->begin(); I != Active->lower_bound(Backing + Size);
         ++I) {
      const auto &Existing = Regions.at(I->second);
      const uint64_t First = std::max(Backing, Existing.Backing);
      const uint64_t End =
          std::min(Backing + Size, Existing.Backing + Existing.Size);
      if (First >= End)
        continue;
      const auto Old = llvm::cantFail(
          Existing.Storage.subview(First - Existing.Backing, End - First));
      const auto New =
          llvm::cantFail(Storage->subview(First - Backing, End - First));
      if (Old.overlaps(New))
        return physicalError(physical_diagnostic::OwnerOverlap);
    }
  }
  const Region Proposed{Backing, Size, false, std::move(*Storage)};
  std::vector<uint64_t> NewPages;
  std::set<PageKey, PageKeyLess> Added;
  const uint64_t Last = pageBase(Backing + Size - 1);
  const uint64_t Limit = physical::PhysicalSize / physical::PageSize;
  for (uint64_t Page = pageBase(Backing);; Page += physical::PageSize) {
    const auto Key = pageKey(Proposed, Page);
    const auto Existing = Pages.find(Key);
    if (Existing != Pages.end() && Existing->second.Cache != Cache)
      return physicalError(physical_diagnostic::CacheConflict);
    if (Existing == Pages.end() && Added.insert(Key).second) {
      if (Pages.size() + NewPages.size() >= Limit)
        return llvm::make_error<PhysicalMemoryLimitError>(
            physical_diagnostic::PageLimit);
      NewPages.push_back(Page);
    }
    if (Page == Last)
      break;
  }
  return NewPages;
}

llvm::Error KernelPhysicalMemory::canRegisterRegion(uint64_t Owner,
                                                    uint64_t Backing,
                                                    uint64_t Size,
                                                    CacheType Cache) const {
  auto Plan = planRegion(Owner, Backing, Size, Cache);
  if (!Plan)
    return Plan.takeError();
  return llvm::Error::success();
}

llvm::Error KernelPhysicalMemory::registerRegion(uint64_t Owner,
                                                 uint64_t Backing,
                                                 uint64_t Size,
                                                 CacheType Cache) {
  auto Plan = planRegion(Owner, Backing, Size, Cache);
  if (!Plan)
    return Plan.takeError();
  auto Storage = Memory.pinBacking(Backing, Size);
  if (!Storage)
    return Storage.takeError();
  Region OwnerRegion{Backing, Size, false, std::move(*Storage)};
  std::set<uint64_t> Used;
  for (const auto &[BackingPage, Page] : Pages)
    Used.insert(Page.Physical);
  uint64_t Candidate = physical::PhysicalBase;
  for (uint64_t Page : *Plan) {
    while (Used.contains(Candidate))
      Candidate += physical::PageSize;
    Pages.emplace(pageKey(OwnerRegion, Page), PageRecord{Candidate, Cache});
    Used.insert(Candidate);
  }
  OwnersBySpace[OwnerRegion.Storage.addressSpaceIdentity()].emplace(Backing,
                                                                    Owner);
  Regions.emplace(Owner, std::move(OwnerRegion));
  UsedOwners.insert(Owner);
  return llvm::Error::success();
}

llvm::Expected<std::vector<uint64_t>>
KernelPhysicalMemory::planAllocatedPages(uint64_t Low, uint64_t High,
                                         uint64_t Skip, uint64_t PageCount,
                                         uint64_t ChunkPages) const {
  if (Low > High || Skip % physical::PageSize || !PageCount ||
      (ChunkPages && (PageCount % ChunkPages ||
                      (Skip && ((Skip & (Skip - 1)) ||
                                ChunkPages != Skip / physical::PageSize)))))
    return physicalError(physical_diagnostic::AllocationBounds);
  std::set<uint64_t> Used;
  for (const auto &[Backing, Page] : Pages)
    Used.insert(Page.Physical);
  auto Eligible = [&](uint64_t Page) {
    if (Page < Low || Used.contains(Page))
      return false;
    uint64_t Offset = Page - Low;
    if (Skip)
      Offset %= Skip;
    const uint64_t Width = High - Low;
    return Offset <= Width && physical::PageSize - 1 <= Width - Offset;
  };
  std::vector<uint64_t> Selected;
  const uint64_t End = physical::PhysicalBase + physical::PhysicalSize;
  if (!ChunkPages) {
    for (uint64_t Page = physical::PhysicalBase;
         Page < End && Selected.size() < PageCount; Page += physical::PageSize)
      if (Eligible(Page))
        Selected.push_back(Page);
    return Selected;
  }
  if (ChunkPages > physical::PhysicalSize / physical::PageSize)
    return Selected;
  const uint64_t ChunkSize = ChunkPages * physical::PageSize;
  for (uint64_t Page = physical::PhysicalBase;
       ChunkSize <= End - Page && Selected.size() < PageCount;) {
    bool Available = !Skip || Page % ChunkSize == 0;
    for (uint64_t Offset = 0; Available && Offset < ChunkSize;
         Offset += physical::PageSize)
      Available = Eligible(Page + Offset);
    if (!Available) {
      Page += physical::PageSize;
      continue;
    }
    for (uint64_t Offset = 0; Offset < ChunkSize; Offset += physical::PageSize)
      Selected.push_back(Page + Offset);
    Page += ChunkSize;
  }
  return Selected;
}

llvm::Error KernelPhysicalMemory::registerAllocatedPages(
    uint64_t Owner, uint64_t Backing, llvm::ArrayRef<uint64_t> PhysicalPages,
    std::optional<CacheType> Cache) {
  if (PhysicalPages.empty() ||
      PhysicalPages.size() > physical::PhysicalSize / physical::PageSize ||
      Backing % physical::PageSize || (Cache && !validCacheType(*Cache)))
    return physicalError(physical_diagnostic::InvalidAllocation);
  const uint64_t Size = PhysicalPages.size() * physical::PageSize;
  auto Plan =
      planRegion(Owner, Backing, Size, Cache.value_or(CacheType::Cached));
  if (!Plan)
    return Plan.takeError();
  if (Plan->size() != PhysicalPages.size())
    return physicalError(physical_diagnostic::PrivateBackingRequired);
  std::set<uint64_t> Used;
  for (const auto &[Address, Page] : Pages)
    Used.insert(Page.Physical);
  for (uint64_t Page : PhysicalPages) {
    if (Page < physical::PhysicalBase ||
        Page - physical::PhysicalBase >= physical::PhysicalSize ||
        Page % physical::PageSize || !Used.insert(Page).second)
      return physicalError(physical_diagnostic::UnavailablePage);
  }
  auto Storage = Memory.pinBacking(Backing, Size);
  if (!Storage)
    return Storage.takeError();
  Region OwnerRegion{Backing, Size, true, std::move(*Storage)};
  for (size_t I = 0; I != PhysicalPages.size(); ++I)
    Pages.emplace(pageKey(OwnerRegion, Backing + I * physical::PageSize),
                  PageRecord{PhysicalPages[I], Cache, true});
  OwnersBySpace[OwnerRegion.Storage.addressSpaceIdentity()].emplace(Backing,
                                                                    Owner);
  Regions.emplace(Owner, std::move(OwnerRegion));
  UsedOwners.insert(Owner);
  return llvm::Error::success();
}

llvm::Expected<KernelPhysicalMemory::CacheType>
KernelPhysicalMemory::cacheTypeForMapping(uint64_t Backing, uint64_t Size,
                                          CacheType Requested) const {
  if (!validCacheType(Requested))
    return physicalError(physical_diagnostic::InvalidMappingCache);
  auto Owner = ownerForRange(Backing, Size);
  if (!Owner)
    return Owner.takeError();
  std::optional<CacheType> Existing;
  const uint64_t Last = pageBase(Backing + Size - 1);
  for (uint64_t Page = pageBase(Backing);; Page += physical::PageSize) {
    const auto Cache = Pages.at(pageKey(Regions.at(*Owner), Page)).Cache;
    if (Cache) {
      if (Existing && Existing != Cache)
        return physicalError(physical_diagnostic::InconsistentCache);
      Existing = Cache;
    }
    if (Page == Last)
      break;
  }
  return Existing.value_or(Requested);
}

llvm::Error KernelPhysicalMemory::commitMappingCache(uint64_t Backing,
                                                     uint64_t Size,
                                                     CacheType Cache) {
  auto Effective = cacheTypeForMapping(Backing, Size, Cache);
  if (!Effective)
    return Effective.takeError();
  if (*Effective != Cache)
    return physicalError(physical_diagnostic::CacheCommitMismatch);
  auto Owner = ownerForRange(Backing, Size);
  if (!Owner)
    return Owner.takeError();
  const uint64_t Last = pageBase(Backing + Size - 1);
  for (uint64_t Page = pageBase(Backing);; Page += physical::PageSize) {
    Pages.at(pageKey(Regions.at(*Owner), Page)).Cache = Cache;
    if (Page == Last)
      break;
  }
  return llvm::Error::success();
}

llvm::Error KernelPhysicalMemory::canRetire(uint64_t Owner,
                                            uint64_t IgnoredPin) const {
  if (!find(Owner))
    return physicalError(physical_diagnostic::RetirementOwner);
  if (IgnoredPin) {
    const auto I = Pins.find(IgnoredPin);
    if (I == Pins.end() || I->second.Owner != Owner)
      return physicalError(physical_diagnostic::IgnoredOwnerPin);
  }
  for (const auto &[ID, Pin] : Pins)
    if (Pin.Owner == Owner && ID != IgnoredPin)
      return physicalError(physical_diagnostic::PinnedOwner);
  return llvm::Error::success();
}

llvm::Error KernelPhysicalMemory::retire(uint64_t Owner) {
  if (auto E = canRetire(Owner))
    return E;
  auto Region = std::move(Regions.at(Owner));
  Regions.erase(Owner);
  auto &Owners = OwnersBySpace.at(Region.Storage.addressSpaceIdentity());
  const auto [First, End] = Owners.equal_range(Region.Backing);
  for (auto I = First; I != End; ++I)
    if (I->second == Owner) {
      Owners.erase(I);
      break;
    }
  if (Owners.empty())
    OwnersBySpace.erase(Region.Storage.addressSpaceIdentity());
  for (auto I = Pages.begin(); I != Pages.end();) {
    if (!I->second.Reusable) {
      ++I;
      continue;
    }
    const bool Referenced =
        std::any_of(Regions.begin(), Regions.end(), [&](const auto &Entry) {
          return containsPage(Entry.second.Storage, I->first);
        });
    if (Referenced)
      ++I;
    else
      I = Pages.erase(I);
  }
  return llvm::Error::success();
}

llvm::Error KernelPhysicalMemory::canReleaseRange(uint64_t Backing,
                                                  uint64_t Size,
                                                  uint64_t IgnoredPin) const {
  if (!Size || Size > UINT64_MAX - Backing)
    return physicalError(physical_diagnostic::InvalidRelease);
  if (IgnoredPin) {
    auto Space = Memory.addressSpace();
    if (!Space)
      return physicalError(physical_diagnostic::AddressSpaceRequired);
    auto Released = Space->pinBacking(Backing, Size);
    if (!Released)
      return Released.takeError();
    const auto I = Pins.find(IgnoredPin);
    if (I == Pins.end() ||
        !Released->overlaps(Regions.at(I->second.Owner).Storage))
      return physicalError(physical_diagnostic::IgnoredRangePin);
  }
  return canReleaseRanges({{Backing, Size}},
                          IgnoredPin ? llvm::ArrayRef<uint64_t>(IgnoredPin)
                                     : llvm::ArrayRef<uint64_t>());
}

llvm::Error KernelPhysicalMemory::canReleaseRanges(
    llvm::ArrayRef<std::pair<uint64_t, uint64_t>> Ranges,
    llvm::ArrayRef<uint64_t> RetiringPins) const {
  std::set<uint64_t> Retiring;
  for (uint64_t Pin : RetiringPins) {
    if (auto E = canUnpin(Pin))
      return E;
    if (!Retiring.insert(Pin).second)
      return physicalError(physical_diagnostic::RepeatedRetiringPin);
  }
  for (const auto &[Backing, Size] : Ranges) {
    if (!Size || Size > UINT64_MAX - Backing)
      return physicalError(physical_diagnostic::InvalidRelease);
    auto Space = Memory.addressSpace();
    if (!Space)
      return physicalError(physical_diagnostic::AddressSpaceRequired);
    auto Released = Space->pinBacking(Backing, Size);
    if (!Released)
      return Released.takeError();
    for (const auto &[ID, Pin] : Pins) {
      if (Retiring.count(ID))
        continue;
      const auto &Region = Regions.at(Pin.Owner);
      if (Released->overlaps(Region.Storage))
        return physicalError(physical_diagnostic::PinnedRelease);
    }
  }
  return llvm::Error::success();
}

llvm::Expected<uint64_t>
KernelPhysicalMemory::viewBacking(uint64_t Owner, uint64_t Offset,
                                  uint64_t Length) const {
  const auto *Region = find(Owner);
  if (!Region)
    return physicalError(physical_diagnostic::ViewOwner);
  if (!Length || Offset >= Region->Size || Length > Region->Size - Offset)
    return physicalError(physical_diagnostic::ViewRange);
  const uint64_t Address = Region->Backing + Offset;
  if (auto E = Memory.validatePinned(Region->Storage, Offset, Length))
    return std::move(E);
  return Address;
}

llvm::Expected<std::vector<KernelPhysicalMemory::Segment>>
KernelPhysicalMemory::describe(uint64_t Owner, uint64_t Offset,
                               uint64_t Length) const {
  auto Backing = viewBacking(Owner, Offset, Length);
  if (!Backing)
    return Backing.takeError();
  std::vector<Segment> Result;
  uint64_t Address = *Backing;
  while (Length) {
    const uint64_t Page = pageBase(Address);
    const uint64_t PageOffset = Address - Page;
    const uint64_t Count = std::min(Length, physical::PageSize - PageOffset);
    Result.push_back(
        {Pages.at(pageKey(Regions.at(Owner), Page)).Physical + PageOffset,
         Address, Count});
    Address += Count;
    Length -= Count;
  }
  return Result;
}

llvm::Error KernelPhysicalMemory::canPin(uint64_t Owner, uint64_t Offset,
                                         uint64_t Length) const {
  auto Backing = viewBacking(Owner, Offset, Length);
  if (!Backing)
    return Backing.takeError();
  if (Pins.size() >= physical::PinLimit || NextPin == UINT64_MAX)
    return llvm::make_error<PhysicalMemoryLimitError>(
        physical_diagnostic::PinLimit);
  return llvm::Error::success();
}

llvm::Expected<uint64_t>
KernelPhysicalMemory::pin(uint64_t Owner, uint64_t Offset, uint64_t Length) {
  if (auto E = canPin(Owner, Offset, Length))
    return std::move(E);
  const uint64_t ID = NextPin++;
  Pins.emplace(ID, PinRecord{Owner, Offset, Length});
  return ID;
}

bool KernelPhysicalMemory::hasPinnedPages(uint64_t Backing,
                                          uint64_t Size) const {
  if (!Size || Size > UINT64_MAX - Backing)
    return false;
  const uint64_t Last = pageBase(Backing + Size - 1);
  for (uint64_t Page = pageBase(Backing);; Page += physical::PageSize) {
    auto Key = pageKey(Page);
    if (!Key) {
      llvm::consumeError(Key.takeError());
      return false;
    }
    const bool Pinned =
        std::any_of(Pins.begin(), Pins.end(), [&](const auto &Entry) {
          const auto &Pin = Entry.second;
          const auto View = llvm::cantFail(
              Regions.at(Pin.Owner).Storage.subview(Pin.Offset, Pin.Length));
          return containsPage(View, *Key);
        });
    if (!Pinned)
      return false;
    if (Page == Last)
      return true;
  }
}

llvm::Error KernelPhysicalMemory::canUnpin(uint64_t Pin) const {
  if (!Pins.contains(Pin))
    return physicalError(physical_diagnostic::UnpinRequiresPin);
  return llvm::Error::success();
}

llvm::Error KernelPhysicalMemory::unpin(uint64_t Pin) {
  if (auto E = canUnpin(Pin))
    return E;
  Pins.erase(Pin);
  return llvm::Error::success();
}

llvm::Error KernelPhysicalMemory::canExtendPin(uint64_t Pin,
                                               uint64_t NewLength) const {
  const auto I = Pins.find(Pin);
  if (I == Pins.end())
    return physicalError(physical_diagnostic::ExtensionRequiresPin);
  if (NewLength < I->second.Length)
    return physicalError(physical_diagnostic::ShrinkingPin);
  auto Backing = viewBacking(I->second.Owner, I->second.Offset, NewLength);
  if (!Backing)
    return Backing.takeError();
  return llvm::Error::success();
}

llvm::Error KernelPhysicalMemory::extendPin(uint64_t Pin, uint64_t NewLength) {
  if (auto E = canExtendPin(Pin, NewLength))
    return E;
  Pins.at(Pin).Length = NewLength;
  return llvm::Error::success();
}

llvm::Expected<uint64_t>
KernelPhysicalMemory::pinOffset(uint64_t Pin, uint64_t Offset,
                                uint64_t Length) const {
  const auto I = Pins.find(Pin);
  if (I == Pins.end())
    return physicalError(physical_diagnostic::AccessRequiresPin);
  const auto &View = I->second;
  if (Offset > View.Length || Length > View.Length - Offset)
    return physicalError(physical_diagnostic::AccessRange);
  const auto *Region = find(View.Owner);
  if (!Region)
    return physicalError(physical_diagnostic::LostOwner);
  if (auto E =
          Memory.validatePinned(Region->Storage, View.Offset + Offset, Length))
    return E;
  return View.Offset + Offset;
}

llvm::Error KernelPhysicalMemory::read(uint64_t Pin, uint64_t Offset,
                                       llvm::MutableArrayRef<uint8_t> Bytes) {
  auto Backing = pinOffset(Pin, Offset, Bytes.size());
  if (!Backing)
    return Backing.takeError();
  return Memory.readPinned(Regions.at(Pins.at(Pin).Owner).Storage, *Backing,
                           Bytes);
}

llvm::Error KernelPhysicalMemory::write(uint64_t Pin, uint64_t Offset,
                                        llvm::ArrayRef<uint8_t> Bytes) {
  auto Backing = pinOffset(Pin, Offset, Bytes.size());
  if (!Backing)
    return Backing.takeError();
  return Memory.writePinned(Regions.at(Pins.at(Pin).Owner).Storage, *Backing,
                            Bytes);
}

llvm::Expected<uint64_t>
KernelPhysicalMemory::ownerForRange(uint64_t Backing, uint64_t Size) const {
  if (!Size || Size > UINT64_MAX - Backing)
    return physicalError(physical_diagnostic::InvalidLookup);
  const auto *Active = activeOwners();
  if (!Active)
    return physicalError(physical_diagnostic::MissingOwner);
  const auto &OwnersByAddress = *Active;
  auto Current = Memory.addressSpace()->pinBacking(Backing, Size);
  if (!Current)
    return Current.takeError();
  for (auto I = OwnersByAddress.begin();
       I != OwnersByAddress.upper_bound(Backing); ++I) {
    const auto &Region = Regions.at(I->second);
    const uint64_t Offset = Backing - Region.Backing;
    if (Offset >= Region.Size || Size > Region.Size - Offset)
      continue;
    const auto Captured = llvm::cantFail(Region.Storage.subview(Offset, Size));
    if (Captured.describesSameBytes(*Current))
      return I->second;
  }
  return physicalError(physical_diagnostic::MissingSingleOwner);
}

llvm::Expected<uint64_t>
KernelPhysicalMemory::physicalAddress(uint64_t Backing) const {
  auto Owner = ownerForRange(Backing, 1);
  if (!Owner)
    return Owner.takeError();
  const uint64_t Page = pageBase(Backing);
  return Pages.at(pageKey(Regions.at(*Owner), Page)).Physical + Backing - Page;
}
} // namespace neverd::emulation
