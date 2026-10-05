//===- HighFlowOracle.cpp - Structured flow versus MedIR edges ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Every statement keeps the address of the MedIR block it came from, so the
/// block pairs that structured flow runs through can be checked against the
/// MedIR edges: a pair no edge joins is a path the structuring invented, an
/// edge it never takes is a path it lost.  PHI copies, statements without an
/// address, jumps and empty label anchors are transparent: they belong to
/// whichever edge passes through them.  A statement that does not return, such
/// as `int 0x29` or a call that ends a MedIR block, ends its path.  A lost PHI
/// copy is not detected.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/high/HighFlowOracle.h"

#include "neverd/libc/LibCNames.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace neverd {
namespace {

/// Successors of every statement of a structured body.  Node 0 is the
/// function exit; a jump reaches the first statement that starts a label at
/// its target.
struct FlowGraph {
  struct Node {
    const HighStmt *Statement = nullptr;
    std::vector<size_t> Successors;
  };
  std::vector<Node> Nodes;
  size_t Entry = 0;
  bool Complete = true;

  FlowGraph(const std::vector<HighStmt> &Body,
            std::function<bool(const HighStmt &)> EndsPath)
      : EndsPath(std::move(EndsPath)) {
    Nodes.emplace_back();
    number(Body, 0);
    Entry = buildList(Body, 0, 0, 0);
  }

private:
  std::function<bool(const HighStmt &)> EndsPath;
  std::map<const HighStmt *, size_t> NodeOf;
  std::map<va_t, size_t> LabelAt;

  void number(const std::vector<HighStmt> &L, va_t Parent) {
    for (size_t I = 0; I < L.size(); ++I) {
      const HighStmt &S = L[I];
      const size_t N = Nodes.size();
      NodeOf[&S] = N;
      Nodes.push_back({&S, {}});
      if (S.Addr && S.Addr != InvalidVA &&
          S.Addr != (I == 0 ? Parent : L[I - 1].Addr))
        LabelAt.emplace(S.Addr, N);
      number(S.Body, S.Addr);
      number(S.ElseBody, 0);
      for (const SwitchCase &C : S.Cases)
        number(C.Body, 0);
      number(S.DefaultBody, 0);
      for (const auto &Clause : S.EHClauseBodies)
        number(Clause, 0);
    }
  }

  size_t buildList(const std::vector<HighStmt> &L, size_t Next, size_t Brk,
                   size_t Cnt) {
    for (size_t I = L.size(); I-- > 0;)
      Next = buildStmt(L[I], Next, Brk, Cnt);
    return Next;
  }

  /// Link \p S, which continues at \p Next; returns where it is entered.
  size_t buildStmt(const HighStmt &S, size_t Next, size_t Brk, size_t Cnt) {
    const size_t N = NodeOf[&S];
    std::vector<size_t> Succ;
    const bool Forever =
        !S.Cond || (S.Cond->Kind == ExprKind::Const && S.Cond->ConstVal != 0);
    size_t Entry = N;
    switch (S.Kind) {
    case StmtKind::Goto:
      if (auto It = LabelAt.find(S.GotoTarget); It != LabelAt.end())
        Succ.push_back(It->second);
      else
        Complete = false;
      break;
    case StmtKind::Return:
      Succ.push_back(0);
      break;
    case StmtKind::Break:
      Succ.push_back(Brk);
      break;
    case StmtKind::Continue:
      Succ.push_back(Cnt);
      break;
    case StmtKind::If:
      Succ = {buildList(S.Body, Next, Brk, Cnt), Next};
      break;
    case StmtKind::IfElse:
      Succ = {buildList(S.Body, Next, Brk, Cnt),
              buildList(S.ElseBody, Next, Brk, Cnt)};
      break;
    case StmtKind::While:
    case StmtKind::For:
      Succ.push_back(buildList(S.Body, N, Next, N));
      if (!Forever)
        Succ.push_back(Next);
      break;
    case StmtKind::DoWhile:
      Entry = buildList(S.Body, N, Next, N);
      Succ = {Entry, Next};
      break;
    case StmtKind::Switch: {
      size_t Following = Next;
      for (size_t I = S.Cases.size(); I-- > 0;) {
        const SwitchCase &C = S.Cases[I];
        Following =
            buildList(C.Body, C.FallsThrough ? Following : Next, Next, Cnt);
        Succ.push_back(Following);
      }
      Succ.push_back(S.DefaultBody.empty()
                         ? Next
                         : buildList(S.DefaultBody, Next, Next, Cnt));
      break;
    }
    default:
      if (EndsPath(S)) {
        Succ.push_back(0);
      } else if (!S.Body.empty() || !S.EHClauseBodies.empty()) {
        Succ.push_back(buildList(S.Body, Next, Brk, Cnt));
        for (const auto &Clause : S.EHClauseBodies)
          buildList(Clause, Next, Brk, Cnt);
      } else {
        Succ.push_back(Next);
      }
      break;
    }
    Nodes[N].Successors = std::move(Succ);
    return Entry;
  }
};

} // namespace

