//===- RegistrationCall.h - Checked PE32 call projections --------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_REGISTRATIONCALL_H
#define NEVERD_IR_REGISTRATIONCALL_H

#include "neverd/loader/ExceptionCommon.h"

#include <optional>
#include <vector>

namespace neverd {

struct RegistrationObjectExtent {
  int32_t Begin = 0;
  int32_t End = 0;
  bool operator==(const RegistrationObjectExtent &) const = default;
};

/// A checked original callee's contract. A returning leaf may borrow ECX;
/// a private scalar-throw helper terminates without borrowing the parent.
/// This is source evidence, not a compiler or native installation receipt.
struct RegistrationCalleeFrameContract {
  enum class Kind : uint8_t { Leaf, PrivateThrow } CalleeKind = Kind::Leaf;
  va_t Target = InvalidVA;
  uint32_t StackPopBytes = 0;
  bool DoesNotReturn = false;
  va_t ThrownTypeVA = 0;
  uint32_t ThrownObjectSize = 0;
  std::vector<RegistrationObjectExtent> ECXReads;
  std::vector<RegistrationObjectExtent> ECXWrites;
  std::vector<ExceptionAddressRange> ImageReads;
  std::vector<ExceptionAddressRange> ImageWrites;
  std::vector<ExceptionAddressRange> CallerPCWrites;
};

/// One exact source call with a proved stack and bounded, initialized object
/// borrow. Frame extents use established parent EBP coordinates. CalleeIndex
/// selects the immutable callee contract retained by the state analysis.
struct RegistrationCallFrameEffect {
  va_t Address = InvalidVA;
  va_t EndAddress = InvalidVA;
  int OpSeq = -1;
  va_t Target = InvalidVA;
  uint32_t CalleeIndex = 0;
  uint32_t StackPopBytes = 0;
  bool DoesNotReturn = false;
  std::optional<int32_t> ECXFrameOffset;
  std::vector<RegistrationObjectExtent> FrameReads;
  std::vector<RegistrationObjectExtent> FrameWrites;
  bool operator==(const RegistrationCallFrameEffect &) const = default;
};

} // namespace neverd

#endif
