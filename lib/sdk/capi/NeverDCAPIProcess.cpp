//===- NeverDCAPIProcess.cpp - Guest process C ABI -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/sdk/NeverDCAPIProcess.h"

#include "SessionImpl.h"

#include "neverd/emulation/ProcessReportFields.h"
#ifdef NEVERD_ENABLE_CPU_EMULATION
#include "neverd/emulation/ProcessReport.h"
#endif

#include <exception>

using namespace neverd;
using namespace neverd::sdk;

extern "C" const char *neverd_emulate_process_json(neverd_session_t Sess,
                                                   const char *Path,
                                                   const char *Profile,
                                                   const char *OptionsJSON) {
  namespace field = emulation::process_report;
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  try {
    if (!Path || !*Path) {
      S->setError(field::PathRequired);
      return nullptr;
    }
    if (!Profile || !*Profile) {
      S->setError(field::ProfileRequired);
      return nullptr;
    }
#ifdef NEVERD_ENABLE_CPU_EMULATION
    auto Guest = emulation::parseProcessProfile(Profile);
    if (!Guest) {
      S->setError(llvm::toString(Guest.takeError()));
      return nullptr;
    }
    emulation::ProcessOptions Options;
    if (OptionsJSON) {
      size_t Length = 0;
      while (Length <= field::JSONLimit && OptionsJSON[Length])
        ++Length;
      if (Length > field::JSONLimit) {
        S->setError(field::TooLarge);
        return nullptr;
      }
      auto Parsed = emulation::processOptionsFromJSON(
          llvm::StringRef(OptionsJSON, Length));
      if (!Parsed) {
        S->setError(llvm::toString(Parsed.takeError()));
        return nullptr;
      }
      Options = std::move(*Parsed);
    }
    auto Result = emulation::emulateProcess(std::filesystem::u8path(Path),
                                            *Guest, Options);
    if (!Result) {
      S->setError(llvm::toString(Result.takeError()));
      return nullptr;
    }
    char *Report = dupStr(emulation::processResultJSON(*Result));
    if (!Report)
      S->setError(field::AllocationFailed);
    return Report;
#else
    S->setError(field::Disabled);
    return nullptr;
#endif
  } catch (const std::exception &Error) {
    S->setError(std::string(field::RunFailed) + Error.what());
  } catch (...) {
    S->setError(field::UnexpectedException);
  }
  return nullptr;
}
