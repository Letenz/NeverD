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
#include <memory>
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
  const Class *findClass(std::string_view descriptor) const;
  const Field *findStaticField(const FieldRef &reference) const;
  std::optional<std::string>
  unsupportedClass(std::string_view descriptor) const;
  const Method *findStatic(const MethodRef &reference) const;
  // Only supplied integral static state and code are admitted. Object/FP
  // values, hierarchy dispatch and runtime library calls are not inferred.
  std::optional<std::string> unsupported(const Method &method) const;
  // One-shot convenience. Use Runtime to retain class state across calls.
  ExecutionResult execute(const Method &method, std::span<const uint64_t> args,
                          const AdmitInstruction &admit,
                          uint64_t stack_bytes) const;

private:
  ClassMap classes;
};

// One workload's mutable class state over an immutable Program, which must
// outlive it. Execution is synchronous and single-owner; no Java scheduling or
// monitor/memory-model equivalence is implied. A non-Java stop poisons the
// runtime: partially executed code cannot be retried as a fresh invocation.
class Runtime {
public:
  explicit Runtime(const Program &program);
  ~Runtime();
  Runtime(const Runtime &) = delete;
  Runtime &operator=(const Runtime &) = delete;
  ExecutionResult initialize(std::string_view descriptor,
                             const AdmitInstruction &admit,
                             uint64_t stack_bytes);
  ExecutionResult execute(const Method &method, std::span<const uint64_t> args,
                          const AdmitInstruction &admit, uint64_t stack_bytes);

private:
  struct State;
  std::unique_ptr<State> state;
};
} // namespace neverd::mobile::dalvik
