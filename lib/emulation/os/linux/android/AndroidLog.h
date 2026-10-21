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
#include "neverd/emulation/Limits.h"

#include "llvm/Support/Error.h"

namespace neverd::emulation::android_model {
inline constexpr unsigned LogPayloadLimit = 4068;
llvm::Error
validateLogRecords(const std::optional<std::vector<NativeLogRecord>> &Records);
} // namespace neverd::emulation::android_model
// Preserve interface spellings; resource values are defined by Limits.h.
namespace neverd::emulation::android_model {
inline constexpr unsigned LogRecordLimit =
    ::neverd::limits::emulation::kAndroidLogLogRecordLimit;
inline constexpr unsigned LogSnapshotLimit =
    ::neverd::limits::emulation::kAndroidLogLogSnapshotLimit;
} // namespace neverd::emulation::android_model

#endif
