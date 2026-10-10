//===- COFFRegistrationSEHTables.cpp - PE32 SEH compiler tables -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationIRProof.h"
#include "COFFRegistrationTableProof.h"

#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/backend/llvm/WindowsEHSemanticDigest.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/IR/Mangler.h"
#include "llvm/TargetParser/Triple.h"

namespace neverd {
namespace {
using coff_registration::rejectIR;
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
using coff_registration::absolutePointer;
using coff_registration::exactPointerFixup;
using coff_registration::ownerVA;
using coff_registration::sectionAt;

using coff_registration::callbacks;
#endif
} // namespace

llvm::Error
validateCOFFRegistrationSemanticRows(const llvm::Function &Function,
                                     const ExceptionFunction &Source,
                                     const CompiledImage &Compiled) {
#ifndef LLVM_NEVERD_X86_REGISTRATION_EH
  return rejectIR("LLVM does not provide indexed x86 SEH output receipts");
#else
  if (!Source.Registration || !Function.hasPersonalityFn() ||
      !llvm::isa<llvm::GlobalValue>(Function.getPersonalityFn()) ||
      !classifyWindowsEHNativeSource(Source, Arch::X86, BinaryFormat::COFF,
                                     WindowsEHNativeCapability::IRLowering)
           .canLowerNativeIR() ||
      !Compiled.Success || !Compiled.FunctionRangesValid ||
      !Compiled.WinEHSemanticsValid || Compiled.TargetArch != Arch::X86 ||
      Compiled.Format != BinaryFormat::COFF || Compiled.PointerWidth != 4 ||
      Compiled.ByteOrder != llvm::endianness::little ||
      llvm::Triple(Compiled.TargetTriple).getArch() != llvm::Triple::x86 ||
      !llvm::Triple(Compiled.TargetTriple).isWindowsMSVCEnvironment() ||
      !llvm::mc_rewrite::validateRewriteWinEHSemanticRecords(
          Compiled.WinEHSemanticRecords, Compiled.SourceFunctionOwners,
          Compiled.FunctionRanges, Compiled.FunctionOwnerAddrs))
    return rejectIR("compiler x86 SEH receipts are incomplete or inconsistent");
  auto Root = ownerVA(Function, Compiled);
  if (!Root)
    return Root.takeError();
  auto CallbackMap = callbacks(Function, Source);
  if (!CallbackMap)
    return CallbackMap.takeError();
  const auto &Chain = *Source.Registration;
  const bool EH4 = Source.Personality == ExceptionPersonality::ExceptHandler4;
  const int32_t Sentinel = EH4 ? -2 : -1;
  std::map<uint32_t, const CompiledWinEHSemanticRecord *> ByState, ByRegion;
  uint64_t ContainerVA = 0;
  uint64_t ContainerEndVA = 0;
  std::string ContainerSymbol;
  for (const auto &Row : Compiled.WinEHSemanticRecords) {
    if (Row.SourceFunction != Function.getName())
      continue;
    auto Token = windows_eh_semantics::getSEHScopeSemanticToken(
        Source, Arch::X86, Row.Token.Region);
    if (!Token || Row.Token != *Token || Row.OwnerVA != *Root ||
        Row.Token.Clause || Row.Token.Region >= Chain.Scopes.size() ||
        Row.Encoding !=
            (EH4 ? llvm::mc_rewrite::RewriteWinEHSemanticEncoding::X86SEH4
                 : llvm::mc_rewrite::RewriteWinEHSemanticEncoding::X86SEH3) ||
        Row.RecordSize != 12 ||
        !ByState.emplace(Row.GeneratedState, &Row).second ||
        !ByRegion.emplace(Row.Token.Region, &Row).second)
      return rejectIR(
          "generated scope row has no exact source semantic identity");
    if (!ContainerVA) {
      ContainerVA = Row.ContainerVA;
      ContainerEndVA = Row.ContainerEndVA;
      ContainerSymbol = Row.ContainerSymbol;
    }
    if (ContainerVA != Row.ContainerVA ||
        ContainerEndVA != Row.ContainerEndVA ||
        ContainerSymbol != Row.ContainerSymbol)
      return rejectIR(
          "one source registration has multiple generated scope tables");
    const auto *Section =
        sectionAt(Compiled, Row.RecordVA, Row.RecordSize,
                  llvm::mc_rewrite::RewriteSectionKind::ReadOnlyData);
    if (!Section || !sectionAt(Compiled, Row.HandlerVA, 1,
                               llvm::mc_rewrite::RewriteSectionKind::Code))
      return rejectIR("scope row or handler has no exact generated section");
    const auto *Bytes = Compiled.Bytes.data() + Row.RecordVA - Compiled.BaseVA;
    if (readLE<int32_t>(Bytes) != Row.EnclosingState ||
        readLE<uint32_t>(Bytes + 4) != Row.FilterVA ||
        readLE<uint32_t>(Bytes + 8) != Row.HandlerVA ||
        !exactPointerFixup(*Section, Row.RecordVA + 8, Row.HandlerSymbol,
                           Row.HandlerVA))
      return rejectIR(
          "scope row bytes disagree with compiler pointer provenance");
    const auto &Scope = Chain.Scopes[Row.Token.Region];
    if (Scope.IsFinally) {
      if (Row.FilterVA || !Row.FilterSymbol.empty())
        return rejectIR("finally row acquired a searching filter");
    } else {
      auto Filter =
          ownerVA(*CallbackMap->at({Scope.FilterVA, false}), Compiled);
      if (!Filter)
        return Filter.takeError();
      if (Row.FilterVA != *Filter ||
          !exactPointerFixup(*Section, Row.RecordVA + 4, Row.FilterSymbol,
                             *Filter))
        return rejectIR("scope filter does not name its recovered callback");
    }
  }
  if (ByState.size() != Chain.Scopes.size() ||
      ByRegion.size() != Chain.Scopes.size() || ContainerEndVA <= ContainerVA ||
      ContainerEndVA - ContainerVA != (EH4 ? 16u : 0u) + 12 * ByState.size() ||
      !sectionAt(Compiled, ContainerVA, ContainerEndVA - ContainerVA,
                 llvm::mc_rewrite::RewriteSectionKind::ReadOnlyData))
    return rejectIR("generated indexed scope table is not physically closed");
  for (uint32_t I = 0; I < ByState.size(); ++I) {
    if (!ByState.count(I))
      return rejectIR("generated scope state numbering has a hole");
    const auto &Row = *ByState.at(I);
    const int32_t SourceOuter = Chain.Scopes[Row.Token.Region].EnclosingLevel;
    if (Row.RecordVA != ContainerVA + (EH4 ? 16 : 0) + 12 * I ||
        (SourceOuter < 0
             ? Row.EnclosingState != Sentinel
             : (Row.EnclosingState < 0 || !ByState.count(Row.EnclosingState) ||
                ByState.at(Row.EnclosingState)->Token.Region !=
                    uint32_t(SourceOuter))))
      return rejectIR(
          "generated enclosing state changed the source scope graph");
  }
  if (EH4) {
    const auto *Header = Compiled.Bytes.data() + ContainerVA - Compiled.BaseVA;
    const auto &Cookies = ByState.begin()->second->RegistrationCookieOffsets;
    for (unsigned I = 0; I != Cookies.size(); ++I)
      if (readLE<int32_t>(Header + I * 4) != Cookies[I])
        return rejectIR(
            "generated EH4 cookie header changed its machine frame offsets");
    const int32_t GS = readLE<int32_t>(Header);
    const int32_t EH = readLE<int32_t>(Header + 8);
    if ((Chain.GSCookieOffset == -2
             ? GS != -2 || readLE<uint32_t>(Header + 4)
             : GS == -2 || GS % 4 || readLE<int32_t>(Header + 4) % 4) ||
        EH % 4 || readLE<uint32_t>(Header + 12))
      return rejectIR(
          "generated EH4 cookie header is not compiler frame derived");
  }
  bool TableReferenced = false;
  bool PersonalityReferenced = false;
  llvm::SmallString<64> PersonalitySymbol;
  llvm::Mangler Mangler;
  Mangler.getNameWithPrefix(
      PersonalitySymbol,
      llvm::cast<llvm::GlobalValue>(Function.getPersonalityFn()), false);
  for (const auto &Section : Compiled.Sections) {
    if (Section.Kind != llvm::mc_rewrite::RewriteSectionKind::Code)
      continue;
    for (const auto &Fixup : Section.FixupReferences) {
      if (!absolutePointer(Fixup) || Fixup.Addend)
        continue;
      bool InRoot = false;
      for (const auto &Range : Compiled.FunctionRanges)
        if (Range.OwnerVA == *Root &&
            Section.VA + Fixup.Offset >= Range.BeginVA &&
            Section.VA + Fixup.Offset + 4 <= Range.EndVA)
          InRoot = true;
      if (!InRoot)
        continue;
      TableReferenced |=
          Fixup.Symbol == ContainerSymbol && Fixup.ResolvedValue == ContainerVA;
      PersonalityReferenced |= Fixup.Symbol == PersonalitySymbol &&
                               Fixup.ResolvedValue == Source.PersonalityVA;
    }
  }
  if (!TableReferenced || !PersonalityReferenced)
    return rejectIR(
        "live registration node does not reference its exact native contract");
  return llvm::Error::success();
#endif
}

} // namespace neverd
