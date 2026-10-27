//===- ExecutionHandle.h - Retained C workload state ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Retained C workload state.
///

#ifndef NEVERD_SDK_EXECUTIONHANDLE_H
#define NEVERD_SDK_EXECUTIONHANDLE_H
#include "ModuleHandle.h"

#include "neverd/emulation/ExecutionRunFactory.h"

namespace neverd::sdk::resources {
struct ExecutionHandleState {
  std::shared_ptr<ModuleHandleState> Sources;
  emulation::ExecutionRunFactoryResult Workload;
  std::atomic<bool> Busy{false};
};
} // namespace neverd::sdk::resources
#endif
