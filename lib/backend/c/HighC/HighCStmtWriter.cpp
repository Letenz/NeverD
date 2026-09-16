//===- HighCStmtWriter.cpp - HighIR statement rendering -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Statement rendering for the HighIR C emitter: converts HighStmt
/// structures into indented C source blocks.  Function-level orchestration
/// lives in HighCFuncWriter.cpp.
///
//===----------------------------------------------------------------------===//

#include "HighCWriter.h"

#include "neverd/loader/ExceptionInfo.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

#include <cctype>
#include <functional>

namespace neverd {

namespace {

// MSVC `HandlerType` adjectives from CRT `ehdata.h`.
constexpr uint32_t kCxxCatchConst = 0x1u;
constexpr uint32_t kCxxCatchVolatile = 0x2u;
constexpr uint32_t kCxxCatchReference = 0x8u;

bool isCIdentifier(llvm::StringRef Name) {
  if (Name.empty() ||
      (!std::isalpha(static_cast<unsigned char>(Name.front())) &&
       Name.front() != '_'))
    return false;
  return llvm::all_of(Name, [](char Ch) {
    return std::isalnum(static_cast<unsigned char>(Ch)) || Ch == '_';
  });
}

void writeCxxCatchType(llvm::raw_ostream &OS, const HighEHClause &Clause) {
  if (Clause.TypeName.empty() && Clause.TypeDescriptorVA == 0) {
    OS << "...";
    return;
  }
  if ((Clause.Adjectives & kCxxCatchConst) != 0)
    OS << "const ";
  if ((Clause.Adjectives & kCxxCatchVolatile) != 0)
    OS << "volatile ";
  if (!Clause.TypeName.empty() && isCIdentifier(Clause.TypeName))
    OS << Clause.TypeName;
  else if (!Clause.TypeName.empty())
    OS << "/* " << Clause.TypeName << " */";
  else
    OS << "/* type @ 0x" << llvm::utohexstr(Clause.TypeDescriptorVA) << " */";
  if ((Clause.Adjectives & kCxxCatchReference) != 0)
    OS << " &";
}

} // namespace

void HighCWriter::emitIndent(int Indent) { emitCIndent(OS, Indent); }

void HighCWriter::writeStmt(const HighStmt &Stmt, int Indent) {
  if (Analysis.DeadStmts.count(&Stmt))
    return;
  const bool HideEHRuntimeMemory =
      CurrentFunc && CurrentFunc->ExceptionMetadata.has_value();
  auto IsEHRuntimeSpace = [&](NdMemoryAddressSpace Space) {
    return HideEHRuntimeMemory &&
           (Space == NdMemoryAddressSpace::X86FS ||
            Space == NdMemoryAddressSpace::X86GS);
  };
  switch (Stmt.Kind) {
  case StmtKind::Assign: {
    if (!Stmt.Dst || !Stmt.Val)
      return;
    if (Stmt.Val->Kind == ExprKind::Load &&
        IsEHRuntimeSpace(Stmt.Val->MemoryAddressSpace))
      return;
    if (Stmt.Val->Kind == ExprKind::Call) {
      auto Rendered = renderX86SegmentedIntrinsicStatement(
          Opts.TheArch, *Stmt.Val, Stmt.Dst.get(),
          [this](const HighExpr &E) { return exprStr(E); },
          [this](const MedVar &V) { return varName(V); },
          [this](const MedVar &V) {
            return !Analysis.DeadVars.count(varName(V));
          });
      if (!Rendered.empty()) {
        emitIndent(Indent);
        OS << Rendered;
        break;
      }
    }
    if (Stmt.Val->Kind == ExprKind::Call &&
        !Stmt.Val->IntrinsicOutputs.empty()) {
      auto Rendered = MultiOutputRender{}(
          Opts.TheArch, Stmt.Val->IntrinsicId, Stmt.Val->IntrinsicOutputs,
          Stmt.Val->Operands, [this](const HighExpr &E) { return exprStr(E); },
          [this](const MedVar &V) { return varName(V); },
          [this](const MedVar &V) {
            return !Analysis.DeadVars.count(varName(V));
          });
      if (!Rendered.empty()) {
        emitIndent(Indent);
        OS << Rendered;
        HasCIntrinsics = true;
        break;
      }
    }
    bool DeadIntrinsicResult = Stmt.Dst->Kind == ExprKind::Var &&
                               Analysis.DeadVars.count(varName(Stmt.Dst->Var));
    if (Stmt.Val->Kind == ExprKind::Call &&
        Stmt.Val->IntrinsicId != Intrinsic::None && DeadIntrinsicResult &&
        (isSideeffectIntrinsic(Stmt.Val->IntrinsicId) ||
         !intrinsicCName(Stmt.Val->IntrinsicId))) {
      emitIndent(Indent);
      OS << exprStr(*Stmt.Val) << ";\n";
      break;
    }
    if (Stmt.Dst->Kind == ExprKind::Load && !Stmt.Dst->Operands.empty()) {
      if (IsEHRuntimeSpace(Stmt.Dst->MemoryAddressSpace))
        return;
      if (Stmt.Dst->MemoryOrdering == NdMemoryOrdering::None &&
          Stmt.Dst->MemoryAddressSpace == NdMemoryAddressSpace::Default)
        if (auto Slot = namedFrameSlot(*Stmt.Dst->Operands[0])) {
          if (isParamCopy(*Stmt.Val)) {
            if (auto Src = copyForwardSource(*Stmt.Val)) {
              CopyForward[*Slot] = *Src;
              Analysis.DeadVars.insert(*Slot);
              break;
            }
          }
          if (isCompilerEHConstant(*Stmt.Val))
            break;
          CopyForward.erase(*Slot);
          emitIndent(Indent);
          OS << *Slot << " = " << exprStr(*Stmt.Val) << ";\n";
          break;
        }
      if (auto VA = constAddress(*Stmt.Dst->Operands[0])) {
        if (auto Name = imageObjectName(*VA)) {
          emitIndent(Indent);
          OS << *Name << " = " << exprStr(*Stmt.Val) << ";\n";
          break;
        }
      }
      emitIndent(Indent);
      OS << memoryStoreExpr(Stmt.Dst->Type, exprStr(*Stmt.Dst->Operands[0]),
                            exprStr(*Stmt.Val), Stmt.Dst->MemoryOrdering,
                            Stmt.Dst->MemoryAddressSpace)
         << ";\n";
      break;
    }
    if ((Stmt.Dst->Kind == ExprKind::Var || Stmt.Dst->Kind == ExprKind::Phi) &&
        isCopyForwardDestination(Stmt.Dst->Var)) {
      const std::string DstName = varName(Stmt.Dst->Var);
      if (auto Src = copyForwardSource(*Stmt.Val)) {
        CopyForward[DstName] = *Src;
        Analysis.DeadVars.insert(DstName);
        break;
      }
      CopyForward.erase(DstName);
    }
    emitIndent(Indent);
    if (Stmt.Dst->Kind == ExprKind::Var || Stmt.Dst->Kind == ExprKind::Phi) {
      // Variable destinations are lvalues, unlike exprStr's machine-value
      // projection of typed pointer parameters.
      OS << varName(Stmt.Dst->Var) << " = ";
      auto DeclaredType = declaredParamType(Stmt.Dst->Var);
      if (DeclaredType && DeclaredType->Kind == NdTypeKind::Ptr)
        OS << "(" << typeToC(DeclaredType) << ")(uintptr_t)("
           << exprStr(*Stmt.Val) << ")";
      else
        OS << exprStr(*Stmt.Val);
    } else {
      OS << exprStr(*Stmt.Dst) << " = " << exprStr(*Stmt.Val);
    }
    OS << ";\n";
    break;
  }

  case StmtKind::Store:
    if (!Stmt.StoreAddr || !Stmt.StoreVal)
      return;
    if (IsEHRuntimeSpace(Stmt.MemoryAddressSpace))
      return;
    if (isCompilerEHConstant(*Stmt.StoreVal))
      break;
    if (Stmt.MemoryOrdering == NdMemoryOrdering::None &&
        Stmt.MemoryAddressSpace == NdMemoryAddressSpace::Default)
      if (auto Slot = namedFrameSlot(*Stmt.StoreAddr)) {
        if (isParamCopy(*Stmt.StoreVal)) {
          if (auto Src = copyForwardSource(*Stmt.StoreVal)) {
            CopyForward[*Slot] = *Src;
            Analysis.DeadVars.insert(*Slot);
            break;
          }
        }
        CopyForward.erase(*Slot);
        emitIndent(Indent);
        OS << *Slot << " = " << exprStr(*Stmt.StoreVal) << ";\n";
        break;
      }
    if (auto VA = constAddress(*Stmt.StoreAddr)) {
      if (auto Name = imageObjectName(*VA)) {
        emitIndent(Indent);
        OS << *Name << " = " << exprStr(*Stmt.StoreVal) << ";\n";
        break;
      }
    }
    emitIndent(Indent);
    OS << memoryStoreExpr(Stmt.StoreVal->Type, exprStr(*Stmt.StoreAddr),
                          exprStr(*Stmt.StoreVal), Stmt.MemoryOrdering,
                          Stmt.MemoryAddressSpace)
       << ";\n";
    break;

  case StmtKind::Call:
    if (!Stmt.CallExpr)
      return;
    {
      auto Rendered = renderX86SegmentedIntrinsicStatement(
          Opts.TheArch, *Stmt.CallExpr, nullptr,
          [this](const HighExpr &E) { return exprStr(E); },
          [this](const MedVar &V) { return varName(V); },
          [this](const MedVar &V) {
            return !Analysis.DeadVars.count(varName(V));
          });
      if (!Rendered.empty()) {
        emitIndent(Indent);
        OS << Rendered;
        break;
      }
    }
    if (!Stmt.CallExpr->IntrinsicOutputs.empty()) {
      auto Rendered = MultiOutputRender{}(
          Opts.TheArch, Stmt.CallExpr->IntrinsicId,
          Stmt.CallExpr->IntrinsicOutputs, Stmt.CallExpr->Operands,
          [this](const HighExpr &E) { return exprStr(E); },
          [this](const MedVar &V) { return varName(V); },
          [this](const MedVar &V) {
            return !Analysis.DeadVars.count(varName(V));
          });
      if (!Rendered.empty()) {
        emitIndent(Indent);
        OS << Rendered;
        HasCIntrinsics = true;
        break;
      }
    }
    emitIndent(Indent);
    OS << exprStr(*Stmt.CallExpr) << ";\n";
    break;

  case StmtKind::Return:
    emitIndent(Indent);
    if (InferredVoid)
      OS << "return;\n";
    else if (Stmt.RetVal)
      OS << "return " << formatReturnExpr(*Stmt.RetVal) << ";\n";
    else
      OS << "return;\n";
    break;

  case StmtKind::If:
    if (!Stmt.Cond)
      return;
    emitIndent(Indent);
    OS << "if (" << exprStr(*Stmt.Cond) << ") {\n";
    writeStmts(Stmt.Body, Indent + 1);
    emitIndent(Indent);
    OS << "}\n";
    break;

  case StmtKind::IfElse:
    if (!Stmt.Cond)
      return;
    if (stmtsEffectivelyEmpty(Stmt.Body) &&
        !stmtsEffectivelyEmpty(Stmt.ElseBody)) {
      emitIndent(Indent);
      OS << "if (" << invertCondStr(*Stmt.Cond) << ") {\n";
      writeStmts(Stmt.ElseBody, Indent + 1);
      emitIndent(Indent);
      OS << "}\n";
      break;
    }
    emitIndent(Indent);
    OS << "if (" << exprStr(*Stmt.Cond) << ") {\n";
    writeStmts(Stmt.Body, Indent + 1);
    emitIndent(Indent);
    if (stmtsEffectivelyEmpty(Stmt.ElseBody)) {
      OS << "}\n";
      break;
    }
    OS << "} else {\n";
    writeStmts(Stmt.ElseBody, Indent + 1);
    emitIndent(Indent);
    OS << "}\n";
    break;

  case StmtKind::While:
    emitIndent(Indent);
    OS << "while (" << (Stmt.Cond ? exprStr(*Stmt.Cond) : "1") << ") {\n";
    writeStmts(Stmt.Body, Indent + 1);
    emitIndent(Indent);
    OS << "}\n";
    break;

  case StmtKind::DoWhile:
    emitIndent(Indent);
    OS << "do {\n";
    writeStmts(Stmt.Body, Indent + 1);
    emitIndent(Indent);
    OS << "} while (" << (Stmt.Cond ? exprStr(*Stmt.Cond) : "1") << ");\n";
    break;

  case StmtKind::For:
    emitIndent(Indent);
    OS << "for (;;) {\n";
    if (Stmt.Cond) {
      emitIndent(Indent + 1);
      OS << "if (!(" << exprStr(*Stmt.Cond) << ")) break;\n";
    }
    writeStmts(Stmt.Body, Indent + 1);
    emitIndent(Indent);
    OS << "}\n";
    break;

  case StmtKind::Switch:
    if (!Stmt.SwitchExpr)
      return;
    emitIndent(Indent);
    OS << "switch (" << exprStr(*Stmt.SwitchExpr) << ") {\n";
    for (auto &C : Stmt.Cases) {
      emitIndent(Indent);
      OS << "case " << constStr(C.Value) << ":\n";
      writeStmts(C.Body, Indent + 1);
      emitIndent(Indent + 1);
      OS << "break;\n";
    }
    if (!Stmt.DefaultBody.empty()) {
      emitIndent(Indent);
      OS << "default:\n";
      writeStmts(Stmt.DefaultBody, Indent + 1);
      emitIndent(Indent + 1);
      OS << "break;\n";
    }
    emitIndent(Indent);
    OS << "}\n";
    break;

  case StmtKind::Goto:
    emitIndent(Indent);
    OS << "goto L_" + llvm::utohexstr(Stmt.GotoTarget) + ";\n";
    break;

  case StmtKind::Break:
    emitIndent(Indent);
    OS << "break;\n";
    break;

  case StmtKind::Continue:
    emitIndent(Indent);
    OS << "continue;\n";
    break;

  case StmtKind::Block:
    emitIndent(Indent);
    OS << "{\n";
    writeStmts(Stmt.Body, Indent + 1);
    emitIndent(Indent);
    OS << "}\n";
    break;

  case StmtKind::ExprStmt:
    if (Stmt.Val) {
      if (Stmt.Val->Kind == ExprKind::Load && !Stmt.Val->Operands.empty() &&
          namedFrameSlot(*Stmt.Val->Operands[0]))
        break;
      emitIndent(Indent);
      OS << exprStr(*Stmt.Val) << ";\n";
    }
    break;

  case StmtKind::SEHTry: {
    if (!Stmt.EHIsReducible || Stmt.EHClauses.size() != 1 ||
        Stmt.EHClauseBodies.size() != 1) {
      emitIndent(Indent);
      OS << "__try {\n";
      writeTryBody(Stmt.Body, Indent + 1);
      emitIndent(Indent);
      OS << "} __except (EXCEPTION_EXECUTE_HANDLER) {\n";
      emitIndent(Indent + 1);
      OS << "/* unstructured SEH region [0x"
         << llvm::utohexstr(Stmt.EHRange.Begin) << ", 0x"
         << llvm::utohexstr(Stmt.EHRange.End) << ") */\n";
      emitIndent(Indent);
      OS << "}\n";
      break;
    }
    const HighEHClause &Clause = Stmt.EHClauses.front();
    emitIndent(Indent);
    OS << "__try {\n";
    writeTryBody(Stmt.Body, Indent + 1);
    emitIndent(Indent);
    if (Clause.Kind == HighEHClauseKind::SEHFinally) {
      OS << "} __finally {\n";
      {
        const bool SavedHandler = InEHClauseBody;
        InEHClauseBody = true;
        writeStmts(Stmt.EHClauseBodies.front(), Indent + 1);
        InEHClauseBody = SavedHandler;
      }
      if (Stmt.EHClauseBodies.front().empty()) {
        emitIndent(Indent + 1);
        OS << "/* finally handler @ 0x"
           << llvm::utohexstr(Clause.FilterOrActionVA) << " */\n";
      }
    } else {
      OS << "} __except (";
      if (Clause.FilterOrActionVA == 0) {
        OS << "EXCEPTION_EXECUTE_HANDLER";
      } else {
        std::string FilterName;
        if (auto It = DefinedFunctionsByAddress.find(Clause.FilterOrActionVA);
            It != DefinedFunctionsByAddress.end() && It->second)
          FilterName = functionIdentifier(*It->second);
        else if (Dbg) {
          if (auto Sym = Dbg->resolveFunction(Clause.FilterOrActionVA);
              Sym && !Sym->Name.empty())
            FilterName = functionIdentifier(Sym->Name);
        }
        if (FilterName.empty())
          FilterName = "nd_seh_filter_0x" +
                       llvm::utohexstr(Clause.FilterOrActionVA);
        OS << FilterName << "(GetExceptionInformation())";
      }
      OS << ") {\n";
      {
        const bool SavedHandler = InEHClauseBody;
        InEHClauseBody = true;
        writeStmts(Stmt.EHClauseBodies.front(), Indent + 1);
        InEHClauseBody = SavedHandler;
      }
      if (Stmt.EHClauseBodies.front().empty()) {
        emitIndent(Indent + 1);
        if (CurrentFunc && CurrentFunc->ExceptionMetadata &&
            CurrentFunc->ExceptionMetadata->CodeRange.contains(
                Clause.HandlerVA))
          OS << "goto L_" << llvm::utohexstr(Clause.HandlerVA) << ";\n";
        else
          OS << "/* handler @ 0x" << llvm::utohexstr(Clause.HandlerVA)
             << " */\n";
      }
    }
    emitIndent(Indent);
    OS << "}\n";
    break;
  }

  case StmtKind::CxxTry: {
    emitIndent(Indent);
    OS << "try {\n";
    writeTryBody(Stmt.Body, Indent + 1);
    emitIndent(Indent);
    OS << "}";
    for (size_t I = 0; I < Stmt.EHClauses.size(); ++I) {
      const HighEHClause &Clause = Stmt.EHClauses[I];
      if (Clause.Kind == HighEHClauseKind::CxxCleanup) {
        OS << "\n";
        emitIndent(Indent);
        OS << "/* unwind cleanup(state=" << Clause.State
           << ", kind=" << getCxxUnwindActionKindName(Clause.UnwindActionKind)
           << ", object at frame+" << Clause.UnwindObjectOffset;
        if (Clause.FilterOrActionVA)
          OS << ", dtor @ 0x" << llvm::utohexstr(Clause.FilterOrActionVA);
        OS << " */\n";
        if (I < Stmt.EHClauseBodies.size()) {
          const bool SavedHandler = InEHClauseBody;
          InEHClauseBody = true;
          writeStmts(Stmt.EHClauseBodies[I], Indent);
          InEHClauseBody = SavedHandler;
        }
        continue;
      }
      OS << " catch (";
      writeCxxCatchType(OS, Clause);
      OS << ") {\n";
      if (I < Stmt.EHClauseBodies.size()) {
        const bool SavedHandler = InEHClauseBody;
        InEHClauseBody = true;
        writeStmts(Stmt.EHClauseBodies[I], Indent + 1);
        InEHClauseBody = SavedHandler;
        if (Stmt.EHClauseBodies[I].empty() && Clause.HandlerVA) {
          emitIndent(Indent + 1);
          OS << "/* handler @ 0x" << llvm::utohexstr(Clause.HandlerVA)
             << " */\n";
        }
      }
      emitIndent(Indent);
      OS << "}";
    }
    OS << "\n";
    break;
  }

  case StmtKind::ItaniumTry: {
    emitIndent(Indent);
    OS << "/* Itanium try [0x" << llvm::utohexstr(Stmt.EHRange.Begin) << ", 0x"
       << llvm::utohexstr(Stmt.EHRange.End)
       << ") recovered from the call-site table */\n";
    emitIndent(Indent);
    OS << "{\n";
    writeStmts(Stmt.Body, Indent + 1);
    emitIndent(Indent);
    OS << "}\n";
    // Every clause of an Itanium region enters the same landing pad, which is
    // ordinary code in this function rather than a funclet.  Naming the pad is
    // therefore the whole of what a clause adds: the pad selects between the
    // clauses itself, from the selector the personality left it.
    for (const HighEHClause &Clause : Stmt.EHClauses) {
      emitIndent(Indent);
      if (Clause.Kind == HighEHClauseKind::ItaniumSpec) {
        OS << "/* exception specification ";
        if (Clause.SpecTypeNames.empty()) {
          OS << "throw()";
        } else {
          OS << "throw(";
          for (size_t I = 0; I < Clause.SpecTypeNames.size(); ++I)
            OS << (I ? ", " : "") << Clause.SpecTypeNames[I];
          OS << ")";
        }
      } else {
        OS << "/* catch (";
        if (!Clause.TypeName.empty())
          OS << Clause.TypeName;
        else if (Clause.TypeDescriptorVA)
          OS << "typeinfo@0x" << llvm::utohexstr(Clause.TypeDescriptorVA);
        else
          OS << "...";
        OS << ")";
      }
      OS << ", filter=" << Clause.TypeFilter << ", depth=" << Clause.ChainDepth;
      if (Clause.ParseStatus != ExceptionParseStatus::Complete)
        OS << ", parse=" << getExceptionParseStatusName(Clause.ParseStatus);
      for (va_t Pad : Clause.LandingPadVAs)
        OS << " -> L_" << llvm::utohexstr(Pad);
      OS << " */\n";
    }
    break;
  }

  case StmtKind::Nop:
    // A removed PHI copy may still own a goto label, including the last label
    // in a compound statement. C11 requires a statement after that label.
    if (GotoTargets.count(Stmt.Addr)) {
      emitIndent(Indent);
      OS << ";\n";
    }
    break;
  }
}

void HighCWriter::writeStmts(const std::vector<HighStmt> &Stmts, int Indent) {
  va_t LastLabel = InvalidVA;
  for (auto &S : Stmts) {
    if (S.Addr != 0 && S.Addr != InvalidVA && GotoTargets.count(S.Addr) &&
        S.Addr != LastLabel) {
      OS << "L_" + llvm::utohexstr(S.Addr) + ":\n";
      LastLabel = S.Addr;
    }
    writeStmt(S, Indent);
  }
}

void HighCWriter::writeTryBody(const std::vector<HighStmt> &Stmts, int Indent) {
  size_t End = Stmts.size();
  while (End > 0) {
    const HighStmt &Last = Stmts[End - 1];
    if (Analysis.DeadStmts.count(&Last) || Last.Kind == StmtKind::Nop) {
      --End;
      continue;
    }
    if (Last.Kind == StmtKind::Goto) {
      --End;
      continue;
    }
    break;
  }
  va_t LastLabel = InvalidVA;
  for (size_t I = 0; I < End; ++I) {
    const HighStmt &S = Stmts[I];
    if (S.Addr != 0 && S.Addr != InvalidVA && GotoTargets.count(S.Addr) &&
        S.Addr != LastLabel) {
      OS << "L_" + llvm::utohexstr(S.Addr) + ":\n";
      LastLabel = S.Addr;
    }
    writeStmt(S, Indent);
  }
}

bool HighCWriter::isCompilerEHConstant(const HighExpr &Val) const {
  if (Val.Kind != ExprKind::Const || !CurrentFunc ||
      !CurrentFunc->ExceptionMetadata)
    return false;
  const ExceptionFunction &EH = *CurrentFunc->ExceptionMetadata;
  const uint64_t C = Val.ConstVal;
  if (!EH.Registration)
    return false;
  const RegistrationChainInfo &Reg = *EH.Registration;
  if (Reg.HandlerVA && C == Reg.HandlerVA)
    return true;
  if (Reg.ScopeTableVA && C == Reg.ScopeTableVA)
    return true;
  if (Reg.SeededTryLevel && static_cast<int32_t>(C) == *Reg.SeededTryLevel)
    return true;
  for (const RegistrationTryLevelStore &Store : Reg.TryLevelStores)
    if (Store.Level < 0 && static_cast<int32_t>(C) == Store.Level)
      return true;
  return false;
}

void HighCWriter::collectCopyForward(const HighFunc &Func) {
  std::function<void(const std::vector<HighStmt> &, bool)> Walk;
  Walk = [&](const std::vector<HighStmt> &Stmts, bool InHandler) {
    const bool Saved = InEHClauseBody;
    InEHClauseBody = InHandler;
    for (const HighStmt &Stmt : Stmts) {
      if (!Analysis.DeadStmts.count(&Stmt)) {
        if (Stmt.Kind == StmtKind::Assign && Stmt.Dst && Stmt.Val) {
          if (Stmt.Dst->Kind == ExprKind::Load && !Stmt.Dst->Operands.empty() &&
              Stmt.Dst->MemoryOrdering == NdMemoryOrdering::None &&
              Stmt.Dst->MemoryAddressSpace == NdMemoryAddressSpace::Default) {
            if (auto Slot = namedFrameSlot(*Stmt.Dst->Operands[0])) {
              if (isParamCopy(*Stmt.Val)) {
                if (auto Src = copyForwardSource(*Stmt.Val)) {
                  CopyForward[*Slot] = *Src;
                  Analysis.DeadVars.insert(*Slot);
                } else {
                  CopyForward.erase(*Slot);
                }
              } else {
                CopyForward.erase(*Slot);
              }
            }
          } else if ((Stmt.Dst->Kind == ExprKind::Var ||
                      Stmt.Dst->Kind == ExprKind::Phi) &&
                     isCopyForwardDestination(Stmt.Dst->Var)) {
            const std::string DstName = varName(Stmt.Dst->Var);
            if (auto Src = copyForwardSource(*Stmt.Val)) {
              CopyForward[DstName] = *Src;
              Analysis.DeadVars.insert(DstName);
            } else {
              CopyForward.erase(DstName);
            }
          }
        } else if (Stmt.Kind == StmtKind::Store && Stmt.StoreAddr &&
                   Stmt.StoreVal &&
                   Stmt.MemoryOrdering == NdMemoryOrdering::None &&
                   Stmt.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
          if (auto Slot = namedFrameSlot(*Stmt.StoreAddr)) {
            if (isParamCopy(*Stmt.StoreVal)) {
              if (auto Src = copyForwardSource(*Stmt.StoreVal)) {
                CopyForward[*Slot] = *Src;
                Analysis.DeadVars.insert(*Slot);
              } else {
                CopyForward.erase(*Slot);
              }
            } else {
              CopyForward.erase(*Slot);
            }
          }
        }
      }
      Walk(Stmt.Body, InHandler);
      Walk(Stmt.ElseBody, InHandler);
      for (const auto &C : Stmt.Cases)
        Walk(C.Body, InHandler);
      Walk(Stmt.DefaultBody, InHandler);
      for (const auto &ClauseBody : Stmt.EHClauseBodies)
        Walk(ClauseBody, true);
    }
    InEHClauseBody = Saved;
  };
  Walk(Func.Body, false);
}

void HighCWriter::collectGotoTargets(const std::vector<HighStmt> &Stmts) {
  for (auto &S : Stmts) {
    if (S.Kind == StmtKind::Goto && S.GotoTarget != 0 &&
        S.GotoTarget != InvalidVA)
      GotoTargets.insert(S.GotoTarget);
    collectGotoTargets(S.Body);
    collectGotoTargets(S.ElseBody);
    for (auto &C : S.Cases)
      collectGotoTargets(C.Body);
    collectGotoTargets(S.DefaultBody);
    for (auto &ClauseBody : S.EHClauseBodies)
      collectGotoTargets(ClauseBody);
  }
}

} // namespace neverd
