//===- COFFRegistrationCxx.cpp - Checked PE32 C++ table closure -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "COFFRegistrationTableProof.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/backend/llvm/WindowsEHSemanticDigest.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Mangler.h"
#ifdef LLVM_NEVERD_X86_CXX_FUNCTION_RECEIPTS
#include "llvm/IR/WinEHFrame.h"
#endif
#include "llvm/Support/Endian.h"
#include "llvm/TargetParser/Triple.h"

#include <functional>
#include <set>

namespace neverd {
namespace {
llvm::Error rejectCxx(const llvm::Twine &Detail) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "coff registration C++ tables: " + Detail);
}
} // namespace

llvm::Expected<COFFRegistrationCxxTableReceipt>
getCheckedCOFFRegistrationCxxTableReceipt(const llvm::Function &Function,
                                          const ExceptionFunction &Source,
                                          const BinaryImage &Image,
                                          const CompiledImage &Compiled) {
#ifndef LLVM_NEVERD_X86_CXX_FUNCTION_RECEIPTS
  return rejectCxx("LLVM does not provide complete PE32 C++ table receipts");
#else
  using Kind = llvm::mc_rewrite::RewriteWinEHSemanticKind;
  using Encoding = llvm::mc_rewrite::RewriteWinEHSemanticEncoding;
  using SectionKind = llvm::mc_rewrite::RewriteSectionKind;
  const auto Classification =
      classifyWindowsEHNativeSource(Source, Arch::X86, BinaryFormat::COFF,
                                    WindowsEHNativeCapability::IRLowering);
  auto OriginalVA = rewrite_source::getOriginalVA(Function);
  if (!OriginalVA)
    return OriginalVA.takeError();
  if (*OriginalVA != Source.CodeRange.Begin ||
      Classification.Model != WindowsEHNativeSourceModel::X86RegistrationCxx ||
      !Classification.canLowerNativeIR() || Image.Arch != Arch::X86 ||
      Image.Format != BinaryFormat::COFF ||
      !coff_loader::getCheckedX86CxxMetadataRanges(Image, Source) ||
      !coff_loader::getCheckedX86CxxPersonalityABI(Image, Source) ||
      !Compiled.Success || !Compiled.Unresolved.empty() ||
      !Compiled.FunctionRangesValid || !Compiled.WinEHSemanticsValid ||
      Compiled.TargetArch != Arch::X86 ||
      Compiled.Format != BinaryFormat::COFF || Compiled.PointerWidth != 4 ||
      Compiled.ByteOrder != llvm::endianness::little ||
      llvm::Triple(Compiled.TargetTriple).getArch() != llvm::Triple::x86 ||
      !llvm::Triple(Compiled.TargetTriple).isWindowsMSVCEnvironment() ||
      !llvm::mc_rewrite::validateRewriteWinEHSemanticRecords(
          Compiled.WinEHSemanticRecords, Compiled.SourceFunctionOwners,
          Compiled.FunctionRanges, Compiled.FunctionOwnerAddrs))
    return rejectCxx("source or compiler contract is incomplete");
  const auto &Cxx = *Source.Cxx;
  // The current source emitter has one typed try and one runtime context.
  // Do not infer mappings for an unlowered nested/catch cleanup graph.
  if (Cxx.TryBlocks.size() != 1 || Cxx.TryBlocks[0].Handlers.size() != 1 ||
      Cxx.TryBlocks[0].TryLow != 0 || Cxx.UnwindMap.size() > 128)
    return rejectCxx("source dispatch graph has no complete native projection");
  const auto &Try = Cxx.TryBlocks[0];
  const auto &Catch = Try.Handlers[0];
  auto Root = coff_registration::ownerVA(Function, Compiled);
  if (!Root)
    return Root.takeError();
  const CompiledWinEHSemanticRecord *CatchRow = nullptr;
  std::map<uint32_t, const CompiledWinEHSemanticRecord *> BySource, ByState;
  for (const auto &Row : Compiled.WinEHSemanticRecords) {
    if (Row.SourceFunction != Function.getName())
      continue;
    if (Row.OwnerVA != *Root || Row.Encoding != Encoding::X86CxxFH3 ||
        !Row.X86CxxLayout)
      return rejectCxx("row changed its generated function or encoding");
    if (Row.Token.Kind == Kind::CxxCatch) {
      const auto Token = windows_eh_semantics::getCxxCatchSemanticToken(
          Source, Arch::X86, Row.Token.Region, Row.Token.Clause);
      if (CatchRow || Row.Token.Region || Row.Token.Clause || !Token ||
          Row.Token != *Token)
        return rejectCxx("catch row has no unique source identity");
      CatchRow = &Row;
    } else if (Row.Token.Kind == Kind::CxxCleanup) {
      const auto Token = windows_eh_semantics::getCxxCleanupSemanticToken(
          Source, Arch::X86, Row.Token.Region);
      if (!Token || Row.Token != *Token ||
          !BySource.emplace(Row.Token.Region, &Row).second ||
          !ByState.emplace(Row.GeneratedState, &Row).second)
        return rejectCxx("cleanup row has no unique source identity");
    } else {
      return rejectCxx("unrecognized row belongs to a C++ source function");
    }
  }
  if (!CatchRow)
    return rejectCxx("generated FuncInfo lost its catch row");
  const auto &Layout = *CatchRow->X86CxxLayout;
  for (const auto &Table : Layout.Tables)
    if (!coff_registration::sectionAt(Compiled, Table.BeginVA,
                                      Table.EndVA - Table.BeginVA,
                                      SectionKind::ReadOnlyData))
      return rejectCxx("language table has no unique generated byte extent");
  auto Word = [&](va_t VA) {
    return llvm::support::endian::read32le(Compiled.Bytes.data() + VA -
                                           Compiled.BaseVA);
  };
  const va_t Info = Layout.Tables[0].BeginVA;
  const va_t Unwind = Layout.Tables[1].BeginVA;
  const va_t TryMap = Layout.Tables[2].BeginVA;
  const va_t HandlerMap = Layout.Tables[3].BeginVA;
  const uint32_t MaxState = (Layout.Tables[1].EndVA - Unwind) / 8;
  if (!MaxState || MaxState > 130 || Word(Info) != 0x19930522 ||
      Word(Info + 4) != MaxState || Word(Info + 8) != Unwind ||
      Word(Info + 12) != 1 || Word(Info + 16) != TryMap || Word(Info + 20) ||
      Word(Info + 24) || Word(Info + 28) || Word(Info + 32) != 1 ||
      Layout.Tables[2].EndVA - TryMap != 20 ||
      Layout.Tables[3].EndVA - HandlerMap != 16 ||
      CatchRow->ContainerVA != TryMap || CatchRow->RecordVA != HandlerMap ||
      Word(TryMap + 12) != 1 || Word(TryMap + 16) != HandlerMap)
    return rejectCxx("raw FuncInfo or try-map closure changed");
  const int32_t TryLow = int32_t(Word(TryMap));
  const int32_t TryHigh = int32_t(Word(TryMap + 4));
  const int32_t CatchHigh = int32_t(Word(TryMap + 8));
  if (TryLow < 0 || TryLow > TryHigh || TryHigh >= CatchHigh ||
      CatchHigh >= int32_t(MaxState))
    return rejectCxx("generated try interval is malformed");
  COFFRegistrationCxxTableReceipt Receipt;
  Receipt.OwnerVA = *Root;
  Receipt.FuncInfoVA = Info;
  for (unsigned I = 0; I != 3; ++I)
    Receipt.Tables[I] = {Layout.Tables[I].BeginVA, Layout.Tables[I].EndVA};
  auto Pointer = [&](va_t Field, llvm::StringRef Symbol, va_t Target) {
    const auto *Section = coff_registration::sectionAt(
        Compiled, Field, 4, SectionKind::ReadOnlyData);
    if (!Section || Word(Field) != Target ||
        !coff_registration::exactPointerFixup(*Section, Field, Symbol, Target))
      return false;
    Receipt.AbsolutePointerFields.push_back(Field);
    return true;
  };
  if (!Pointer(Info + 8, Layout.Tables[1].BeginSymbol, Unwind) ||
      !Pointer(Info + 16, Layout.Tables[2].BeginSymbol, TryMap) ||
      !Pointer(TryMap + 16, Layout.Tables[3].BeginSymbol, HandlerMap) ||
      !Pointer(HandlerMap + 12, CatchRow->HandlerSymbol, CatchRow->HandlerVA))
    return rejectCxx("table pointers lost exact compiler fixup ownership");
  const llvm::CatchPadInst *Pad = nullptr;
  for (const auto &Block : Function)
    for (const auto &I : Block)
      if (const auto *Candidate = llvm::dyn_cast<llvm::CatchPadInst>(&I)) {
        if (Pad)
          return rejectCxx("one source catch has multiple generated pads");
        Pad = Candidate;
      }
  if (!Pad || Pad->arg_size() != 3 ||
      !Function.hasFnAttribute(llvm::RewriteWinX86CxxFrameAttribute))
    return rejectCxx("catch object has no compiler shared-frame contract");
  auto Object = llvm::getRewriteWinX86CxxCatchFrameObject(*Pad);
  if (!Object)
    return Object.takeError();
  const auto *Type = llvm::dyn_cast<llvm::GlobalVariable>(
      Pad->getArgOperand(0)->stripPointerCasts());
  if (!Type || !Type->isDeclaration() || !Type->hasExternalLinkage() ||
      !Object->Frame || Word(HandlerMap) != Catch.Adjectives ||
      Word(HandlerMap + 4) != Catch.TypeDescriptorVA ||
      int32_t(Word(HandlerMap + 8)) != Layout.Frame[0] + Layout.Frame[2] ||
      Layout.Frame[2] != Object->Offset || Layout.Frame[3] != Object->Size)
    return rejectCxx("catch row changed its type or object subfield");
  const auto Size =
      Object->Frame->getAllocationSize(Function.getParent()->getDataLayout());
  if (!Size || Size->isScalable() ||
      Size->getFixedValue() != uint64_t(Layout.Frame[1]))
    return rejectCxx("catch row changed its whole-frame allocation");
  llvm::SmallString<64> TypeSymbol;
  llvm::Mangler Mangler;
  Mangler.getNameWithPrefix(TypeSymbol, Type, false);
  if (!Pointer(HandlerMap + 4, TypeSymbol, Catch.TypeDescriptorVA))
    return rejectCxx("typed catch lost the original RTTI pointer identity");
  auto &Mapping = Receipt.SourceToGeneratedStates;
  Mapping.emplace(-1, -1);
  Mapping.emplace(Try.TryLow, TryLow);
  if (!Mapping.emplace(Try.CatchHigh, CatchHigh).second)
    return rejectCxx("catch and try states conflict");
  size_t Actions = 0;
  for (uint32_t I = 0; I < Cxx.UnwindMap.size(); ++I) {
    const auto &Action = Cxx.UnwindMap[I];
    if (!Action.ActionVA)
      continue;
    ++Actions;
    const auto Found = BySource.find(I);
    if (Found == BySource.end() || int32_t(I) <= Try.TryLow ||
        int32_t(I) > Try.TryHigh ||
        !Mapping.emplace(I, Found->second->GeneratedState).second)
      return rejectCxx("source cleanup is absent or outside the lowered try");
    const auto &Row = *Found->second;
    if (Row.RecordVA != Unwind + uint64_t(Row.GeneratedState) * 8 ||
        int32_t(Word(Row.RecordVA)) != Row.EnclosingState ||
        !Pointer(Row.RecordVA + 4, Row.HandlerSymbol, Row.HandlerVA))
      return rejectCxx("cleanup bytes disagree with their indexed row");
  }
  if (Actions != BySource.size())
    return rejectCxx("extra cleanup rows have no source unwind action");
  std::function<std::optional<int32_t>(int32_t)> Project =
      [&](int32_t State) -> std::optional<int32_t> {
    const auto Found = Mapping.find(State);
    if (Found != Mapping.end())
      return Found->second;
    if (State <= Try.TryLow || State > Try.TryHigh ||
        uint32_t(State) >= Cxx.UnwindMap.size())
      return std::nullopt;
    const auto &Action = Cxx.UnwindMap[State];
    if (Action.ActionVA || Action.ToState < Try.TryLow ||
        Action.ToState >= State)
      return std::nullopt;
    auto Outer = Project(Action.ToState);
    if (Outer)
      Mapping.emplace(State, *Outer);
    return Outer;
  };
  for (int32_t I = 0; I < int32_t(Cxx.UnwindMap.size()); ++I) {
    const auto State = Project(I);
    if (!State || *State < TryLow || *State >= int32_t(MaxState) ||
        (I <= Try.TryHigh && *State > TryHigh))
      return rejectCxx(
          "source state has no exact generated dispatch projection");
  }
  std::map<int32_t, int32_t> GeneratedSources = {{TryLow, Try.TryLow},
                                                 {CatchHigh, Try.CatchHigh}};
  for (const auto &[SourceState, Row] : BySource)
    if (!GeneratedSources.emplace(Row->GeneratedState, SourceState).second)
      return rejectCxx("cleanup reused another generated dispatch state");
  if (GeneratedSources.size() != MaxState)
    return rejectCxx("generated unwind map contains an unbound state");
  for (uint32_t I = 0; I < MaxState; ++I) {
    if (!GeneratedSources.count(I))
      return rejectCxx("generated unwind state numbering has a hole");
    const int32_t SourceState = GeneratedSources.at(I);
    const auto ExpectedOuter = Project(Cxx.UnwindMap[SourceState].ToState);
    if (!ExpectedOuter || int32_t(Word(Unwind + I * 8)) != *ExpectedOuter ||
        (!ByState.count(I) && Word(Unwind + I * 8 + 4)))
      return rejectCxx(
          "generated unwind edges changed the source action graph");
  }
  llvm::sort(Receipt.AbsolutePointerFields);
  if (std::adjacent_find(Receipt.AbsolutePointerFields.begin(),
                         Receipt.AbsolutePointerFields.end()) !=
      Receipt.AbsolutePointerFields.end())
    return rejectCxx("language pointer fields overlap");
  return Receipt;
#endif
}
} // namespace neverd
