//===- SourcePatchBindings.cpp - Original-image reference admission -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/SourcePatchBindings.h"

#include "neverd/ArchSupport.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/object/ELFLayout.h"
#include "neverd/object/MachOLayout.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Errc.h"

#include <set>

namespace neverd {
namespace {

bool overlaps(va_t Left, uint64_t LeftSize, va_t Right, uint64_t RightSize) {
  if (!LeftSize || !RightSize)
    return false;
  if (LeftSize > InvalidVA - Left || RightSize > InvalidVA - Right)
    return true;
  return Left < Right + RightSize && Right < Left + LeftSize;
}

bool isRuntimeReadOnly(const BinaryImage &Image, va_t Address, uint64_t Size) {
  bool Protected = false;
  if (Image.isELF()) {
    forEachELFPhdr(
        Image.Raw.data(), Image.Raw.size(),
        [&](const ELFPhdrFields &Segment, const uint8_t *, bool) {
          if (Segment.Type != llvm::ELF::PT_GNU_RELRO || !Segment.MemSz)
            return;
          const uint64_t Page = elfPageSize(Image.Arch);
          const uint64_t Start = Segment.VAddr & ~(Page - 1);
          if (Segment.MemSz > InvalidVA - Segment.VAddr ||
              Segment.VAddr + Segment.MemSz > InvalidVA - (Page - 1)) {
            Protected = true;
            return;
          }
          // Do not authorize writes to a page intersecting RELRO:
          // loaders protect pages, including bytes before p_vaddr.
          const uint64_t End = alignUp(Segment.VAddr + Segment.MemSz, Page);
          if (overlaps(Address, Size, Start, End - Start))
            Protected = true;
        });
  } else if (Image.isMachO()) {
    forEachMachOLoadCommand(
        Image.Raw.data(), Image.Raw.size(),
        [&](const uint8_t *Command, uint32_t ID, uint32_t, bool Is64) {
          if (ID != getMachOSegmentCmdID(Is64))
            return;
          const auto Segment = readMachOSegment(Command, Is64);
          const uint32_t Flags =
              Is64 ? reinterpret_cast<const llvm::MachO::segment_command_64 *>(
                         Command)
                         ->flags
                   : reinterpret_cast<const llvm::MachO::segment_command *>(
                         Command)
                         ->flags;
          if ((Flags & llvm::MachO::SG_READ_ONLY) &&
              overlaps(Address, Size, Segment.VMAddr, Segment.VMSize))
            Protected = true;
        });
  }
  return Protected;
}

} // namespace

llvm::Error
validateSourcePatchBinding(const SourcePatchBinding &Binding,
                           const BinaryImage &Image,
                           llvm::ArrayRef<SourcePatchMapping> Replacements) {
  auto Fail = [](const llvm::Twine &Detail) {
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "source binding: " + Detail);
  };
  if (Binding.Symbol.empty() || llvm::StringRef(Binding.Symbol).contains('\0'))
    return Fail("empty or invalid symbol");
  if (Binding.Kind == SourcePatchBindingKind::Function) {
    if (Binding.Size != 0 ||
        (!Image.hasAuthenticatedFunctionEntryAt(Binding.OriginalVA) &&
         !Image.findImportStubAt(Binding.OriginalVA)))
      return Fail("reference is not an authenticated function: " +
                  Binding.Symbol);
    for (const auto &Replacement : Replacements)
      if (Replacement.OriginalVA == Binding.OriginalVA)
        return Fail(
            "original-body callbacks are not installed for replaced entries");
    return llvm::Error::success();
  }
  if (Binding.Kind != SourcePatchBindingKind::ReadOnlyData &&
      Binding.Kind != SourcePatchBindingKind::WritableData)
    return Fail("unknown reference kind");
  if (!Binding.Size || Binding.Size > InvalidVA - Binding.OriginalVA)
    return Fail("invalid data extent: " + Binding.Symbol);
  const auto *Segment = Image.getSegmentFor(Binding.OriginalVA);
  const auto *Section = Image.getSectionFor(Binding.OriginalVA);
  const va_t Last = Binding.OriginalVA + Binding.Size - 1;
  if (!Segment || !Segment->isReadable() ||
      Image.getSegmentFor(Last) != Segment || !Section ||
      Image.getSectionFor(Last) != Section || !Section->isReadable() ||
      Image.isCodeAddress(Binding.OriginalVA) || Image.isCodeAddress(Last))
    return Fail("data extent is not contained in original data storage");
  if (Binding.Kind == SourcePatchBindingKind::WritableData &&
      (!Segment->isWritable() || !Section->isWritable() ||
       isRuntimeReadOnly(Image, Binding.OriginalVA, Binding.Size)))
    return Fail("original data is not writable at runtime");
  return llvm::Error::success();
}

