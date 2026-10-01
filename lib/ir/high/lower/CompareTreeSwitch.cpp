//===- CompareTreeSwitch.cpp - Switch recovery from compare trees --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A sparse switch compiles to a tree of conditional branches on one value:
/// `sub ecx, 9; je A; sub ecx, 12; je B; ...; cmp ecx, 3; jne D` chains,
/// joined by a binary search of `cmp ecx, K; jg upper; je C`.  Each compared
/// value is the selector plus a constant, so every branch splits the set of
/// selector values reaching it.  Walking the tree from its root gives the
/// exact set of values that reaches each target outside it.  When one target
/// receives the large remainder and every other target a few values, the
/// tree is one switch: those few values are its case labels.
///
//===----------------------------------------------------------------------===//

#include "CompareTreeSwitch.h"

#include "neverd/Limits.h"
#include "neverd/ir/high/HighIR.h"

#include <algorithm>
#include <optional>
#include <set>
#include <utility>

namespace neverd {

namespace {

uint64_t maskOf(unsigned Bits) {
  return Bits >= 64 ? ~uint64_t(0) : (uint64_t(1) << Bits) - 1;
}

/// A set of Bits-wide values as sorted, disjoint, non-adjacent closed ranges.
class ValueSet {
public:
  explicit ValueSet(unsigned Bits) : Bits(Bits), Mask(maskOf(Bits)) {}

  static ValueSet all(unsigned Bits) { return wrapped(Bits, 0, maskOf(Bits)); }

  /// Lo up to Hi, wrapping past the top when Lo > Hi; Hi one below Lo is the
  /// whole range.
  static ValueSet wrapped(unsigned Bits, uint64_t Lo, uint64_t Hi) {
    ValueSet S(Bits);
    Lo &= S.Mask;
    Hi &= S.Mask;
    if (Lo <= Hi) {
      S.Ranges.push_back({Lo, Hi});
    } else {
      S.Ranges.push_back({0, Hi});
      S.Ranges.push_back({Lo, S.Mask});
    }
    S.normalize();
    return S;
  }

  bool empty() const { return Ranges.empty(); }

  ValueSet intersect(const ValueSet &O) const {
    ValueSet R(Bits);
    size_t I = 0, J = 0;
    while (I < Ranges.size() && J < O.Ranges.size()) {
      const uint64_t Lo = std::max(Ranges[I].first, O.Ranges[J].first);
      const uint64_t Hi = std::min(Ranges[I].second, O.Ranges[J].second);
      if (Lo <= Hi)
        R.Ranges.push_back({Lo, Hi});
      if (Ranges[I].second < O.Ranges[J].second)
        ++I;
      else
        ++J;
    }
    return R;
  }

  ValueSet unite(const ValueSet &O) const {
    ValueSet R(*this);
    R.Ranges.insert(R.Ranges.end(), O.Ranges.begin(), O.Ranges.end());
    R.normalize();
    return R;
  }

  ValueSet complement() const {
    ValueSet R(Bits);
    uint64_t Next = 0;
    for (const auto &[Lo, Hi] : Ranges) {
      if (Lo > Next)
        R.Ranges.push_back({Next, Lo - 1});
      if (Hi == Mask)
        return R;
      Next = Hi + 1;
    }
    R.Ranges.push_back({Next, Mask});
    return R;
  }

  /// The values V - Delta for each V in the set.
  ValueSet shiftedDown(uint64_t Delta) const {
    ValueSet R(Bits);
    for (const auto &[Lo, Hi] : Ranges)
      R = R.unite(wrapped(Bits, Lo - Delta, Hi - Delta));
    return R;
  }

  /// The number of values, or Limit + 1 when there are more than Limit.
  uint64_t count(uint64_t Limit) const {
    uint64_t N = 0;
    for (const auto &[Lo, Hi] : Ranges) {
      if (Hi - Lo >= Limit - N)
        return Limit + 1;
      N += Hi - Lo + 1;
    }
    return N;
  }

  /// Every value, in increasing order; only for a set count() bounds.
  std::vector<uint64_t> values() const {
    std::vector<uint64_t> Out;
    for (const auto &[Lo, Hi] : Ranges)
      for (uint64_t V = Lo;; ++V) {
        Out.push_back(V);
        if (V == Hi)
          break;
      }
    return Out;
  }

private:
  void normalize() {
    std::sort(Ranges.begin(), Ranges.end());
    std::vector<std::pair<uint64_t, uint64_t>> Merged;
    for (const auto &Range : Ranges) {
      if (!Merged.empty() && (Merged.back().second == Mask ||
                              Range.first <= Merged.back().second + 1)) {
        Merged.back().second = std::max(Merged.back().second, Range.second);
        continue;
      }
      Merged.push_back(Range);
    }
    Ranges = std::move(Merged);
  }

