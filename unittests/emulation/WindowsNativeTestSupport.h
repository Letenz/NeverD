//===- WindowsNativeTestSupport.h - Native Windows oracles -------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNITTESTS_EMULATION_WINDOWS_NATIVE_TEST_SUPPORT_H
#define NEVERD_UNITTESTS_EMULATION_WINDOWS_NATIVE_TEST_SUPPORT_H
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <filesystem>

namespace neverd::emulation::native_test {
/// Observe an original fixture with the system default error mode, independent
/// of GoogleTest's inherited alignment-fault suppression.
llvm::Expected<uint32_t> observeNativeProcess(
    const std::filesystem::path &Program, const std::filesystem::path &Output,
    const std::filesystem::path &Error, uint32_t TimeoutSeconds);
llvm::Expected<uint32_t> observeNativeThread(
    const std::filesystem::path &Program, llvm::StringRef Argument,
    const std::filesystem::path &Output, const std::filesystem::path &Error,
    uint32_t TimeoutSeconds);
} // namespace neverd::emulation::native_test
#endif
