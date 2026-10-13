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