  unsigned Bits;
  uint64_t Mask;
  std::vector<std::pair<uint64_t, uint64_t>> Ranges;
};

/// A value known to equal (Base + Offset) modulo 2^Bits.
struct Affine {
  MedVar Base;
  uint64_t Offset = 0;
  unsigned Bits = 0;
};

/// The selector values, as a set over Base, for which a condition holds.
struct Constraint {
  MedVar Base;
  unsigned Bits = 0;
  ValueSet True{1};
};

/// A block ending in a conditional branch on the selector.
struct Decision {
  Constraint Condition;
  int Taken = -1;
  int Other = -1;
};

bool sameBase(const MedVar &A, unsigned ABits, const MedVar &B,
              unsigned BBits) {
  return ABits == BBits && varKey(A) == varKey(B);
}

/// Operations that read no memory, write nothing and cannot trap, so they can
/// run on a path that did not run them before.
bool isHoistable(const MedOp &Op) {
  if (Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !Op.IntrinsicOutputs.empty())
    return false;
  switch (Op.Opcode) {
  case NdOp::NOP:
  case NdOp::COPY:
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_AND:
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
  case NdOp::INT_LEFT:
  case NdOp::INT_RIGHT:
  case NdOp::INT_ASHR:
  case NdOp::INT_MULT:
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_SLESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESSEQUAL:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
  case NdOp::INT_NEGATE:
  case NdOp::INT_NOT:
  case NdOp::INT_NEG2:
  case NdOp::INT_CARRY:
  case NdOp::INT_SOVF:
  case NdOp::INT_SBOR:
  case NdOp::BOOL_AND:
  case NdOp::BOOL_OR:
  case NdOp::BOOL_XOR:
  case NdOp::BOOL_NOT:
  case NdOp::CONCAT:
  case NdOp::SUBBYTES:
  case NdOp::POPCOUNT:
  case NdOp::LZCOUNT:
    return true;
  default:
    return false;
  }
}

class CompareTreeFinder {
  /// A tree edge to a block outside the tree and the selector values that
  /// take it.
  struct Leg {
    int From;
    int To;
    ValueSet Values;
  };

public:
  explicit CompareTreeFinder(const MedFunc &Med)
      : Med(Med), InTree(Med.Blocks.size(), false),
        Decisions(Med.Blocks.size()), Decided(Med.Blocks.size(), false) {
    for (const MedBlock &Block : Med.Blocks)
      for (const MedOp &Op : Block.Ops)
        if (!Op.Output.isConst() && Op.Output.Size > 0)
          Defs.emplace(varKey(Op.Output), &Op);
  }

  std::vector<CompareTreeSwitch> run() {
    std::vector<CompareTreeSwitch> Trees;
    for (int Root : reversePostOrder()) {
      if (InTree[Root])
        continue;
      std::optional<CompareTreeSwitch> Tree = grow(Root);
      if (!Tree)
        continue;
      InTree[Root] = true;
      for (int Block : Tree->Interior)
        InTree[Block] = true;
      Trees.push_back(std::move(*Tree));
    }
    return Trees;
  }

private:
  bool validBlock(int Id) const {
    return Id >= 0 && Id < static_cast<int>(Med.Blocks.size()) &&
           Med.Blocks[Id].Id == Id;
  }

  /// Blocks in reverse post-order from the entry, so a tree's root comes
  /// before every block it dominates; blocks the entry does not reach follow
  /// in index order.
  std::vector<int> reversePostOrder() const {
    const size_t N = Med.Blocks.size();
    std::vector<int> Order;
    std::vector<bool> Seen(N, false);
    if (N == 0)
      return Order;
    std::vector<std::pair<int, size_t>> Stack{{0, 0}};
    Seen[0] = true;
    while (!Stack.empty()) {
      auto &[Block, Next] = Stack.back();
      const auto &Succs = Med.Blocks[Block].Succs;
      if (Next < Succs.size()) {
        const int Succ = Succs[Next++];
        if (validBlock(Succ) && !Seen[Succ]) {
          Seen[Succ] = true;
          Stack.push_back({Succ, 0});
        }
        continue;
      }
      Order.push_back(Block);
      Stack.pop_back();
    }
    std::reverse(Order.begin(), Order.end());
    for (size_t I = 0; I < N; ++I)
      if (!Seen[I])
        Order.push_back(static_cast<int>(I));
    return Order;
  }

