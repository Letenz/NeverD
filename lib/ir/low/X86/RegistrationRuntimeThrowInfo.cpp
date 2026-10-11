//===- RegistrationRuntimeThrowInfo.cpp - Direct PE32 throw types ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Decode bounded immutable candidates without assigning a type to a call.
//===----------------------------------------------------------------------===//
#include "RegistrationABIPrivate.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"

namespace neverd::registration_abi {
std::optional<std::vector<RegistrationRuntimeThrowInfo>>
collectRegistrationRuntimeThrowInfos(const LowFunc &Function,
                                     const BinaryImage &Image, size_t &Work,
                                     ImageFrameEffects &Effects,
                                     std::set<std::pair<va_t, va_t>> &Code) {
  std::set<va_t> Candidates;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops) {
      if (Op.NumInputs > std::size(Op.Inputs) ||
          !chargeCalleeWork(Work, 1 + Op.NumInputs))
        return std::nullopt;
      for (unsigned I = 0; I != Op.NumInputs; ++I) {
        const auto &Value = Op.Inputs[I];
        if (Value.isConst() && Value.Size == 4 && Value.Offset <= UINT32_MAX)
          Candidates.insert(Value.Offset);
      }
    }
  std::vector<RegistrationRuntimeThrowInfo> Result;
  for (va_t Address : Candidates) {
    const auto *Owner = Image.getSegmentFor(Address);
    if (!Owner || !Owner->isReadable() || Owner->isWritable() ||
        Owner->isExecutable() || !Image.readVA(Address, 16))
      continue;
    if (const auto Object =
            getCheckedX86RegistrationThrowObjectABI(Image, Address, &Work)) {
      const auto &Info = Object->Info;
      Result.push_back({Info.Address, Info.TypeDescriptorVA, Info.ObjectSize});
      if (const auto &Destructor = Object->Destructor) {
        if (!chargeCalleeWork(Work, Destructor->ImageReads.size() +
                                        Destructor->ImageWrites.size() +
                                        Destructor->CodeRanges.size()))
          return std::nullopt;
        for (const auto &Range : Destructor->ImageReads)
          Effects.Reads.emplace(Range.Begin, Range.End);
        for (const auto &Range : Destructor->ImageWrites)
          Effects.Writes.emplace(Range.Begin, Range.End);
        for (const auto &Range : Destructor->CodeRanges)
          Code.emplace(Range.Begin, Range.End);
      }
    }
    if (!chargeCalleeWork(Work, 1))
      return std::nullopt;
  }
  return Result;
}
} // namespace neverd::registration_abi
