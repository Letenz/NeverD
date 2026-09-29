//===- NeverDCAPICPU.cpp - CPU query C ABI -------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/sdk/NeverDCAPICPU.h"

#include "SessionImpl.h"

#include "neverd/emulation/ExecutionReportFields.h"

#ifdef NEVERD_ENABLE_CPU_EMULATION
#include "neverd/emulation/ExecutionReport.h"
#endif

#include <exception>

using namespace neverd;
using namespace neverd::sdk;

extern "C" const char *
neverd_cpu_capabilities_json(neverd_session_t Sess,
                             const char *ConfigurationJSON, int ProbeHost) {
  namespace report = emulation::execution_report;
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  try {
    if (ProbeHost != 0 && ProbeHost != 1) {
      S->setError(report::ProbeBoolean);
      return nullptr;
    }
#ifdef NEVERD_ENABLE_CPU_EMULATION
    emulation::ExecutionConfiguration Config;
    if (ConfigurationJSON) {
      size_t Length = 0;
      while (Length <= report::JSONLimit && ConfigurationJSON[Length])
        ++Length;
      if (Length > report::JSONLimit) {
        S->setError(report::ConfigurationTooLarge);
        return nullptr;
      }
      auto Parsed = emulation::executionConfigurationFromJSON(
          llvm::StringRef(ConfigurationJSON, Length));
      if (!Parsed) {
        S->setError(llvm::toString(Parsed.takeError()));
        return nullptr;
      }
      Config = *Parsed;
    }
    auto Report = emulation::executionCapabilitiesJSON(Config, ProbeHost != 0);
    if (!Report) {
      S->setError(llvm::toString(Report.takeError()));
      return nullptr;
    }
    char *Result = dupStr(*Report);
    if (!Result)
      S->setError(report::AllocationFailed);
    return Result;
#else
    S->setError(report::Disabled);
    return nullptr;
#endif
  } catch (const std::exception &Error) {
    S->setError(std::string(report::QueryFailed) + Error.what());
  } catch (...) {
    S->setError(report::UnexpectedException);
  }
  return nullptr;
}
