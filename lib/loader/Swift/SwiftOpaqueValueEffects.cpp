#include "neverd/loader/Swift/SwiftOpaqueValueEffects.h"

#include "../MachO/DarwinRuntimeImport.h"
#include "../MachO/ImmutableNativeFrame.h"
#include "../MachO/SourceLocalCall.h"
#include "SwiftBooleanSourceBinding.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>

namespace neverd {
namespace {
struct ValueContract {
  const char *Metadata, *Equality, *Provider;
  size_t Bytes, CopySlot, DestroySlot;
};
constexpr ValueContract Contracts[] = {
#include "SwiftOpaqueValues.inc"
};
using Action = SourceFrameValueEffect::Action;

bool strongImport(const BinaryImage &Image, va_t Slot, const char *Name,
                  const char *Provider) {
  const auto Import = darwinRuntimeImport(Image, Slot);
  const auto Found = Image.DyldBindSlots.find(Slot);
  const auto *Section = Image.getSectionFor(Slot);
  return Section &&
         (Section->Type & llvm::MachO::SECTION_TYPE) ==
             llvm::MachO::S_NON_LAZY_SYMBOL_POINTERS &&
         Import && *Import == std::string("_") + Name &&
         isImmutableImageImportSlot(Image, Slot) &&
         Found != Image.DyldBindSlots.end() &&
         Found->second.Module == Provider &&
         std::count(Image.DynInfo.NeededLibs.begin(),
                    Image.DynInfo.NeededLibs.end(), Provider) == 1;
}

void valueParameter(SourceFrameEffects &Effects, const ValueContract &Value,
                    size_t Index, Action Operation) {
  const std::string Identity =
      std::string(Value.Provider) + ":" + Value.Metadata;
  Effects.OpaqueValueParameters.emplace(
      Index, SourceFrameValueEffect{Operation, Identity, Value.Bytes});
  (Operation == Action::Read ? Effects.ReadOnlyFrameParameters
                             : Effects.WritableFrameParameters)
      .emplace(Index, Value.Bytes);
}

bool nativeABI(const SourceFunctionTypeHint &Signature, size_t Parameters) {
  if (Signature.Origin != SourceFunctionTypeHint::OriginKind::NativeAnalysis ||
      Signature.Parameters.size() != Parameters || !Signature.ReturnType ||
      Signature.ReturnType->Size != 8 ||
      (Signature.ReturnType->Kind != NdTypeKind::Int &&
       Signature.ReturnType->Kind != NdTypeKind::Ptr))
    return false;
  SourceFunctionTypeHint Expected;
  Expected.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Expected.ReturnType = Signature.ReturnType;
  for (size_t I = 0; I < Parameters; ++I)
    Expected.Parameters.push_back(
        {Signature.Parameters[I].Name, NdType::makePtr(NdType::makeVoid())});
  std::string Error;
  return assignDarwinScalarSourceABI(Expected, Arch::AArch64, Error) &&
         equalSourceABIs(Signature, Expected);
}

std::optional<SourceFrameEffects>
nativeEffects(const BinaryImage &Image, const LowFunc &Callee,
              const SourceFunctionTypeHint &Signature, size_t &Budget) {
  // Both complete functions preserve the actual destination through the
  // dynamic witness. Only the metadata ADRP/LDR immediate fields may vary.
  constexpr std::array<uint32_t, 15> Copy = {
      0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0xaa0103f3, 0xaa0003e1,
      0x90000002, 0xf9400042, 0xf85f8048, 0xf9400908, 0xaa1303e0,
      0xd63f0100, 0xaa1303e0, 0xa9417bfd, 0xa8c24ff4, 0xd65f03c0};
  constexpr std::array<uint32_t, 13> Destroy = {
      0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0xaa0003f3, 0x90000001,
      0xf9400021, 0xf85f8028, 0xf9400508, 0xd63f0100, 0xaa1303e0,
      0xa9417bfd, 0xa8c24ff4, 0xd65f03c0};
  const bool IsCopy = Signature.Parameters.size() == 2;
  const auto Words = IsCopy ? llvm::ArrayRef<uint32_t>(Copy)
                            : llvm::ArrayRef<uint32_t>(Destroy);
  const size_t PageIndex = IsCopy ? 5 : 4;
  const auto Bytes =
      readImmutableCodeBytes(Image, Callee.Entry, Words.size() * 4);
  if (!nativeABI(Signature, IsCopy ? 2 : 1) || !Bytes ||
      !sourceLeafCodeRange(Image, Callee.Entry, Words.size() * 4) ||
      !Image.hasAuthenticatedFunctionEntryAt(Callee.Entry) ||
      Callee.DecodedInstructionCount != Words.size())
    return std::nullopt;
  for (size_t I = 0; I < Words.size(); ++I) {
    const uint32_t Mask = I == PageIndex       ? 0x9f00001f
                          : I == PageIndex + 1 ? 0xffc003ff
                                               : UINT32_MAX;
    if ((llvm::support::endian::read32le(Bytes->data() + I * 4) & Mask) !=
        Words[I])
      return std::nullopt;
  }
  if (!immutableNativeFrameMachineMatches(Image, Callee, Budget))
    return std::nullopt;
  // Interpret the canonical decoder's full-width ADRP definition only after
  // complete current machine/LowIR/CFG replay. The LDR stays a dynamic load.
  std::optional<va_t> Slot;
  const unsigned Register = IsCopy ? 2 : 1;
  const uint32_t Load =
      llvm::support::endian::read32le(Bytes->data() + (PageIndex + 1) * 4);
  const uint64_t Offset = ((Load >> 10) & 0xfff) * 8;
  for (const auto &Block : Callee.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Addr == Callee.Entry + PageIndex * 4) {
        if (Slot || Op.Opcode != NdOp::COPY || Op.Seq != 0 ||
            Op.Output != NdVar::reg(Register * 8, 8) || Op.NumInputs != 1 ||
            !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 8 ||
            Op.Inputs[0].Offset > UINT64_MAX - Offset)
          return std::nullopt;
        Slot = Op.Inputs[0].Offset + Offset;
      }
  for (const auto &Value : Contracts) {
    if (!Slot || Value.CopySlot != 16 || Value.DestroySlot != 8 ||
        !strongImport(Image, *Slot, Value.Metadata, Value.Provider))
      continue;
    SourceFrameEffects Effects;
    if (IsCopy) {
      valueParameter(Effects, Value, 0, Action::Read);
      valueParameter(Effects, Value, 1, Action::Initialize);
    } else
      valueParameter(Effects, Value, 0, Action::Destroy);
    Effects.ReturnFrameOrExternal =
        SourceFrameReturnAlias{IsCopy ? 1U : 0U, Value.Bytes};
    return sourceFrameEffectsMatchABI(Effects, Signature)
               ? std::optional(Effects)
               : std::nullopt;
  }
  return std::nullopt;
}
} // namespace

std::optional<SourceFrameEffects>
swiftOpaqueValueCallEffects(const BinaryImage &Image, const LowFunc &Caller,
                            const SourceCallOccurrenceKey &Site,
                            const SourceCallTypeHint &Binding,
                            const LowFunc *Callee) {
  if (Image.Arch != Arch::AArch64 || Image.Format != BinaryFormat::MachO ||
      Image.Bits != Bitness::Bits64 || Image.IsRelocatable ||
      !Site.StaticTarget || Binding.WeakImport || Binding.DoesNotReturn ||
      !sourceLocalCalls(Image, Caller).count(Site))
    return std::nullopt;
  size_t Budget = 262144;
  if (!immutableNativeFrameMachineMatches(Image, Caller, Budget))
    return std::nullopt;
  if (Binding.CallKind == SourceCallTypeHint::Kind::Native && Callee &&
      Callee->Entry == *Site.StaticTarget &&
      Binding.TargetAddress == Callee->Entry)
    return nativeEffects(Image, *Callee, Binding.Signature, Budget);
  if (Callee || !isSwiftBooleanSourceBinding(Binding) ||
      Binding.BooleanResult->FunctionEntry != Caller.Entry ||
      Binding.BooleanResult->Site != Site)
    return std::nullopt;
  const auto Runtime =
      swiftBooleanRuntimeVeneerCandidate(Image, *Site.StaticTarget);
  if (!Runtime || Runtime->ImportSlot != Binding.TargetAddress)
    return std::nullopt;
  for (const auto &Value : Contracts) {
    if (Binding.TargetName != Value.Equality ||
        Runtime->ImportName != std::string("_") + Value.Equality ||
        !strongImport(Image, Runtime->ImportSlot, Value.Equality,
                      Value.Provider))
      continue;
    SourceFrameEffects Effects;
    valueParameter(Effects, Value, 0, Action::Read);
    valueParameter(Effects, Value, 1, Action::Read);
    return sourceFrameEffectsMatchABI(Effects, Binding.Signature)
               ? std::optional(Effects)
               : std::nullopt;
  }
  return std::nullopt;
}
} // namespace neverd
