//===- RegistrationThrowObjectABI.cpp - PE32 object runtime callbacks ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind immutable ThrowInfo to bounded preserved destruction effects.
//===----------------------------------------------------------------------===//
#include "RegistrationABIPrivate.h"

#include "neverd/loader/BinaryImage.h"

namespace neverd {
std::optional<RegistrationThrowObjectABI>
getCheckedX86RegistrationThrowObjectABI(const BinaryImage &Image, va_t Address,
                                        size_t *CumulativeWork) {
  size_t LocalWork = 0;
  size_t &Work = CumulativeWork ? *CumulativeWork : LocalWork;
  // Three fixed records and at most 4096 TypeDescriptor name bytes.
  if (!registration_abi::chargeCalleeWork(Work, 16 + 8 + 28 + 4096))
    return std::nullopt;
  auto Info = coff_loader::getCheckedX86IdentityCxxThrowInfo(Image, Address);
  if (!Info)
    return std::nullopt;
  RegistrationThrowObjectABI Result{std::move(*Info), std::nullopt};
  if (!Result.Info.DestructorVA)
    return Result;
  auto Destructor = getCheckedX86RegistrationLeafCalleeABI(
      Image, Result.Info.DestructorVA, &Work,
      RegistrationLeafResult::Discarded);
  if (!Destructor || Destructor->StackPopBytes ||
      !Destructor->CallerPCWrites.empty())
    return std::nullopt;
  for (const auto &Range : Destructor->CodeRanges) {
    const auto *Owner = Image.getSegmentFor(Range.Begin);
    if (!registration_abi::chargeCalleeWork(Work, 1) || !Owner ||
        !Owner->isReadable() || !Owner->isExecutable() ||
        (Owner->isWritable() && !Owner->ReadOnlyAfterRelocations) ||
        Range.End - Owner->VA > Owner->Size)
      return std::nullopt;
  }
  for (const auto *Ranges : {&Destructor->ECXReads, &Destructor->ECXWrites})
    for (const auto &Range : *Ranges)
      if (!registration_abi::chargeCalleeWork(Work, 1) || Range.Begin < 0 ||
          Range.End <= Range.Begin ||
          uint32_t(Range.End) > Result.Info.ObjectSize)
        return std::nullopt;
  for (const auto &Write : Destructor->ImageWrites) {
    const auto *Owner = Image.getSegmentFor(Write.Begin);
    if (!registration_abi::chargeCalleeWork(
            Work, Result.Info.ReadOnlyRanges.size() + 1) ||
        !Owner || !Owner->isWritable() || Owner->isExecutable() ||
        Owner->ReadOnlyAfterRelocations ||
        Write.overlaps(Result.Info.TypeDescriptorRange))
      return std::nullopt;
    for (const auto &Range : Result.Info.ReadOnlyRanges)
      if (Write.overlaps(Range))
        return std::nullopt;
  }
  Result.Destructor = std::move(Destructor);
  return Result;
}
} // namespace neverd
