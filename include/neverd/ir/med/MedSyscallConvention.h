//===- MedSyscallConvention.h - Supervisor call register ABIs ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The register contract of a supervisor call under one operating system's
/// ABI.  The lifter only knows `svc #imm`; LowToMed, which knows the image,
/// gives the call the registers its kernel reads and writes.  Each operating
/// system's conventions live in their own MedSyscallConvention<OS>.cpp.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDSYSCALLCONVENTION_H
#define NEVERD_IR_MED_MEDSYSCALLCONVENTION_H

#include "neverd/Common.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/ADT/ArrayRef.h"

#include <array>
#include <cstdint>

namespace neverd {

struct BinaryImage;

struct SyscallConvention {
  Arch TheArch = Arch::Unknown;
  /// The intrinsic the lifter emits for the supervisor call.
  Intrinsic Lifted = Intrinsic::None;
  /// The call's immediate that enters this kernel.
  uint64_t Immediate = 0;
  /// The intrinsic the call becomes: it reads NumberRegister, then the
  /// ArgumentRegisters in order, and writes ResultRegister.
  Intrinsic Modeled = Intrinsic::None;
  uint64_t NumberRegister = 0;
  std::array<uint64_t, 7> ArgumentRegisters = {};
  uint8_t ArgumentCount = 0;
  uint64_t ResultRegister = 0;
  uint16_t RegisterSize = 0;
};

/// The convention supervisor call \p Lifted takes with \p Immediate in
/// \p Image, or null when the image's operating system defines none.
const SyscallConvention *syscallConvention(const BinaryImage &Image,
                                           Arch TheArch, Intrinsic Lifted,
                                           uint64_t Immediate);

/// The convention a call already given the register contract follows.
const SyscallConvention *modeledSyscallConvention(Intrinsic Modeled);

/// Every convention, for a consumer that recognizes a call lowered from one.
llvm::ArrayRef<const SyscallConvention *> syscallConventions();

/// The registers an SMC or HVC #0 reads under the SMC Calling Convention on
/// \p TheArch: the function identifier, then its arguments (x0-x7, r0-r3).
/// It returns its results in the first four, which the lift binds.  Arm
/// defines the convention whatever the operating system; empty for another
/// intrinsic or immediate.
llvm::ArrayRef<uint64_t> smcccArgumentRegisters(Arch TheArch, Intrinsic Lifted,
                                                uint64_t Immediate);

} // namespace neverd

#endif // NEVERD_IR_MED_MEDSYSCALLCONVENTION_H