  /// The value of \p V when its definitions reduce to a constant, such as a
  /// register loaded with an immediate for a `cmp ecx, eax`.
  std::optional<uint64_t> constantOf(const MedVar &V, int Depth) const {
    if (V.Size == 0 || V.Size > 8)
      return std::nullopt;
    const uint64_t Mask = maskOf(V.Size * 8u);
    if (V.isConst())
      return V.ConstVal & Mask;
    auto It = Defs.find(varKey(V));
    if (It == Defs.end() || Depth >= limits::kMaxCompareTreeEvalDepth)
      return std::nullopt;
    const MedOp &Op = *It->second;
    if (Op.NumInputs < 1)
      return std::nullopt;
    std::optional<uint64_t> A = constantOf(Op.Inputs[0], Depth + 1);
    if (!A)
      return std::nullopt;
    switch (Op.Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
      return *A & Mask;
    case NdOp::INT_SEXT: {
      const unsigned From = Op.Inputs[0].Size * 8u;
      if (From == 0 || From >= 64)
        return *A & Mask;
      const uint64_t Sign = uint64_t(1) << (From - 1);
      return ((*A ^ Sign) - Sign) & Mask;
    }
    case NdOp::SUBBYTES:
      if (Op.NumInputs < 2 || !Op.Inputs[1].isConst() ||
          Op.Inputs[1].ConstVal >= 8)
        return std::nullopt;
      return (*A >> (8 * Op.Inputs[1].ConstVal)) & Mask;
    default:
      return std::nullopt;
    }
  }

  std::optional<Affine> affine(const MedVar &V, int Depth) const {
    if (V.isConst() || V.Kind == MedVar::Unspecified || V.Size == 0 ||
        V.Size > 8 || constantOf(V, Depth))
      return std::nullopt;
    const unsigned Bits = V.Size * 8u;
    const Affine Self{V, 0, Bits};
    auto It = Defs.find(varKey(V));
    if (It == Defs.end() || Depth >= limits::kMaxCompareTreeEvalDepth)
      return Self;
    const MedOp &Op = *It->second;
    auto Through = [&](const MedVar &X, uint64_t Delta) {
      std::optional<Affine> Inner = affine(X, Depth + 1);
      if (!Inner || Inner->Bits != Bits)
        return Self;
      Inner->Offset = (Inner->Offset + Delta) & maskOf(Bits);
      return *Inner;
    };
    const bool Two = Op.NumInputs >= 2;
    std::optional<uint64_t> First, Second;
    if (Two) {
      First = constantOf(Op.Inputs[0], Depth + 1);
      Second = constantOf(Op.Inputs[1], Depth + 1);
    }
    switch (Op.Opcode) {
    case NdOp::COPY:
      if (Op.NumInputs >= 1 && !Op.Inputs[0].isConst())
        return Through(Op.Inputs[0], 0);
      return Self;
    case NdOp::INT_ADD:
      if (Second && !First)
        return Through(Op.Inputs[0], *Second);
      if (First && !Second)
        return Through(Op.Inputs[1], *First);
      return Self;
    case NdOp::INT_SUB:
      if (Second && !First)
        return Through(Op.Inputs[0], uint64_t(0) - *Second);
      return Self;
    case NdOp::SUBBYTES: {
      // The low bytes of a widened value are that value.
      if (!Two || !Op.Inputs[1].isConst() || Op.Inputs[1].ConstVal != 0 ||
          Op.Inputs[0].isConst())
        return Self;
      const MedVar &Wide = Op.Inputs[0];
      if (Wide.Size == V.Size)
        return Through(Wide, 0);
      auto W = Defs.find(varKey(Wide));
      if (W == Defs.end())
        return Self;
      const MedOp &Widen = *W->second;
      if ((Widen.Opcode == NdOp::INT_ZEXT || Widen.Opcode == NdOp::INT_SEXT) &&
          Widen.NumInputs >= 1 && !Widen.Inputs[0].isConst() &&
          Widen.Inputs[0].Size == V.Size)
        return Through(Widen.Inputs[0], 0);
      return Self;
    }
    default:
      return Self;
    }
  }

