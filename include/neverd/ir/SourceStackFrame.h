#pragma once
#include "neverd/ir/low/LowIR.h"

#include <map>
#include <optional>
#include <string>

namespace neverd {
/// Analyst assertions are entry-SP coordinates, never machine lift evidence.
struct SourceStackFrame {
  struct Variable {
    int64_t Offset = 0;
    uint16_t Bytes = 0;
    std::string Name;
    std::string Type;
    bool operator==(const Variable &) const = default;
  };
  std::string Snapshot;
  std::map<va_t, int64_t> Points;
  std::vector<Variable> Variables;
  bool operator==(const SourceStackFrame &) const = default;
  bool empty() const { return Points.empty() && Variables.empty(); }
};
struct StackFrameAccess {
  va_t Instruction = 0;
  int Sequence = 0;
  int64_t Offset = 0;
  uint16_t Bytes = 0;
  bool Write = false;
};
struct StackFrameInstruction {
  std::optional<int64_t> Before, After;
  bool Correctable = false;
};
struct StackFrameAnalysis {
  std::map<va_t, StackFrameInstruction> Instructions;
  std::vector<StackFrameAccess> Accesses;
  std::string Error;
};
/// Bounded must-analysis. Unknown is not zero. No memory or callee guesses.
StackFrameAnalysis analyzeSourceStackFrame(const LowFunc &, Arch,
                                           const SourceStackFrame & = {});
/// Private source interpretation only; refuses roots without an entry carrier.
std::optional<LowFunc> projectSourceStackFrame(const LowFunc &, Arch,
                                               const SourceStackFrame &,
                                               std::string &Error);
} // namespace neverd
