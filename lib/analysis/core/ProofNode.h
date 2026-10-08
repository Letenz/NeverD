//===- ProofNode.h - Proof-local structural admission ===========//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_PROOFNODE_H
#define NEVERD_ANALYSIS_PROOFNODE_H
#include "neverd/symbolic/SymExpr.h"
namespace neverd::analysis::detail {
// The caller charges this node and all operands before inspection. Topological
// references and exact operator widths are required before
// rebuilding/evaluating.
inline bool validProofNode(const symbolic::SymContext &C, symbolic::SymRef R) {
  using namespace symbolic;
  if (!R || R.index() >= C.numNodes() || !C.width(R))
    return false;
  const auto &N = C.node(R);
  const auto A = C.operands(R);
  for (auto V : A)
    if (!V || V.index() >= R.index() || !C.width(V))
      return false;
  const auto SameWidth = [&] {
    for (auto V : A)
      if (C.width(V) != N.Width)
        return false;
    return true;
  };
  bool Shape = false;
  switch (N.Op) {
  case SymOp::Const:
    Shape = A.empty();
    break;
  case SymOp::Var:
    Shape =
        A.empty() && N.Aux < C.numVars() && C.varInfo(N.Aux).Width == N.Width;
    break;
  case SymOp::Add:
  case SymOp::Mul:
  case SymOp::And:
  case SymOp::Or:
  case SymOp::Xor:
    Shape = !A.empty() && SameWidth();
    break;
  case SymOp::Not:
    Shape = A.size() == 1 && SameWidth();
    break;
  case SymOp::Shl:
  case SymOp::LShr:
  case SymOp::AShr:
  case SymOp::Rol:
  case SymOp::Ror:
    Shape = A.size() == 2 && C.width(A[0]) == N.Width;
    break;
  case SymOp::UDiv:
  case SymOp::SDiv:
  case SymOp::URem:
  case SymOp::SRem:
    Shape = A.size() == 2 && SameWidth();
    break;
  case SymOp::Extract:
    Shape = A.size() == 1 && N.Aux <= C.width(A[0]) &&
            N.Width <= C.width(A[0]) - N.Aux;
    break;
  case SymOp::Concat: {
    uint64_t Width = 0;
    for (auto V : A)
      Width += C.width(V);
    Shape = !A.empty() && Width == N.Width;
    break;
  }
  case SymOp::ZExt:
  case SymOp::SExt:
    Shape = A.size() == 1 && C.width(A[0]) <= N.Width;
    break;
  case SymOp::Ite:
    Shape = A.size() == 3 && C.width(A[0]) == 1 && C.width(A[1]) == N.Width &&
            C.width(A[2]) == N.Width;
    break;
  case SymOp::Eq:
  case SymOp::Ult:
  case SymOp::Ule:
  case SymOp::Slt:
  case SymOp::Sle:
    Shape = A.size() == 2 && N.Width == 1 && C.width(A[0]) == C.width(A[1]);
    break;
  }
  return Shape;
}
} // namespace neverd::analysis::detail
#endif
