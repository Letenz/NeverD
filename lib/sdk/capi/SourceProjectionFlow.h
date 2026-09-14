//===- SourceProjectionFlow.h - Source control-flow completeness ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_SOURCEPROJECTIONFLOW_H
#define NEVERD_SDK_CAPI_SOURCEPROJECTIONFLOW_H

#include "SourceProjectionDiagnostics.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd::sdk::objc_projection_detail {

using LocalIdentity = std::tuple<int, int, int, int64_t>;

inline LocalIdentity localIdentity(const MedVar &Variable) {
  // Phi cleanup can merge different SSA values/kinds into one emitted local.
  // Unrenamed kinds and SSA values must not borrow each other's definitions.
  if (Variable.RenameTag >= 0)
    return {-1, Variable.RenameTag, 0, 0};
  if (Variable.Kind == MedVar::Stack)
    return {MedVar::Stack, 0, 0, Variable.StackOff};
  if (Variable.Kind == MedVar::RetVal)
    return {MedVar::RetVal, 0, 0, 0};
  return {Variable.Kind, Variable.Id, Variable.SSAVer, 0};
}

inline bool sourceFrameBase(const HighFunc &Function, const MedVar &Variable) {
  return Variable.Kind == MedVar::Reg && Variable.SSAVer == 0 &&
         Variable.RenameTag < 0 &&
         (Variable.TheArch == Arch::X64 || Variable.TheArch == Arch::AArch64) &&
         (Function.FrameSize > 0 || Function.FrameHeadroom > 0) &&
         Variable.RegOff == getTargetRegInfo(Variable.TheArch).StackPointer;
}

/// This graph models emitted statement control flow, not the earlier native
/// CFG. In particular, source switch arms have implicit breaks, and For has
/// only a condition and body. Goto edges are resolved after all source labels
/// are known, including edges entering or leaving a structured region.
class SourceProjectionFlow {
  static constexpr size_t NoNode = std::numeric_limits<size_t>::max();
  static constexpr size_t MaxNodes = 100000;
  static constexpr size_t MaxEdges = 4 * MaxNodes;
  static constexpr size_t MaxSwitchCases = 4096;
  static constexpr size_t MaxLocals = 8192;
  static constexpr size_t MaxOperations = 10000000;
  static constexpr size_t MaxStateWords = 8 * 1024 * 1024; // 64 MiB.
  struct Failure {
    std::string Reason;
    SourceProjectionIssue Issue;
    va_t Address;
  };
  struct Node {
    std::vector<size_t> Next, Previous, Uses;
    std::optional<size_t> Definition;
    bool Returns = false;
    va_t Address = 0;
    std::map<size_t, const HighExpr *> UseExpressions;
  };
  struct Control {
    size_t Break = NoNode, Continue = NoNode;
  };

  const HighFunc &Function;
  SourceProjectionDiagnostics &Diagnostics;
  va_t CurrentAddress = 0;
  std::vector<Node> Nodes;
  std::map<LocalIdentity, size_t> Locals;
  std::map<va_t, size_t> Labels;
  std::vector<std::pair<size_t, va_t>> Gotos;
  size_t Operations = 0, EdgeCount = 0, CaseCount = 0;

