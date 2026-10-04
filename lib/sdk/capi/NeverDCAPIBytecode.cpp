//===- NeverDCAPIBytecode.cpp - External bytecode recovery C API ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/sdk/NeverDCAPIBytecode.h"

#include "JSONText.h"

#include "neverd/pipeline/BytecodeRecovery.h"

#include "llvm/Support/Errc.h"
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
struct InstructionReply {
  std::string Text;
  bool Called = false;
  bool Invalid = false;
};

int instructionReply(void *Context, const char *JSON, size_t Size) noexcept {
  auto &Reply = *static_cast<InstructionReply *>(Context);
  const bool Repeated = Reply.Called;
  Reply.Called = true;
  if (Repeated || !JSON || !Size || Size > neverd::BytecodeRecoveryInputLimit) {
    Reply.Invalid = true;
    return 0;
  }
  try {
    Reply.Text.assign(JSON, Size);
    return 1;
  } catch (...) {
    Reply.Invalid = true;
    return 0;
  }
}

const char *recover(const unsigned char *Code, size_t CodeSize,
                    const char *RequestJSON, size_t RequestSize,
                    ND_BytecodeDecoderV1 Decoder, void *UserData,
                    bool RequireDecoder) {
  try {
    if (RequireDecoder && !Decoder)
      return failure("external bytecode decoder callback is required");
    if ((!Code && CodeSize) || !RequestJSON || !RequestSize)
      return failure("invalid bytecode recovery input buffer");
    if (CodeSize > neverd::BytecodeRecoveryInputLimit ||
        RequestSize > neverd::BytecodeRecoveryInputLimit)
      return failure("input exceeds the 64 MiB limit");
    neverd::analysis::BytecodeDecodeCallback Decode;
    if (Decoder)
      Decode = [Decoder, UserData](llvm::ArrayRef<uint8_t> Bytes, uint64_t PC)
          -> llvm::Expected<neverd::analysis::BytecodeEncoding> {
        InstructionReply Reply;
        Decoder(UserData, Bytes.data(), Bytes.size(), PC, instructionReply,
                &Reply);
        if (!Reply.Called || Reply.Invalid)
          return llvm::createStringError(llvm::errc::invalid_argument,
                                         "invalid or missing decoder reply");
        return neverd::analysis::readBytecodeEncoding(Reply.Text);
      };
    auto Result = neverd::recoverBytecode(
        {Code, CodeSize}, llvm::StringRef(RequestJSON, RequestSize),
        std::move(Decode));
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
        {"with_context", Result->WithContext},
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

} // namespace

extern "C" const char *
neverd_bytecode_recover_json_v1(const unsigned char *Code, size_t CodeSize,
                                const char *RequestJSON, size_t RequestSize) {
  return recover(Code, CodeSize, RequestJSON, RequestSize, nullptr, nullptr,
                 false);
}

extern "C" const char *neverd_bytecode_recover_decoder_json_v1(
    const unsigned char *Code, size_t CodeSize, const char *RequestJSON,
    size_t RequestSize, ND_BytecodeDecoderV1 Decoder, void *UserData) {
  return recover(Code, CodeSize, RequestJSON, RequestSize, Decoder, UserData,
                 true);
}