  std::optional<Constraint> comparison(const MedOp &Op, int Depth) const {
    if (Op.NumInputs < 2)
      return std::nullopt;
    const std::optional<uint64_t> LC = constantOf(Op.Inputs[0], Depth + 1);
    const std::optional<uint64_t> RC = constantOf(Op.Inputs[1], Depth + 1);
    if (LC.has_value() == RC.has_value())
      return std::nullopt;
    const bool ConstOnLeft = LC.has_value();
    std::optional<Affine> Value =
        affine(Op.Inputs[ConstOnLeft ? 1 : 0], Depth + 1);
    if (!Value)
      return std::nullopt;
    const unsigned Bits = Value->Bits;
    const uint64_t Mask = maskOf(Bits);
    const uint64_t K = (ConstOnLeft ? *LC : *RC) & Mask;
    const bool Signed =
        Op.Opcode == NdOp::INT_SLESS || Op.Opcode == NdOp::INT_SLESSEQUAL;
    // A signed order is the unsigned order of values with the sign bit
    // flipped; an interval there is a (wrapping) interval of the values.
    const uint64_t Bias = Signed ? uint64_t(1) << (Bits - 1) : 0;
    const uint64_t KB = K ^ Bias;
    ValueSet Holds(Bits);
    auto Biased = [&](bool NonEmpty, uint64_t Lo, uint64_t Hi) {
      if (NonEmpty)
        Holds = ValueSet::wrapped(Bits, Lo ^ Bias, Hi ^ Bias);
    };
    switch (Op.Opcode) {
    case NdOp::INT_EQUAL:
      Holds = ValueSet::wrapped(Bits, K, K);
      break;
    case NdOp::INT_NOTEQUAL:
      Holds = ValueSet::wrapped(Bits, K, K).complement();
      break;
    case NdOp::INT_LESS:
    case NdOp::INT_SLESS:
      if (ConstOnLeft)
        Biased(KB != Mask, KB + 1, Mask); // K < x
      else
        Biased(KB != 0, 0, KB - 1); // x < K
      break;
    case NdOp::INT_LESSEQUAL:
    case NdOp::INT_SLESSEQUAL:
      if (ConstOnLeft)
        Biased(true, KB, Mask); // K <= x
      else
        Biased(true, 0, KB); // x <= K
      break;
    default:
      return std::nullopt;
    }
    Constraint C;
    C.Base = Value->Base;
    C.Bits = Bits;
    C.True = Holds.shiftedDown(Value->Offset);
    return C;
  }

  /// Two conditions on one selector compared as truth values: `SF == OF`.
  std::optional<Constraint> sameTruth(const MedOp &Op, bool Equal,
                                      int Depth) const {
    if (Op.NumInputs < 2)
      return std::nullopt;
    std::optional<Constraint> A = constraint(Op.Inputs[0], Depth + 1);
    std::optional<Constraint> B = constraint(Op.Inputs[1], Depth + 1);
    if (!A || !B || !sameBase(A->Base, A->Bits, B->Base, B->Bits))
      return std::nullopt;
    const ValueSet Both = A->True.intersect(B->True);
    const ValueSet Neither =
        A->True.complement().intersect(B->True.complement());
    A->True = Both.unite(Neither);
    if (!Equal)
      A->True = A->True.complement();
    return A;
  }

  /// The carry or signed overflow of `x + K` or `x - K` as a condition.
  std::optional<Constraint> overflow(const MedOp &Op, int Depth) const {
    if (Op.NumInputs < 2)
      return std::nullopt;
    const std::optional<uint64_t> LC = constantOf(Op.Inputs[0], Depth + 1);
    const std::optional<uint64_t> RC = constantOf(Op.Inputs[1], Depth + 1);
    // Subtraction does not commute; only `x - K` is a test of x.
    if (LC.has_value() == RC.has_value() || (Op.Opcode == NdOp::INT_SBOR && LC))
      return std::nullopt;
    std::optional<Affine> Value = affine(Op.Inputs[LC ? 1 : 0], Depth + 1);
    if (!Value)
      return std::nullopt;
    const unsigned Bits = Value->Bits;
    const uint64_t Mask = maskOf(Bits);
    const uint64_t K = (LC ? *LC : *RC) & Mask;
    const uint64_t Sign = uint64_t(1) << (Bits - 1);
    const bool Negative = (K & Sign) != 0;
    // Intervals of x with the sign bit flipped, where signed order is
    // unsigned order: 0 is the most negative value and Mask the largest.
    std::optional<std::pair<uint64_t, uint64_t>> Biased;
    switch (Op.Opcode) {
    case NdOp::INT_CARRY:
      if (K != 0)
        Biased = std::make_pair(((Mask - K + 1) & Mask) ^ Sign, Mask ^ Sign);
      break;
    case NdOp::INT_SOVF:
      // x + K leaves the signed range above the top or below the bottom.
      if (K != 0 && !Negative)
        Biased = std::make_pair(Mask - K + 1, Mask);
      else if (Negative)
        Biased = std::make_pair(uint64_t(0), ((0 - K) & Mask) - 1);
      break;
    case NdOp::INT_SBOR:
      // x - K leaves the signed range below the bottom or above the top.
      if (K != 0 && !Negative)
        Biased = std::make_pair(uint64_t(0), K - 1);
      else if (Negative)
        Biased = std::make_pair((Mask - ((0 - K) & Mask) + 1) & Mask, Mask);
      break;
    default:
      return std::nullopt;
    }
    ValueSet Holds(Bits);
    if (Biased)
      Holds =
          ValueSet::wrapped(Bits, Biased->first ^ Sign, Biased->second ^ Sign);
    Constraint C;
    C.Base = Value->Base;
    C.Bits = Bits;
    C.True = Holds.shiftedDown(Value->Offset);
    return C;
  }

