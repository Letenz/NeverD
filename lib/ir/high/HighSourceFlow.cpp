//===- HighSourceFlow.cpp - Source flow analysis
//---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/ir/high/HighSourceFlow.h"

#include "neverd/ir/TargetRegInfo.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace neverd {
HighSourceLocalIdentity highSourceLocalIdentity(const MedVar &Variable) {
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

bool highSourceFrameBase(const HighFunc &Function, const MedVar &Variable) {
  return Variable.Kind == MedVar::Reg && Variable.SSAVer == 0 &&
         Variable.RenameTag < 0 &&
         (Variable.TheArch == Arch::X64 || Variable.TheArch == Arch::AArch64) &&
         (Function.FrameSize > 0 || Function.FrameHeadroom > 0) &&
         Variable.RegOff == getTargetRegInfo(Variable.TheArch).StackPointer;
}

void HighSourceFlowReport::add(HighSourceFlowIssue Issue, std::string Reason,
                               va_t Address, const HighExpr *Expression,
                               va_t RelatedAddress) {
  constexpr size_t MaxDiagnostics = 4096;
  if (Items.size() < MaxDiagnostics)
    Items.push_back(
        {Issue, std::move(Reason), Address, Expression, RelatedAddress});
  else if (Items.size() == MaxDiagnostics) {
    Items.push_back(
        {HighSourceFlowIssue::Budget,
         "source projection evidence exceeds its diagnostic limit"});
    Complete = false;
  }
}
namespace {
/// This graph models emitted statement control flow, not the earlier native
/// CFG. In particular, source switch arms have implicit breaks, and For has
/// only a condition and body. Goto edges are resolved after all source labels
/// are known, including edges entering or leaving a structured region.
class SourceFlow {
  static constexpr size_t NoNode = std::numeric_limits<size_t>::max();
  static constexpr size_t MaxNodes = 100000;
  static constexpr size_t MaxEdges = 4 * MaxNodes;
  static constexpr size_t MaxSwitchCases = 4096;
  static constexpr size_t MaxLocals = 8192;
  static constexpr size_t MaxOperations = 10000000;
  static constexpr size_t MaxStateWords = 8 * 1024 * 1024; // 64 MiB.
  struct Failure {
    std::string Reason;
    HighSourceFlowIssue Issue;
    va_t Address;
  };
  struct Predicate {
    size_t Local;
    uint16_t Width;
    bool Nonzero;
  };
  struct Node {
    std::vector<size_t> Next, Previous, Uses, Writes;
    std::vector<std::optional<Predicate>> EdgeFacts;
    const HighStmt *Statement = nullptr;
    ExprPtr Test;
    bool PhiCopy = false;
    std::optional<size_t> Definition;
    bool Returns = false;
    va_t Address = 0;
    std::map<size_t, const HighExpr *> UseExpressions;
  };
  struct Control {
    size_t Break = NoNode, Continue = NoNode;
  };

  const HighFunc &Function;
  HighSourceFlowReport &Diagnostics;
  va_t CurrentAddress = 0;
  std::vector<Node> Nodes;
  std::map<HighSourceLocalIdentity, size_t> Locals;
  std::map<va_t, size_t> Labels;
  std::vector<std::pair<size_t, va_t>> Gotos;
  size_t Operations = 0, EdgeCount = 0, CaseCount = 0;
  std::set<size_t> AddressTaken;
  std::set<const HighStmt *> *DeadCopies;

  [[noreturn]] void
  fail(const char *Reason,
       HighSourceFlowIssue Issue = HighSourceFlowIssue::ControlFlow) {
    throw Failure{Reason, Issue, CurrentAddress};
  }
  void spend(size_t Count = 1) {
    if (Count > MaxOperations - Operations)
      fail("method source-flow analysis exceeds its work limit",
           HighSourceFlowIssue::Budget);
    Operations += Count;
  }
  size_t node() {
    spend();
    if (Nodes.size() == MaxNodes)
      fail("method source-flow graph exceeds its node limit",
           HighSourceFlowIssue::Budget);
    Nodes.emplace_back();
    return Nodes.size() - 1;
  }
  void edge(size_t From, size_t To,
            std::optional<Predicate> Fact = std::nullopt) {
    CurrentAddress = Nodes[From].Address;
    spend();
    if (To == NoNode)
      fail("method has a break or continue outside its source control scope");
    if (EdgeCount == MaxEdges)
      fail("method source-flow graph exceeds its edge limit",
           HighSourceFlowIssue::Budget);
    ++EdgeCount;
    Nodes[From].Next.push_back(To);
    Nodes[From].EdgeFacts.push_back(Fact);
    Nodes[To].Previous.push_back(From);
  }
  size_t local(const MedVar &Variable) {
    spend();
    const auto Identity = highSourceLocalIdentity(Variable);
    if (auto It = Locals.find(Identity); It != Locals.end())
      return It->second;
    if (Locals.size() == MaxLocals)
      fail("method source-flow graph exceeds its local-value limit",
           HighSourceFlowIssue::Budget);
    return Locals.emplace(Identity, Locals.size()).first->second;
  }
  bool entryValue(const MedVar &Variable) const {
    // The outer source validator still checks every parameter's exact ABI.
    return Variable.Kind == MedVar::Param ||
           highSourceFrameBase(Function, Variable);
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
             HighSourceFlowIssue::Budget);
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
      // Only a direct address-of-local can expose an emitted C local to a
      // memory write. Its identity must never carry a stable branch fact.
      if (Expression->Kind == ExprKind::Addr)
        for (const auto &Operand : Expression->Operands)
          if (Operand && (Operand->Kind == ExprKind::Var ||
                          Operand->Kind == ExprKind::Phi))
            AddressTaken.insert(local(Operand->Var));
      for (const auto &Output : Expression->IntrinsicOutputs)
        Nodes[Index].Writes.push_back(local(Output));
      spend(Expression->Operands.size());
      for (const auto &Operand : Expression->Operands) {
        if (!Operand)
          fail("method contains a missing expression operand",
               HighSourceFlowIssue::MalformedExpression);
        Pending.emplace_back(Operand.get(), Depth + 1);
      }
    }
  }
  static bool terminates(const ExprPtr &Expression) {
    if (isNonReturningSourceCall(Expression))
      return true;
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
  static bool scalarLocal(const ExprPtr &Expression) {
    if (!Expression ||
        (Expression->Kind != ExprKind::Var &&
         Expression->Kind != ExprKind::Phi) ||
        !Expression->Operands.empty() ||
        !Expression->IntrinsicOutputs.empty() ||
        Expression->MemoryOrdering != NdMemoryOrdering::None ||
        Expression->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        !Expression->Type ||
        (Expression->Type->Kind != NdTypeKind::Int &&
         Expression->Type->Kind != NdTypeKind::Ptr) ||
        Expression->Type->Size != Expression->Var.Size ||
        !Expression->Type->Size || Expression->Type->Size > 8)
      return false;
    // Stack storage can be addressed through the synthetic frame. A register,
    // temporary, or parameter denotes an ordinary emitted scalar local.
    return Expression->Var.Kind == MedVar::Reg ||
           Expression->Var.Kind == MedVar::Temp ||
           Expression->Var.Kind == MedVar::Param;
  }
  std::optional<Predicate> predicate(const ExprPtr &Expression) {
    ExprPtr Value = Expression;
    bool Nonzero = true;
    if (Value && Value->Kind == ExprKind::BinOp &&
        (Value->Op == NdOp::INT_EQUAL || Value->Op == NdOp::INT_NOTEQUAL) &&
        Value->Operands.size() == 2) {
      auto IsZero = [](const ExprPtr &E) {
        return E && E->Kind == ExprKind::Const && E->ConstVal == 0 && E->Type &&
               E->Type->Kind == NdTypeKind::Int && E->Operands.empty();
      };
      Nonzero = Value->Op == NdOp::INT_NOTEQUAL;
      if (IsZero(Value->Operands[1]))
        Value = Value->Operands[0];
      else if (IsZero(Value->Operands[0]))
        Value = Value->Operands[1];
      else
        return std::nullopt;
    }
    if (!scalarLocal(Value) || highSourceFrameBase(Function, Value->Var))
      return std::nullopt;
    return Predicate{local(Value->Var), Value->Type->Size, Nonzero};
  }
  void branch(size_t Index, const ExprPtr &Condition, size_t Yes, size_t No) {
    Nodes[Index].Test = Condition;
    reads(Index, Condition);
    auto Known = truth(Condition);
    auto Fact = predicate(Condition);
    if (!Known || *Known)
      edge(Index, Yes, Fact);
    if (Fact)
      Fact->Nonzero = !Fact->Nonzero;
    if (!Known || !*Known)
      edge(Index, No, Fact);
  }

  size_t block(const std::vector<HighStmt> &Body, size_t Next, Control Scope,
               unsigned Depth) {
    if (Depth > 200)
      fail("method control flow exceeds the source projection depth limit",
           HighSourceFlowIssue::Budget);
    for (auto It = Body.rbegin(); It != Body.rend(); ++It)
      Next = statement(*It, Next, Scope, Depth);
    return Next;
  }
  size_t statement(const HighStmt &Statement, size_t Next, Control Scope,
                   unsigned Depth) {
    CurrentAddress = Statement.Addr;
    const size_t Index = node();
    Nodes[Index].Address = Statement.Addr;
    Nodes[Index].Statement = &Statement;
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
        const size_t Written = local(Statement.Dst->Var);
        Nodes[Index].Writes.push_back(Written);
        if (!entryValue(Statement.Dst->Var))
          Nodes[Index].Definition = Written;
        Nodes[Index].PhiCopy =
            Statement.IsPhiCopy && Statement.Body.empty() &&
            Statement.ElseBody.empty() && Statement.Cases.empty() &&
            Statement.DefaultBody.empty() && scalarLocal(Statement.Dst) &&
            !entryValue(Statement.Dst->Var) &&
            (scalarLocal(Statement.Val) ||
             (Statement.Val->Kind == ExprKind::Const &&
              Statement.Val->Operands.empty() && Statement.Val->Type &&
              Statement.Val->Type->Kind == NdTypeKind::Int));
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
      Nodes[Index].Test = Statement.SwitchExpr;
      std::map<uint64_t, size_t> Cases;
      for (const auto &Case : Statement.Cases) {
        spend();
        if (CaseCount == MaxSwitchCases)
          fail("method source-flow graph exceeds its switch-case limit",
               HighSourceFlowIssue::Budget);
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
           HighSourceFlowIssue::Exception);
    default:
      fail("method source contains an unsupported statement kind");
    }
    return Index;
  }

  // Partition the emitted CFG by repeated zero/nonzero tests. Facts belong
  // to individual edges, including two edges with the same destination. Every
  // write kills the old fact before the next test. Inconsistent widths and
  // escaped locals remain unknown; no memory-value or alias guess is needed.
  size_t refine(size_t Entry) {
    struct Observations {
      size_t Count = 0;
      uint16_t Width = 0;
      bool Consistent = true;
    };
    std::map<size_t, Observations> Tests;
    for (const auto &N : Nodes) {
      if (N.EdgeFacts.empty() || !N.EdgeFacts[0])
        continue;
      const auto &Fact = *N.EdgeFacts[0];
      auto &Seen = Tests[Fact.Local];
      Seen.Consistent &= !Seen.Count || Seen.Width == Fact.Width;
      Seen.Width = Fact.Width;
      ++Seen.Count;
    }
    std::map<size_t, uint32_t> Masks;
    for (const auto &[Local, Seen] : Tests)
      if (Seen.Count > 1 && Seen.Consistent && !AddressTaken.count(Local) &&
          Masks.size() < 32)
        Masks.emplace(Local, uint32_t{1} << Masks.size());
    if (Masks.empty())
      return Entry;

    struct State {
      size_t Original;
      uint32_t Known, Nonzero;
    };
    std::vector<Node> Refined;
    std::vector<State> States;
    using Key = std::tuple<size_t, uint32_t, uint32_t>;
    std::map<Key, size_t> Indices;
    std::vector<unsigned> Counts(Nodes.size());
    size_t Units = 0, Edges = 0;
    const size_t Limit = std::min(MaxNodes, Nodes.size() * 4 + 256);
    struct ExpansionLimit {};
    auto Add = [&](size_t Original, uint32_t Known, uint32_t Nonzero) {
      // Function exit has no uses or successors and needs no partition.
      if (!Original)
        Known = Nonzero = 0;
      Key K{Original, Known, Nonzero};
      if (auto It = Indices.find(K); It != Indices.end())
        return It->second;
      const auto &N = Nodes[Original];
      Units += 1 + N.Uses.size() + N.Writes.size();
      if (Refined.size() == Limit || Counts[Original] == 32 || Units > 1000000)
        throw ExpansionLimit{};
      ++Counts[Original];
      const size_t Index = Refined.size();
      Indices.emplace(K, Index);
      Refined.push_back(N);
      Refined.back().Next.clear();
      Refined.back().Previous.clear();
      Refined.back().EdgeFacts.clear();
      States.push_back({Original, Known, Nonzero});
      return Index;
    };
    try {
      Add(0, 0, 0); // Keep the shared fallthrough exit at index zero.
      const size_t NewEntry = Add(Entry, 0, 0);
      for (size_t I = 1; I < States.size(); ++I) {
        const auto S = States[I];
        uint32_t Known = S.Known, Nonzero = S.Nonzero;
        const auto &Original = Nodes[S.Original];
        for (size_t Written : Original.Writes)
          if (auto It = Masks.find(Written); It != Masks.end()) {
            Known &= ~It->second;
            Nonzero &= ~It->second;
          }
        for (size_t E = 0; E < Original.Next.size(); ++E) {
          uint32_t NextKnown = Known, NextNonzero = Nonzero;
          if (auto Fact = Original.EdgeFacts[E])
            if (auto It = Masks.find(Fact->Local); It != Masks.end()) {
              const uint32_t Mask = It->second;
              if ((Known & Mask) && bool(Nonzero & Mask) != Fact->Nonzero)
                continue;
              NextKnown |= Mask;
              if (Fact->Nonzero)
                NextNonzero |= Mask;
              else
                NextNonzero &= ~Mask;
            }
          if (++Edges > MaxEdges)
            throw ExpansionLimit{};
          const size_t Target = Add(Original.Next[E], NextKnown, NextNonzero);
          Refined[I].Next.push_back(Target);
          Refined[Target].Previous.push_back(I);
        }
      }
      const size_t Words = (Locals.size() + 63) / 64;
      if (Words && Refined.size() > MaxStateWords / Words)
        return Entry;
      Nodes = std::move(Refined);
      return NewEntry;
    } catch (const ExpansionLimit &) {
      // Precision is optional. Discard the whole speculative graph and retain
      // the original conservative analysis when a partition budget is reached.
      return Entry;
    }
  }

  void deadPhiCopies(const std::vector<bool> &Reachable, size_t Words) {
    std::map<const HighStmt *, std::vector<size_t>> Candidates;
    for (size_t I = 0; I < Nodes.size(); ++I)
      if (Reachable[I] && Nodes[I].PhiCopy && Nodes[I].Definition &&
          !AddressTaken.count(*Nodes[I].Definition))
        Candidates[Nodes[I].Statement].push_back(I);
    while (!Candidates.empty()) {
      spend(Nodes.size() * Words);
      std::vector<uint64_t> Live(Nodes.size() * Words, 0);
      std::vector<uint64_t> Incoming(Words);
      std::vector<size_t> Pending;
      std::vector<bool> Queued = Reachable;
      for (size_t I = 0; I < Nodes.size(); ++I)
        if (Reachable[I])
          Pending.push_back(I);
      auto Outputs = [&](size_t Index) {
        std::fill(Incoming.begin(), Incoming.end(), 0);
        for (size_t Next : Nodes[Index].Next) {
          spend(Words + 1);
          for (size_t W = 0; W < Words; ++W)
            Incoming[W] |= Live[Next * Words + W];
        }
      };
      while (!Pending.empty()) {
        spend();
        const size_t Index = Pending.back();
        Pending.pop_back();
        Queued[Index] = false;
        Outputs(Index);
        if (auto Definition = Nodes[Index].Definition)
          Incoming[*Definition / 64] &= ~(uint64_t{1} << (*Definition % 64));
        for (size_t Use : Nodes[Index].Uses) {
          spend();
          Incoming[Use / 64] |= uint64_t{1} << (Use % 64);
        }
        bool Changed = false;
        spend(Words);
        for (size_t W = 0; W < Words; ++W) {
          auto &State = Live[Index * Words + W];
          Changed |= State != Incoming[W];
          State = Incoming[W];
        }
        if (Changed)
          for (size_t Previous : Nodes[Index].Previous) {
            spend();
            if (Reachable[Previous] && !Queued[Previous]) {
              Queued[Previous] = true;
              Pending.push_back(Previous);
            }
          }
      }
      bool Changed = false;
      for (auto It = Candidates.begin(); It != Candidates.end();) {
        bool Dead = true;
        for (size_t Index : It->second) {
          Outputs(Index);
          const size_t Definition = *Nodes[Index].Definition;
          Dead &=
              !(Incoming[Definition / 64] & (uint64_t{1} << (Definition % 64)));
        }
        if (!Dead) {
          ++It;
          continue;
        }
        DeadCopies->insert(It->first);
        for (size_t Index : It->second) {
          Nodes[Index].Uses.clear();
          Nodes[Index].Definition.reset();
        }
        It = Candidates.erase(It);
        Changed = true;
      }
      if (!Changed)
        return;
      // A statement is erased only if dead in EVERY feasible context. Keeping
      // a read in one context must keep its dependencies in all contexts until
      // that entire source statement can be removed.
    }
  }

  std::optional<size_t> build() {
    node(); // Node zero is the emitted function's fallthrough exit.
    size_t Entry = block(Function.Body, 0, {}, 1);
    CurrentAddress = 0;
    bool MissingTarget = false;
    for (const auto &[Index, Address] : Gotos) {
      auto Target = Labels.find(Address);
      if (!Address || Address == InvalidVA || Target == Labels.end() ||
          Target->second == NoNode) {
        MissingTarget = true;
        Diagnostics.Complete = false;
        Diagnostics.add(HighSourceFlowIssue::ControlFlow,
                        "method source goto has no unique emitted target",
                        Nodes[Index].Address, nullptr, Address);
      } else {
        edge(Index, Target->second);
      }
    }
    // Unknown edges invalidate reachability and must-defined conclusions.
    // The outer validator can still inventory independent expressions.
    if (MissingTarget)
      return std::nullopt;
    return refine(Entry);
  }

  void analyze(bool NeedsReturn) {
    const auto Built = build();
    if (!Built)
      return;
    const size_t Entry = *Built;
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
          HighSourceFlowIssue::ControlFlow,
          "method source has a reachable fallthrough exit without a return");
    for (size_t Index : Pending)
      if (Function.DoesNotReturn && Nodes[Index].Returns)
        Diagnostics.add(
            HighSourceFlowIssue::ControlFlow,
            "method source returns despite its noreturn declaration",
            Nodes[Index].Address);

    // Must-defined is a greatest fixed point: entry starts empty, all other
    // states start at top, and predecessor intersections only remove facts.
    // No facts are imported from unreachable code or a skipped loop body.
    const size_t Words = (Locals.size() + 63) / 64;
    CurrentAddress = 0;
    if (Words && Nodes.size() > MaxStateWords / Words)
      fail("method source-flow analysis exceeds its state memory limit",
           HighSourceFlowIssue::Budget);
    if (DeadCopies) {
      deadPhiCopies(Reachable, Words);
      return;
    }
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
    std::set<std::pair<const HighStmt *, size_t>> Reported;
    for (size_t Index = 0; Index < Nodes.size(); ++Index) {
      spend();
      if (!Reachable[Index])
        continue;
      inputs(Index);
      for (size_t Use : Nodes[Index].Uses) {
        spend();
        if (!(Incoming[Use / 64] & (uint64_t{1} << (Use % 64))) &&
            Reported.emplace(Nodes[Index].Statement, Use).second)
          Diagnostics.add(
              HighSourceFlowIssue::DefiniteAssignment,
              "method reads a local value before it is defined on every "
              "reaching source path",
              Nodes[Index].Address, Nodes[Index].UseExpressions.at(Use));
      }
    }
  }

