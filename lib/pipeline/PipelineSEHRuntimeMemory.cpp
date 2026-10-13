//===- PipelineSEHRuntimeMemory.cpp - Authenticate Windows CRT effects ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticates PE import providers, immutable formats and image storage.

#include "PipelineSEHRuntimeMemory.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/LowScalarConstant.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCFormattedCalls.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <optional>
#include <set>
#include <tuple>

namespace neverd {
namespace {

std::optional<LowSEHRuntimeCall> runtimeCall(const BinaryImage &Image,
                                             const Import &Imported,
                                             size_t &Remaining) {
  const llvm::StringRef Module = Imported.Module;
  if (Imported.IsDelayImport || !Imported.IATAddr)
    return std::nullopt;
  const bool UCRT = Module.equals_insensitive("ucrtbase.dll") ||
                    Module.equals_insensitive("ucrtbased.dll");
  const bool Stdio =
      UCRT || Module.equals_insensitive("api-ms-win-crt-stdio-l1-1-0.dll");
  LowSEHRuntimeCall Call;
  if (Stdio && Imported.Name == "__acrt_iob_func")
    Call.Routine = LowSEHRuntimeCall::Kind::CRTStream;
  else if (Stdio && Imported.Name == "__stdio_common_vfprintf")
    Call.Routine = LowSEHRuntimeCall::Kind::CRTVfprintf;
  else if (Imported.Name == "_controlfp_s" &&
           (UCRT ||
            Module.equals_insensitive("api-ms-win-crt-runtime-l1-1-0.dll") ||
            Module.equals_insensitive("api-ms-win-crt-math-l1-1-0.dll")))
    Call.Routine = LowSEHRuntimeCall::Kind::CRTControlFP;
  else
    return std::nullopt;
  if (Image.Imports.size() + 1 > Remaining) {
    Remaining = 0;
    return std::nullopt;
  }
  Remaining -= Image.Imports.size() + 1;
  if (!isFileBackedPEImportSlot(Image, Imported.IATAddr))
    return std::nullopt;
  Call.IATAddress = Imported.IATAddr;
  Call.Module = Imported.Module;
  Call.Name = Imported.Name;
  return Call;
}

std::optional<std::string> scalarFormat(const BinaryImage &Image, va_t Address,
                                        size_t &Remaining) {
  constexpr uint32_t MaxFormatBytes = 4096;
  std::vector<uint16_t> Units;
  for (uint32_t I = 0; I != MaxFormatBytes; ++I) {
    if (!Remaining || Address > InvalidVA - I)
      return std::nullopt;
    --Remaining;
    const auto *Byte = Image.readVA(Address + I, 1);
    if (!Byte)
      return std::nullopt;
    if (*Byte) {
      Units.push_back(*Byte);
      continue;
    }
    // Byte availability alone is not an immutable format contract.
    auto Bytes = readImmutableImageBytes(Image, Address, I + 1);
    auto Arguments = objcFormatArgumentTypes(
        Units, SourceCallTypeHint::FormatSyntax::Printf);
    if (!Bytes || !Arguments ||
        llvm::any_of(*Arguments, [](const TypeRef &Type) {
          return !Type || (Type->Kind != NdTypeKind::Int &&
                           Type->Kind != NdTypeKind::Float);
        }))
      return std::nullopt;
    return std::string(Bytes->begin(), Bytes->end());
  }
  return std::nullopt;
}

bool runtimeVeneer(const BinaryImage &Image, va_t Address, va_t Slot,
                   size_t &Remaining) {
  if (Remaining < 6)
    return false;
  Remaining -= 6;
  const auto Bytes = readImmutableCodeBytes(Image, Address, 6);
  if (!Bytes || (*Bytes)[0] != 0xff || (*Bytes)[1] != 0x25)
    return false;
  const int64_t Displacement = readLE<int32_t>(Bytes->data() + 2);
  const va_t Next = Address + 6;
  if ((Displacement < 0 && Next < uint64_t(-Displacement)) ||
      (Displacement >= 0 && uint64_t(Displacement) > InvalidVA - Next))
    return false;
  return Next + Displacement == Slot;
}

} // namespace

LowSEHRuntimeMemory
collectLowSEHRuntimeMemory(const BinaryImage &Image,
                           llvm::ArrayRef<LowFunc> Functions) {
  LowSEHRuntimeMemory Memory;
  if (Image.Arch != Arch::X64 || Image.Bits != Bitness::Bits64 ||
      Image.Format != BinaryFormat::COFF)
    return Memory;
  // The module scan uses the same cap as complete SEH frame propagation.
  // A four-thousand-operation jump-table query is not a module-size bound.
  size_t Remaining = size_t{limits::kMaxJumpTableEvidenceWork} *
                     size_t{limits::kMaxMultiStageRetries} * 16;
  std::set<va_t> Candidates;
  using Key = std::tuple<VnodeSpace, uint64_t, uint16_t>;
  const auto KeyOf = [](const NdVar &V) {
    return Key{V.Space, V.Offset, V.Size};
  };
  for (const auto &Function : Functions)
    for (const auto &Block : Function.Blocks) {
      std::map<Key, uint64_t> Values;
      va_t Instruction = InvalidVA;
      for (const auto &Op : Block.Ops) {
        if (Op.NumInputs > std::size(Op.Inputs))
          return {};
        const size_t Work = Values.size() + Op.NumInputs + 1;
        if (Work > Remaining)
          return {};
        Remaining -= Work;
        if (Op.Addr != Instruction) {
          Values.clear();
          Instruction = Op.Addr;
        }
        const auto Constant = [&](const NdVar &V) -> std::optional<uint64_t> {
          if (!V.Size || V.Size > 8)
            return std::nullopt;
          if (V.isConst())
            return V.Offset & (UINT64_MAX >> ((8 - V.Size) * 8));
          const auto It = Values.find(KeyOf(V));
          return It == Values.end() ? std::nullopt
                                    : std::optional<uint64_t>{It->second};
        };
        for (unsigned I = 0; I != Op.NumInputs; ++I)
          if (Op.Inputs[I].isConst() && Op.Inputs[I].Size == 8)
            Candidates.insert(Op.Inputs[I].Offset);
        const auto Result = lowScalarConstantResult(Op, Constant);
        if (!Op.Output.Size || Op.Output.Offset > UINT64_MAX - Op.Output.Size)
          continue;
        std::erase_if(Values, [&](const auto &Item) {
          const auto &[Space, Offset, Size] = Item.first;
          return Space == Op.Output.Space &&
                 Offset < Op.Output.Offset + Op.Output.Size &&
                 Op.Output.Offset < Offset + Size;
        });
        if (Result) {
          Values[KeyOf(Op.Output)] = *Result;
          if (Op.Output.Size == 8)
            Candidates.insert(*Result);
        }
      }
    }
  for (const auto &Imported : Image.Imports) {
    if (!Remaining)
      return {};
    --Remaining;
    if (auto Call = runtimeCall(Image, Imported, Remaining))
      if (!Memory.Calls.emplace(std::make_pair(Imported.IATAddr, true), *Call)
               .second)
        return {};
  }
  for (va_t Candidate : Candidates) {
    if (!Remaining)
      return {};
    --Remaining;
    if (const auto *Imported = Image.findImportStubAt(Candidate))
      if (auto Call = runtimeCall(Image, *Imported, Remaining))
        if (runtimeVeneer(Image, Candidate, Call->IATAddress, Remaining))
          Memory.Calls.emplace(std::make_pair(Candidate, false), *Call);
    const auto *Segment = Image.getSegmentFor(Candidate);
    if (!Segment || !Segment->isReadable() || Segment->isWritable() ||
        Segment->isExecutable())
      continue;
    if (auto Format = scalarFormat(Image, Candidate, Remaining))
      Memory.ScalarFormats.emplace(Candidate, std::move(*Format));
    if (!Remaining)
      return {};
  }
  for (const auto &Segment : Image.Segments) {
    if (!Segment.isWritable() || Segment.isExecutable())
      continue;
    const auto Bytes =
        std::min<uint64_t>({Segment.Data.size(), Segment.Size, Segment.FileSz});
    if (!Bytes || Segment.VA > InvalidVA - Bytes)
      continue;
    const va_t End = Segment.VA + Bytes;
    for (va_t Begin = Segment.VA; Begin < End;) {
      if (!Remaining)
        return {};
      --Remaining;
      const auto OwnerEnd = Image.mappedObjectOwnerEnd(Begin);
      const va_t Limit = OwnerEnd ? std::min(*OwnerEnd, End) : Begin;
      if (Limit <= Begin || Limit - Begin > UINT32_MAX ||
          !isFileBackedWritableImageRange(Image, Begin,
                                          uint32_t(Limit - Begin))) {
        ++Begin;
        continue;
      }
      if (!Memory.WritableImageRanges.empty() &&
          Memory.WritableImageRanges.back().End == Begin)
        Memory.WritableImageRanges.back().End = Limit;
      else {
        if (Memory.WritableImageRanges.size() == 256)
          return {};
        Memory.WritableImageRanges.push_back({Begin, Limit});
      }
      Begin = Limit;
    }
  }
  llvm::sort(Memory.WritableImageRanges,
             [](const auto &A, const auto &B) { return A.Begin < B.Begin; });
  if (lowSEHRuntimeMemoryDigest(Memory).empty())
    return {};
  return Memory;
}

} // namespace neverd
