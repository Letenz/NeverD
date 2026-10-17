//===- AndroidLog.h - Explicit Android log snapshot admission -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared admission for C++ and JSON log inputs, independent of guest handles.
#ifndef NEVERD_EMULATION_ANDROIDLOG_H
#define NEVERD_EMULATION_ANDROIDLOG_H

#include "neverd/emulation/AndroidNative.h"

#include "llvm/Support/Error.h"

namespace neverd::emulation::android_model {
inline constexpr unsigned LogPayloadLimit = 4068;
inline constexpr unsigned LogRecordLimit = 4096;
inline constexpr unsigned LogSnapshotLimit = 1024 * 1024;
llvm::Error
validateLogRecords(const std::optional<std::vector<NativeLogRecord>> &Records);
} // namespace neverd::emulation::android_model
#endif
