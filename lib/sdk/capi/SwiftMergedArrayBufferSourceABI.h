#ifndef NEVERD_SDK_CAPI_SWIFTMERGEDARRAYBUFFERSOURCEABI_H
#define NEVERD_SDK_CAPI_SWIFTMERGEDARRAYBUFFERSOURCEABI_H

#include "../../loader/MachO/DarwinRuntimeImport.h"
#include "../../loader/MachO/SourceLocalCall.h"
#include "../../loader/SourceUnwind.h"
#include "SwiftMangledSourceABI.h"

#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BranchEncoding.h"

#include "llvm/Support/Endian.h"

namespace neverd::sdk {
namespace swift_merged_array_detail {

inline bool declaration(const BinaryImage &Image, va_t Entry) {
  const Symbol *Only = nullptr;
  for (const auto &Symbol : Image.Symbols)
    if (Symbol.Addr == Entry && Symbol.IsFunc) {
      if (Only)
        return false;
      Only = &Symbol;
    }
  if (!Only)
    return false;
  llvm::StringRef Name(Only->Name);
  Name.consume_front("_");
  if (!Name.starts_with("$s"))
    return false;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 1024;
  Options.MaxNodes = 128;
  Options.MaxDepth = 24;
  Options.MaxMemoryBytes = 65536;
  Options.MaxOperations = 10000;
  const auto Parsed = llvm::swiftDemangle(Name.str(), Options);
  using Node = llvm::SwiftDemangleNode;
  const auto Shape = [](const Node &N, llvm::StringRef Kind, size_t Children) {
    return N.Kind == Kind && !N.Text && !N.Index &&
           N.Children.size() == Children;
  };
  const auto Text = [](const Node &N, llvm::StringRef Kind,
                       llvm::StringRef Value) {
    return N.Kind == Kind && N.Text && *N.Text == Value && !N.Index &&
           N.Children.empty();
  };
  const auto Index = [](const Node &N, llvm::StringRef Kind, uint64_t Value) {
    return N.Kind == Kind && !N.Text && N.Index == Value && N.Children.empty();
  };
  const auto Nominal = [&](const Node &N, llvm::StringRef Module,
                           llvm::StringRef Identifier) {
    return Shape(N, "Type", 1) && Shape(N.Children[0], "Structure", 2) &&
           Text(N.Children[0].Children[0], "Module", Module) &&
           Text(N.Children[0].Children[1], "Identifier", Identifier);
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 3) ||
      !Shape(Parsed.Root->Children[0], "MergedFunction", 0) ||
      !Shape(Parsed.Root->Children[1], "GenericSpecialization", 2) ||
      !Shape(Parsed.Root->Children[2], "Function", 4))
    return false;
  const auto &Specialization = Parsed.Root->Children[1];
  if (!Index(Specialization.Children[0], "SpecializationPassID", 5) ||
      !Shape(Specialization.Children[1], "GenericSpecializationParam", 1))
    return false;
  const auto &Element = Specialization.Children[1].Children[0];
  if (!Shape(Element, "Type", 1) ||
      !Shape(Element.Children[0], "BoundGenericEnum", 2))
    return false;
  const auto &Optional = Element.Children[0];
  if (!Shape(Optional.Children[0], "Type", 1) ||
      !Shape(Optional.Children[0].Children[0], "Enum", 2) ||
      !Text(Optional.Children[0].Children[0].Children[0], "Module", "Swift") ||
      !Text(Optional.Children[0].Children[0].Children[1], "Identifier",
            "Optional") ||
      !Shape(Optional.Children[1], "TypeList", 1) ||
      !Nominal(Optional.Children[1].Children[0], "Foundation", "URL"))
    return false;
  const auto &Function = Parsed.Root->Children[2];
  if (!Shape(Function.Children[0], "Structure", 2) ||
      !Text(Function.Children[0].Children[0], "Module", "Swift") ||
      !Text(Function.Children[0].Children[1], "Identifier", "_ArrayBuffer") ||
      !Text(Function.Children[1], "Identifier", "_consumeAndCreateNew") ||
      !Shape(Function.Children[2], "LabelList", 3) ||
      !Text(Function.Children[2].Children[0], "Identifier", "bufferIsUnique") ||
      !Text(Function.Children[2].Children[1], "Identifier",
            "minimumCapacity") ||
      !Text(Function.Children[2].Children[2], "Identifier", "growForAppend") ||
      !Shape(Function.Children[3], "Type", 1) ||
      !Shape(Function.Children[3].Children[0], "FunctionType", 2))
    return false;
  const auto &Type = Function.Children[3].Children[0];
  if (!Shape(Type.Children[0], "ArgumentTuple", 1) ||
      !Shape(Type.Children[0].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0], "Tuple", 3) ||
      !Shape(Type.Children[1], "ReturnType", 1) ||
      !Shape(Type.Children[1].Children[0], "Type", 1) ||
      !Shape(Type.Children[1].Children[0].Children[0], "BoundGenericStructure",
             2))
    return false;
  const auto &Tuple = Type.Children[0].Children[0].Children[0];
  for (size_t I = 0; I < 3; ++I)
    if (!Shape(Tuple.Children[I], "TupleElement", 1) ||
        !Nominal(Tuple.Children[I].Children[0], "Swift",
                 I == 1 ? "Int" : "Bool"))
      return false;
  const auto &Return = Type.Children[1].Children[0].Children[0];
  if (!Nominal(Return.Children[0], "Swift", "_ArrayBuffer") ||
      !Shape(Return.Children[1], "TypeList", 1) ||
      !Shape(Return.Children[1].Children[0], "Type", 1) ||
      !Shape(Return.Children[1].Children[0].Children[0],
             "DependentGenericParamType", 2))
    return false;
  const auto &Generic = Return.Children[1].Children[0].Children[0];
  return Index(Generic.Children[0], "Index", 0) &&
         Index(Generic.Children[1], "Index", 0);
}

