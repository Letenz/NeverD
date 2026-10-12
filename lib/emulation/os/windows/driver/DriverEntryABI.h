//===- DriverEntryABI.h - Check the loader's Win64 return state -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The concrete caller state owned by one DriverEntry invocation. This checks
/// that invocation's return; it is not a proof for other callers or a recipe
/// for resuming a captured initializer.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WINDOWS_DRIVERENTRYABI_H
#define NEVERD_EMULATION_WINDOWS_DRIVERENTRYABI_H

#include "neverd/emulation/CPU.h"

#include <array>

namespace neverd::emulation {
class DriverEntryABI {
public:
  static llvm::Expected<DriverEntryABI> capture(ExecutionBackend &CPU);
  llvm::Error validateReturn(ExecutionBackend &CPU) const;

private:
  std::array<RegisterValue, 20> Registers;
};
} // namespace neverd::emulation
#endif