  std::optional<Constraint> constraint(const MedVar &Cond, int Depth) const {
    if (Cond.isConst() || Depth >= limits::kMaxCompareTreeEvalDepth)
      return std::nullopt;
    auto It = Defs.find(varKey(Cond));
    if (It == Defs.end())
      return std::nullopt;
    const MedOp &Op = *It->second;
    switch (Op.Opcode) {
    case NdOp::COPY:
      if (Op.NumInputs < 1)
        return std::nullopt;
      return constraint(Op.Inputs[0], Depth + 1);
    case NdOp::BOOL_NOT: {
      if (Op.NumInputs < 1)
        return std::nullopt;
      std::optional<Constraint> C = constraint(Op.Inputs[0], Depth + 1);
      if (C)
        C->True = C->True.complement();
      return C;
    }
    case NdOp::BOOL_AND:
    case NdOp::BOOL_OR: {
      if (Op.NumInputs < 2)
        return std::nullopt;
      std::optional<Constraint> A = constraint(Op.Inputs[0], Depth + 1);
      std::optional<Constraint> B = constraint(Op.Inputs[1], Depth + 1);
      if (!A || !B || !sameBase(A->Base, A->Bits, B->Base, B->Bits))
        return std::nullopt;
      A->True = Op.Opcode == NdOp::BOOL_AND ? A->True.intersect(B->True)
                                            : A->True.unite(B->True);
      return A;
    }
    case NdOp::BOOL_XOR:
      return sameTruth(Op, false, Depth);
    case NdOp::INT_EQUAL:
    case NdOp::INT_NOTEQUAL:
      if (std::optional<Constraint> C = comparison(Op, Depth))
        return C;
      return sameTruth(Op, Op.Opcode == NdOp::INT_EQUAL, Depth);
    case NdOp::INT_LESS:
    case NdOp::INT_SLESS:
    case NdOp::INT_LESSEQUAL:
    case NdOp::INT_SLESSEQUAL:
      return comparison(Op, Depth);
    case NdOp::INT_CARRY:
    case NdOp::INT_SOVF:
    case NdOp::INT_SBOR:
      return overflow(Op, Depth);
    default:
      return std::nullopt;
    }
  }

  static va_t entryOf(const MedBlock &Block) {
    if (Block.StartAddr)
      return Block.StartAddr;
    return Block.Ops.empty() ? 0 : Block.Ops.front().Addr;
  }

  /// The selector test and the two successors of a block ending in a
  /// conditional branch.
  const std::optional<Decision> &decision(int Id) {
    if (Decided[Id])
      return Decisions[Id];
    Decided[Id] = true;
    const MedBlock &Block = Med.Blocks[Id];
    if (Block.Ops.empty() || Block.Succs.size() != 2)
      return Decisions[Id];
    const MedOp &Branch = Block.Ops.back();
    if (Branch.Opcode != NdOp::COND_BR || Branch.NumInputs < 2 ||
        !Branch.Inputs[0].isConst())
      return Decisions[Id];
    Decision D;
    for (int Succ : Block.Succs) {
      if (!validBlock(Succ))
        return Decisions[Id];
      if (entryOf(Med.Blocks[Succ]) == Branch.Inputs[0].ConstVal && D.Taken < 0)
        D.Taken = Succ;
      else
        D.Other = Succ;
    }
    if (D.Taken < 0 || D.Other < 0 || D.Taken == D.Other)
      return Decisions[Id];
    std::optional<Constraint> C = constraint(Branch.Inputs[1], 0);
    if (!C)
      return Decisions[Id];
    D.Condition = std::move(*C);
    Decisions[Id] = std::move(D);
    return Decisions[Id];
  }

