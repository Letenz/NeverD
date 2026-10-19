//===- AndroidApplication.h - Explicit framework process state --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#pragma once

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
};
std::optional<bool>
androidApplicationLoaderState(const AndroidApplicationSnapshot &input);
// Shared structural admission for typed callers and the JSON adapter.
// Declaration conflicts additionally require the Runtime's actual Program.
void validateAndroidClassLookupFailures(
    const AndroidApplicationSnapshot &input);
} // namespace neverd::mobile::dalvik
