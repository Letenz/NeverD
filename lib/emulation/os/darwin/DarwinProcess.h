//===- DarwinProcess.h - Shared Darwin process contracts --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_DARWIN_DARWINPROCESS_H
#define NEVERD_EMULATION_OS_DARWIN_DARWINPROCESS_H

#include "neverd/emulation/ImageMapping.h"
#include "neverd/emulation/IntegerABI.h"
#include "neverd/emulation/ProcessSession.h"

namespace neverd::emulation::darwin_model {
namespace value {
#define NEVERD_DARWIN_VALUE(Name, Value) inline constexpr uint64_t Name = Value;
#include "DarwinValues.def"
#undef NEVERD_DARWIN_VALUE
} // namespace value
inline llvm::Error failure(llvm::StringRef Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
struct ProfileSpec {
  ProcessProfile Profile;
  uint32_t Platform;
  bool AllowX64;
};
ProfileSpec macOSProfile();
ProfileSpec iOSProfile(bool Simulator);
struct MaximumProtection {
  uint64_t Address, Size;
  unsigned Protection;
};
struct ProcessImage {
  GuestArchitecture Architecture;
  uint64_t PageSize, MinimumAddress;
  bool MainEntry;
  ImageMappingPlan Plan;
  std::vector<MaximumProtection> Maximum;
};
struct InitialStack {
  uint64_t SP, Argc, Argv, Envp, Apple;
};
struct ServiceResult {
  uint64_t Value;
  bool Error = false;
};
enum class ServiceKind {
#define NEVERD_DARWIN_SERVICE(Name, Number, ReturnType) Name,
#include "DarwinValues.def"
#undef NEVERD_DARWIN_SERVICE
};
class DarwinMemory;
llvm::Expected<ProcessImage> loadImage(const std::filesystem::path &Path,
                                       ProfileSpec Profile,
                                       const ProcessOptions &Options);
llvm::Expected<InitialStack> prepareStack(GuestMemory &Memory,
                                          const ProcessOptions &Options,
                                          llvm::StringRef ExecutableName);
llvm::Expected<ProcessServiceEvent> readService(ExecutionBackend &CPU,
                                                const ServiceRequest &Request);
llvm::Error returnService(ExecutionBackend &CPU, const ServiceRequest &Request,
                          ServiceResult Result);
llvm::Expected<std::optional<ServiceResult>>
handleService(ExecutionBackend &CPU, DarwinMemory &Memory,
              const ProcessServiceEvent &Event, const ProcessImage &Image,
              const ProcessOptions &Options, ProcessResult &Result);
llvm::Expected<ProcessResult> runProcess(const std::filesystem::path &Path,
                                         ProfileSpec Profile,
                                         const ProcessOptions &Options);
} // namespace neverd::emulation::darwin_model
#endif
