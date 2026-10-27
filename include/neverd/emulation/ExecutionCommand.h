//===- ExecutionCommand.h - One validated public command --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// One validated public command.
///

#ifndef NEVERD_EMULATION_EXECUTIONCOMMAND_H
#define NEVERD_EMULATION_EXECUTIONCOMMAND_H
#include "neverd/emulation/ExecutionLimits.h"

#include "llvm/Support/Error.h"

#include <vector>

namespace neverd::emulation {
enum class ExecutionCommandKind {
#define NEVERD_EXECUTION_COMMAND(Name, Text) Name,
#include "neverd/emulation/ExecutionCommand.def"
#undef NEVERD_EXECUTION_COMMAND
};
struct ExecutionCommand {
  ExecutionCommandKind Kind;
  std::string ID, Payload, InstanceID, ModuleID, Symbol;
  uint64_t Address = 0, Size = 0;
  std::vector<uint8_t> Bytes;
};
/// Strict transport shape, then existing operation-specific decoders own
/// semantic admission. No IO, handle lookup, CPU access, retry or execution.
/// IDs are bounded UTF-8 strings. Unknown/duplicate keys and lossy numbers fail
/// before serialization of nested inputs can hide their original spelling.
llvm::Expected<ExecutionCommand>
executionCommandFromJSON(llvm::StringRef, uint64_t TransferLimit);
} // namespace neverd::emulation
#endif
