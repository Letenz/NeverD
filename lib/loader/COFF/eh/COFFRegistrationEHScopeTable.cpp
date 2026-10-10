//===- COFFRegistrationEHScopeTable.cpp - x86-32 SEH scope tables --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationEHDetail.h"

#include "neverd/Limits.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace neverd::coff_loader::registration_detail {
namespace {

/// True when a scope-table entry names a level that already exists.  Levels
/// are indices into the same table, so a forward or out-of-range reference
/// would make the nesting graph cyclic or dangling.
bool isValidEnclosingLevel(int32_t Level, uint32_t Index, bool IsEH4) {
  if (Level == (IsEH4 ? -2 : -1))
    return true;
  return Level >= 0 && static_cast<uint32_t>(Level) < Index;
}

/// One immediate byte, word or dword store relative to a checked base. A narrow
/// immediate is an observation of written bits, not a complete runtime state.
struct FrameSlotStore {
  va_t StoreVA = 0;
  va_t EndVA = 0;
  int32_t Displacement = 0;
  int32_t Value = 0;
  uint8_t Width = 4;
};

/// Every such store inside a code range.
///
/// This is a byte scan rather than a decode, so it can also match bytes that
/// are the tail of some other instruction. The CFG owner must authenticate
/// each hit against its decoded instruction boundary before using it.
std::vector<FrameSlotStore>
findFrameSlotStores(const BinaryImage &Img, const ExceptionAddressRange &Range,
                    uint8_t BaseRegister, int32_t BaseOffset) {
  std::vector<FrameSlotStore> Stores;
  const Segment *Seg = Img.getSegmentFor(Range.Begin);
  if (!Seg || !Seg->isExecutable() || Range.Begin < Seg->VA ||
      Range.End <= Range.Begin)
    return Stores;
  const uint64_t Begin = Range.Begin - Seg->VA;
  const uint64_t End =
      std::min<uint64_t>(Range.End - Seg->VA, Seg->Data.size());
  if (Begin >= End)
    return Stores;

  const uint8_t *Data = Seg->Data.data();
  for (uint64_t I = Begin; I + 4 <= End; ++I) {
    uint64_t Opcode = I;
    uint8_t Width = 4;
    if (Data[Opcode] == 0x66) {
      ++Opcode;
      Width = 2;
    }
    if (Data[Opcode] == 0xC6 && Width == 4)
      Width = 1;
    else if (Data[Opcode] != 0xC7)
      continue;
    // ModRM /0 with the selected unindexed frame base and a displacement.
    const uint8_t ModRM = Data[Opcode + 1];
    const unsigned Mod = ModRM >> 6;
    const uint64_t DispBytes = Mod == 1 ? 1 : Mod == 2 ? 4 : 0;
    if ((ModRM & 0x38) || (ModRM & 7) != BaseRegister || !DispBytes ||
        Opcode + 2 + DispBytes + Width > End)
      continue;
    const uint64_t Immediate = Opcode + 2 + DispBytes;
    const int64_t Displacement =
        int64_t(BaseOffset) + (DispBytes == 1
                                   ? int8_t(Data[Opcode + 2])
                                   : readLE<int32_t>(Data + Opcode + 2));
    if (Displacement < INT32_MIN || Displacement > INT32_MAX)
      continue;
    const int32_t Value = Width == 1   ? Data[Immediate]
                          : Width == 2 ? readLE<uint16_t>(Data + Immediate)
                                       : readLE<int32_t>(Data + Immediate);
    Stores.push_back({static_cast<va_t>(Seg->VA + I),
                      static_cast<va_t>(Seg->VA + Immediate + Width),
                      int32_t(Displacement), Value, Width});
    // Do not publish a second dword candidate inside the operand-size prefix.
    if (Opcode != I)
      I = Opcode;
  }
  return Stores;
}

} // namespace

/// Decode the scope-table entry array.
///
/// The array is unsized: nothing in the image records how many entries a table
/// has, because the runtime only ever indexes it by the try level held in the
/// frame.  Validating entries until one fails is therefore not enough on its
/// own — the compiler emits these tables back to back, so the first entry of
/// the *next* function's table is a perfectly well-formed entry and a walk
/// that only checks well-formedness runs straight into it, attributing another
/// function's handlers to this one.
///
/// \p Limit is the address the next table begins at, which caps the walk at
/// the one boundary the image does establish.  It is zero for the last table
/// in the image, where validation is all there is.
uint32_t decodeScopeRecords(const BinaryImage &Img, va_t ArrayVA, va_t Limit,
                            bool IsEH4,
                            std::vector<RegistrationScopeRecord> &Scopes,
                            bool &BudgetExhausted) {
  BudgetExhausted = false;
  for (uint32_t Index = 0; Index < MaxRegistrationRecords; ++Index) {
    uint64_t Offset = uint64_t(Index) * 12;
    if (Offset > InvalidVA - ArrayVA)
      break;
    if (Limit != 0 && ArrayVA + Offset + 12 > Limit)
      break;
    const uint8_t *Entry = Img.readVA(ArrayVA + Offset, 12);
    if (!Entry)
      break;
    int32_t Level = readLE<int32_t>(Entry);
    uint32_t Filter = readLE<uint32_t>(Entry + 4);
    uint32_t Handler = readLE<uint32_t>(Entry + 8);
    if (!isValidEnclosingLevel(Level, Index, IsEH4))
      break;
    // A `__finally` has no filter; an `__except` has both.  An entry with no
    // handler at all describes nothing and marks the end of the array.
    if (Handler == 0 || !isExecutableAddress(Img, Handler))
      break;
    if (Filter != 0 && !isExecutableAddress(Img, Filter))
      break;

    RegistrationScopeRecord Scope;
    Scope.EnclosingLevel = Level;
    Scope.FilterVA = Filter;
    Scope.HandlerVA = Handler;
    Scope.IsFinally = Filter == 0;
    Scopes.push_back(Scope);
  }
  if (Scopes.size() == MaxRegistrationRecords) {
    const uint64_t Bytes = uint64_t(MaxRegistrationRecords) * 12;
    BudgetExhausted =
        Bytes > InvalidVA - ArrayVA || Limit == 0 || ArrayVA + Bytes != Limit;
  }
  return static_cast<uint32_t>(Scopes.size());
}