  /// Whether \p Id, reached from \p Parent, can fold into a tree on Base: it
  /// is entered only from Parent, holds only pure operations, and ends in a
  /// branch on the same selector or a plain jump.
  bool absorbable(int Parent, int Id, const MedVar &Base, unsigned Bits) {
    const MedBlock &Block = Med.Blocks[Id];
    if (Id == 0 || InTree[Id] || Block.Preds.size() != 1 ||
        Block.Preds[0] != Parent || !Block.Phis.empty() ||
        !Block.ExceptionalPreds.empty() || !Block.ExceptionalSuccs.empty() ||
        Block.Ops.empty())
      return false;
    for (size_t I = 0; I + 1 < Block.Ops.size(); ++I)
      if (!isHoistable(Block.Ops[I]))
        return false;
    const MedOp &Last = Block.Ops.back();
    if (Last.Opcode == NdOp::BRANCH)
      return Block.Succs.size() == 1 && validBlock(Block.Succs[0]);
    const std::optional<Decision> &D = decision(Id);
    return D && sameBase(D->Condition.Base, D->Condition.Bits, Base, Bits);
  }

  /// Whether \p A and \p B hold the same value: one SSA value, one constant,
  /// or one selector-relative value seen through copies and extensions.
  bool sameValue(const MedVar &A, const MedVar &B, int Depth) const {
    if (A == B)
      return true;
    if (A.Size != B.Size || Depth >= limits::kMaxCompareTreeEvalDepth)
      return false;
    std::optional<uint64_t> CA = constantOf(A, Depth + 1);
    std::optional<uint64_t> CB = constantOf(B, Depth + 1);
    if (CA || CB)
      return CA && CB && *CA == *CB;
    std::optional<Affine> FA = affine(A, Depth + 1);
    std::optional<Affine> FB = affine(B, Depth + 1);
    if (FA && FB && FA->Offset == FB->Offset &&
        sameBase(FA->Base, FA->Bits, FB->Base, FB->Bits))
      return true;
    auto DA = Defs.find(varKey(A));
    auto DB = Defs.find(varKey(B));
    if (DA == Defs.end() || DB == Defs.end())
      return false;
    const MedOp &OA = *DA->second;
    const MedOp &OB = *DB->second;
    return OA.Opcode == OB.Opcode &&
           (OA.Opcode == NdOp::INT_ZEXT || OA.Opcode == NdOp::INT_SEXT) &&
           OA.NumInputs >= 1 && OB.NumInputs >= 1 &&
           sameValue(OA.Inputs[0], OB.Inputs[0], Depth + 1);
  }

  /// Whether the edges From1 -> To and From2 -> To give every PHI of To the
  /// same value, so one transfer serves both.  A PHI no operation reads still
  /// counts: call and return recovery read register PHIs directly.
  bool sameEdgeValues(int From1, int From2, int To) const {
    for (const PhiNode &Phi : Med.Blocks[To].Phis) {
      const MedVar *A = nullptr;
      const MedVar *B = nullptr;
      for (const auto &[Pred, Arg] : Phi.Args) {
        if (Pred == From1 && !A)
          A = &Arg;
        if (Pred == From2 && !B)
          B = &Arg;
      }
      if (!A || !B || !sameValue(*A, *B, 0))
        return false;
    }
    return true;
  }

  /// The selector values a block's value can take: the low range of a
  /// zero-extended value, else everything.
  ValueSet domain(const MedVar &Base, unsigned Bits) const {
    auto It = Defs.find(varKey(Base));
    if (It != Defs.end() && It->second->Opcode == NdOp::INT_ZEXT &&
        It->second->NumInputs >= 1 && It->second->Inputs[0].Size > 0 &&
        It->second->Inputs[0].Size * 8u < Bits)
      return ValueSet::wrapped(Bits, 0,
                               maskOf(It->second->Inputs[0].Size * 8u));
    return ValueSet::all(Bits);
  }

