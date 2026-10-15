//===- RegistrationCopyABI.cpp - PE32 runtime copy constructors -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate complete object copies and their disjoint source reads.
//===----------------------------------------------------------------------===//

#include "RegistrationABIPrivate.h"

#include "neverd/Limits.h"

namespace neverd {

std::optional<RegistrationCopyCalleeABI>
getCheckedX86RegistrationCopyCalleeABI(const BinaryImage &Image, va_t Target,
                                       uint32_t ObjectSize,
                                       size_t *CumulativeWork) {
  if (!ObjectSize || ObjectSize > limits::kMaxRegistrationEHStateWork)
    return std::nullopt;
  size_t LocalWork = 0;
  size_t &Work = CumulativeWork ? *CumulativeWork : LocalWork;
  RegistrationCopyCalleeABI Result;
  auto Leaf = registration_abi::checkedLeafCalleeABI(
      Image, Target, Work, RegistrationLeafResult::Discarded, ObjectSize,
      &Result.SourceReads);
  if (!Leaf || Leaf->StackPopBytes != 4 || !Leaf->CallerPCWrites.empty())
    return std::nullopt;
  Result.Destination = std::move(*Leaf);
  return Result;
}

} // namespace neverd
