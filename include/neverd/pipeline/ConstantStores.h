//===- ConstantStores.h - Selected native initializer evidence -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded literal stores derived from the authoritative native LowIR lift.
/// No runtime destination, object type or whole-program behavior is inferred.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/Support/Error.h"

#include <string_view>
#include <vector>

namespace neverd {

inline constexpr std::string_view NativeConstantStoresProfile =
    "x64-local-full-width-constant-stores-v1";

struct NativeConstantStore {
  va_t Instruction = 0;
  uint64_t Value = 0;
  ConstantAddressProvenance Provenance = ConstantAddressProvenance::Unknown;
};

struct NativeConstantStores {
  va_t Start = 0, End = 0, FunctionEntry = 0;
  uint64_t BaseRegister = 0;
  int64_t BaseOffset = 0;
  uint64_t Instructions = 0, Steps = 0;
  std::vector<NativeConstantStore> Stores;
};

/// Select an instruction boundary in a loader-recorded x64 function. Within
/// at most 256 bytes / 32 instructions, admit only full-width COPY/ADD/SUB and
/// one to sixteen consecutive eight-byte constant STOREs through the same
/// unchanged symbolic register base. Other effects refuse atomically.
/// This states the values those selected instructions would store, not their
/// reachability, successful execution or final memory contents.
llvm::Expected<NativeConstantStores>
projectNativeConstantStores(const BinaryImage &Image, va_t Start,
                            uint32_t Count);

} // namespace neverd