  [[noreturn]] void
  fail(const char *Reason,
       SourceProjectionIssue Issue = SourceProjectionIssue::ControlFlow) {
    throw Failure{Reason, Issue, CurrentAddress};
  }
  void spend(size_t Count = 1) {
    if (Count > MaxOperations - Operations)
      fail("method source-flow analysis exceeds its work limit",
           SourceProjectionIssue::Budget);
    Operations += Count;
  }
  size_t node() {
    spend();
    if (Nodes.size() == MaxNodes)
      fail("method source-flow graph exceeds its node limit",
           SourceProjectionIssue::Budget);
    Nodes.emplace_back();
    return Nodes.size() - 1;
  }
  void edge(size_t From, size_t To) {
    CurrentAddress = Nodes[From].Address;
    spend();
    if (To == NoNode)
      fail("method has a break or continue outside its source control scope");
    if (EdgeCount == MaxEdges)
      fail("method source-flow graph exceeds its edge limit",
           SourceProjectionIssue::Budget);
    ++EdgeCount;
    Nodes[From].Next.push_back(To);
    Nodes[To].Previous.push_back(From);
  }
  size_t local(const MedVar &Variable) {
    spend();
    const auto Identity = localIdentity(Variable);
    if (auto It = Locals.find(Identity); It != Locals.end())
      return It->second;
    if (Locals.size() == MaxLocals)
      fail("method source-flow graph exceeds its local-value limit",
           SourceProjectionIssue::Budget);
    return Locals.emplace(Identity, Locals.size()).first->second;
  }
  bool entryValue(const MedVar &Variable) const {
    // The outer source validator still checks every parameter's exact ABI.
    return Variable.Kind == MedVar::Param ||
           sourceFrameBase(Function, Variable);
  }
  void reads(size_t Index, const ExprPtr &Root) {
    CurrentAddress = Nodes[Index].Address;
    if (!Root)
      return;
    std::vector<std::pair<const HighExpr *, unsigned>> Pending{{Root.get(), 1}};
    std::map<const HighExpr *, unsigned> Seen;
    while (!Pending.empty()) {
      spend();
      auto [Expression, Depth] = Pending.back();
      Pending.pop_back();
      if (Depth > 200)
        fail("method expression exceeds the source projection depth limit",
             SourceProjectionIssue::Budget);
      auto [It, Fresh] = Seen.emplace(Expression, Depth);
      if (!Fresh && It->second >= Depth)
        continue;
      It->second = Depth;
      if ((Expression->Kind == ExprKind::Var ||
           Expression->Kind == ExprKind::Phi) &&
          !entryValue(Expression->Var)) {
        const size_t Local = local(Expression->Var);
        Nodes[Index].Uses.push_back(Local);
        Nodes[Index].UseExpressions.emplace(Local, Expression);
      }
      spend(Expression->Operands.size());
      for (const auto &Operand : Expression->Operands) {
        if (!Operand)
          fail("method contains a missing expression operand",
               SourceProjectionIssue::MalformedExpression);
        Pending.emplace_back(Operand.get(), Depth + 1);
      }
    }
  }
  static bool terminates(const ExprPtr &Expression) {
    if (!Expression || Expression->Kind != ExprKind::Call ||
        Expression->IsIndirectCall || Expression->SourceCallHint ||
        !Expression->Operands.empty() ||
        !Expression->IntrinsicOutputs.empty() ||
        Expression->MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return false;
    // These actual source intrinsics unconditionally trap. A native function
    // flag, an arbitrary call spelling, and resumable debug traps do not prove
    // source termination. Arguments are still checked as ordinary reads.
    return isUnconditionalTrapIntrinsic(Expression->IntrinsicId);
  }
  static std::optional<bool> truth(const ExprPtr &Expression) {
    if (!Expression)
      return true; // The C emitter renders a missing loop test as true.
    if (Expression->Kind != ExprKind::Const || !Expression->Type ||
        Expression->Type->Kind != NdTypeKind::Int)
      return std::nullopt;
    return Expression->ConstVal != 0;
  }
  void branch(size_t Index, const ExprPtr &Condition, size_t Yes, size_t No) {
    reads(Index, Condition);
    auto Known = truth(Condition);
    if (!Known || *Known)
      edge(Index, Yes);
    if (!Known || !*Known)
      edge(Index, No);
  }

  size_t block(const std::vector<HighStmt> &Body, size_t Next, Control Scope,
               unsigned Depth) {
    if (Depth > 200)
      fail("method control flow exceeds the source projection depth limit",
           SourceProjectionIssue::Budget);
    for (auto It = Body.rbegin(); It != Body.rend(); ++It)
      Next = statement(*It, Next, Scope, Depth);
    return Next;
  }
  size_t statement(const HighStmt &Statement, size_t Next, Control Scope,
                   unsigned Depth) {
    CurrentAddress = Statement.Addr;
    const size_t Index = node();
    Nodes[Index].Address = Statement.Addr;
    if (Statement.Addr && Statement.Addr != InvalidVA) {
      auto [It, Fresh] = Labels.emplace(Statement.Addr, Index);
      if (!Fresh)
        It->second = NoNode; // An emitted goto label must be unique.
    }
    switch (Statement.Kind) {
    case StmtKind::Assign:
      if (!Statement.Dst || !Statement.Val)
        fail("method source assignment is incomplete");
      reads(Index, Statement.Val);
      if (Statement.Dst->Kind == ExprKind::Var ||
          Statement.Dst->Kind == ExprKind::Phi) {
        if (!entryValue(Statement.Dst->Var))
          Nodes[Index].Definition = local(Statement.Dst->Var);
      } else {
        reads(Index, Statement.Dst);
      }
      if (!terminates(Statement.Val))
        edge(Index, Next);
      break;
    case StmtKind::ExprStmt:
      reads(Index, Statement.Val);
      if (!terminates(Statement.Val))
        edge(Index, Next);
      break;
    case StmtKind::Store:
      if (!Statement.StoreAddr || !Statement.StoreVal)
        fail("method source store is incomplete");
      reads(Index, Statement.StoreAddr);
      reads(Index, Statement.StoreVal);
      edge(Index, Next);
      break;
    case StmtKind::Call:
      if (!Statement.CallExpr)
        fail("method source call is incomplete");
      reads(Index, Statement.CallExpr);
      if (!terminates(Statement.CallExpr))
        edge(Index, Next);
      break;
    case StmtKind::Return:
      reads(Index, Statement.RetVal);
      Nodes[Index].Returns = true;
      break;
    case StmtKind::Nop:
      edge(Index, Next);
      break;
    case StmtKind::Block:
      edge(Index, block(Statement.Body, Next, Scope, Depth + 1));
      break;
    case StmtKind::If:
    case StmtKind::IfElse: {
      if (!Statement.Cond ||
          (Statement.Kind == StmtKind::If && !Statement.ElseBody.empty()))
        fail("method source conditional has no complete branch structure");
      const size_t Yes = block(Statement.Body, Next, Scope, Depth + 1);
      const size_t No = Statement.Kind == StmtKind::IfElse
                            ? block(Statement.ElseBody, Next, Scope, Depth + 1)
                            : Next;
      branch(Index, Statement.Cond, Yes, No);
      break;
    }
    case StmtKind::While:
    case StmtKind::For: {
      const size_t Body =
          block(Statement.Body, Index, {Next, Index}, Depth + 1);
      branch(Index, Statement.Cond, Body, Next);
      break;
    }
    case StmtKind::DoWhile: {
      // A goto to the do statement enters its body, whereas continue and the
      // normal back edge evaluate its trailing condition first.
      const size_t Test = node();
      Nodes[Test].Address = Statement.Addr;
      const size_t Body = block(Statement.Body, Test, {Next, Test}, Depth + 1);
      edge(Index, Body);
      branch(Test, Statement.Cond, Body, Next);
      break;
    }
    case StmtKind::Switch: {
      if (!Statement.SwitchExpr)
        fail("method source switch has no recovered selector");
      reads(Index, Statement.SwitchExpr);
      std::map<uint64_t, size_t> Cases;
      for (const auto &Case : Statement.Cases) {
        spend();
        if (CaseCount == MaxSwitchCases)
          fail("method source-flow graph exceeds its switch-case limit",
               SourceProjectionIssue::Budget);
        ++CaseCount;
        const size_t Body =
            block(Case.Body, Next, {Next, Scope.Continue}, Depth + 1);
        if (!Cases.emplace(Case.Value, Body).second)
          fail("method source switch has duplicate case values");
      }
      const size_t Default =
          block(Statement.DefaultBody, Next, {Next, Scope.Continue}, Depth + 1);
      if (Statement.SwitchExpr->Kind == ExprKind::Const) {
        auto It = Cases.find(Statement.SwitchExpr->ConstVal);
        edge(Index, It == Cases.end() ? Default : It->second);
      } else {
        for (const auto &[Value, Body] : Cases)
          edge(Index, Body);
        edge(Index, Default);
      }
      break;
    }
    case StmtKind::Goto:
      Gotos.emplace_back(Index, Statement.GotoTarget);
      break;
    case StmtKind::Break:
      edge(Index, Scope.Break);
      break;
    case StmtKind::Continue:
      edge(Index, Scope.Continue);
      break;
    case StmtKind::SEHTry:
    case StmtKind::CxxTry:
    case StmtKind::ItaniumTry:
      fail("exception-dependent method projection is not supported",
           SourceProjectionIssue::Exception);
    default:
      fail("method source contains an unsupported statement kind");
    }
    return Index;
  }

  void analyze(bool NeedsReturn) {
    node(); // Node zero is the emitted function's fallthrough exit.
    const size_t Entry = block(Function.Body, 0, {}, 1);
    CurrentAddress = 0;
    bool MissingTarget = false;
    for (const auto &[Index, Address] : Gotos) {
      auto Target = Labels.find(Address);
      if (!Address || Address == InvalidVA || Target == Labels.end() ||
          Target->second == NoNode) {
        MissingTarget = true;
        Diagnostics.Complete = false;
        Diagnostics.add(SourceProjectionIssue::ControlFlow,
                        "method source goto has no unique emitted target",
                        Nodes[Index].Address, nullptr, Address);
      } else {
        edge(Index, Target->second);
      }
    }
    // Unknown edges invalidate reachability and must-defined conclusions.
    // The outer validator can still inventory independent expressions.
    if (MissingTarget)
      return;
    std::vector<bool> Reachable(Nodes.size());
    std::vector<size_t> Pending{Entry};
    Reachable[Entry] = true;
    for (size_t I = 0; I < Pending.size(); ++I) {
      spend();
      const size_t Index = Pending[I];
      for (size_t Successor : Nodes[Index].Next) {
        spend();
        if (!Reachable[Successor]) {
          Reachable[Successor] = true;
          Pending.push_back(Successor);
        }
      }
    }
    if (Reachable[0] && (NeedsReturn || Function.DoesNotReturn))
      Diagnostics.add(
          SourceProjectionIssue::ControlFlow,
          "method source has a reachable fallthrough exit without a return");
    for (size_t Index : Pending)
      if (Function.DoesNotReturn && Nodes[Index].Returns)
        Diagnostics.add(
            SourceProjectionIssue::ControlFlow,
            "method source returns despite its noreturn declaration",
            Nodes[Index].Address);

    // Must-defined is a greatest fixed point: entry starts empty, all other
    // states start at top, and predecessor intersections only remove facts.
    // No facts are imported from unreachable code or a skipped loop body.
    const size_t Words = (Locals.size() + 63) / 64;
    CurrentAddress = 0;
    if (Words && Nodes.size() > MaxStateWords / Words)
      fail("method source-flow analysis exceeds its state memory limit",
           SourceProjectionIssue::Budget);
    spend(Nodes.size() * Words);
    std::vector<uint64_t> States(Nodes.size() * Words, ~uint64_t{0});
    std::vector<uint64_t> Incoming(Words);
    std::vector<bool> Queued = Reachable;
    auto inputs = [&](size_t Index) {
      CurrentAddress = Nodes[Index].Address;
      std::fill(Incoming.begin(), Incoming.end(),
                Index == Entry ? 0 : ~uint64_t{0});
      for (size_t Predecessor : Nodes[Index].Previous) {
        spend();
        if (!Reachable[Predecessor])
          continue;
        spend(Words);
        for (size_t Word = 0; Word < Words; ++Word)
          Incoming[Word] &= States[Predecessor * Words + Word];
      }
    };
    while (!Pending.empty()) {
      spend();
      const size_t Index = Pending.back();
      Pending.pop_back();
      Queued[Index] = false;
      inputs(Index);
      if (auto Definition = Nodes[Index].Definition)
        Incoming[*Definition / 64] |= uint64_t{1} << (*Definition % 64);
      bool Changed = false;
      spend(Words);
      for (size_t Word = 0; Word < Words; ++Word) {
        auto &State = States[Index * Words + Word];
        Changed |= State != Incoming[Word];
        State = Incoming[Word];
      }
      if (Changed)
        for (size_t Successor : Nodes[Index].Next) {
          spend();
          if (!Queued[Successor]) {
            Queued[Successor] = true;
            Pending.push_back(Successor);
          }
        }
    }
    for (size_t Index = 0; Index < Nodes.size(); ++Index) {
      spend();
      if (!Reachable[Index])
        continue;
      inputs(Index);
      for (size_t Use : Nodes[Index].Uses) {
        spend();
        if (!(Incoming[Use / 64] & (uint64_t{1} << (Use % 64))))
          Diagnostics.add(
              SourceProjectionIssue::DefiniteAssignment,
              "method reads a local value before it is defined on every "
              "reaching source path",
              Nodes[Index].Address, Nodes[Index].UseExpressions.at(Use));
      }
    }
  }

public:
  SourceProjectionFlow(const HighFunc &Function,
                       SourceProjectionDiagnostics &Diagnostics)
      : Function(Function), Diagnostics(Diagnostics) {}

  void collect(bool NeedsReturn) {
    try {
      analyze(NeedsReturn);
    } catch (const Failure &Error) {
      Diagnostics.Complete = false;
      Diagnostics.add(Error.Issue, Error.Reason, Error.Address);
    }
  }
};

} // namespace neverd::sdk::objc_projection_detail

#endif
