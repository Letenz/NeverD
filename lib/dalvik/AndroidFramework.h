//===- AndroidFramework.h - Workload-owned framework identities --*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#pragma once

#include "neverd/dalvik/Library.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string_view>

namespace neverd::mobile::dalvik {
class AndroidFramework {
public:
  AndroidFramework(std::optional<AndroidApplicationSnapshot> input,
                   const Program &program);
  ExecutionResult execute(Heap &heap, const Program &program,
                          const LibraryMethod &method, uint64_t receiver,
                          std::span<const uint64_t> arguments,
                          const AdmitInstruction &admit);
  ExecutionResult field(Heap &heap, const Field &field, uint64_t receiver,
                        std::optional<uint64_t> write,
                        const AdmitInstruction &admit);
  AndroidFrameworkState inspect() const;
  std::optional<AndroidApplicationSnapshot::ClassLookupFailure::Loader>
  definingLoader(std::string_view type, const Program &program) const;

private:
  ExecutionResult stringValue(Heap &heap, std::string_view text,
                              const AdmitInstruction &admit);
  ExecutionResult applicationInfoStringValue(Heap &heap, uint64_t receiver,
                                             bool native_directory,
                                             std::optional<uint64_t> write,
                                             const AdmitInstruction &admit);
  ExecutionResult instrumentationValue(Heap &heap, uint64_t receiver,
                                       std::optional<uint64_t> write,
                                       const AdmitInstruction &admit);
  std::optional<AndroidApplicationSnapshot> snapshot;
  std::optional<uint64_t> instrumentation;
  uint32_t thread = 0, application = 0, context = 0;
  uint32_t boot_loader = 0, system_loader = 0, app_loader = 0, package = 0;
  uint32_t files_directory = 0, files_path = 0;
  uint32_t content_resolver = 0;
  uint32_t package_manager = 0;
  uint32_t secure_android_id = 0, global_adb_enabled = 0;
  uint32_t application_info = 0;
  struct ApplicationInfoState {
    // No index denotes the Context's retained object. Query results refer to
    // immutable input by index, so framework moves do not invalidate pointers.
    std::optional<size_t> query;
    std::optional<uint64_t> source_directory;
    std::optional<uint64_t> native_library_directory;
  };
  std::map<uint64_t, ApplicationInfoState> application_info_states;
};
} // namespace neverd::mobile::dalvik