public:
  SourceFlow(const HighFunc &Function, HighSourceFlowReport &Diagnostics,
             std::set<const HighStmt *> *DeadCopies = nullptr)
      : Function(Function), Diagnostics(Diagnostics), DeadCopies(DeadCopies) {}

  void graph(HighSourceFlowGraph &Result) {
    try {
      const auto Entry = build();
      if (!Entry)
        return;
      Result.Entry = *Entry;
      Result.Nodes.reserve(Nodes.size());
      for (auto &N : Nodes)
        Result.Nodes.push_back({N.Statement, N.Test, std::move(N.Next)});
    } catch (const Failure &Error) {
      Diagnostics.Complete = false;
      Diagnostics.add(Error.Issue, Error.Reason, Error.Address);
    }
  }

  void collect(bool NeedsReturn) {
    try {
      analyze(NeedsReturn);
    } catch (const Failure &Error) {
      Diagnostics.Complete = false;
      Diagnostics.add(Error.Issue, Error.Reason, Error.Address);
    }
  }
};

} // namespace
HighSourceFlowGraph buildHighSourceFlowGraph(const HighFunc &Function) {
  HighSourceFlowGraph Result;
  SourceFlow(Function, Result.Diagnostics).graph(Result);
  return Result;
}
HighSourceFlowReport analyzeHighSourceFlow(const HighFunc &Function,
                                           bool NeedsReturn) {
  HighSourceFlowReport Result;
  SourceFlow(Function, Result).collect(NeedsReturn);
  return Result;
}
bool eliminateHighDeadPhiCopies(HighFunc &Function) {
  std::vector<HighStmt *> Statements;
  std::vector<std::pair<HighStmt *, unsigned>> Pending;
  for (auto &S : Function.Body)
    Pending.emplace_back(&S, 1);
  bool HasPhiCopy = false;
  while (!Pending.empty()) {
    const auto [S, Depth] = Pending.back();
    Pending.pop_back();
    if (Depth > 200 || Statements.size() == 100000)
      return false;
    Statements.push_back(S);
    HasPhiCopy |= S->Kind == StmtKind::Assign && S->IsPhiCopy;
    auto Append = [&](auto &Body) {
      for (auto &Child : Body)
        Pending.emplace_back(&Child, Depth + 1);
    };
    Append(S->Body);
    Append(S->ElseBody);
    Append(S->DefaultBody);
    for (auto &Case : S->Cases)
      Append(Case.Body);
  }
  if (!HasPhiCopy)
    return false;
  HighSourceFlowReport Report;
  std::set<const HighStmt *> Dead;
  SourceFlow(Function, Report, &Dead).collect(false);
  if (!Report.Complete || Dead.empty())
    return false;
  for (auto *S : Statements)
    if (Dead.count(S)) {
      const va_t Address = S->Addr;
      *S = HighStmt{};
      S->Kind = StmtKind::Nop;
      S->Addr = Address; // A goto may still target this source label.
    }
  return true;
}
} // namespace neverd