template <typename T>
inline const T *unique(const std::vector<T> &Values, va_t Entry) {
  const T *Found = nullptr;
  for (const auto &Value : Values)
    if (Value.Entry == Entry) {
      if (Found)
        return nullptr;
      Found = &Value;
    }
  return Found;
}

// A merged Swift function's name does not describe the extra callback. Prove
// the complete compiler-observed forwarding wrapper: three scalar inputs,
// Array storage loaded from swiftself, and a strong C swift_release pointer.
inline bool wrapper(const BinaryImage &Image, const PipelineResult &Result,
                    const LowFunc &Low, va_t Target) {
  const va_t Entry = Low.Entry;
  if (Entry % 4 || Entry > InvalidVA - 36 || Low.DecodedInstructionCount != 9 ||
      Low.Blocks.size() != 1 ||
      !std::any_of(Low.Blocks.front().Ops.begin(), Low.Blocks.front().Ops.end(),
                   [&](const LowOp &Op) {
                     return Op.Addr == Entry + 20 && Op.Opcode == NdOp::CALL &&
                            Op.NumInputs == 1 && Op.Inputs[0].isConst() &&
                            Op.Inputs[0].Offset == Target;
                   }))
    return false;
  const auto Expected = swiftMangledURLArrayBufferSourceABI(Image, Entry);
  const auto *Audit = unique(Result.FunctionAudits, Entry);
  const auto *Med = unique(Result.MedFuncs, Entry);
  const auto *High = unique(Result.HighFuncs, Entry);
  if (!Expected || !Audit || !Med || !High ||
      Audit->Disposition != PipelineFunctionDisposition::Accepted ||
      !Audit->HasLowIR || !Audit->HasMedIR || !Audit->MedIRVerified ||
      Audit->DecodedInstructions != 9 || Audit->LiftedInstructions != 9 ||
      !Audit->DecodeFailures.empty() ||
      !Audit->UnsupportedInstructions.empty() ||
      !Audit->TruncatedPaths.empty() || !Med->SourceParametersBound ||
      !Med->SourceTypeHint || !High->SourceTypeHint ||
      !equalSourceABIs(*Expected, *Med->SourceTypeHint) ||
      !equalSourceABIs(*Expected, *High->SourceTypeHint))
    return false;
  const auto &Block = Low.Blocks.front();
  if (Block.StartAddr != Entry || !Block.Preds.empty() ||
      !Block.Succs.empty() || !Block.ExceptionalPreds.empty() ||
      !Block.ExceptionalSuccs.empty() ||
      Block.InstructionBoundaries.size() != 9)
    return false;
  for (size_t I = 0; I < 9; ++I)
    if (Block.InstructionBoundaries[I].Address != Entry + 4 * I)
      return false;
  if (High->ExceptionMetadata &&
      (High->ExceptionMetadata->CodeRange.Begin != Entry ||
       High->ExceptionMetadata->CodeRange.End != Entry + 36 ||
       !isPlainSourceUnwind(*High->ExceptionMetadata)))
    return false;
  for (const auto &Symbol : Image.Symbols)
    if (Symbol.IsFunc && Symbol.Addr > Entry && Symbol.Addr < Entry + 36)
      return false;
  const auto Bytes = readImmutableCodeBytes(Image, Entry, 36);
  if (!Bytes)
    return false;
  const auto Word = [&](unsigned I) {
    return llvm::support::endian::read32le(Bytes->data() + 4 * I);
  };
  if (Word(0) != 0xa9bf7bfd || Word(1) != 0x910003fd || Word(2) != 0xf9400283 ||
      (Word(3) & 0x9f00001f) != 0x90000004 ||
      (Word(4) & 0xffc003ff) != 0xf9400084 ||
      !branch::A64BranchLink.matches(Word(5)) ||
      branch::a64BranchTarget(Word(5), Entry + 20) != Target ||
      Word(6) != 0xf9000280 || Word(7) != 0xa8c17bfd || Word(8) != 0xd65f03c0)
    return false;
  const auto Calls = sourceLocalCalls(Image, Low);
  if (Calls.size() != 1 || Calls.begin()->first.Instruction != Entry + 20 ||
      Calls.begin()->first.StaticTarget != Target ||
      Calls.begin()->second != Word(5))
    return false;
  const uint32_t Immediate =
      ((Word(3) >> 29) & 3) | (((Word(3) >> 5) & 0x7ffff) << 2);
  const int64_t Delta =
      (int64_t(Immediate) - ((Immediate & 0x100000) ? 0x200000 : 0)) * 4096;
  const va_t Page = (Entry + 12) & ~va_t(4095);
  if ((Delta < 0 && Page < uint64_t(-Delta)) ||
      (Delta >= 0 && Page > InvalidVA - uint64_t(Delta)))
    return false;
  const va_t TargetPage = Page + Delta;
  const va_t Offset = ((Word(4) >> 10) & 4095) * 8;
  if (TargetPage > InvalidVA - Offset)
    return false;
  const va_t Slot = TargetPage + Offset;
  const auto Import = darwinRuntimeImport(Image, Slot);
  const auto Bind = Image.DyldBindSlots.find(Slot);
  return Import && *Import == "_swift_release" &&
         Bind != Image.DyldBindSlots.end() &&
         Bind->second.Module == "/usr/lib/swift/libswiftCore.dylib" &&
         isImmutableImageImportSlot(Image, Slot);
}
} // namespace swift_merged_array_detail

