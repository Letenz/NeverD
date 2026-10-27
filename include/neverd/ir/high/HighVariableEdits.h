//===- HighVariableEdits.h - Proven source-local associations -*- C++ -*-===//
#ifndef NEVERD_IR_HIGH_HIGHVARIABLEEDITS_H
#define NEVERD_IR_HIGH_HIGHVARIABLEEDITS_H
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/high/HighSourceMap.h"

#include <string>
#include <vector>
namespace neverd {
struct HighVariableEdit {
  enum Kind { Map, Split } TheKind = Map;
  std::string From;
  std::string To;
};
struct HighVariableEditResult {
  HighFunc Function;
  HighSourceMap Sources;
  std::vector<std::string> Outputs;
  std::string Refusal;
  explicit operator bool() const { return Refusal.empty() && !Outputs.empty(); }
};
/// A private source variant, regenerated from an immutable analysis baseline.
/// No machine IR, image facts or original expression objects are modified.
HighVariableEditResult editHighVariable(const HighFunc &Function,
                                        const HighSourceMap &Sources,
                                        const HighVariableEdit &Edit);
} // namespace neverd
#endif
