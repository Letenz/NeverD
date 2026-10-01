//===- LinuxProcess.h - Linux process model boundaries ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_LINUX_LINUXPROCESS_H
#define NEVERD_EMULATION_OS_LINUX_LINUXPROCESS_H

#include "neverd/emulation/GuestMemory.h"
#include "neverd/emulation/IntegerABI.h"
#include "neverd/emulation/ProcessSession.h"

namespace neverd {
struct BinaryImage;
namespace emulation::linux_model {
#define NEVERD_LINUX_VALUE(Name, Value) inline constexpr uint64_t Name = Value;
#define NEVERD_LINUX_DIAGNOSTIC(Name, Text) inline constexpr char Name[] = Text;
#include "LinuxValues.def"
#undef NEVERD_LINUX_DIAGNOSTIC
#undef NEVERD_LINUX_VALUE

inline llvm::Error failure(const char *Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
enum class ServiceKind {
#define NEVERD_LINUX_SERVICE(Name, X64Number, ARMNumber, Count) Name,
#define NEVERD_LINUX_X64_SERVICE(Name, Number, Count) Name,
#include "LinuxValues.def"
#undef NEVERD_LINUX_X64_SERVICE
#undef NEVERD_LINUX_SERVICE
};
class LinuxMemory;
struct ProcessLayout {
  GuestArchitecture Architecture;
  IntegerABI Calls;
  uint64_t UserLimit, ProgramHeaderAddress, PageSize, LoadBias;
  bool ExecutableStack;
};
struct ServiceABI {
  ServiceRequestKind Trap;
  CPURegister Number, Result, StackPointer, PC;
  std::array<CPURegister, process_defaults::ServiceArguments> Arguments;
};
const ServiceABI &serviceABI(GuestArchitecture Architecture);
llvm::Expected<ProcessServiceEvent> readService(ExecutionBackend &CPU,
                                                const ServiceRequest &Request);
llvm::Error returnService(ExecutionBackend &CPU, const ServiceRequest &Request,
                          uint64_t Result);
llvm::Expected<std::optional<uint64_t>>
archPrctl(ExecutionBackend &CPU, const ProcessServiceEvent &Event,
          const ProcessLayout &Layout, ProcessResult &Result);
llvm::Expected<std::optional<uint64_t>>
handleService(ExecutionBackend &CPU, LinuxMemory &Memory,
              const ProcessServiceEvent &Event, const ProcessLayout &Layout,
              const ProcessOptions &Options, ProcessResult &Result);
/// Named entry for libc wrappers; syscall numbering stays in serviceABI policy.
llvm::Expected<std::optional<uint64_t>>
handleService(ExecutionBackend &CPU, LinuxMemory &Memory, ServiceKind Kind,
              const ProcessServiceEvent &Event, const ProcessLayout &Layout,
              const ProcessOptions &Options, ProcessResult &Result);
llvm::Expected<ProcessLayout> processLayout(const BinaryImage &Image);
llvm::Expected<uint64_t> prepareStack(GuestMemory &Memory,
                                      const BinaryImage &Image,
                                      const ProcessLayout &Layout,
                                      const ProcessOptions &Options,
                                      llvm::StringRef ExecutableName);
llvm::Expected<ProcessResult> runProcess(const std::filesystem::path &Path,
                                         const ProcessOptions &Options);
} // namespace emulation::linux_model
} // namespace neverd
#endif
