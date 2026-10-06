#ifndef NEVERD_SDK_CAPI_OBJCMERGEDSETTERSOURCES_H
#define NEVERD_SDK_CAPI_OBJCMERGEDSETTERSOURCES_H

#include "../../loader/MachO/ImmutableNativeFrame.h"
#include "../../loader/Swift/SwiftVirtualSlot.h"
#include "ObjCSuperGetterSources.h"

namespace neverd::sdk {
namespace objc_merged_setter_detail {
using namespace objc_super_getter_detail;

struct Contract {
  va_t Root = 0, Entry = 0, SelectorSlot = 0, Counter = 0, ProfileBase = 0;
  uint32_t VirtualSlot = 0;
  ObjCClassAccessorContract Accessor;
  SourceFunctionTypeHint Method, Helper, Message, Virtual;
  SourceCallTypeHint Mask;
  SourceCallTypeHint Layout;
};

inline bool canonicalSetter(const SourceFunctionTypeHint &Signature) {
  if (Signature.Parameters.size() != 3)
    return false;
  const auto Expected = objcMergedSetterSourceDeclaration(
      Signature.Architecture, Signature.Parameters[2].Type);
  if (!Expected)
    return false;
  auto Normalized = Signature;
  Normalized.Origin = Expected->Origin;
  return equalSourceABIs(Normalized, *Expected);
}

inline bool canonicalBoolSetter(const SourceFunctionTypeHint &Signature) {
  return canonicalSetter(Signature) &&
         Signature.Parameters[2].Components.empty();
}

inline bool canonicalCGRectSetter(const SourceFunctionTypeHint &Signature) {
  return canonicalSetter(Signature) &&
         Signature.Parameters[2].Components.size() == 4;
}

inline bool isCGRect(const Contract &C) {
  return canonicalCGRectSetter(C.Method);
}

inline const LowFunc *machine(const BinaryImage &Image,
                              const PipelineResult &Result, va_t Entry,
                              unsigned Count, size_t &Budget) {
  const auto *Low = completeLow(Result, Entry, Count);
  const auto *Med = uniqueEntry(Result.MedFuncs, Entry);
  const auto *High = uniqueEntry(Result.HighFuncs, Entry);
  if (!Low || !Med || !High || High->DoesNotReturn ||
      High->StructuredExceptionRegions || High->UnstructuredExceptionRegions ||
      !immutableNativeFrameMachineMatches(Image, *Low, Budget))
    return nullptr;
  return Low;
}

// A complete compiler body is proved in the context of one typed Objective-C
// tail caller. This never invents an ABI from a merged Swift symbol and never
// makes that ABI available to another, unproved caller of the same helper.
inline std::optional<Contract> proveBool(const BinaryImage &Image,
                                         const PipelineResult &Result,
                                         const ObjCProfileStorage &Storage,
                                         va_t Root) {
  if (Result.SourceImage != &Image || Image.Format != BinaryFormat::MachO ||
      Image.Arch != Arch::AArch64 || Image.Bits != Bitness::Bits64 ||
      Image.IsRelocatable || Root % 4 ||
      !objc::RuntimeData(Image).supportsPlainObjectPointers())
    return std::nullopt;
  const auto Caller = readImmutableCodeBytes(Image, Root, 20);
  if (!Caller)
    return std::nullopt;
  const auto CW = [&](unsigned I) {
    return llvm::support::endian::read32le(Caller->data() + I * 4);
  };
  const auto Selector = pageAddress(CW(0), CW(1), Root, 3);
  const auto Counter = pageAddress(CW(2), CW(3), Root + 8, 4);
  const auto Entry = branch(CW(4), Root + 16, false);
  const auto Base = Counter && *Counter % 8 == 0
                        ? Storage.sectionFor(*Counter, 8)
                        : std::nullopt;
  if (!Selector || !Counter || !Entry || !Base)
    return std::nullopt;
  const ObjCMethod *Method = nullptr;
  for (const auto &M : Image.ObjCMethods)
    if (M.Implementation == Root) {
      if (Method || M.Status != "supported" || M.IsClassMethod || !M.TypeHint)
        return std::nullopt;
      Method = &M;
    }
  const auto Declaration = objcMethodSourceTypeHint(Image, Root);
  const auto *High = uniqueEntry(Result.HighFuncs, Root);
  const auto Ref = Image.ObjCSourceReferences.find(*Selector);
  if (!Method || !Declaration || !canonicalBoolSetter(*Declaration) || !High ||
      !High->SourceTypeHint || High->Params.size() != 3 ||
      !equalSourceABIs(*Declaration, *High->SourceTypeHint) ||
      !equalSourceTypes(High->ReturnType, Declaration->ReturnType) ||
      Ref == Image.ObjCSourceReferences.end() ||
      Ref->second.Address != *Selector || Ref->second.Size != 8 ||
      Ref->second.TheKind != ObjCSourceReference::Kind::Selector ||
      Ref->second.Name != Method->Selector)
    return std::nullopt;
  for (unsigned I = 0; I < 3; ++I)
    if (!equalSourceTypes(High->Params[I].Type,
                          Declaration->Parameters[I].Type))
      return std::nullopt;
  const auto Message = objcSelectorSourceTypeHint(Image, Method->Selector);
  if (!Message || !canonicalBoolSetter(*Message))
    return std::nullopt;

  const auto Prefix = readImmutableCodeBytes(Image, *Entry, 52);
  if (!Prefix)
    return std::nullopt;
  const bool OrdinaryARC =
      llvm::support::endian::read32le(Prefix->data() + 48) == 0xaa1603e0u;
  const unsigned Extra = OrdinaryARC ? 1 : 0, Count = 35 + 2 * Extra;
  const auto Bytes = readImmutableCodeBytes(Image, *Entry, Count * 4);
  if (!Bytes || !isMachOLocalFunctionRange(Image, *Entry, Count * 4))
    return std::nullopt;
  const auto Word = [&](unsigned I) {
    return llvm::support::endian::read32le(Bytes->data() + I * 4);
  };
  constexpr uint32_t PrefixWords[] = {0xd10103ff, 0xa90157f6, 0xa9024ff4,
                                      0xa9037bfd, 0x9100c3fd, 0xaa0403f3,
                                      0xaa0303f4, 0xaa0203f5, 0xaa0003f6};
  for (unsigned I = 0; I < std::size(PrefixWords); ++I)
    if (Word(I) != PrefixWords[I])
      return std::nullopt;
  constexpr std::pair<unsigned, uint32_t> Middle[] = {
      {13, 0xaa0003f6}, {14, 0x910003e0}, {15, 0xaa1403e1}, {16, 0xaa1503e2},
      {18, 0xf9400268}, {19, 0x91000508}, {20, 0xf9000268}, {21, 0xf94002c8},
      {24, 0xf9400129}, {25, 0x8a080128}, {27, 0xaa1603f4}, {28, 0xd63f0100}};
  for (const auto &[I, Bits] : Middle)
    if (Word(I + Extra) != Bits)
      return std::nullopt;
  constexpr uint32_t Suffix[] = {0xa9437bfd, 0xa9424ff4, 0xa94157f6, 0x910103ff,
                                 0xd65f03c0};
  for (unsigned I = 0; I < std::size(Suffix); ++I)
    if (Word(30 + 2 * Extra + I) != Suffix[I])
      return std::nullopt;
  if (Word(10) != 0xa90003f6 || Word(11) != 0xf9400294 ||
      (OrdinaryARC && Word(29 + Extra) != 0xaa1603e0u) ||
      (Word(23 + Extra) & 0xffc003ff) != 0xf9400129u ||
      (Word(26 + Extra) & 0xffc003ff) != 0xf9400108u)
    return std::nullopt;
  const auto AccessorTarget = branch(Word(9), *Entry + 36, true);
  const auto RetainTarget =
      branch(Word(12 + Extra), *Entry + (12 + Extra) * 4, true);
  const auto SuperTarget =
      branch(Word(17 + Extra), *Entry + (17 + Extra) * 4, true);
  const auto ReleaseTarget =
      branch(Word(29 + 2 * Extra), *Entry + (29 + 2 * Extra) * 4, true);
  const auto RetainSlot =
      RetainTarget
          ? runtimeSlot(Image, *RetainTarget,
                        OrdinaryARC ? "_objc_retain" : "_objc_retain_x22")
          : std::nullopt;
  const auto ReleaseSlot =
      ReleaseTarget
          ? runtimeSlot(Image, *ReleaseTarget,
                        OrdinaryARC ? "_objc_release" : "_objc_release_x22")
          : std::nullopt;
  const auto Retain =
      RetainSlot ? objcRuntimeSourceCallHint(Image, *RetainSlot) : std::nullopt;
  const auto Release = ReleaseSlot
                           ? objcRuntimeSourceCallHint(Image, *ReleaseSlot)
                           : std::nullopt;
  const auto Accessor =
      AccessorTarget ? validatedClassAccessor(Image, Result, *AccessorTarget)
                     : std::nullopt;
  const auto MaskPage = page(Word(22 + Extra), *Entry + (22 + Extra) * 4, 9);
  const auto MaskSlot =
      MaskPage ? addSigned(*MaskPage, ((Word(23 + Extra) >> 10) & 4095) * 8)
               : std::nullopt;
  const auto Mask = MaskSlot ? darwinRuntimeGlobalAddressHint(Image, *MaskSlot)
                             : std::nullopt;
  const uint32_t Slot = ((Word(26 + Extra) >> 10) & 4095) * 8;
  const auto Virtual = swift_virtual_detail::voidClassVirtualSlotDeclaration(
      Image, *Method, Slot);
  if (!Retain || !Release || Retain->DoesNotReturn || Release->DoesNotReturn ||
      !SuperTarget ||
      !runtimeSlot(Image, *SuperTarget, "_objc_msgSendSuper2") || !Accessor ||
      Accessor->ClassAddress != Method->ClassAddress || !Mask ||
      Mask->CallKind != SourceCallTypeHint::Kind::DarwinRuntimeGlobalAddress ||
      Mask->TargetName != "swift_isaMask" || !Virtual)
    return std::nullopt;
  const ObjCClass *Class = nullptr;
  for (const auto &Candidate : Image.ObjCClasses)
    if (Candidate.Address == Method->ClassAddress ||
        Candidate.Name == Method->ClassName) {
      if (Class || Candidate.Address != Method->ClassAddress ||
          Candidate.Name != Method->ClassName || Candidate.RootClass ||
          Candidate.InheritanceStatus != "resolved" ||
          Candidate.SuperclassName.empty())
        return std::nullopt;
      Class = &Candidate;
    }
  if (!Class)
    return std::nullopt;
  size_t Budget = 1U << 18;
  const auto *CallerLow = machine(Image, Result, Root, 5, Budget);
  const auto *Low = machine(Image, Result, *Entry, Count, Budget);
  if (!CallerLow || !Low || !machine(Image, Result, Accessor->Entry, 8, Budget))
    return std::nullopt;
  Contract C;
  C.Root = Root;
  C.Entry = *Entry;
  C.SelectorSlot = *Selector;
  C.Counter = *Counter;
  C.ProfileBase = *Base;
  C.VirtualSlot = Slot;
  C.Accessor = *Accessor;
  C.Method = *Declaration;
  C.Message = *Message;
  C.Virtual = *Virtual;
  C.Mask = *Mask;
  C.Helper.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  C.Helper.ReturnType = NdType::makeVoid();
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  C.Helper.Parameters = {{"self", Pointer},
                         {"command", Pointer},
                         {"value", C.Method.Parameters[2].Type},
                         {"selector_slot", Pointer},
                         {"counter", Pointer}};
  std::string Error;
  if (!assignDarwinScalarSourceABI(C.Helper, Image.Arch, Error))
    return std::nullopt;
  NativeSourceCalls Calls;
  for (const auto &Op : Low->Blocks.front().Ops) {
    if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
      continue;
    const auto Key = nativeSourceCallKey(Op);
    if (!Key)
      return std::nullopt;
    NativeSourceCallContract Call;
    if (Op.Opcode == NdOp::INDIR_CALL && Op.Addr == *Entry + (28 + Extra) * 4 &&
        Op.NumInputs == 1 && Op.Inputs[0] == NdVar::reg(64, 8))
      Call.Signature = &C.Virtual;
    else if (Op.Opcode == NdOp::CALL && Op.NumInputs == 1 &&
             Op.Inputs[0].isConst()) {
      const auto Target = Op.Inputs[0].Offset;
      if (Op.Addr == *Entry + 36 && Target == Accessor->Entry)
        Call.Signature = &C.Accessor.Signature;
      else if (Op.Addr == *Entry + (12 + Extra) * 4 && Target == *RetainTarget)
        Call.Signature = &Retain->Signature;
      else if (Op.Addr == *Entry + (17 + Extra) * 4 && Target == *SuperTarget) {
        Call.Signature = &C.Message;
        // The exact objc_super record is synchronous runtime input. Its two
        // words are ordinary entry/runtime pointers, not private frame aliases.
        Call.ReadOnlyFrameParameters.emplace(0, 16);
      } else if (Op.Addr == *Entry + (29 + 2 * Extra) * 4 &&
                 Target == *ReleaseTarget)
        Call.Signature = &Release->Signature;
    }
    if (!Call.Signature || !Calls.emplace(*Key, Call).second)
      return std::nullopt;
  }
  if (Calls.size() != 5 ||
      !restoresNativeSourceState(*Low, Image.Arch, Calls) ||
      !callsRestore(*CallerLow, Root + 16, *Entry, C.Helper))
    return std::nullopt;
  return C;
}

// This compiler body preserves four CGRect parameter carriers across the class
// accessor and ARC call, dispatches to the superclass, updates numeric profile
// storage and then requests layout on the retained receiver. All operations,
// including the selector-loading layout stub, are checked anew for each caller.
inline std::optional<Contract> proveCGRect(const BinaryImage &Image,
                                           const PipelineResult &Result,
                                           const ObjCProfileStorage &Storage,
                                           va_t Root) {
  if (Result.SourceImage != &Image || Image.Format != BinaryFormat::MachO ||
      Image.Arch != Arch::AArch64 || Image.Bits != Bitness::Bits64 ||
      Image.IsRelocatable || Root % 4 ||
      !objc::RuntimeData(Image).supportsPlainObjectPointers())
    return std::nullopt;
  const auto Caller = readImmutableCodeBytes(Image, Root, 20);
  if (!Caller)
    return std::nullopt;
  const auto CW = [&](unsigned I) {
    return llvm::support::endian::read32le(Caller->data() + I * 4);
  };
  const auto Selector = pageAddress(CW(0), CW(1), Root, 2);
  const auto Counter = pageAddress(CW(2), CW(3), Root + 8, 3);
  const auto Entry = branch(CW(4), Root + 16, false);
  const auto Base = Counter && *Counter % 8 == 0
                        ? Storage.sectionFor(*Counter, 8)
                        : std::nullopt;
  if (!Selector || !Counter || !Entry || !Base)
    return std::nullopt;
  const ObjCMethod *Method = nullptr;
  for (const auto &M : Image.ObjCMethods)
    if (M.Implementation == Root) {
      if (Method || M.Status != "supported" || M.IsClassMethod || !M.TypeHint)
        return std::nullopt;
      Method = &M;
    }
  const auto Declaration = objcMethodSourceTypeHint(Image, Root);
  const auto *High = uniqueEntry(Result.HighFuncs, Root);
  const auto Ref = Image.ObjCSourceReferences.find(*Selector);
  if (!Method || !Declaration || !canonicalCGRectSetter(*Declaration) ||
      !High || !High->SourceTypeHint || High->Params.size() != 3 ||
      !equalSourceABIs(*Declaration, *High->SourceTypeHint) ||
      !equalSourceTypes(High->ReturnType, Declaration->ReturnType) ||
      Ref == Image.ObjCSourceReferences.end() ||
      Ref->second.Address != *Selector || Ref->second.Size != 8 ||
      Ref->second.TheKind != ObjCSourceReference::Kind::Selector ||
      Ref->second.Name != Method->Selector)
    return std::nullopt;
  for (unsigned I = 0; I != 3; ++I)
    if (!equalSourceTypes(High->Params[I].Type,
                          Declaration->Parameters[I].Type))
      return std::nullopt;
  const auto Message = objcSelectorSourceTypeHint(Image, Method->Selector);
  if (!Message || !canonicalCGRectSetter(*Message))
    return std::nullopt;
  auto Normalized = *Message;
  Normalized.Origin = Declaration->Origin;
  if (!equalSourceABIs(Normalized, *Declaration))
    return std::nullopt;
  constexpr unsigned Count = 39;
  const auto Bytes = readImmutableCodeBytes(Image, *Entry, Count * 4);
  if (!Bytes || !isMachOLocalFunctionRange(Image, *Entry, Count * 4))
    return std::nullopt;
  const auto Word = [&](unsigned I) {
    return llvm::support::endian::read32le(Bytes->data() + I * 4);
  };
  constexpr std::pair<unsigned, uint32_t> Fixed[] = {
      {0, 0xd10183ff},  {1, 0x6d012beb},  {2, 0x6d0223e9},  {3, 0xa90357f6},
      {4, 0xa9044ff4},  {5, 0xa9057bfd},  {6, 0x910143fd},  {7, 0xaa0303f3},
      {8, 0xaa0203f4},  {9, 0x4ea31c68},  {10, 0x4ea21c49}, {11, 0x4ea11c2a},
      {12, 0x4ea01c0b}, {13, 0xaa0003f5}, {15, 0xa90003f5}, {16, 0xf9400294},
      {18, 0xaa0003f5}, {19, 0x910003e0}, {20, 0xaa1403e1}, {21, 0x4eab1d60},
      {22, 0x4eaa1d41}, {23, 0x4ea91d22}, {24, 0x4ea81d03}, {26, 0xf9400268},
      {27, 0x91000508}, {28, 0xf9000268}, {29, 0xaa1503e0}, {32, 0xa9457bfd},
      {33, 0xa9444ff4}, {34, 0xa94357f6}, {35, 0x6d4223e9}, {36, 0x6d412beb},
      {37, 0x910183ff}, {38, 0xd65f03c0}};
  for (const auto &[I, Bits] : Fixed)
    if (Word(I) != Bits)
      return std::nullopt;
  const auto AccessorTarget = branch(Word(14), *Entry + 56, true);
  const auto RetainTarget = branch(Word(17), *Entry + 68, true);
  const auto SuperTarget = branch(Word(25), *Entry + 100, true);
  const auto LayoutTarget = branch(Word(30), *Entry + 120, true);
  const auto ReleaseTarget = branch(Word(31), *Entry + 124, true);
  const auto Accessor =
      AccessorTarget ? validatedClassAccessor(Image, Result, *AccessorTarget)
                     : std::nullopt;
  const auto RetainSlot =
      RetainTarget ? runtimeSlot(Image, *RetainTarget, "_objc_retain_x21")
                   : std::nullopt;
  const auto ReleaseSlot =
      ReleaseTarget ? runtimeSlot(Image, *ReleaseTarget, "_objc_release_x21")
                    : std::nullopt;
  const auto Retain =
      RetainSlot ? objcRuntimeSourceCallHint(Image, *RetainSlot) : std::nullopt;
  const auto Release = ReleaseSlot
                           ? objcRuntimeSourceCallHint(Image, *ReleaseSlot)
                           : std::nullopt;
  const auto Layout =
      LayoutTarget
          ? objcImmutableSelectorStubSourceCallHint(Image, *LayoutTarget)
          : std::nullopt;
  auto VoidMessage = parseObjCMethodEncoding("setNeedsLayout", "v16@0:8");
  std::string Error;
  if (!Accessor || Accessor->ClassAddress != Method->ClassAddress || !Retain ||
      !Release || Retain->DoesNotReturn || Release->DoesNotReturn ||
      !SuperTarget ||
      !runtimeSlot(Image, *SuperTarget, "_objc_msgSendSuper2") || !Layout ||
      Layout->Selector != "setNeedsLayout" || !VoidMessage ||
      !assignDarwinObjCSourceABI(*VoidMessage, Image.Arch, Error))
    return std::nullopt;
  auto LayoutSignature = Layout->Signature;
  LayoutSignature.Origin = VoidMessage->Origin;
  if (!equalSourceABIs(LayoutSignature, *VoidMessage))
    return std::nullopt;
  const ObjCClass *Class = nullptr;
  for (const auto &Candidate : Image.ObjCClasses)
    if (Candidate.Address == Method->ClassAddress ||
        Candidate.Name == Method->ClassName) {
      if (Class || Candidate.Address != Method->ClassAddress ||
          Candidate.Name != Method->ClassName || Candidate.RootClass ||
          Candidate.InheritanceStatus != "resolved" ||
          Candidate.SuperclassName.empty())
        return std::nullopt;
      Class = &Candidate;
    }
  if (!Class)
    return std::nullopt;
  size_t Budget = 1U << 18;
  const auto *CallerLow = machine(Image, Result, Root, 5, Budget);
  const auto *Low = machine(Image, Result, *Entry, Count, Budget);
  if (!CallerLow || !Low || !machine(Image, Result, Accessor->Entry, 8, Budget))
    return std::nullopt;
  Contract C;
  C.Root = Root;
  C.Entry = *Entry;
  C.SelectorSlot = *Selector;
  C.Counter = *Counter;
  C.ProfileBase = *Base;
  C.Accessor = *Accessor;
  C.Method = *Declaration;
  C.Message = *Message;
  C.Layout = *Layout;
  const auto Helper = objcMergedSetterHelperSourceDeclaration(
      Image.Arch, Declaration->Parameters[2].Type);
  if (!Helper)
    return std::nullopt;
  C.Helper = *Helper;
  C.Helper.Parameters.resize(5);
  C.Helper.Parameters[4].Name = "counter";
  if (!assignDarwinFixedSourceABI(C.Helper, Image.Arch, Error))
    return std::nullopt;
  NativeSourceCalls Calls;
  for (const auto &Op : Low->Blocks.front().Ops) {
    if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
      continue;
    const auto Key = nativeSourceCallKey(Op);
    if (!Key || Op.Opcode != NdOp::CALL || Op.NumInputs != 1 ||
        !Op.Inputs[0].isConst())
      return std::nullopt;
    NativeSourceCallContract Call;
    const auto Target = Op.Inputs[0].Offset;
    if (Op.Addr == *Entry + 56 && Target == Accessor->Entry)
      Call.Signature = &C.Accessor.Signature;
    else if (Op.Addr == *Entry + 68 && Target == *RetainTarget)
      Call.Signature = &Retain->Signature;
    else if (Op.Addr == *Entry + 100 && Target == *SuperTarget) {
      Call.Signature = &C.Message;
      Call.ReadOnlyFrameParameters.emplace(0, 16);
    } else if (Op.Addr == *Entry + 120 && Target == *LayoutTarget)
      Call.Signature = &C.Layout.Signature;
    else if (Op.Addr == *Entry + 124 && Target == *ReleaseTarget)
      Call.Signature = &Release->Signature;
    if (!Call.Signature || !Calls.emplace(*Key, Call).second)
      return std::nullopt;
  }
  if (Calls.size() != 5 ||
      !restoresNativeSourceState(*Low, Image.Arch, Calls) ||
      !callsRestore(*CallerLow, Root + 16, *Entry, C.Helper))
    return std::nullopt;
  return C;
}

inline std::optional<Contract> prove(const BinaryImage &Image,
                                     const PipelineResult &Result,
                                     const ObjCProfileStorage &Storage,
                                     va_t Root) {
  if (const auto C = proveBool(Image, Result, Storage, Root))
    return C;
  return proveCGRect(Image, Result, Storage, Root);
}

inline std::string helperName(va_t Root) {
  return "neverd_objc_merged_setter_" + llvm::utohexstr(Root, true);
}

inline SourceCallTypeHint setterHint(const Contract &C) {
  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::RuntimeObjCMergedSetter;
  Hint.TargetAddress = C.Root;
  Hint.TargetName = helperName(C.Root);
  const auto Signature = objcMergedSetterHelperSourceDeclaration(
      Arch::AArch64, C.Method.Parameters[2].Type);
  if (!Signature)
    throw std::runtime_error("invalid merged setter source ABI");
  Hint.Signature = *Signature;
  return Hint;
}

inline std::vector<SourceCallTypeHint> addressHints(const BinaryImage &Image,
                                                    const Contract &C) {
  const auto Profile =
      objc_binding_detail::profileStorageHint(Image.Arch, C.ProfileBase);
  if (!Profile)
    return {};
  return {addressHint(SourceCallTypeHint::Kind::RuntimeSelectorReferenceAddress,
                      C.SelectorSlot, selectorName(C.SelectorSlot)),
          *Profile,
          addressHint(SourceCallTypeHint::Kind::NativeAddress, C.Accessor.Entry,
                      {}),
          isCGRect(C)
              ? addressHint(
                    SourceCallTypeHint::Kind::RuntimeSelectorReferenceAddress,
                    C.Layout.SelectorReferenceAddress,
                    selectorName(C.Layout.SelectorReferenceAddress))
              : C.Mask};
}
} // namespace objc_merged_setter_detail

struct ObjCMergedSetterSourcePlan {
  const PipelineResult *Pipeline = nullptr;
  std::set<va_t> Callers;
};

inline size_t seedObjCMergedSetterAccessorHints(const BinaryImage &Image,
                                                PipelineOptions &Options) {
  using namespace objc_merged_setter_detail;
  if (Image.Format != BinaryFormat::MachO || Image.Arch != Arch::AArch64 ||
      Image.Bits != Bitness::Bits64 || Image.IsRelocatable)
    return 0;
  const auto Classes = objc_binding_detail::classObjectIdentities(Image);
  size_t Added = 0;
  for (const auto &M : Image.ObjCMethods) {
    if (M.Status != "supported" || M.IsClassMethod || !M.TypeHint ||
        !canonicalSetter(*M.TypeHint))
      continue;
    const bool Rect = canonicalCGRectSetter(*M.TypeHint);
    const auto Caller = readImmutableCodeBytes(Image, M.Implementation, 20);
    if (!Caller ||
        !pageAddress(llvm::support::endian::read32le(Caller->data()),
                     llvm::support::endian::read32le(Caller->data() + 4),
                     M.Implementation, Rect ? 2 : 3) ||
        !pageAddress(llvm::support::endian::read32le(Caller->data() + 8),
                     llvm::support::endian::read32le(Caller->data() + 12),
                     M.Implementation + 8, Rect ? 3 : 4))
      continue;
    const auto Entry =
        branch(llvm::support::endian::read32le(Caller->data() + 16),
               M.Implementation + 16, false);
    const unsigned CallOffset = Rect ? 56 : 36;
    const auto Body =
        Entry ? readImmutableCodeBytes(Image, *Entry, CallOffset + 4)
              : std::nullopt;
    const auto Target =
        Body
            ? branch(llvm::support::endian::read32le(Body->data() + CallOffset),
                     *Entry + CallOffset, true)
            : std::nullopt;
    const auto A =
        Target ? objcClassAccessorMachine(Image, *Target) : std::nullopt;
    const auto Identity = A ? Classes.find(A->ClassAddress) : Classes.end();
    // Only the accessor's complete independent machine proof supplies its
    // declaration. Discovering this use grants nothing to the shared setter.
    if (!A || Identity == Classes.end() ||
        Identity->second.Kind != SourceCallTypeHint::Kind::RuntimeClass ||
        Identity->second.Name != M.ClassName ||
        A->ClassAddress != M.ClassAddress)
      continue;
    Added += Options.SourceTypeHints.emplace(A->Entry, A->Signature).second;
  }
  return Added;
}

inline ObjCMergedSetterSourcePlan
discoverObjCMergedSetterSources(const BinaryImage &Image,
                                const PipelineResult &Result,
                                const ObjCProfileStorage &Storage) {
  ObjCMergedSetterSourcePlan Plan{&Result, {}};
  for (const auto &Method : Image.ObjCMethods) {
    const auto *Bytes = Image.readVA(Method.Implementation, 4);
    if (Bytes &&
        ((llvm::support::endian::read32le(Bytes) & 0x9f00001f) == 0x90000003u ||
         (llvm::support::endian::read32le(Bytes) & 0x9f00001f) ==
             0x90000002u) &&
        objc_merged_setter_detail::prove(Image, Result, Storage,
                                         Method.Implementation))
      Plan.Callers.insert(Method.Implementation);
  }
  return Plan;
}

inline std::optional<objc_merged_setter_detail::Contract>
validatedObjCMergedSetter(const BinaryImage &Image,
                          const ObjCMergedSetterSourcePlan &Plan,
                          const ObjCProfileStorage &Storage, va_t Root) {
  if (!Plan.Pipeline || !Plan.Callers.count(Root))
    return std::nullopt;
  // Storage ownership must still hold in the current image, not merely in a
  // discovery-time snapshot. The shared renderer retains the original bytes.
  const ObjCProfileStorage Current(Image);
  const auto C =
      objc_merged_setter_detail::prove(Image, *Plan.Pipeline, Current, Root);
  return C &&
                 Storage.sectionFor(C->Counter, 8) ==
                     std::optional<va_t>(C->ProfileBase) &&
                 Storage.matchesSection(Current, C->ProfileBase)
             ? C
             : std::nullopt;
}

struct ObjCMergedSetterSourceProjection {
  HighFunc Function;
  std::set<va_t> Dependencies, ProfileSections;
  bool Projected = false;
};

inline ObjCMergedSetterSourceProjection
projectObjCMergedSetter(const HighFunc &Function, const BinaryImage &Image,
                        const ObjCMergedSetterSourcePlan &Plan,
                        const ObjCProfileStorage &Storage) {
  ObjCMergedSetterSourceProjection P{Function, {}, {}, false};
  const auto C =
      validatedObjCMergedSetter(Image, Plan, Storage, Function.Entry);
  if (!C || !Function.SourceTypeHint || Function.Params.size() != 3 ||
      !equalSourceABIs(*Function.SourceTypeHint, C->Method))
    return P;
  const auto Hint = objc_merged_setter_detail::setterHint(*C);
  std::vector<ExprPtr> Args;
  for (unsigned I = 0; I < 3; ++I) {
    if (!equalSourceTypes(Function.Params[I].Type,
                          C->Method.Parameters[I].Type))
      return P;
    MedVar V;
    V.Kind = MedVar::Param;
    V.Id = I;
    V.Size = Function.Params[I].Type->Size;
    V.RegOff = C->Method.Parameters[I].Location.RegisterOffset;
    V.TheArch = Image.Arch;
    Args.push_back(HighExpr::makeVar(V, Function.Params[I].Type));
  }
  const auto Addresses = objc_merged_setter_detail::addressHints(Image, *C);
  if (Addresses.size() != 4)
    return P;
  for (const auto &A : Addresses) {
    auto E = HighExpr::makeCall({}, 0, {});
    E->SourceCallHint = std::make_shared<SourceCallTypeHint>(A);
    E->Type = A.Signature.ReturnType;
    Args.push_back(std::move(E));
  }
  HighStmt Call;
  Call.Kind = StmtKind::Call;
  Call.Addr = Function.Entry + 16;
  Call.CallExpr = HighExpr::makeCall({}, C->Root, std::move(Args));
  Call.CallExpr->SourceCallHint = std::make_shared<SourceCallTypeHint>(Hint);
  Call.CallExpr->Type = Hint.Signature.ReturnType;
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.Addr = Function.Entry + 16;
  P.Function.Body = {std::move(Call), std::move(Return)};
  P.Function.Locals.clear();
  P.Dependencies.insert(C->Accessor.Entry);
  P.ProfileSections.insert(C->ProfileBase);
  P.Projected = true;
  return P;
}

inline bool objCMergedSetterSourceCallBound(
    const HighExpr &Expression, const BinaryImage &Image,
    const ObjCMergedSetterSourcePlan &Plan, const ObjCProfileStorage &Storage,
    const HighFunc &Function,
    const std::map<va_t, const HighFunc *> &Functions) {
  using namespace objc_merged_setter_detail;
  const auto C =
      validatedObjCMergedSetter(Image, Plan, Storage, Function.Entry);
  if (!C || !Function.SourceTypeHint ||
      !equalSourceABIs(*Function.SourceTypeHint, C->Method) ||
      !equalSourceTypes(Function.ReturnType, C->Method.ReturnType) ||
      Function.DoesNotReturn || Function.StructuredExceptionRegions ||
      Function.UnstructuredExceptionRegions || Function.Params.size() != 3 ||
      !Function.Locals.empty() || Function.Body.size() != 2 ||
      Function.Body[0].Kind != StmtKind::Call || !Function.Body[0].CallExpr ||
      Function.Body[0].Addr != C->Root + 16 ||
      Function.Body[1].Kind != StmtKind::Return ||
      Function.Body[1].Addr != C->Root + 16 || Function.Body[1].RetVal)
    return false;
  const auto &Call = *Function.Body[0].CallExpr;
  if (!callMatches(Call, setterHint(*C), C->Root))
    return false;
  for (unsigned I = 0; I < 3; ++I) {
    if (!Call.Operands[I] || !equalSourceTypes(Function.Params[I].Type,
                                               C->Method.Parameters[I].Type))
      return false;
    // A logical HFA parameter has a component list instead of one physical
    // word. Its complete current ABI was independently authenticated above;
    // retain its actual logical location and reuse the expression validator.
    const auto &E = *Call.Operands[I];
    auto Identity = E;
    Identity.Var.RegOff = I * 8;
    if (E.Var.TheArch != Image.Arch ||
        E.Var.RegOff != C->Method.Parameters[I].Location.RegisterOffset ||
        !parameter(Identity, I, C->Method.Parameters[I].Type,
                   C->Method.Parameters[I].Type->Size))
      return false;
  }
  const auto Addresses = addressHints(Image, *C);
  if (Addresses.size() != 4)
    return false;
  for (unsigned I = 0; I < 4; ++I)
    if (!Call.Operands[I + 3] ||
        !callMatches(*Call.Operands[I + 3], Addresses[I], 0))
      return false;
  const auto Provider = Functions.find(C->Accessor.Entry);
  if (Provider == Functions.end() || !Provider->second ||
      Provider->second->Entry != C->Accessor.Entry ||
      !Provider->second->SourceTypeHint ||
      !equalSourceABIs(*Provider->second->SourceTypeHint,
                       C->Accessor.Signature))
    return false;
  if (&Expression == &Call)
    return true;
  for (unsigned I = 3; I < 7; ++I)
    if (&Expression == Call.Operands[I].get())
      return true;
  return false;
}

inline std::string renderObjCMergedSetterHelpers(
    const BinaryImage &Image, const ObjCMergedSetterSourcePlan &Plan,
    const ObjCProfileStorage &Storage, const std::set<va_t> &Callers,
    std::set<std::string> &SharedFunctions) {
  if (Callers.empty())
    return {};
  std::map<va_t, objc_merged_setter_detail::Contract> Contracts;
  std::map<va_t, std::string> Selectors;
  for (const auto Root : Callers) {
    const auto C = validatedObjCMergedSetter(Image, Plan, Storage, Root);
    if (!C)
      throw std::runtime_error(
          "merged setter source contract is no longer valid");
    Contracts.emplace(Root, *C);
    Selectors.emplace(C->SelectorSlot,
                      Image.ObjCSourceReferences.at(C->SelectorSlot).Name);
    if (objc_merged_setter_detail::isCGRect(*C))
      Selectors.emplace(C->Layout.SelectorReferenceAddress, C->Layout.Selector);
  }
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  OS << "\n#include <objc/runtime.h>\nextern void objc_msgSendSuper2(void);\n"
        "extern void objc_msgSend(void);\n"
        "extern void *objc_retain(void *);\nextern void objc_release(void "
        "*);\n";
  OS << renderObjCSelectorReferenceHelpers(Selectors, SharedFunctions);
  for (const auto &[Root, C] : Contracts) {
    const bool Rect = objc_merged_setter_detail::isCGRect(C);
    const auto Name = objc_merged_setter_detail::helperName(Root);
    SharedFunctions.insert(Name);
    OS << "\nvoid " << Name << "(void *self, void *command, "
       << typeToC(C.Method.Parameters[2].Type)
       << " value, void *selector_slot, void *storage, void *metadata, void *"
       << (Rect ? "layout_selector_slot" : "isa_mask")
       << ") {\n"
          "  (void)command;\n"
          "  struct { void *receiver; void *current_class; } super;\n"
          "  super.receiver = self;\n"
          "  super.current_class = (void *)(uintptr_t)(("
       << typeToC(C.Accessor.Signature.ReturnType)
       << " (*)(void))metadata)();\n"
          "  void *selector;\n  __builtin_memcpy(&selector, selector_slot, "
          "8);\n"
          "  void *retained = objc_retain(self);\n"
          "  ((void (*)(void *, void *, "
       << typeToC(C.Message.Parameters[2].Type)
       << "))objc_msgSendSuper2)(&super, selector, value);\n"
          "  unsigned char *counter = (unsigned char *)storage + "
       << C.Counter - C.ProfileBase
       << ";\n"
          "  uint64_t count;\n  __builtin_memcpy(&count, counter, 8);\n"
          "  ++count;\n  __builtin_memcpy(counter, &count, 8);\n";
    if (Rect)
      OS << "  void *layout_selector;\n"
            "  __builtin_memcpy(&layout_selector, layout_selector_slot, 8);\n"
            "  ((void (*)(void *, void *))objc_msgSend)(retained, "
            "layout_selector);\n";
    else
      OS << "  uintptr_t isa, mask, target;\n"
            "  __builtin_memcpy(&isa, retained, 8);\n"
            "  __builtin_memcpy(&mask, isa_mask, 8);\n"
            "  __builtin_memcpy(&target, (const void *)((isa & mask) + "
         << C.VirtualSlot
         << "), 8);\n"
            "  ((void __attribute__((swiftcall)) (*)(void * "
            "__attribute__((swift_context))))target)(retained);\n";
    OS << "  objc_release(retained);\n}\n";
  }
  return Source;
}
} // namespace neverd::sdk
#endif
