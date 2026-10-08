//===- LoadOptions.h - How a session reads its input ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_LOADOPTIONS_H
#define NEVERD_SDK_CAPI_LOADOPTIONS_H

#include "neverd/support/BinaryLoading.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <filesystem>
#include <string>

namespace neverd::sdk {

/// Why analysis refuses a binary file: nothing states its calling convention,
/// so lifting it would have to guess one.
inline constexpr llvm::StringLiteral BinaryFileNotAnalyzed =
    "a binary file states no calling convention; choose one to decompile it "
    "(not available yet)";

/// Parse neverd_session_set_load_options JSON: the loader the user chose.
llvm::Expected<LoaderChoice> parseLoadOptions(llvm::StringRef Text);

/// The load options JSON of the loader \p Choice names.
std::string loadOptionsJson(const LoaderChoice &Choice);

/// The loader the input's `.neverd-load.json` keeps; the file's own format
/// without one.
llvm::Expected<LoaderChoice>
readLoadOptionsSidecar(const std::filesystem::path &Input);

} // namespace neverd::sdk

#endif // NEVERD_SDK_CAPI_LOADOPTIONS_H
