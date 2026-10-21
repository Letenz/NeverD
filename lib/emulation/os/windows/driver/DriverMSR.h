//===- DriverMSR.h - Declared read-only MSR inputs ---------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Validation and lookup for explicitly declared driver MSR read values.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_WINDOWS_DRIVERMSR_H
#define NEVERD_EMULATION_WINDOWS_DRIVERMSR_H

#include "neverd/emulation/DriverSession.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/JSON.h"

namespace neverd::emulation::driver_msr {
llvm::Error validate(llvm::ArrayRef<DriverMSRRead> Entries);
llvm::Expected<std::vector<DriverMSRRead>>
parse(const llvm::json::Value &Value);
llvm::json::Array toJSON(llvm::ArrayRef<DriverMSRRead> Entries);
llvm::Expected<uint64_t> query(llvm::ArrayRef<DriverMSRRead> Entries,
                               uint32_t Index);
} // namespace neverd::emulation::driver_msr

#endif
