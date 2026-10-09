//===- PipelineFormattedCalls.cpp - Calls a constant format describes -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A printf-family call reads its fixed parameters and the further arguments
/// its format's conversions name.  Where the format is a constant string the
/// image holds, those are known at the call: the call's summary reads exactly
/// them, and the call passes them in the source's order.
///
//===----------------------------------------------------------------------===//

#include "PipelineLowIRDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/ir/low/ImportCallee.h"
#include "neverd/ir/med/MedCallConvention.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/ObjC/ObjCFormattedCalls.h"

#include <map>
#include <optional>

namespace neverd::pipeline_detail {
namespace {
/// The C string at \p Address, when the program cannot change it: in memory
/// it reads but neither writes nor executes.
std::optional<std::vector<uint16_t>> constantFormat(const BinaryImage &Img,
                                                    va_t Address) {
  constexpr uint32_t MaxUnits = 65536;
  const Segment *Seg = Img.getSegmentFor(Address);
  const Section *Sec = Img.getSectionFor(Address);
  if (!Address || !Seg || !Seg->isReadable() || Seg->isWritable() ||
      (Sec ? Sec->isWritable() || Sec->isExecutable() : Seg->isExecutable()))
    return std::nullopt;
  std::vector<uint16_t> Units;
  for (uint32_t I = 0; I < MaxUnits && Address <= InvalidVA - I; ++I) {
    const uint8_t *Byte = Img.readVA(Address + I, 1);
    if (!Byte)
      return std::nullopt;
    if (!*Byte)
      return Units;
    Units.push_back(*Byte);
  }
  return std::nullopt;
}

/// The source type of a fixed parameter or return of C type \p Type: what
/// the carrier assignment needs of it.
TypeRef fixedType(std::string_view Type) {
  if (Type == "void")
    return NdType::makeVoid();
  if (Type == "int")
    return NdType::makeInt(sizeof(int32_t), true);
  if (Type == "size_t")
    return NdType::makeInt(sizeof(uint64_t), false);
  if (libc::isPointerType(Type))
    return NdType::makePtr(NdType::makeVoid());
  return nullptr;
}

/// The arguments of a call to \p Target, given the constant each integer
/// register family holds at the call and the count of vector arguments AL
/// holds, if known.
std::optional<FormattedCall>
formattedCall(const BinaryImage &Img, va_t Target,
              const std::map<unsigned, uint64_t> &Constants,
              std::optional<uint8_t> VectorCount) {
  const std::string Name = importCalleeName(Img, Target);
  const libc::LibCPrintfFormat *Routine =
      Name.empty() ? nullptr : libc::libcPrintfFormat(Name);
  if (!Routine || !Routine->ParamCount)
    return std::nullopt;
  const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
  SourceFunctionTypeHint Signature;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Signature.Architecture = Img.Arch;
  Signature.ReturnType = fixedType(Routine->Return);
  if (!Signature.ReturnType)
    return std::nullopt;
  for (unsigned I = 0; I < Routine->ParamCount; ++I) {
    TypeRef Type = fixedType(Routine->Params[I]);
    if (!Type || Type->Kind == NdTypeKind::Void)
      return std::nullopt;
    Signature.Parameters.push_back({"", std::move(Type)});
  }
  // The format is the last fixed parameter, an integer register argument
  // since every fixed parameter here is one.
  const unsigned FormatParameter = Routine->ParamCount - 1;
  if (FormatParameter >= TRI.IntParamRegs.size())
    return std::nullopt;
  const std::optional<unsigned> FormatFamily =
      gprFamilyOf(Img.Arch, TRI.IntParamRegs[FormatParameter]);
  const auto Format =
      FormatFamily ? Constants.find(*FormatFamily) : Constants.end();
  if (Format == Constants.end())
    return std::nullopt;
  // Each conversion names one promoted argument; a format the strict parser
  // cannot read whole (%n, long double, a wide string) binds nothing.
  const std::optional<std::vector<uint16_t>> Units =
      constantFormat(Img, Format->second);
  const std::optional<std::vector<TypeRef>> Named =
      Units ? objcFormatArgumentTypes(*Units,
                                      SourceCallTypeHint::FormatSyntax::Printf)
            : std::nullopt;
  if (!Named || Signature.Parameters.size() + Named->size() >
                    static_cast<size_t>(limits::kMaxBoundSourceCallArgs))
    return std::nullopt;
  for (const TypeRef &Type : *Named)
    Signature.Parameters.push_back({"", Type});
  std::string Diagnostic;
  if (!assignSysVX64VariadicSourceABI(Signature, Routine->ParamCount,
                                      Diagnostic))
    return std::nullopt;
  FormattedCall Result;
  unsigned Vectors = 0;
  for (const SourceParameterTypeHint &Parameter : Signature.Parameters) {
    const SourceABIValueLocation &Location = Parameter.Location;
    const bool Floating =
        Location.Kind == SourceABICarrierKind::FloatingRegister;
    // An argument the registers do not hold is on the stack, which this
    // binding does not read: such a call keeps the ordinary argument scan.
    if (!Floating && Location.Kind != SourceABICarrierKind::IntegerRegister)
      return std::nullopt;
    Result.Arguments.push_back(
        {Location.RegisterOffset, Location.ValueBytes, Floating,
         Parameter.Type && Parameter.Type->Kind == NdTypeKind::Ptr});
    Vectors += Floating;
  }
  // The caller counts the vector arguments in AL; a format that disagrees
  // with it is not the one the call passes.
  if (VectorCount && *VectorCount != Vectors)
    return std::nullopt;
  return Result;
}
} // namespace

void collectFormattedCalls(const BinaryImage &Img, const LowFunc &F,
                           std::map<va_t, FormattedCall> &Calls) {
  const CallArgumentConvention *Convention =
      callArgumentConvention(Img.Arch, Img.abiFormat());
  if (!Convention || !Convention->FormattedCallArguments)
    return;
  const std::optional<unsigned> VectorCountFamily =
      gprFamilyOf(Img.Arch, x86reg::RAX);
  for (const LowBlock &Block : F.Blocks) {
    // The constant each integer register family, and each temporary, holds
    // here, and the count AL holds.
    std::map<unsigned, uint64_t> Constants;
    std::map<uint64_t, uint64_t> Temporaries;
    std::optional<uint8_t> VectorCount;
    auto Truncated = [](uint64_t Value, uint16_t Size) {
      return Size >= 8 ? Value : Value & ((uint64_t(1) << (8 * Size)) - 1);
    };
    auto ValueOf = [&](const NdVar &V) -> std::optional<uint64_t> {
      if (V.isConst())
        return Truncated(V.Offset, V.Size);
      if (V.isTemp())
        if (const auto It = Temporaries.find(V.Offset); It != Temporaries.end())
          return Truncated(It->second, V.Size);
      if (V.isReg() && V.Offset % 8 == 0)
        if (const auto Family = gprFamilyOf(Img.Arch, V.Offset))
          if (const auto It = Constants.find(*Family); It != Constants.end())
            return Truncated(It->second, V.Size);
      return std::nullopt;
    };
    for (const LowOp &Op : Block.Ops) {
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        if (Op.NumInputs > 0 && Op.Inputs[0].isConst())
          if (auto Call = formattedCall(Img, Op.Inputs[0].Offset, Constants,
                                        VectorCount))
            Calls.emplace(Op.Addr, std::move(*Call));
        Constants.clear();
        Temporaries.clear();
        VectorCount.reset();
        continue;
      }
      // `lea rdx, [rip + d]` lifts as the next instruction's address plus d.
      std::optional<uint64_t> Value;
      if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT) &&
          Op.NumInputs == 1) {
        Value = ValueOf(Op.Inputs[0]);
      } else if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
                 Op.NumInputs == 2) {
        const auto Left = ValueOf(Op.Inputs[0]);
        const auto Right = ValueOf(Op.Inputs[1]);
        if (Left && Right)
          Value = Op.Opcode == NdOp::INT_ADD ? *Left + *Right : *Left - *Right;
      } else if (Op.Opcode == NdOp::INT_XOR && Op.NumInputs == 2 &&
                 Op.Inputs[0] == Op.Inputs[1] && !Op.Inputs[0].isConst()) {
        Value = 0;
      }
      if (Value)
        Value = Truncated(*Value, Op.Output.Size);
      if (Op.Output.isTemp()) {
        if (Value)
          Temporaries[Op.Output.Offset] = *Value;
        else
          Temporaries.erase(Op.Output.Offset);
        continue;
      }
      if (!Op.Output.isReg())
        continue;
      const std::optional<unsigned> Family =
          gprFamilyOf(Img.Arch, Op.Output.Offset);
      if (!Family)
        continue;
      const bool LowByte = Op.Output.Offset % 8 == 0;
      if (Family == VectorCountFamily)
        VectorCount = Value && LowByte ? std::optional<uint8_t>(*Value & 0xFF)
                                       : std::nullopt;
      // A write of four or eight bytes defines the whole register; a
      // narrower one keeps the rest of an unknown old value.
      if (Value && LowByte && Op.Output.Size >= 4)
        Constants[*Family] = *Value;
      else
        Constants.erase(*Family);
    }
  }
}

} // namespace neverd::pipeline_detail