/// Collect immediate stores in the prologue-authenticated state slot. For
/// other prologues retain a unique value-pattern observation for inspection;
/// that heuristic does not establish registration ownership or CFG states.
void recoverTryLevelStores(const BinaryImage &Img,
                           const ExceptionAddressRange &Range, int32_t Seed,
                           size_t ScopeCount, RegistrationChainInfo &Chain) {
  if (ScopeCount == 0 || ScopeCount > MaxRegistrationRecords)
    return;
  const int32_t Highest = static_cast<int32_t>(ScopeCount) - 1;

  std::map<int32_t, std::vector<FrameSlotStore>> BySlot;
  const uint8_t BaseRegister =
      Chain.RealignedFrame ? Chain.RealignedFrame->BaseRegister : 5;
  const int32_t BaseOffset =
      Chain.RealignedFrame ? Chain.RealignedFrame->BaseOffset : 0;
  for (const FrameSlotStore &Store :
       findFrameSlotStores(Img, Range, BaseRegister, BaseOffset)) {
    // The try level lives in the frame the prologue established, which is
    // below the frame pointer.  A positive displacement addresses an incoming
    // argument and cannot be it.
    if (Store.Displacement < 0)
      BySlot[Store.Displacement].push_back(Store);
  }

  const std::vector<FrameSlotStore> *Winner = nullptr;
  int32_t WinningSlot = 0;
  if (Chain.RegistrationOffset && Chain.TryLevelOffset) {
    auto It = BySlot.find(*Chain.TryLevelOffset);
    if (It != BySlot.end()) {
      Winner = &It->second;
      WinningSlot = It->first;
    }
  }
  // Legacy observations without an authenticated layout remain inspectable,
  // but cannot authorize CFG state recovery or native frame ownership.
  for (const auto &[Slot, Stores] : BySlot) {
    if (Chain.RegistrationOffset)
      break;
    bool SawSeed = false;
    bool SawScope = false;
    bool AllInRange = true;
    for (const FrameSlotStore &Store : Stores) {
      if (Store.Width != 4) {
        AllInRange = false;
        continue;
      }
      if (Store.Value == Seed)
        SawSeed = true;
      else if (Store.Value >= 0 && Store.Value <= Highest)
        SawScope = true;
      else
        AllInRange = false;
    }
    if (!AllInRange || !SawSeed || !SawScope)
      continue;
    if (Winner)
      return;
    Winner = &Stores;
    WinningSlot = Slot;
  }
  if (!Winner)
    return;

  Chain.TryLevelOffset = WinningSlot;
  Chain.TryLevelStores.reserve(Winner->size());
  for (const FrameSlotStore &Store : *Winner)
    Chain.TryLevelStores.push_back(
        {Store.StoreVA, Store.EndVA, Store.Value, Store.Width});
  std::sort(
      Chain.TryLevelStores.begin(), Chain.TryLevelStores.end(),
      [](const RegistrationTryLevelStore &A,
         const RegistrationTryLevelStore &B) { return A.StoreVA < B.StoreVA; });
}

/// `_except_handler4` prefixes the entry array with the frame displacements of
/// the security cookies it verifies before trusting the table.  A `-2` cookie
/// offset is the sentinel for "this frame has no cookie of that kind".
bool decodeEH4Header(const BinaryImage &Img, va_t TableVA,
                     RegistrationChainInfo &Chain) {
  const uint8_t *Header = Img.readVA(TableVA, 16);
  if (!Header)
    return false;
  Chain.GSCookieOffset = readLE<int32_t>(Header);
  Chain.GSCookieXOROffset = readLE<int32_t>(Header + 4);
  Chain.EHCookieOffset = readLE<int32_t>(Header + 8);
  Chain.EHCookieXOROffset = readLE<int32_t>(Header + 12);
  Chain.HasSecurityCookies = Chain.GSCookieOffset != -2;
  return true;
}

} // namespace neverd::coff_loader::registration_detail

namespace neverd::coff_loader {
std::optional<ExceptionAddressRange>
getX86RegistrationSEHScopeTableRange(const ExceptionFunction &Function) {
  const bool EH3 =
      Function.Personality == ExceptionPersonality::ExceptHandler3 &&
      Function.Encoding == ExceptionEncoding::X86ScopeTableEH3;
  const bool EH4 =
      Function.Personality == ExceptionPersonality::ExceptHandler4 &&
      Function.Encoding == ExceptionEncoding::X86ScopeTableEH4;
  if ((!EH3 && !EH4) || !Function.Registration)
    return std::nullopt;
  const auto &Chain = *Function.Registration;
  if (!Chain.ScopeTableVA || Chain.ScopeTableVA > UINT32_MAX ||
      Chain.Scopes.empty() ||
      Chain.Scopes.size() > limits::kMaxRegistrationEHRecords)
    return std::nullopt;
  const uint64_t Bytes = uint64_t(Chain.Scopes.size()) * 12 + (EH4 ? 16 : 0);
  if (Bytes > uint64_t(UINT32_MAX) + 1 - Chain.ScopeTableVA)
    return std::nullopt;
  return ExceptionAddressRange{Chain.ScopeTableVA, Chain.ScopeTableVA + Bytes};
}
} // namespace neverd::coff_loader