  /// Immediate dominators over the ordinary CFG from the entry; -1 for a
  /// block the entry does not reach.
  void computeDominators() {
    if (!IDom.empty())
      return;
    const size_t N = Med.Blocks.size();
    IDom.assign(N, -1);
    RpoIndex.assign(N, -1);
    const std::vector<int> Order = reversePostOrder();
    // Blocks the entry does not reach follow the reachable ones in Order.
    std::vector<bool> Reached(N, false);
    {
      std::vector<int> Work{0};
      Reached[0] = true;
      while (!Work.empty()) {
        const int B = Work.back();
        Work.pop_back();
        for (int S : Med.Blocks[B].Succs)
          if (validBlock(S) && !Reached[S]) {
            Reached[S] = true;
            Work.push_back(S);
          }
      }
    }
    for (size_t I = 0; I < Order.size(); ++I)
      if (Reached[Order[I]])
        RpoIndex[Order[I]] = static_cast<int>(I);
    if (N == 0)
      return;
    IDom[0] = 0;
    auto Intersect = [&](int A, int B) {
      while (A != B) {
        while (RpoIndex[A] > RpoIndex[B])
          A = IDom[A];
        while (RpoIndex[B] > RpoIndex[A])
          B = IDom[B];
      }
      return A;
    };
    for (bool Changed = true; Changed;) {
      Changed = false;
      for (int B : Order) {
        if (B == 0 || !Reached[B])
          continue;
        int New = -1;
        for (int P : Med.Blocks[B].Preds)
          if (validBlock(P) && IDom[P] != -1)
            New = New == -1 ? P : Intersect(P, New);
        if (New != -1 && IDom[B] != New) {
          IDom[B] = New;
          Changed = true;
        }
      }
    }
    Children.assign(N, {});
    for (size_t B = 1; B < N; ++B)
      if (IDom[B] >= 0)
        Children[IDom[B]].push_back(static_cast<int>(B));
  }

  /// The blocks \p Head dominates, in index order.
  std::vector<int> regionOf(int Head) {
    computeDominators();
    std::vector<int> Region;
    std::vector<int> Work{Head};
    while (!Work.empty()) {
      const int B = Work.back();
      Work.pop_back();
      Region.push_back(B);
      for (int C : Children[B])
        Work.push_back(C);
    }
    std::sort(Region.begin(), Region.end());
    return Region;
  }

  /// The targets outside \p Region that its blocks branch to.
  std::set<int> exitsOf(const std::vector<int> &Region) const {
    std::set<int> Exits;
    for (int B : Region)
      for (int S : Med.Blocks[B].Succs)
        if (!std::binary_search(Region.begin(), Region.end(), S))
          Exits.insert(S);
    return Exits;
  }

  /// A block that only returns, after pure operations: copying it to each
  /// jump turns the jump into a return.
  bool isReturnTail(int Id) const {
    const MedBlock &Block = Med.Blocks[Id];
    if (Block.Ops.empty() || Block.Ops.back().Opcode != NdOp::RETURN ||
        Block.Ops.size() > 5)
      return false;
    for (size_t I = 0; I + 1 < Block.Ops.size(); ++I)
      if (!isHoistable(Block.Ops[I]))
        return false;
    return true;
  }

  /// Whether the case targets own their code: each is entered only from the
  /// tree (the default target may also be a join), and the blocks they
  /// dominate leave for at most one shared place besides blocks that just
  /// return.  Then each case can end in `break` before that place.  Cases
  /// sharing code with other paths, or leaving for different places, keep
  /// jumps that a switch adds to what nested ifs need.
  /// \p Regions receives the region of each owned target, and stays empty
  /// for the default target when it is a join.
  bool ownsItsCases(int Root, const std::vector<int> &Interior,
                    const std::vector<int> &Targets, size_t Default,
                    std::vector<std::vector<int>> &Regions) {
    std::vector<bool> Node(Med.Blocks.size(), false);
    Node[Root] = true;
    for (int Id : Interior)
      Node[Id] = true;
    std::set<int> Exits;
    Regions.assign(Targets.size(), {});
    for (size_t I = 0; I < Targets.size(); ++I) {
      const MedBlock &Target = Med.Blocks[Targets[I]];
      const bool Owned =
          std::all_of(Target.Preds.begin(), Target.Preds.end(),
                      [&](int Pred) { return validBlock(Pred) && Node[Pred]; });
      if (Owned) {
        Regions[I] = regionOf(Targets[I]);
        const std::set<int> Leaves = exitsOf(Regions[I]);
        Exits.insert(Leaves.begin(), Leaves.end());
      } else if (I == Default) {
        Exits.insert(Targets[I]);
      } else {
        return false;
      }
    }
    for (auto It = Exits.begin(); It != Exits.end();)
      It = isReturnTail(*It) ? Exits.erase(It) : std::next(It);
    return Exits.size() <= 1;
  }

