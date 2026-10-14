//===- HighCSEH.cpp - Native Windows SEH annotation -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep native dispatch visible when its protected ranges are not reducible.
//===----------------------------------------------------------------------===//

#include "../HighCWriter.h"

#include "llvm/ADT/StringExtras.h"

#include <stdexcept>

namespace neverd {

void HighCWriter::writeSEHScopeAnnotation(const HighFunc &Func, int Indent) {
  if (!Func.ExceptionMetadata || !Func.ExceptionMetadata->SEH)
    return;
  const auto &Scopes = Func.ExceptionMetadata->SEH->Scopes;
  for (size_t I = 0; I < Scopes.size(); ++I) {
    const auto &Scope = Scopes[I];
    const char *Kind = Scope.Kind == SEHScopeKind::Finally    ? "finally"
                       : Scope.Kind == SEHScopeKind::CatchAll ? "except-all"
                                                              : "filter";
    emitIndent(Indent);
    OS << " * seh.scope[" << I << "]: " << Kind << " [0x"
       << llvm::utohexstr(Scope.GuardedRange.Begin) << ", 0x"
       << llvm::utohexstr(Scope.GuardedRange.End) << ")";
    if (Scope.FilterOrFinallyVA)
      OS << " filter_or_finally=0x" << llvm::utohexstr(Scope.FilterOrFinallyVA);
    if (Scope.NormalizedFilterVA)
      OS << " normalized_filter=0x"
         << llvm::utohexstr(Scope.NormalizedFilterVA);
    if (Scope.HandlerVA)
      OS << " handler=0x" << llvm::utohexstr(Scope.HandlerVA);
    if (Scope.ContinuationVA)
      OS << " continuation=0x" << llvm::utohexstr(Scope.ContinuationVA);
    OS << ", status=" << getExceptionParseStatusName(Scope.ParseStatus) << "\n";
  }
}

void HighCWriter::writeUnstructuredSEHRegion(const HighStmt &Stmt, int Indent) {
  for (const auto &Body : Stmt.EHClauseBodies)
    if (!Body.empty())
      throw std::invalid_argument(
          "unstructured SEH view requires the original native handler body");
  emitIndent(Indent);
  OS << "/* unstructured SEH region [0x" << llvm::utohexstr(Stmt.EHRange.Begin)
     << ", 0x" << llvm::utohexstr(Stmt.EHRange.End)
     << "): dispatch is supplied by the image's exception runtime.\n";
  for (const auto &Clause : Stmt.EHClauses) {
    const bool Finally = Clause.Kind == HighEHClauseKind::SEHFinally;
    emitIndent(Indent);
    OS << " * " << (Finally ? "finally: action" : "except: filter") << " @ 0x"
       << llvm::utohexstr(Clause.FilterOrActionVA) << ", handler @ 0x"
       << llvm::utohexstr(Clause.HandlerVA) << ", state=" << Clause.State
       << ", status=" << getExceptionParseStatusName(Clause.ParseStatus);
    for (va_t Continuation : Clause.ContinuationVAs)
      OS << ", continuation @ 0x" << llvm::utohexstr(Continuation);
    if (!Finally && Clause.FilterValue)
      OS << ", filter_value=" << sehFilterValueText(*Clause.FilterValue);
    OS << "\n";
  }
  // These records remain essential when optional descriptive comments are
  // disabled. With comments enabled the function annotation already owns them.
  if (CurrentFunc && !Opts.EmitComments) {
    writeSEHScopeAnnotation(*CurrentFunc, Indent);
    writeRegistrationAnnotation(*CurrentFunc, Indent);
  }
  emitIndent(Indent);
  OS << " */\n";
  emitIndent(Indent);
  OS << "{\n";
  writeTryBody(Stmt.Body, Indent + 1);
  emitIndent(Indent);
  OS << "}\n";
}

} // namespace neverd