void reportHighFlowOracle(const HighFunc &Func, const MedFunc &Med,
                          llvm::StringRef Stage) {
  const char *Mode = std::getenv("NEVERD_HIGH_FLOW_ORACLE");
  if (!Mode)
    return;
  const bool Detail = Mode[0] == '2';
  const std::string StageName = Stage.str();
  const auto Entry = static_cast<unsigned long long>(Med.Entry);

  std::map<va_t, int> Owner;
  std::map<int, size_t> Index;
  for (size_t I = 0; I < Med.Blocks.size(); ++I) {
    const MedBlock &B = Med.Blocks[I];
    Index[B.Id] = I;
    if (B.StartAddr)
      Owner.emplace(B.StartAddr, B.Id);
    for (const MedOp &Op : B.Ops)
      if (Op.Addr)
        Owner.emplace(Op.Addr, B.Id);
  }
  std::set<std::pair<int, int>> Edges;
  for (const MedBlock &B : Med.Blocks)
    for (int S : B.Succs)
      Edges.insert({B.Id, S});

  // A call that ends a block with no successors does not return, and a copy
  // of it elsewhere, often without an address, ends its path as well; so
  // does a call to a routine known never to return.
  auto CallOf = [](const HighStmt &S) -> const ExprPtr * {
    if (S.Kind != StmtKind::Call && S.Kind != StmtKind::Assign &&
        S.Kind != StmtKind::ExprStmt)
      return nullptr;
    const ExprPtr &E = S.Kind == StmtKind::Call ? S.CallExpr : S.Val;
    return E && E->Kind == ExprKind::Call ? &E : nullptr;
  };
  std::set<va_t> FinalCalls;
  for (const MedBlock &B : Med.Blocks) {
    if (!B.Succs.empty())
      continue;
    auto Last = std::find_if(B.Ops.rbegin(), B.Ops.rend(),
                             [](const MedOp &Op) { return !Op.Dead; });
    if (Last != B.Ops.rend() &&
        (Last->Opcode == NdOp::CALL || Last->Opcode == NdOp::INDIR_CALL))
      FinalCalls.insert(Last->Addr);
  }
  std::set<std::string> NoReturn;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (const ExprPtr *Call = CallOf(S);
        Call && FinalCalls.count(S.Addr) && !(*Call)->CallTarget.empty())
      NoReturn.insert((*Call)->CallTarget);
  });
  const FlowGraph G(Func.Body, [&](const HighStmt &S) {
    const ExprPtr *Call = CallOf(S);
    if (!Call)
      return false;
    const std::string &Callee = (*Call)->CallTarget;
    return isTerminatingHighCall(*Call) || (*Call)->DoesNotReturn ||
           (!Callee.empty() && (NoReturn.count(Callee) ||
                                ((*Call)->IntrinsicId == Intrinsic::None &&
                                 libc::isNoReturnFunction(Callee))));
  });
  if (!G.Complete) {
    std::fprintf(stderr, "FLOWORACLE stage=%s entry=%llx incomplete\n",
                 StageName.c_str(), Entry);
    return;
  }

  // Jumps and empty label anchors run nothing of a block of their own, and
  // a loop test may run at the loop's latch while carrying one address.  An
  // if tests in the block of its branch: when it merges the tests of several
  // blocks (`a || b`) it names the first, and the others keep no statement,
  // which `Reaches` crosses.  Counting it keeps a block that is only a test
  // in the check, so that an edge into it can be found missing.
  auto BlockOf = [&](size_t N) -> int {
    const HighStmt *S = G.Nodes[N].Statement;
    if (!S || S->IsPhiCopy || !S->Addr || S->Addr == InvalidVA)
      return -1;
    switch (S->Kind) {
    case StmtKind::If:
    case StmtKind::IfElse:
      if (!S->Cond)
        return -1;
      break;
    case StmtKind::Goto:
    case StmtKind::Break:
    case StmtKind::Continue:
    case StmtKind::Nop:
    case StmtKind::While:
    case StmtKind::DoWhile:
    case StmtKind::For:
    case StmtKind::Switch:
      return -1;
    case StmtKind::Block:
      if (S->Body.empty())
        return -1;
      break;
    default:
      break;
    }
    auto It = Owner.find(S->Addr);
    return It == Owner.end() ? -1 : It->second;
  };
  const size_t Count = G.Nodes.size();

  std::set<int> Present;
  for (size_t N = 0; N < Count; ++N)
    if (const int B = BlockOf(N); B >= 0)
      Present.insert(B);
  std::set<std::pair<int, int>> Pairs;
  std::set<int> ExitFrom;
  for (size_t N = 0; N < Count; ++N) {
    const int A = BlockOf(N);
    if (A < 0)
      continue;
    std::vector<size_t> Work(G.Nodes[N].Successors.begin(),
                             G.Nodes[N].Successors.end());
    std::set<size_t> Seen;
    while (!Work.empty()) {
      const size_t M = Work.back();
      Work.pop_back();
      if (!Seen.insert(M).second)
        continue;
      const HighStmt *T = G.Nodes[M].Statement;
      if (M == 0 || (T && T->Kind == StmtKind::Return))
        ExitFrom.insert(A);
      const int B = BlockOf(M);
      if (B < 0) {
        Work.insert(Work.end(), G.Nodes[M].Successors.begin(),
                    G.Nodes[M].Successors.end());
        continue;
      }
      if (B != A)
        Pairs.insert({A, B});
    }
  }

  // An edge may run through blocks with no statement left.
  auto Reaches = [&](int From, int To) {
    std::vector<int> Work{From};
    std::set<int> Seen{From};
    while (!Work.empty()) {
      const int X = Work.back();
      Work.pop_back();
      auto It = Index.find(X);
      if (It == Index.end())
        continue;
      for (int S : Med.Blocks[It->second].Succs) {
        if (S == To)
          return true;
        if (!Present.count(S) && Seen.insert(S).second)
          Work.push_back(S);
      }
    }
    return false;
  };
  auto Start = [&](int B) {
    auto It = Index.find(B);
    return It == Index.end() ? 0ULL
                             : static_cast<unsigned long long>(
                                   Med.Blocks[It->second].StartAddr);
  };
  // A block without successors never runs into the next statement; a pair
  // from it is the dead fall-through after a call that does not return.
  auto Ends = [&](int B) {
    auto It = Index.find(B);
    return It != Index.end() && Med.Blocks[It->second].Succs.empty();
  };
  // Blocks from which every path returns: a copied return tail need not keep
  // their addresses.
  std::map<int, bool> ReturnOnly;
  std::function<bool(int, int)> Returns = [&](int B, int Depth) {
    if (auto It = ReturnOnly.find(B); It != ReturnOnly.end())
      return It->second;
    auto It = Index.find(B);
    if (It == Index.end() || Depth > 8)
      return false;
    ReturnOnly[B] = false;
    bool All = true;
    for (int S : Med.Blocks[It->second].Succs)
      All = All && Returns(S, Depth + 1);
    return ReturnOnly[B] = All;
  };

  size_t Extra = 0, Missing = 0;
  for (const auto &[A, B] : Pairs)
    if (!Edges.count({A, B}) && !Reaches(A, B) && !Ends(A)) {
      ++Extra;
      if (Detail)
        std::fprintf(stderr, "FLOWEXTRA %s %llx -> %llx\n", StageName.c_str(),
                     Start(A), Start(B));
    }
  for (const auto &[A, B] : Edges)
    if (A != B && Present.count(A) && Present.count(B) &&
        !Pairs.count({A, B}) && !(ExitFrom.count(A) && Returns(B, 0))) {
      ++Missing;
      if (Detail)
        std::fprintf(stderr, "FLOWMISSING %s %llx -> %llx\n", StageName.c_str(),
                     Start(A), Start(B));
    }
  std::fprintf(stderr, "FLOWORACLE stage=%s entry=%llx extra=%zu missing=%zu\n",
               StageName.c_str(), Entry, Extra, Missing);
}

} // namespace neverd
