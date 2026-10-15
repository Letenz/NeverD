//===- Runtime.h - Bounded execution of decoded Dalvik methods --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Integral static method execution; decoding remains owned by parseDex.
//===----------------------------------------------------------------------===//
#pragma once

#include "neverd/dalvik/Model.h"

#include <functional>
#include <span>

namespace neverd::mobile::dalvik {
enum class ExecutionStop {
  Returned,
  Thrown,
  Unsupported,
  Invalid,
  InstructionLimit,
  Timeout,
  StackLimit
};
struct ExecutionResult {
  ExecutionStop stop = ExecutionStop::Returned;
  uint64_t value = 0;
  std::string exception;
  std::string diagnostic;
};
// Return Returned to admit an instruction, or the resource stop to propagate.
// The owner supplies one workload-wide instruction/deadline budget.
using AdmitInstruction = std::function<ExecutionStop()>;
class Program {
public:
  // Input declarations must come from the shared validated DEX reader.
  explicit Program(std::vector<Class> declarations);
  Program(const Program &) = delete;
  Program &operator=(const Program &) = delete;
  bool containsClass(std::string_view descriptor) const;
  std::optional<std::string>
  unsupportedClass(std::string_view descriptor) const;
  const Method *findStatic(const MethodRef &reference) const;
  // No class initializer, field storage, object/FP values or runtime library
  // calls are inferred. Diagnose these boundaries before method execution.
  std::optional<std::string> unsupported(const Method &method) const;
  ExecutionResult execute(const Method &method, std::span<const uint64_t> args,
                          const AdmitInstruction &admit,
                          uint64_t stack_bytes) const;

private:
  ClassMap classes;
};
} // namespace neverd::mobile::dalvik
