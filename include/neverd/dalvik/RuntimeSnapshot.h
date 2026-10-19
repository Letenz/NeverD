//===- RuntimeSnapshot.h - Read-only Java workload state --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#pragma once

#include "neverd/dalvik/AndroidApplication.h"

#include <cstdint>

namespace neverd::mobile::dalvik {
// These are model heap identities, not JNI references or guest addresses.
// Zero means not materialized. The declared phase is separate from lazy
// materialization and does not certify execution of an Application constructor.
struct AndroidFrameworkState {
  std::optional<AndroidApplicationSnapshot::Phase> declared_phase;
  uint32_t activity_thread = 0, application = 0, context = 0;
  uint32_t boot_loader = 0, system_loader = 0, application_loader = 0;
  // Unknown/unread is distinct from a guest-written or declared null value.
  std::optional<uint64_t> instrumentation;
  bool operator==(const AndroidFrameworkState &) const = default;
};

struct JavaClassState {
  std::string descriptor, source_id;
  std::optional<std::string> superclass;
  // Determined by Program ownership, never by the untrusted source_id label.
  bool supplied = false;
  enum class Initialization {
    NotStarted,
    Prepared,
    Initializing,
    Initialized,
    Erroneous
  } initialization = Initialization::NotStarted;
  uint32_t class_object = 0;
  // Ownership in the explicit Android loader projection, not evidence of an
  // APK/classpath on a real device. Absent when that ownership is
  // unestablished.
  std::optional<AndroidApplicationSnapshot::ClassLookupFailure::Loader>
      projected_loader;
  bool operator==(const JavaClassState &) const = default;
};

struct RuntimeSnapshot {
  // Only immutable Program declarations, in descriptor order. This is not a
  // complete boot classpath; library-only intrinsics/array types are excluded.
  std::vector<JavaClassState> classes;
  AndroidFrameworkState framework;
  bool terminal = false;
  bool operator==(const RuntimeSnapshot &) const = default;
};
} // namespace neverd::mobile::dalvik
