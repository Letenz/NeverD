//===- SessionTestAccess.h - Private session test mutations -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#pragma once

#include "neverd/DataAccessWidths.h"
#include "neverd/sdk/NeverDCAPITypes.h"

#include <optional>

namespace neverd::sdk {
struct Session;

// Same-build test seam. Mutations that destroy engine objects run in the
// shared library; this header is not installed with the public SDK.
struct SessionTestAccess {
  NEVERD_API static void resetFunctions(Session &State);
  NEVERD_API static void selectFunction(Session &State, neverd_va_t Entry);
  /// Whether the next selection keeps the function list as it is.
  NEVERD_API static bool keepsFunctionList(const Session &State);
  /// Whether the function list and the names it was built under are those a
  /// rebuild from the image makes now.  The session is left as it was.
  NEVERD_API static bool functionListIsRebuilt(Session &State);
  NEVERD_API static void clearPipeline(Session &State);
  NEVERD_API static void clearFeatures(Session &State);
  NEVERD_API static const DataAccessWidths *dataAccessWidths(Session &State);
  NEVERD_API static std::optional<DataAccessWidths>
  collectDataAccessWidths(Session &State);
};
} // namespace neverd::sdk
