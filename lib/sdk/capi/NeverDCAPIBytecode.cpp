//===- NeverDCAPIBytecode.cpp - External bytecode recovery C API ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/sdk/NeverDCAPIBytecode.h"

#include "JSONText.h"

#include "neverd/pipeline/BytecodeRecovery.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <cstring>
#include <exception>

namespace {
const char *ownedJSON(llvm::json::Object Object) {
  std::string Text;
  llvm::raw_string_ostream(Text) << llvm::json::Value(std::move(Object));
  auto *Memory = static_cast<char *>(std::malloc(Text.size() + 1));
  if (Memory)
    std::memcpy(Memory, Text.c_str(), Text.size() + 1);
  return Memory;
}

const char *failure(llvm::StringRef Message) {
  return ownedJSON(
      llvm::json::Object{{"schemaVersion", 1},
                         {"ok", false},
                         {"error", neverd::sdk::jsonSafeText(Message)}});
}
} // namespace

extern "C" const char *
neverd_bytecode_recover_json_v1(const unsigned char *Code, size_t CodeSize,
                                const char *RequestJSON, size_t RequestSize) {
  try {
    if ((!Code && CodeSize) || !RequestJSON || !RequestSize)
      return failure("invalid bytecode recovery input buffer");
    if (CodeSize > neverd::BytecodeRecoveryInputLimit ||
        RequestSize > neverd::BytecodeRecoveryInputLimit)
      return failure("input exceeds the 64 MiB limit");
    auto Result = neverd::recoverBytecode(
        {Code, CodeSize}, llvm::StringRef(RequestJSON, RequestSize));
    if (!Result)
      return failure(llvm::toString(Result.takeError()));
    return ownedJSON(llvm::json::Object{
        {"schemaVersion", 1},
        {"ok", true},
        {"functions", Result->Functions},
        {"blocks", Result->Blocks},
        {"decoded_instructions", Result->DecodedInstructions},
        {"decoded_bytes", Result->DecodedBytes},
        {"input_bytes", uint64_t(CodeSize)},
        {"scope", Result->CheckedOnly ? "cfg" : "state-c"},
        {"source", std::move(Result->Source)}});
  } catch (const std::exception &Error) {
    // Do not let an allocation failure while reporting another exception
    // cross the pure C boundary either.
    try {
      return failure(Error.what());
    } catch (...) {
      return nullptr;
    }
  } catch (...) {
    return nullptr;
  }
}
