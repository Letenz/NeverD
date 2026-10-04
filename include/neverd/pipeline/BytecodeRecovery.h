//===- BytecodeRecovery.h - External-profile source recovery ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_PIPELINE_BYTECODERECOVERY_H
#define NEVERD_PIPELINE_BYTECODERECOVERY_H

#include "neverd/analysis/BytecodeDecoder.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <string>

namespace neverd {
inline constexpr uint64_t BytecodeRecoveryInputLimit = 64 * 1024 * 1024;

struct BytecodeRecoveryResult {
  uint64_t Functions = 0;
  uint64_t Blocks = 0;
  uint64_t DecodedBytes = 0;
  uint64_t DecodedInstructions = 0;
  bool CheckedOnly = false;
  std::string Source;
  bool WithContext = false;
};

/// Recover externally specified bytecode using JSON request schema version 1.
/// The CLI and both plugin SDKs share this validation and source pipeline.
/// Coverage counts reachable instruction bytes, not authenticated native code.
/// Emission uses the explicit state ABI; it does not infer original C types,
/// run the input, or establish equivalence to an interpreter.
llvm::Expected<BytecodeRecoveryResult>
recoverBytecode(llvm::ArrayRef<uint8_t> Code, llvm::StringRef RequestJSON);

/// Dynamic decoder variant: request uses layout instead of profile. The
/// supplied callback selects instructions; all CFG/state/C rules remain shared.
llvm::Expected<BytecodeRecoveryResult>
recoverBytecode(llvm::ArrayRef<uint8_t> Code, llvm::StringRef RequestJSON,
                analysis::BytecodeDecodeCallback Decode);
} // namespace neverd
#endif
