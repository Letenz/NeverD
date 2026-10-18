//===- SessionTestAccess.cpp - Private session test mutations ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SessionTestAccess.h"

#include "InstructionFlow.h"
#include "SessionImpl.h"

namespace neverd::sdk {
void SessionTestAccess::resetFunctions(Session &State) {
  State.resetFunctionsFromImage();
}
void SessionTestAccess::selectFunction(Session &State, neverd_va_t Entry) {
  State.selectFunction(Entry);
}
bool SessionTestAccess::keepsFunctionList(const Session &State) {
  return State.functionListCurrent();
}
bool SessionTestAccess::functionListIsRebuilt(Session &State) {
  auto Functions = State.Functions;
  auto OriginalNames = State.OriginalNames;
  auto Inputs = State.ListInputs;
  auto Widths = std::move(State.AccessWidths);
  auto Extents = std::move(State.AccessWidthsFunctions);
  State.resetFunctionsFromImage();
  const bool Rebuilt =
      State.Functions == Functions && State.OriginalNames == OriginalNames;
  State.Functions = std::move(Functions);
  State.OriginalNames = std::move(OriginalNames);
  State.ListInputs = std::move(Inputs);
  State.AccessWidths = std::move(Widths);
  State.AccessWidthsFunctions = std::move(Extents);
  return Rebuilt;
}
void SessionTestAccess::clearPipeline(Session &State) { State.clearPipeline(); }
void SessionTestAccess::clearFeatures(Session &State) { State.SigDB.clear(); }
const DataAccessWidths *SessionTestAccess::dataAccessWidths(Session &State) {
  return State.dataAccessWidths();
}
std::optional<DataAccessWidths>
SessionTestAccess::collectDataAccessWidths(Session &State) {
  return sdk::collectDataAccessWidths(State.Img, State.Functions);
}
} // namespace neverd::sdk
