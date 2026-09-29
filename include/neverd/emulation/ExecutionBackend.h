//===- ExecutionBackend.h - Execution backend and contract selection -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_EXECUTIONBACKEND_H
#define NEVERD_EMULATION_EXECUTIONBACKEND_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <string>

namespace neverd::emulation {
enum class BackendAvailability {
#define NEVERD_EXECUTION_AVAILABILITY(Name, Text) Name,
#include "neverd/emulation/ExecutionBackend.def"
#undef NEVERD_EXECUTION_AVAILABILITY
};
class BackendUnavailableError
    : public llvm::ErrorInfo<BackendUnavailableError> {
public:
  static char ID;
  explicit BackendUnavailableError(
      llvm::StringRef Reason, BackendAvailability Availability =
                                  BackendAvailability::InitializationFailed)
      : Reason(Reason.str()), Availability(Availability) {}
  BackendAvailability availability() const { return Availability; }
  llvm::StringRef reason() const { return Reason; }
  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override {
    return llvm::inconvertibleErrorCode();
  }

private:
  std::string Reason;
  BackendAvailability Availability;
};

class UnsupportedExecutionError
    : public llvm::ErrorInfo<UnsupportedExecutionError> {
public:
  static char ID;
  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override {
    return llvm::inconvertibleErrorCode();
  }
};

enum class GuestArchitecture {
#define NEVERD_GUEST_ARCHITECTURE(Name, Text) Name,
#include "neverd/emulation/ExecutionBackend.def"
#undef NEVERD_GUEST_ARCHITECTURE
};
llvm::Expected<GuestArchitecture> parseGuestArchitecture(llvm::StringRef Name);
const char *guestArchitectureName(GuestArchitecture Architecture);
enum class ExecutionBackendKind {
#define NEVERD_EXECUTION_BACKEND(Name, Text) Name,
#include "neverd/emulation/ExecutionBackend.def"
#undef NEVERD_EXECUTION_BACKEND
};
enum class ExecutionContract {
#define NEVERD_EXECUTION_CONTRACT(Name, Text) Name,
#include "neverd/emulation/ExecutionBackend.def"
#undef NEVERD_EXECUTION_CONTRACT
};

llvm::Expected<ExecutionBackendKind>
parseExecutionBackend(llvm::StringRef Name);
llvm::Expected<ExecutionContract> parseExecutionContract(llvm::StringRef Name);
const char *executionBackendName(ExecutionBackendKind Kind);
const char *executionContractName(ExecutionContract Contract);

namespace execution {
#define NEVERD_EXECUTION_BACKEND(Name, Text)                                   \
  inline constexpr char Name[] = Text;
#define NEVERD_EXECUTION_CONTRACT(Name, Text)                                  \
  inline constexpr char Name[] = Text;
#define NEVERD_EXECUTION_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "neverd/emulation/ExecutionBackend.def"
#undef NEVERD_EXECUTION_BACKEND
#undef NEVERD_EXECUTION_CONTRACT
#undef NEVERD_EXECUTION_TEXT
} // namespace execution
} // namespace neverd::emulation
#endif
