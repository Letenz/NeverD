//===- CSourceRecorder.h - Private emission event recorder ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_CSOURCERECORDER_H
#define NEVERD_BACKEND_C_CSOURCERECORDER_H

#include "neverd/backend/c/CSourceMap.h"

#include "llvm/ADT/StringRef.h"

#include <map>
#include <optional>
#include <set>
#include <tuple>

namespace llvm {
class Function;
class Instruction;
class Value;
} // namespace llvm

namespace neverd {
struct HighStmt;

/// Delimiters belong only to a second, private render. They describe emission
/// events, not a search for a printed expression. Publication requires removing
/// them to reproduce the ordinary render byte for byte; otherwise all mappings
/// are discarded and the ordinary render remains the output.
class CSourceRecorder {
public:
  CSourceRecorder(CSourceMap &Map, llvm::StringRef Ordinary,
                  bool InstructionsOnly = false);
  void prepareHighSources();
  void prepareLLVMSources(const LLVMSourceMap &Sources);
  std::string expression(va_t Function, const HighExpr &Expr, std::string Text);
  std::string expression(const llvm::Instruction &Value, std::string Text);
  std::optional<size_t> instruction(const llvm::Instruction &Value);
  std::optional<size_t> statement(va_t Function, const HighStmt &Stmt);
  std::optional<size_t> function(va_t Entry);
  std::optional<size_t> function(const llvm::Function &Function);
  std::string begin(size_t Event) const;
  std::string end(size_t Event) const;
  /// A delimiter where a top-level function definition begins.
  std::string definition(std::optional<va_t> Entry);
  std::string definition(const llvm::Function &Function);
  bool finish(llvm::StringRef Annotated, llvm::StringRef Ordinary);

private:
  size_t event(std::optional<size_t> Region,
               std::vector<sigs::LibraryOccurrence> Coverage,
               std::optional<va_t> Function = std::nullopt);
  CSourceMap &Map;
  bool InstructionsOnly;
  std::string Prefix;
  struct Event {
    std::optional<size_t> Region;
    std::vector<sigs::LibraryOccurrence> Coverage;
    std::optional<va_t> Function;
  };
  std::vector<Event> Events;
  std::vector<std::optional<va_t>> DefinitionEntries;
  std::map<std::pair<va_t, const HighExpr *>, Event> HighRegions;
  std::map<std::pair<va_t, va_t>, Event> HighStores;
  std::map<std::tuple<va_t, va_t, StmtKind>, std::set<sigs::LibraryOccurrence>>
      HighAnchors;
  std::map<const llvm::Function *, va_t> LLVMFunctions;
  std::map<const llvm::Value *, Event> LLVMRegions;
  std::map<const llvm::Instruction *,
           std::pair<va_t, std::set<sigs::LibraryOccurrence>>>
      LLVMAnchors;
};

} // namespace neverd

#endif