llvm::Expected<std::vector<SourcePatchBinding>>
bindSourcePatchReferences(llvm::Module &Module, const BinaryImage &Image,
                          llvm::ArrayRef<SourcePatchMapping> Replacements,
                          llvm::ArrayRef<SourcePatchBinding> Bindings,
                          bool RenameDeclarations) {
  auto Fail = [](const llvm::Twine &Detail) {
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "source binding: " + Detail);
  };
  std::set<std::string> Names;
  std::set<va_t> Addresses;
  std::vector<SourcePatchBinding> Result(Bindings.begin(), Bindings.end());
  for (auto &Binding : Result) {
    if (Binding.Kind != SourcePatchBindingKind::Function &&
        Binding.Kind != SourcePatchBindingKind::ReadOnlyData &&
        Binding.Kind != SourcePatchBindingKind::WritableData)
      return Fail("unknown reference kind");
    if (Binding.Symbol.empty() ||
        llvm::StringRef(Binding.Symbol).contains('\0') ||
        !Names.insert(Binding.Symbol).second ||
        !Addresses.insert(Binding.OriginalVA).second)
      return Fail("empty or duplicate symbol/address");
    llvm::GlobalValue *Value = Module.getNamedValue(Binding.Symbol);
    if (!Value || !Value->isDeclaration() || Value->hasDLLImportStorageClass())
      return Fail("expected a plain external declaration: " + Binding.Symbol);
    if (Binding.Kind == SourcePatchBindingKind::Function) {
      if (!llvm::isa<llvm::Function>(Value))
        return Fail("expected a function declaration: " + Binding.Symbol);
    } else {
      auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(Value);
      if (!Global || Global->isThreadLocal() ||
          !Global->getValueType()->isSized())
        return Fail("expected a sized non-TLS data declaration: " +
                    Binding.Symbol);
      const auto Required =
          Module.getDataLayout().getTypeAllocSize(Global->getValueType());
      if (Required.isScalable())
        return Fail("scalable data declarations are unsupported");
      if (Binding.Size == 0)
        Binding.Size = Required.getFixedValue();
      if (Binding.Size == 0 || Binding.Size < Required.getFixedValue() ||
          Binding.Size > InvalidVA - Binding.OriginalVA)
        return Fail("invalid data extent: " + Binding.Symbol);
      const auto Alignment = Global->getAlign().valueOrOne().value();
      const auto ABIAlignment = Module.getDataLayout()
                                    .getABITypeAlign(Global->getValueType())
                                    .value();
      if (Binding.OriginalVA % std::max(Alignment, ABIAlignment))
        return Fail("data address does not meet its declared alignment");
      if (Binding.Kind == SourcePatchBindingKind::ReadOnlyData &&
          !Global->isConstant())
        return Fail("read-only binding requires a const declaration");
    }
    if (auto Error = validateSourcePatchBinding(Binding, Image, Replacements))
      return std::move(Error);
    const std::string Name = makeNdDataSymbol(Binding.OriginalVA);
    if (Module.getNamedValue(Name) && Module.getNamedValue(Name) != Value)
      return Fail("synthetic original address symbol already exists");
  }
  // All requests validate before any declaration is changed. Explicit binding
  // fixes the symbol to this image, so direct PIC references are appropriate.
  if (!RenameDeclarations)
    return Result;
  for (const auto &Binding : Result) {
    llvm::GlobalValue *Value = Module.getNamedValue(Binding.Symbol);
    Value->setName(makeNdDataSymbol(Binding.OriginalVA));
    Value->setDSOLocal(true);
  }
  return Result;
}

} // namespace neverd
