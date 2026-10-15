//===- CompiledSEHStateWrites.cpp - SEH receipt checks ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Checks stable callback state-write semantics and final code ownership.
///
//===----------------------------------------------------------------------===//

#include "CompiledSEHStateWrites.h"

#ifdef LLVM_NEVERD_X86_REGISTRATION_SEH_STATE_WRITES

#include "llvm/ADT/STLExtras.h"

#include <limits>
#include <map>
#include <tuple>

using namespace neverd;

namespace {

bool hasAllocatedCode(const CompiledImage &Image, uint64_t Begin,
                      uint64_t End) {
  if (Begin >= End)
    return false;
  bool Found = false;
  for (const auto &Section : Image.Sections) {
    if (!Section.IsAllocated || !Section.Size)
      continue;
    if (Section.Size > std::numeric_limits<uint64_t>::max() - Section.VA)
      return false;
    const uint64_t SectionEnd = Section.VA + Section.Size;
    if (End <= Section.VA || SectionEnd <= Begin)
      continue;
    if (Found || Section.Kind != llvm::mc_rewrite::RewriteSectionKind::Code ||
        Begin < Section.VA || End > SectionEnd)
      return false;
    if (Section.IsInImage) {
      if (!rangeInBounds(Section.Offset, Section.Size, Image.Bytes.size()) ||
          Section.VA < Image.BaseVA ||
          Section.VA - Image.BaseVA != Section.Offset)
        return false;
    } else if (Section.ExternalBytes.size() != Section.Size) {
      return false;
    }
    Found = true;
  }
  return Found;
}

} // namespace

std::optional<X86SEHStateEmissionShape> neverd::collectX86SEHStateEmissionShape(
    const llvm::mc_rewrite::RewriteResult &Result) {
  if (!Result.FunctionRangesValid || !Result.WinEHSemanticsValid ||
      !Result.WinX86SEHStateWritesValid ||
      !llvm::mc_rewrite::validateRewriteWinX86SEHStateWrites(
          Result.WinX86SEHFrames, Result.WinX86SEHStateWrites,
          Result.WinEHSemanticRecords, Result.SourceFunctionOwners,
          Result.FunctionRanges, Result.FunctionOwnerAddrs,
          /*ValidateGlobalFunctionRangeOverlap=*/false))
    return std::nullopt;

  std::map<llvm::StringRef, bool> PrivateOwners;
  for (const auto &Owner : Result.SourceFunctionOwners)
    PrivateOwners.emplace(Owner.SourceFunction, Owner.IsPrivate);
  auto FrameShape = [&](const CompiledWinX86SEHFrame &Frame) {
    return X86SEHFrameEmissionShape{
        Frame.SourceFunction,     PrivateOwners.at(Frame.SourceFunction),
        Frame.RegistrationOffset, Frame.RegistrationSize,
        Frame.StateOffset,        Frame.Encoding};
  };
  X86SEHStateEmissionShape Shape;
  for (const auto &Frame : Result.WinX86SEHFrames)
    Shape.Frames.push_back(FrameShape(Frame));
  llvm::sort(Shape.Frames, [](const auto &Left, const auto &Right) {
    return Left.SourceFunction < Right.SourceFunction;
  });

  std::vector<const CompiledWinX86SEHStateWrite *> Ordered;
  for (const auto &Write : Result.WinX86SEHStateWrites)
    Ordered.push_back(&Write);
  llvm::sort(Ordered, [](const auto *Left, const auto *Right) {
    return std::tie(Left->SourceFunction, Left->BeginVA) <
           std::tie(Right->SourceFunction, Right->BeginVA);
  });
  for (const auto *Write : Ordered)
    Shape.Writes.push_back(
        {Write->SourceFunction, PrivateOwners.at(Write->SourceFunction),
         Write->WriteID, FrameShape(Write->ParentFrame), Write->Semantic,
         Write->GeneratedState, Write->AddressRegister,
         Write->EndVA - Write->BeginVA});
  return Shape;
}

bool neverd::validateCompiledX86SEHStateWrites(const CompiledImage &Image) {
  if (!Image.WinX86SEHStateWritesValid)
    return false;
  if (Image.WinX86SEHFrames.empty() && Image.WinX86SEHStateWrites.empty())
    return true;
  if (Image.TargetArch != Arch::X86 || Image.Format != BinaryFormat::COFF ||
      Image.PointerWidth != 4 || Image.ByteOrder != llvm::endianness::little ||
      !Image.FunctionRangesValid || !Image.WinEHSemanticsValid ||
      !llvm::mc_rewrite::validateRewriteWinX86SEHStateWrites(
          Image.WinX86SEHFrames, Image.WinX86SEHStateWrites,
          Image.WinEHSemanticRecords, Image.SourceFunctionOwners,
          Image.FunctionRanges, Image.FunctionOwnerAddrs))
    return false;
  for (const auto &Range : Image.FunctionRanges)
    if (!hasAllocatedCode(Image, Range.BeginVA, Range.EndVA))
      return false;
  for (const auto &Write : Image.WinX86SEHStateWrites)
    if (!hasAllocatedCode(Image, Write.BeginVA, Write.EndVA))
      return false;
  return true;
}

#endif
