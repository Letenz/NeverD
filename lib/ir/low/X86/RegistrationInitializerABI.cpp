//===- RegistrationInitializerABI.cpp - PE32 object initialization --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Prove scalar constructor arguments, return identity and object writes.
//===----------------------------------------------------------------------===//

#include "RegistrationABIPrivate.h"

#include "neverd/loader/BinaryImage.h"

namespace neverd {

std::optional<RegistrationInitializerCalleeABI>
getCheckedX86RegistrationInitializerCalleeABI(const BinaryImage &Image,
                                              va_t Target,
                                              size_t *CumulativeWork) {
  size_t LocalWork = 0;
  size_t &Work = CumulativeWork ? *CumulativeWork : LocalWork;
  const auto Pop = getCheckedX86CalleeStackPop(Image, Target, &Work);
  if (!Pop || *Pop > UINT16_MAX || *Pop % 4)
    return std::nullopt;
  registration_abi::InitializerFrame Frame{*Pop, std::nullopt};
  auto Object = registration_abi::checkedLeafCalleeABI(
      Image, Target, Work, RegistrationLeafResult::Observed, 0, nullptr,
      &Frame);
  if (!Object || !Frame.Initialized || Frame.Initialized->empty() ||
      !Object->CallerPCWrites.empty())
    return std::nullopt;
  for (const auto &Range : Object->CodeRanges) {
    const auto *Owner = Image.getSegmentFor(Range.Begin);
    if (!registration_abi::chargeCalleeWork(Work, 1) || !Owner ||
        !Owner->isReadable() || !Owner->isExecutable() ||
        (Owner->isWritable() && !Owner->ReadOnlyAfterRelocations) ||
        Range.End - Owner->VA > Owner->Size)
      return std::nullopt;
  }
  for (const auto &Write : Object->ImageWrites) {
    const auto *Owner = Image.getSegmentFor(Write.Begin);
    if (!registration_abi::chargeCalleeWork(Work, 1) || !Owner ||
        !Owner->isWritable() || Owner->isExecutable() ||
        Owner->ReadOnlyAfterRelocations)
      return std::nullopt;
  }
  RegistrationInitializerCalleeABI Result;
  Result.Object = std::move(*Object);
  for (int32_t Byte : *Frame.Initialized) {
    if (!registration_abi::chargeCalleeWork(Work, 1))
      return std::nullopt;
    if (!Result.Initialized.empty() && Result.Initialized.back().End == Byte)
      ++Result.Initialized.back().End;
    else
      Result.Initialized.push_back({Byte, Byte + 1});
  }
  return Result;
}

} // namespace neverd
