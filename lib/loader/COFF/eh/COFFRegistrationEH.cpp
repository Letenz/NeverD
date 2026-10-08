//===- COFFRegistrationEH.cpp - x86-32 registration-chain EH -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/COFF/COFFRegistrationEH.h"

#include "COFFRegistrationEHDetail.h"

#include "neverd/Limits.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

namespace neverd::coff_loader {
namespace {

using registration_detail::decodeEH4Header;
using registration_detail::decodeScopeRecords;
using registration_detail::decodeX86FuncInfo;
using registration_detail::diagnose;
using registration_detail::expandPrologueHelpers;
using registration_detail::findInstallSites;
using registration_detail::FunctionRangeMap;
using registration_detail::HandlerIdentity;
using registration_detail::InstallSite;
using registration_detail::recoverTryLevelStores;
using registration_detail::SafeSEHTable;

/// Address at which the table after \p TableVA begins, or zero when it is the
/// last one.  \p Sorted holds every table address the image was proven to
/// install, which is what bounds an otherwise unsized entry array.
va_t findNextTableAddress(const std::vector<va_t> &Sorted, va_t TableVA) {
  auto It = std::upper_bound(Sorted.begin(), Sorted.end(), TableVA);
  return It == Sorted.end() ? 0 : *It;
}

/// Authenticate the direct MSVC frame from its exact entry instruction
/// sequence. A chain-head read alone does not establish a registration, and
/// locals containing -1/0 are not evidence of the runtime's state field.
void proveDirectRegistrationLayout(const BinaryImage &Img,
                                   const InstallSite &Site,
                                   RegistrationChainInfo &Chain) {
  const bool IsCxx = Site.Identity.CxxFuncInfoVA != 0;
  const size_t Size = IsCxx ? 24 : 29;
  const uint8_t *P = Img.readVA(Site.Range.Begin, Size);
  if (!P || Site.Range.size() < Size || P[0] != 0x55 ||
      !((P[1] == 0x8B && P[2] == 0xEC) || (P[1] == 0x89 && P[2] == 0xE5)) ||
      P[3] != 0x6A || !Chain.SeededTryLevel ||
      static_cast<int8_t>(P[4]) != *Chain.SeededTryLevel)
    return;
  size_t Cursor = 5;
  if (!IsCxx) {
    if (P[Cursor] != 0x68 || readLE<uint32_t>(P + Cursor + 1) != Site.TableVA)
      return;
    Cursor += 5;
  }
  if (P[Cursor] != 0x68 || readLE<uint32_t>(P + Cursor + 1) != Site.HandlerVA)
    return;
  Cursor += 5;
  if (Site.InstallVA != Site.Range.Begin + Cursor || P[Cursor] != 0x64 ||
      P[Cursor + 1] != 0xA1 || readLE<uint32_t>(P + Cursor + 2) != 0 ||
      P[Cursor + 6] != 0x50 || P[Cursor + 7] != 0x64 || P[Cursor + 8] != 0x89 ||
      P[Cursor + 9] != 0x25 || readLE<uint32_t>(P + Cursor + 10) != 0)
    return;
  Chain.RegistrationOffset = IsCxx ? -12 : -16;
  Chain.TryLevelOffset = -4;
  Chain.ChainInstallVA = Site.Range.Begin + Cursor + 7;
}

} // namespace

void parseX86RegistrationExceptions(BinaryImage &Img) {
  if (Img.Arch != Arch::X86 || Img.Format != BinaryFormat::COFF)
    return;

  const FunctionRangeMap Functions(Img);
  const SafeSEHTable SafeSEH(Img);
  if (SafeSEH.isMalformed()) {
    Img.ExceptionMetadata.ParseStatus = mergeExceptionParseStatus(
        Img.ExceptionMetadata.ParseStatus, ExceptionParseStatus::Malformed);
    Img.ExceptionMetadata.Diagnostics.push_back("malformed x86 SafeSEH table");
    return;
  }
  std::vector<InstallSite> Sites = findInstallSites(Img, Functions, SafeSEH);
  expandPrologueHelpers(Img, Functions, Sites);
  if (Sites.empty())
    return;

  // Every table address the image installs, so each entry array can be capped
  // at the next one.  Both the scope tables and the C++ `FuncInfo` records
  // take part: the compiler emits them into the same read-only region, so a
  // `FuncInfo` is just as much a boundary for the scope table before it.
  std::vector<va_t> TableAddresses;
  for (const InstallSite &Site : Sites) {
    if (Site.Identity.CxxFuncInfoVA != 0)
      TableAddresses.push_back(Site.Identity.CxxFuncInfoVA);
    if (Site.TableVA != 0)
      TableAddresses.push_back(Site.TableVA);
  }
  std::sort(TableAddresses.begin(), TableAddresses.end());
  TableAddresses.erase(
      std::unique(TableAddresses.begin(), TableAddresses.end()),
      TableAddresses.end());

  std::vector<ExceptionFunction> Recovered;
  Recovered.reserve(Sites.size());
  for (const InstallSite &Site : Sites) {
    const HandlerIdentity &Identity = Site.Identity;
    ExceptionFunction F;
    F.CodeRange = Site.Range;
    F.Kind = RuntimeFunctionKind::Primary;
    F.PersonalityVA = Site.HandlerVA;
    F.PersonalityName = Identity.Name;
    F.Personality = Identity.Personality;
    if (F.Personality == ExceptionPersonality::Unknown)
      diagnose(F, ExceptionParseStatus::Partial,
               "x86 registration handler identity is not proven");

    RegistrationChainInfo Chain;
    Chain.HandlerVA = Site.HandlerVA;
    Chain.ChainInstallVA = Site.InstallVA;
    Chain.ScopeTableVA = Site.TableVA;

    if (Identity.CxxFuncInfoVA != 0) {
      F.Encoding = ExceptionEncoding::X86CxxFuncInfo;
      F.HandlerDataVA = Identity.CxxFuncInfoVA;
      Chain.ScopeTableVA = Identity.CxxFuncInfoVA;
      decodeX86FuncInfo(F, Img, Identity.CxxFuncInfoVA);
      Chain.SeededTryLevel = -1;
    } else if (Site.TableVA != 0 && Img.readVA(Site.TableVA, 12)) {
      // `_except_handler4` seeds -2 as the initial try level and prefixes
      // its table with cookie displacements; `_except_handler3` seeds -1
      // and starts at the entry array.  When the handler kept its name that
      // is authoritative. For an unknown handler the sentinel supplies only
      // an inspectable layout observation, never runtime semantics.
      bool IsEH4 = F.Personality == ExceptionPersonality::ExceptHandler4 ||
                   (F.Personality != ExceptionPersonality::ExceptHandler3 &&
                    Site.TryLevel && *Site.TryLevel == -2);
      F.HandlerDataVA = Site.TableVA;
      va_t ArrayVA = Site.TableVA;
      if (IsEH4) {
        if (!decodeEH4Header(Img, Site.TableVA, Chain)) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "truncated _except_handler4 scope-table header");
        } else {
          ArrayVA = Site.TableVA + 16;
        }
        F.Encoding = ExceptionEncoding::X86ScopeTableEH4;
      } else {
        F.Encoding = ExceptionEncoding::X86ScopeTableEH3;
      }
      // Both sentinels mean "no scope is current"; which one this frame uses
      // follows from the handler it installed.
      Chain.SeededTryLevel = IsEH4 ? -2 : -1;
      const va_t Limit = findNextTableAddress(TableAddresses, Site.TableVA);
      bool ScopeBudgetExhausted = false;
      if (decodeScopeRecords(Img, ArrayVA, Limit, IsEH4, Chain.Scopes,
                             ScopeBudgetExhausted) == 0)
        diagnose(F, ExceptionParseStatus::Partial,
                 "x86 scope table at 0x" + llvm::utohexstr(ArrayVA) +
                     " declares no usable entry");
      if (ScopeBudgetExhausted)
        diagnose(
            F, ExceptionParseStatus::Partial,
            "x86 scope-table decode budget exhausted before a proven boundary");
    } else {
      F.Encoding = ExceptionEncoding::X86ScopeTableEH3;
      diagnose(F, ExceptionParseStatus::Partial,
               "x86 registration record installs a handler with no "
               "recoverable table");
    }

