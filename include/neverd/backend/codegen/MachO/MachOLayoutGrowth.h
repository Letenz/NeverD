//===- MachOLayoutGrowth.h - Source patch support -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reserve native metadata without padding.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_MACHO_MACHOLAYOUTGROWTH_H
#define NEVERD_BACKEND_CODEGEN_MACHO_MACHOLAYOUTGROWTH_H
#include "neverd/Common.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <vector>
namespace neverd {
/// Finish the chained-fixup segment inventory before signing/publication.
llvm::Error completeMachOChainedSegmentTable(std::vector<uint8_t> &Binary,
                                             Arch Architecture);

/// Grow command storage by extending a checked 64-bit PIE header mapping into
/// its PAGEZERO guard. Original code/data VAs stay fixed; all admitted relative
/// loader metadata are relocated transactionally. Explicit header-address code
/// references are refused unless the original mapping remains unchanged.
llvm::Expected<bool> reserveMachOHeaderStorage(std::vector<uint8_t> &Binary,
                                               Arch Architecture,
                                               uint64_t AdditionalCommandBytes);

/// Reserve native unwind storage without moving the Mach header or original
/// code/data. Existing metadata is copied and its encoded references rebased.
/// A live segment blocking the TEXT tail requires a complete relocation
/// contract and is refused. Returns whether the private candidate changed.
/// Declare new TEXT metadata while preserving all existing section ordinals.
llvm::Expected<uint64_t> declareMachOTextMetadata(std::vector<uint8_t> &Binary,
                                                  Arch Architecture,
                                                  llvm::StringRef Name,
                                                  uint64_t Size);

llvm::Expected<bool> reserveMachOSourceUnwind(std::vector<uint8_t> &Binary,
                                              Arch Architecture,
                                              uint64_t AdditionalEHBytes,
                                              uint64_t AdditionalFunctions);

/// Reserve from a real emitted object before new RX/RW segments can occupy the
/// TEXT tail. Final installation still verifies the actual relocated records.
llvm::Expected<bool>
reserveMachONativePatchStorage(std::vector<uint8_t> &Binary, Arch Architecture,
                               llvm::StringRef Object, uint64_t CallbackCount,
                               uint64_t ImportFunctionCount);
} // namespace neverd
#endif
