//===- AndroidApplication.h - Explicit framework process state --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::mobile::dalvik {
// An API 28 snapshot, not execution of ActivityThread.bindApplication. The
// application phase contains the platform Application attached to a ContextImpl
// and a single PathClassLoader for supplied DEX declarations, with boot parent.
// Custom Application subclasses, split loaders and constructors need separate
// evidence/execution; this input does not infer them from native libraries.
struct AndroidApplicationSnapshot {
  enum class Phase {
    Unattached,
    Attached,
    Application
  } phase = Phase::Unattached;
  // Unknown when absent. Only meaningful in the application phase.
  std::optional<std::string> package_name;
  // Initialized, existing app-private files directory. No mkdir/chmod is
  // inferred. Native workloads also require the same directory in LinuxFiles.
  std::optional<std::string> files_directory;
  // Independent of application creation: binding may install instrumentation
  // first, including a custom subclass. Absence is unknown, not null/default.
  enum class Instrumentation { Uninitialized, Platform };
  std::optional<Instrumentation> instrumentation;
  // Explicit negative observations for exactly one loader and binary name.
  // They neither close a classpath nor apply to JNI FindClass. A null message
  // declares a null Throwable message, rather than an inferred path string.
  struct ClassLookupFailure {
    enum class Loader {
      Unspecified,
      Boot,
      System,
      Application
    } loader = Loader::Unspecified;
    std::string binary_name;
    std::optional<std::string> message;
  };
  std::vector<ClassLookupFailure> class_lookup_failures;
  // A LoadedApk can establish its application loader before constructing the
  // Application. Absent is unknown in Attached; the other phases imply false
  // and true respectively. Explicit contradictory values are rejected.
  std::optional<bool> application_loader_established;
  // A populated, generation-stable API 28 NameValueCache for this resolver's
  // own process user. This does not model provider acquisition or Binder I/O.
  // Missing snapshot is unknown; a present null value is a cached absence.
  struct SecureSettingsCache {
    std::optional<std::string> android_id;
  };
  std::optional<SecureSettingsCache> secure_settings_cache;
  // Separate Global NameValueCache state for the deprecated Secure key.
  // The same own-user, populated and stable-generation preconditions apply.
  struct GlobalSettingsCache {
    std::optional<std::string> adb_enabled;
  };
  std::optional<GlobalSettingsCache> global_settings_cache;
  // Fields of the attached Context's retained ApplicationInfo. No APK bytes
  // or filesystem entry are inferred from the Java String. Missing input is
  // unknown; an explicitly null source_dir is a known null field value.
  struct ApplicationInfo {
    std::optional<std::string> source_dir;
    // Missing outer optional is unknown. Present inner null is a known null
    // nativeLibraryDir field, independent of sourceDir and actual file bytes.
    std::optional<std::optional<std::string>> native_library_dir;
  };
  std::optional<ApplicationInfo> application_info;
  // Stable availability of the package service for this Context. Unknown when
  // absent; false makes getPackageManager return null. True permits one cached
  // ApplicationPackageManager identity, not any package query or Binder I/O.
  std::optional<bool> package_manager_available;
  // Positive API 28 package-service responses for this Context's own user,
  // after ApplicationPackageManager's ABI path adjustment. Exact package/flag
  // keys only; absence is unknown, not NameNotFoundException. Each invocation
  // returns a separate parcel result with independent mutable fields.
  struct ApplicationInfoQuery {
    std::string package_name;
    int32_t flags = 0;
    ApplicationInfo info;
  };
  std::vector<ApplicationInfoQuery> application_info_queries;
};
std::optional<bool>
androidApplicationLoaderState(const AndroidApplicationSnapshot &input);
// Shared structural admission for typed callers and the JSON adapter.
// Declaration conflicts additionally require the Runtime's actual Program.
void validateAndroidClassLookupFailures(
    const AndroidApplicationSnapshot &input);
void validateAndroidSecureSettingsCache(
    const AndroidApplicationSnapshot &input);
void validateAndroidGlobalSettingsCache(
    const AndroidApplicationSnapshot &input);
void validateAndroidApplicationInfo(const AndroidApplicationSnapshot &input);
void validateAndroidApplicationInfoQueries(
    const AndroidApplicationSnapshot &input);
} // namespace neverd::mobile::dalvik
