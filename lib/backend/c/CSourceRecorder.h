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

namespace llvm {
class Function;
class Instruction;
class Value;
} // namespace llvm

namespace neverd {

/// Delimiters belong only to a second, private render. They describe emission
/// events, not a search for a printed expression. Publication requires removing
/// them to reproduce the ordinary render byte for byte; otherwise all mappings
/// are discarded and the ordinary render remains the output.
class CSourceRecorder {
public:
  CSourceRecorder(CSourceMap &Map, llvm::StringRef Ordinary);
  void prepareHighSources();
  void prepareLLVMSources(const LLVMSourceMap &Sources);
  std::string expression(va_t Function, const HighExpr &Expr, std::string Text);
  std::string expression(const llvm::Instruction &Value, std::string Text);
  std::optional<size_t> instruction(const llvm::Instruction &Value);
  std::optional<size_t> function(va_t Entry);
  std::optional<size_t> function(const llvm::Function &Function);
  std::string begin(size_t Event) const;
  std::string end(size_t Event) const;
  bool finish(llvm::StringRef Annotated, llvm::StringRef Ordinary);

private:
  size_t event(size_t Region, std::vector<sigs::LibraryOccurrence> Coverage);
  CSourceMap &Map;
  std::string Prefix;
  struct Event {
    size_t Region;
    std::vector<sigs::LibraryOccurrence> Coverage;
  };
  std::vector<Event> Events;
  std::map<std::pair<va_t, const HighExpr *>, size_t> HighRegions;
  std::map<const llvm::Function *, va_t> LLVMFunctions;
  std::map<const llvm::Value *, Event> LLVMRegions;
};

} // namespace neverd

#endif