  std::optional<CompareTreeSwitch> grow(int Root) {
    const std::optional<Decision> &RootDecision = decision(Root);
    if (!RootDecision)
      return std::nullopt;
    const MedVar Base = RootDecision->Condition.Base;
    const unsigned Bits = RootDecision->Condition.Bits;

    std::vector<Leg> Legs;
    CompareTreeSwitch Tree;
    Tree.Root = Root;
    Tree.Selector = Base;

    std::vector<std::pair<int, ValueSet>> Stack{{Root, domain(Base, Bits)}};
    while (!Stack.empty()) {
      auto [Id, Values] = std::move(Stack.back());
      Stack.pop_back();
      if (Id != Root) {
        Tree.Interior.push_back(Id);
        if (Tree.Interior.size() > limits::kMaxCompareTreeBlocks)
          return std::nullopt;
      }
      const MedBlock &Block = Med.Blocks[Id];
      std::vector<std::pair<int, ValueSet>> Edges;
      if (Block.Ops.back().Opcode == NdOp::BRANCH) {
        Edges.push_back({Block.Succs[0], Values});
      } else {
        const Decision &D = *decision(Id);
        Edges.push_back(
            {D.Other, Values.intersect(D.Condition.True.complement())});
        Edges.push_back({D.Taken, Values.intersect(D.Condition.True)});
      }
      for (auto &[To, EdgeValues] : Edges) {
        if (absorbable(Id, To, Base, Bits))
          Stack.push_back({To, std::move(EdgeValues)});
        else
          Legs.push_back({Id, To, std::move(EdgeValues)});
      }
    }
    if (Tree.Interior.empty())
      return std::nullopt;

    // Edges to one target that write its live PHIs alike share one transfer.
    std::vector<Leg> Groups;
    for (Leg &L : Legs) {
      if (L.Values.empty())
        continue;
      auto Same = std::find_if(Groups.begin(), Groups.end(), [&](const Leg &G) {
        return G.To == L.To && sameEdgeValues(G.From, L.From, L.To);
      });
      if (Same != Groups.end())
        Same->Values = Same->Values.unite(L.Values);
      else
        Groups.push_back(std::move(L));
    }
    if (Groups.size() < limits::kMinCompareTreeTargets + 1)
      return std::nullopt;

    // Only the default may take more than a handful of values.
    const uint64_t PerTarget = limits::kMaxCompareTreeValuesPerTarget;
    size_t Default = 0;
    uint64_t DefaultCount = 0;
    unsigned Large = 0;
    for (size_t I = 0; I < Groups.size(); ++I) {
      const uint64_t N = Groups[I].Values.count(PerTarget);
      Large += N > PerTarget;
      if (N > DefaultCount) {
        Default = I;
        DefaultCount = N;
      }
    }
    if (Large > 1)
      return std::nullopt;

    std::vector<int> Targets;
    for (const Leg &G : Groups)
      Targets.push_back(G.To);
    std::vector<std::vector<int>> Regions;
    if (!ownsItsCases(Root, Tree.Interior, Targets, Default, Regions))
      return std::nullopt;
    // A target two groups share cannot hold its code in both cases.
    for (size_t I = 0; I < Targets.size(); ++I)
      if (std::count(Targets.begin(), Targets.end(), Targets[I]) > 1)
        Regions[I].clear();
    size_t Labels = 0;
    for (size_t I = 0; I < Groups.size(); ++I) {
      if (I == Default)
        continue;
      Labels += Groups[I].Values.count(PerTarget);
      if (Labels > limits::kMaxCompareTreeCases)
        return std::nullopt;
      Tree.Cases.push_back({Groups[I].From, Groups[I].To,
                            Groups[I].Values.values(), std::move(Regions[I])});
    }
    std::sort(Tree.Cases.begin(), Tree.Cases.end(),
              [](const CompareTreeEdge &A, const CompareTreeEdge &B) {
                return A.Values.front() < B.Values.front();
              });
    Tree.Default = {Groups[Default].From,
                    Groups[Default].To,
                    {},
                    std::move(Regions[Default])};
    return Tree;
  }

  const MedFunc &Med;
  std::vector<int> IDom;
  std::vector<int> RpoIndex;
  std::vector<std::vector<int>> Children;
  VarKeyMap<const MedOp *> Defs;
  std::vector<bool> InTree;
  std::vector<std::optional<Decision>> Decisions;
  std::vector<bool> Decided;
};

} // namespace

std::vector<CompareTreeSwitch> findCompareTreeSwitches(const MedFunc &Med) {
  return CompareTreeFinder(Med).run();
}

} // namespace neverd