    // MSVC plants the filter thunk and except body immediately after the
    // protected region and often labels them as functions.  Those labels clip
    // CodeRange at the filter, so the handler is never a CFG root.  Grow the
    // range through contiguous thunks so it covers the rest of the C function,
    // stopping at the next function that is not one of this record's thunks.
    {
      std::set<va_t> Thunks;
      for (const RegistrationScopeRecord &Scope : Chain.Scopes) {
        if (Scope.FilterVA)
          Thunks.insert(Scope.FilterVA);
        if (Scope.HandlerVA)
          Thunks.insert(Scope.HandlerVA);
      }
      bool Grew = true;
      for (unsigned Guard = 0;
           Grew && Guard < limits::kMaxRegistrationEHFixedPoint; ++Guard) {
        Grew = false;
        for (va_t Addr : Thunks) {
          if (!registration_detail::isExecutableAddress(Img, Addr) ||
              Addr < F.CodeRange.Begin || Addr > F.CodeRange.End)
            continue;
          auto Range = Functions.find(Img, Addr);
          if (!Range || Range->Begin < F.CodeRange.Begin)
            continue;
          if (Range->End > F.CodeRange.End) {
            F.CodeRange.End = Range->End;
            Grew = true;
          }
        }
      }
    }

    proveDirectRegistrationLayout(Img, Site, Chain);
    if (Chain.SeededTryLevel)
      recoverTryLevelStores(Img, F.CodeRange, *Chain.SeededTryLevel,
                            F.Cxx ? F.Cxx->MaxState : Chain.Scopes.size(),
                            Chain);

    F.Registration = std::move(Chain);
    if (F.Personality == ExceptionPersonality::Unknown)
      diagnose(F, ExceptionParseStatus::Partial,
               "unknown x86 registration handler");

    Recovered.push_back(std::move(F));
  }

  for (ExceptionFunction &F : Recovered) {
    Img.ExceptionMetadata.ParseStatus = mergeExceptionParseStatus(
        Img.ExceptionMetadata.ParseStatus, F.ParseStatus);
    Img.ExceptionMetadata.addModel(F.model());
    Img.ExceptionMetadata.Functions.push_back(std::move(F));
  }
  Img.ExceptionMetadata.rebuildIndex();

  // The data-pointer scan ran before this parse and took each relocated
  // scope-table pointer for a function.  The thunks are code of the function
  // that installed the record, as FuncDetector treats them: a guess goes, and
  // a stated symbol stays as a label.
  const std::set<va_t> Thunks = Img.ExceptionMetadata.registrationScopeThunks();
  llvm::erase_if(Img.Symbols, [&](const Symbol &Sym) {
    return Sym.IsFunc && Sym.Origin == NameOrigin::Synthesized &&
           Thunks.count(Sym.Addr);
  });
  for (Symbol &Sym : Img.Symbols)
    if (Sym.IsFunc && Thunks.count(Sym.Addr))
      Sym.IsFunc = false;
}

} // namespace neverd::coff_loader
