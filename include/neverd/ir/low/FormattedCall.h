//===- FormattedCall.h - Calls whose format names their arguments -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A call to a printf-family routine whose constant format names the
/// arguments it reads after its fixed parameters.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_FORMATTEDCALL_H
#define NEVERD_IR_LOW_FORMATTEDCALL_H

#include <cstdint>
#include <vector>

namespace neverd {

/// One argument of a formatted call, in source order: the register carrying
/// it and the bytes of its value.  A floating argument is the low lane of its
/// vector register; a pointer one is an address the routine reads through.
struct FormattedCallArgument {
  uint64_t Register = 0;
  uint16_t Bytes = 0;
  bool Floating = false;
  bool Pointer = false;
};

/// Every argument a formatted call reads, its fixed parameters first and then
/// the ones its format's conversions name, in order.
struct FormattedCall {
  std::vector<FormattedCallArgument> Arguments;
};

} // namespace neverd

#endif // NEVERD_IR_LOW_FORMATTEDCALL_H
