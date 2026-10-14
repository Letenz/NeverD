//===- SEHFrameFormat.cpp - Source-written Win64 formats ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticates exact format bytes independently of ABI parameter spelling.

#include "SEHFrameFormat.h"

#include "../SEHFrameState.h"

#include "neverd/ir/low/SEHRuntimeMemory.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MathExtras.h"

#include <string>

namespace neverd::seh_frame {

bool checkedX64FrameFormat(const State &Caller, int64_t Address,
                           int64_t HomeEnd,
                           const std::set<int64_t> &ReturnSlots,
                           size_t &Remaining) {
  if (Address < HomeEnd || Address >= 0)
    return false;
  std::string Format;
  for (size_t I = 0; I != kMaxWindowsSEHFormatBytes; ++I) {
    const size_t Work = ReturnSlots.size() + 1;
    if (Work > Remaining) {
      Remaining = 0;
      return false;
    }
    Remaining -= Work;
    int64_t At = 0;
    if (llvm::AddOverflow(Address, int64_t(I), At) || At >= 0 ||
        llvm::any_of(ReturnSlots, [&](int64_t Slot) {
          return Slot > INT64_MAX - 8 || (Slot <= At && At < Slot + 8);
        }))
      return false;
    const auto Byte = Caller.FrameBytes.find(At);
    if (Byte == Caller.FrameBytes.end())
      return false;
    if (Byte->second) {
      Format.push_back(char(Byte->second));
      continue;
    }
    if (Format.size() + 1 > Remaining) {
      Remaining = 0;
      return false;
    }
    Remaining -= Format.size() + 1;
    return isWindowsSEHScalarFormat(Format);
  }
  return false;
}

} // namespace neverd::seh_frame
