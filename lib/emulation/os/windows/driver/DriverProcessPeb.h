//===- DriverProcessPeb.h - Declared process environment inputs ---*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_WINDOWS_DRIVERPROCESSPEB_H
#define NEVERD_EMULATION_WINDOWS_DRIVERPROCESSPEB_H

#include "neverd/emulation/DriverSession.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/JSON.h"

namespace neverd::emulation::driver_process_peb {
llvm::Error validate(llvm::ArrayRef<DriverProcessPeb> Entries);
llvm::Expected<std::vector<DriverProcessPeb>>
parse(const llvm::json::Value &Value);
llvm::json::Array toJSON(llvm::ArrayRef<DriverProcessPeb> Entries);
llvm::Expected<uint64_t> query(llvm::ArrayRef<DriverProcessPeb> Entries,
                               uint32_t ProcessID);
} // namespace neverd::emulation::driver_process_peb

#endif
