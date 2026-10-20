//===- COFFRegistrationEHCxxHelper.cpp - Shared PE32 C++ prologues --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Attribute EAX-handler prologue calls without granting a native frame ABI.

#include "COFFRegistrationEHDetail.h"

#include "neverd/loader/ReadOnlyBytes.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace neverd::coff_loader::registration_detail {
namespace {

bool isCxxEAXPrologue(const BinaryImage &Img, va_t Entry,
                      std::map<va_t, bool> &Checked) {
  const auto Existing = Checked.find(Entry);
  if (Existing != Checked.end())
    return Existing->second;
  // The caller's handler in EAX is pushed below its return PC. The helper
  // saves EBP in the allocation word, seeds -1 and publishes EBP-12.
  static constexpr uint8_t Prefix[] = {
      0x50, 0x64, 0xff, 0x35, 0,    0,    0,    0,    0x8d, 0x44, 0x24, 0x0c,
      0x2b, 0x64, 0x24, 0x0c, 0x53, 0x56, 0x57, 0x89, 0x28, 0x8b, 0xe8, 0xa1};
  static constexpr uint8_t Tail[] = {0xff, 0x75, 0xfc, 0xc7, 0x45, 0xfc, 0xff,
                                     0xff, 0xff, 0xff, 0x8d, 0x45, 0xf4, 0x64,
                                     0xa3, 0,    0,    0,    0,    0xc3};
  const auto *Bytes = Img.readVA(Entry, 51);
  if (!Bytes || !std::equal(std::begin(Prefix), std::end(Prefix), Bytes))
    return false;
  // Cache only actual helper-shaped candidates, not arbitrary call targets.
  auto [Result, New] = Checked.emplace(Entry, false);
  (void)New;
  if (Img.Base > UINT32_MAX || !Img.DynInfo.SecurityCookieRVA ||
      Img.DynInfo.SecurityCookieRVA > UINT32_MAX - Img.Base)
    return false;
  const va_t Cookie = Img.Base + Img.DynInfo.SecurityCookieRVA;
  if (Cookie > UINT32_MAX - 3 || readLE<uint32_t>(Bytes + 24) != Cookie ||
      !Img.readVA(Cookie, 4) || Bytes[28] != 0x33 || Bytes[29] != 0xc5 ||
      Bytes[30] != 0x50)
    return false;
  size_t Suffix = 31;
  // The compiler variants additionally save ESP or the encoded GS cookie.
  if (Bytes[Suffix] == 0x89 &&
      (Bytes[Suffix + 1] == 0x65 || Bytes[Suffix + 1] == 0x45) &&
      Bytes[Suffix + 2] == 0xf0)
    Suffix += 3;
  Bytes = Img.readVA(Entry, Suffix + sizeof(Tail));
  if (!Bytes || !std::equal(std::begin(Tail), std::end(Tail), Bytes + Suffix))
    return false;
  Result->second = readImmutablePE32CodeBytes(Img, Entry, Suffix + sizeof(Tail),
                                              {Entry + 24})
                       .has_value();
  return Result->second;
}

std::optional<va_t> eaxCallerEntry(const Segment &Seg, size_t CallOffset) {
  if (CallOffset < 7 || Seg.Data[CallOffset - 5] != 0xb8)
    return std::nullopt;
  size_t Begin = CallOffset - 7;
  int64_t Allocation;
  if (Seg.Data[Begin] == 0x6a)
    Allocation = int8_t(Seg.Data[Begin + 1]);
  else if (CallOffset >= 10 && Seg.Data[CallOffset - 10] == 0x68) {
    Begin = CallOffset - 10;
    Allocation = readLE<int32_t>(Seg.Data.data() + Begin + 1);
  } else
    return std::nullopt;
  if (Allocation < 0 || Allocation > limits::kMaxSourceCallBorrowedBytes)
    return std::nullopt;
  return Seg.VA + Begin;
}
} // namespace

std::vector<InstallSite>
findCxxEAXPrologueCallers(const BinaryImage &Img,
                          const FunctionRangeMap &Functions,
                          const SafeSEHTable &SafeSEH) {
  std::vector<InstallSite> Sites;
  std::map<va_t, bool> Helpers;
  std::set<va_t> Entries;
  size_t BoundaryWork = 0;
  for (const auto &Seg : Img.Segments) {
    if (!Seg.isExecutable() || Seg.Data.size() < 12)
      continue;
    for (size_t I = 7; I + 5 <= Seg.Data.size(); ++I) {
      const auto *Call = static_cast<const uint8_t *>(
          std::memchr(Seg.Data.data() + I, 0xe8, Seg.Data.size() - I - 4));
      if (!Call)
        break;
      I = Call - Seg.Data.data();
      const auto Entry = eaxCallerEntry(Seg, I);
      if (!Entry)
        continue;
      const va_t CallVA = Seg.VA + I;
      const auto Target =
          addSignedOffset(CallVA + 5, readLE<int32_t>(Call + 1));
      if (!Target || !isCxxEAXPrologue(Img, *Target, Helpers))
        continue;
      const va_t Handler = readLE<uint32_t>(Call - 4);
      if (!isExecutableAddress(Img, Handler) ||
          (SafeSEH.isPresent() && !SafeSEH.contains(Handler)))
        continue;
      HandlerIdentity Identity;
      if (!decodeCxxHandlerThunk(Img, Handler, Identity) ||
          !Identity.CxxFuncInfoVA ||
          (Identity.Personality != ExceptionPersonality::CxxFrameHandler3 &&
           Identity.Personality != ExceptionPersonality::CxxFrameHandlerX86) ||
          !readImmutablePE32CodeBytes(Img, *Entry, CallVA + 5 - *Entry,
                                      {CallVA - 4}))
        continue;
      auto Range = Functions.find(Img, CallVA);
      if (!Range || !Range->contains(*Entry) || !Range->contains(CallVA + 4) ||
          Range->contains(*Target))
        continue;
      const auto Next = Entries.upper_bound(CallVA);
      if (Next != Entries.begin())
        Range->Begin = std::max(Range->Begin, *std::prev(Next));
      if (*Entry < Range->Begin ||
          (Range->Begin < *Entry &&
           !checkedRegistrationRangeEnd(Img, {Range->Begin, *Entry},
                                        {Range->Begin}, BoundaryWork)) ||
          !Entries.insert(*Entry).second)
        continue;
      Range->Begin = *Entry;
      InstallSite Site;
      Site.Range = *Range;
      Site.InstallVA = CallVA;
      Site.HandlerVA = Handler;
      Site.TryLevel = -1;
      Site.Identity = std::move(Identity);
      Sites.push_back(std::move(Site));
    }
  }
  std::sort(Sites.begin(), Sites.end(), [](const auto &A, const auto &B) {
    return A.Range.Begin < B.Range.Begin;
  });
  for (size_t I = 1; I < Sites.size(); ++I)
    Sites[I - 1].Range.End =
        std::min(Sites[I - 1].Range.End, Sites[I].Range.Begin);
  return Sites;
}
} // namespace neverd::coff_loader::registration_detail
