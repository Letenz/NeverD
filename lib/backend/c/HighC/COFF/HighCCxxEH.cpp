//===- HighCCxxEH.cpp - Native Windows C++ exception views -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep table-owned dispatch and callback ABIs outside lexical protection.
//===----------------------------------------------------------------------===//

#include "../HighCWriter.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"

#include <stdexcept>

namespace neverd {

void HighCWriter::writeCxxScopeAnnotation(const HighFunc &Func, int Indent) {
  if (!Func.ExceptionMetadata || !Func.ExceptionMetadata->Cxx)
    return;
  const auto &Cxx = *Func.ExceptionMetadata->Cxx;
  emitIndent(Indent);
  OS << " * cxx.format="
     << (Cxx.NativeEncoding == CxxExceptionInfo::Encoding::FH4 ? "fh4" : "fh3")
     << ", states=" << Cxx.MaxState << ", try_blocks=" << Cxx.TryBlocks.size()
     << ", ip_states=" << Cxx.IPMap.size() << "\n";
  for (size_t I = 0; I < Cxx.UnwindMap.size(); ++I) {
    const auto &Action = Cxx.UnwindMap[I];
    emitIndent(Indent);
    OS << " * cxx.unwind[" << I << "]: to_state=" << Action.ToState
       << ", kind=" << getCxxUnwindActionKindName(Action.Kind);
    if (Action.ActionVA)
      OS << ", action=0x" << llvm::utohexstr(Action.ActionVA);
    if (Action.ObjectOffset)
      OS << ", object_offset=" << Action.ObjectOffset;
    OS << "\n";
  }
  for (size_t I = 0; I < Cxx.TryBlocks.size(); ++I) {
    const auto &Try = Cxx.TryBlocks[I];
    emitIndent(Indent);
    OS << " * cxx.try[" << I << "]: states=" << Try.TryLow << ".."
       << Try.TryHigh << ", catch_high=" << Try.CatchHigh
       << ", handlers=" << Try.Handlers.size() << "\n";
    for (size_t J = 0; J < Try.Handlers.size(); ++J) {
      const auto &Catch = Try.Handlers[J];
      emitIndent(Indent);
      OS << " *   catch[" << J << "]: type=0x"
         << llvm::utohexstr(Catch.TypeDescriptorVA) << ", handler=0x"
         << llvm::utohexstr(Catch.HandlerVA) << ", adjectives=0x"
         << llvm::utohexstr(Catch.Adjectives)
         << ", object_offset=" << Catch.CatchObjectOffset
         << ", parent_frame_offset=" << Catch.ParentFrameOffset;
      if (!Catch.ContinuationVAs.empty()) {
        OS << ", continuations=";
        for (size_t K = 0; K < Catch.ContinuationVAs.size(); ++K) {
          if (K)
            OS << ",";
          OS << "0x" << llvm::utohexstr(Catch.ContinuationVAs[K]);
        }
      }
      OS << "\n";
    }
  }
  for (size_t I = 0; I < Cxx.IPMap.size(); ++I) {
    emitIndent(Indent);
    OS << " * cxx.ip_state[" << I << "]: ip=0x"
       << llvm::utohexstr(Cxx.IPMap[I].IP) << ", state=" << Cxx.IPMap[I].State
       << "\n";
  }
}

void HighCWriter::writeCExceptionRegion(const HighStmt &Stmt, int Indent) {
  // The C projection shows native entry points, not an invented setjmp-based
  // replacement for the platform unwinder. A funclet keeps its own ABI and
  // frame. A checked body within the parent remains a runtime-only label;
  // independent funclets retain their separate definitions.
  for (size_t I = 0; I < Stmt.EHClauses.size(); ++I) {
    const HighEHClause &Clause = Stmt.EHClauses[I];
    const va_t Entry = Clause.Kind == HighEHClauseKind::CxxCleanup
                           ? Clause.FilterOrActionVA
                           : Clause.HandlerVA;
    if (I < Stmt.EHClauseBodies.size() && !Stmt.EHClauseBodies[I].empty() &&
        !isEmbeddedRegistrationCallback(Stmt, I) &&
        !isEmbeddedWindowsCallback(Stmt, I) &&
        (!Entry || (CurrentFunc && Entry == CurrentFunc->Entry) ||
         !DefinedFunctionsByAddress.count(Entry)))
      throw std::invalid_argument(
          "plain C exception view requires the native handler function; "
          "an embedded handler cannot be assigned a new C ABI");
  }
  const std::vector<va_t> OuterLeave = std::move(LeaveTargets);
  LeaveTargets.clear();
  llvm::scope_exit Restore([&] { LeaveTargets = OuterLeave; });
  emitIndent(Indent);
  OS << "/* C++ unwind-table region [0x" << llvm::utohexstr(Stmt.EHRange.Begin)
     << ", 0x" << llvm::utohexstr(Stmt.EHRange.End)
     << "): dispatch is supplied by the image's exception runtime.\n";
  for (const HighEHClause &Clause : Stmt.EHClauses) {
    emitIndent(Indent);
    OS << " * "
       << (Clause.Kind == HighEHClauseKind::CxxCleanup ? "cleanup" : "handler")
       << " @ 0x" << llvm::utohexstr(Clause.HandlerVA) << ", action @ 0x"
       << llvm::utohexstr(Clause.FilterOrActionVA) << ", type descriptor @ 0x"
       << llvm::utohexstr(Clause.TypeDescriptorVA) << ", state=" << Clause.State
       << ", catch-object offset=" << Clause.CatchObjectOffset;
    for (va_t Continuation : Clause.ContinuationVAs)
      OS << ", continuation @ 0x" << llvm::utohexstr(Continuation);
    OS << ", parent-frame offset=" << Clause.ParentFrameOffset
       << ", adjectives=0x" << llvm::utohexstr(Clause.Adjectives)
       << ", status=" << getExceptionParseStatusName(Clause.ParseStatus);
    if (Clause.Kind == HighEHClauseKind::CxxCleanup)
      OS << ", kind=" << getCxxUnwindActionKindName(Clause.UnwindActionKind)
         << ", object offset=" << Clause.UnwindObjectOffset;
    OS << "\n";
  }
  if (CurrentFunc && !Opts.EmitComments && !Stmt.EHIsReducible) {
    writeCxxScopeAnnotation(*CurrentFunc, Indent);
    writeRegistrationAnnotation(*CurrentFunc, Indent);
  }
  emitIndent(Indent);
  OS << " */\n";
  emitIndent(Indent);
  OS << "{\n";
  writeTryBody(Stmt.Body, Indent + 1);
  emitIndent(Indent);
  OS << "}\n";
  writeEmbeddedNativeCallbacks(Stmt, Indent);
}

} // namespace neverd
