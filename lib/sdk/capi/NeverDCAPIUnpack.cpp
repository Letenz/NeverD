//===- NeverDCAPIUnpack.cpp - Packed executable recovery C ABI ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/sdk/NeverDCAPIUnpack.h"

#include "SessionImpl.h"

#include "neverd/unpack/Unpack.h"
#include "neverd/unpack/UnpackStrings.h"

#include <exception>
#include <fstream>

using namespace neverd;
using namespace neverd::sdk;

extern "C" const char *neverd_unpack_json(neverd_session_t Sess,
                                          const char *InputPath,
                                          const char *OutputPath,
                                          const char *OptionsJSON) {
  namespace text = unpack::strings;
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  try {
    if (!InputPath || !*InputPath) {
      S->setError(text::PathRequired);
      return nullptr;
    }
    if (!OutputPath || !*OutputPath) {
      S->setError(text::OutputRequired);
      return nullptr;
    }
#ifdef NEVERD_ENABLE_CPU_EMULATION
    const auto Input = std::filesystem::u8path(InputPath);
    const auto Output = std::filesystem::u8path(OutputPath);
    std::error_code Ignored;
    if (std::filesystem::equivalent(Input, Output, Ignored)) {
      S->setError(text::SamePath);
      return nullptr;
    }
    unpack::UnpackOptions Options;
    if (OptionsJSON) {
      size_t Length = 0;
      while (Length <= unpack::strings::JSONLimit && OptionsJSON[Length])
        ++Length;
      if (Length > unpack::strings::JSONLimit) {
        S->setError(text::TooLarge);
        return nullptr;
      }
      auto Parsed =
          unpack::unpackOptionsFromJSON(llvm::StringRef(OptionsJSON, Length));
      if (!Parsed) {
        S->setError(llvm::toString(Parsed.takeError()));
        return nullptr;
      }
      Options = std::move(*Parsed);
    }
    auto Result = unpack::unpackFile(Input, Options);
    if (!Result) {
      S->setError(llvm::toString(Result.takeError()));
      return nullptr;
    }
    if (Result->Outcome == unpack::UnpackOutcome::Unpacked) {
      std::ofstream Stream(Output, std::ios::binary | std::ios::trunc);
      Stream.write(reinterpret_cast<const char *>(Result->Image.data()),
                   std::streamsize(Result->Image.size()));
      Stream.close();
      if (!Stream) {
        S->setError(std::string(text::WriteFailed) + OutputPath);
        return nullptr;
      }
    }
    char *Report = dupStr(unpack::unpackResultJSON(
        *Result, Result->Outcome == unpack::UnpackOutcome::Unpacked
                     ? llvm::StringRef(OutputPath)
                     : llvm::StringRef()));
    if (!Report)
      S->setError(text::AllocationFailed);
    return Report;
#else
    S->setError(text::Disabled);
    return nullptr;
#endif
  } catch (const std::exception &Error) {
    S->setError(std::string(text::RunFailed) + Error.what());
  } catch (...) {
    S->setError(text::UnexpectedException);
  }
  return nullptr;
}