inline std::optional<SourceFunctionTypeHint>
swiftMergedURLArrayBufferSourceABI(const BinaryImage &Image, va_t Entry,
                                   const PipelineResult &Result) {
  if (Result.SourceImage != &Image || Image.Format != BinaryFormat::MachO ||
      Image.IsRelocatable || Image.Arch != Arch::AArch64 ||
      Image.Bits != Bitness::Bits64 || !Image.isCodeAddress(Entry) ||
      !swift_merged_array_detail::declaration(Image, Entry))
    return std::nullopt;
  bool Proven = false;
  for (const auto &Low : Result.LowFuncs)
    if (swift_merged_array_detail::wrapper(Image, Result, Low, Entry)) {
      if (swift_merged_array_detail::unique(Result.LowFuncs, Low.Entry) != &Low)
        return std::nullopt;
      Proven = true;
    }
  if (!Proven)
    return std::nullopt;
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = Pointer;
  Hint.Parameters = {{"buffer_is_unique", NdType::makeInt(1, false)},
                     {"minimum_capacity", NdType::makeInt(8, true)},
                     {"grow_for_append", NdType::makeInt(1, false)},
                     {"buffer", Pointer},
                     {"release_buffer", NdType::makePtr(NdType::makeFunc(
                                            NdType::makeVoid(), {Pointer}))}};
  std::string Error;
  return assignDarwinSwiftSourceABI(Hint, Image.Arch, Error)
             ? std::optional<SourceFunctionTypeHint>(std::move(Hint))
             : std::nullopt;
}
} // namespace neverd::sdk
#endif
