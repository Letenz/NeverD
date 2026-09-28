//===- SymMBA.cpp - Mixed boolean-arithmetic simplification ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the measure-and-rewrite loop described in SymMBA.h: the entry
/// points, and the split of a region a constant mask has made unmeasurable
/// into mask-uniform columns.
///
/// The theory it rests on, in one paragraph.  Split each bitwise term of a
/// linear MBA into minterms — the bitwise functions that pick out the bit
/// positions where the inputs match one particular pattern.  Minterms have
/// disjoint bit supports and together cover every position, so as integers
/// they sum to the all-ones word.  Substituting the split back in and
/// collecting gives, for any linear MBA over t inputs,
///
///     e  =  sum over the 2^t patterns m of  w_m * M_m
///
/// with the weights `w_m` the only thing that distinguishes one such
/// expression from another.  Setting every input to all-zeros or all-ones puts
/// every bit position into the same pattern k, which leaves `M_k` at all-ones
/// and every other minterm at zero, so the expression evaluates to `-w_k`.
/// One evaluation per pattern therefore reads off every weight, and the size
/// the expression was written at has nothing to do with it.
///
/// The stages live next door: SymMBAAbstract.cpp decides what the measurement
/// can see, SymMBAMeasure.cpp reads the weights and writes them back out,
/// SymMBAPoly.cpp and SymMBAProduct.cpp handle what is not linear, and
/// SymMBARegion.cpp ranks the readings of one region.
///
//===----------------------------------------------------------------------===//

#include "neverd/symbolic/SymMBA.h"

#include "SymMBADetail.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <vector>

namespace neverd::symbolic {

using namespace detail;

namespace {

//===----------------------------------------------------------------------===//
// Regions guarded by constant masks
//===----------------------------------------------------------------------===//

/// The distinct constants that appear as an operand of an `&`, `|` or `^` at
/// \p Width.
///
/// These are exactly the constants a corner measurement cannot see past.  A
/// bitwise term is uniform at a corner — all-zeros or all-ones — which is what
/// the whole measurement depends on; a constant operand breaks that uniformity
/// because it tells one bit position from another.  The builders have already
/// folded away the all-zeros and all-ones cases, so anything left here is a
/// genuine mask.  A constant used as a summand or a coefficient is not
/// collected: it does not defeat the measurement and does not partition the
/// word.
///
/// Only masks at \p Width are collected, because they are the ones that
/// partition this region's bits: a mask inside a narrower or wider subterm
/// belongs to that subterm's own region, which the layered walk visits on its
/// own.  Mixing widths would also be a plain type error -- the column bitmasks
/// are built at \p Width and an `APInt` operation across two widths asserts.
void collectBitwiseMasks(const SymContext &Ctx, SymRef E, uint32_t Width,
                         llvm::SmallVectorImpl<llvm::APInt> &Out) {
  for (uint32_t Index : reachableInOrder(Ctx, E)) {
    SymRef R(Index);
    SymOp Op = Ctx.op(R);
    if (Op != SymOp::And && Op != SymOp::Or && Op != SymOp::Xor)
      continue;
    if (Ctx.width(R) != Width)
      continue;
    for (SymRef C : Ctx.operands(R))
      if (Ctx.isConst(C)) {
        llvm::APInt V = Ctx.constValue(C);
        if (llvm::none_of(Out, [&](const llvm::APInt &S) { return S == V; }))
          Out.push_back(V);
      }
  }
}

/// Partition the bit positions of a \p Width word into the columns on which
/// every mask is constant, each returned as the set of positions it holds.
///
/// Two positions belong together when no mask tells them apart.  Splitting the
/// full word by one mask at a time reaches that partition directly, and drops
/// the empty pieces so the column count is the number of distinct signatures
/// rather than the 2^k an enumeration of signatures would suggest.
llvm::SmallVector<llvm::APInt, 8>
maskColumns(uint32_t Width, llvm::ArrayRef<llvm::APInt> Masks) {
  llvm::SmallVector<llvm::APInt, 8> Cols;
  Cols.push_back(llvm::APInt::getAllOnes(Width));
  for (const llvm::APInt &M : Masks) {
    llvm::SmallVector<llvm::APInt, 8> Next;
    for (const llvm::APInt &Col : Cols) {
      llvm::APInt In = Col & M;
      llvm::APInt Out = Col & ~M;
      if (!In.isZero())
        Next.push_back(std::move(In));
      if (!Out.isZero())
        Next.push_back(std::move(Out));
    }
    Cols = std::move(Next);
  }
  return Cols;
}

/// Rewrite \p E as it behaves on the positions in \p ColMask.
///
/// Every collected mask is constant across the column, so inside a bitwise
/// operator it is all-ones (drop it) or all-zeros (kill the term) — which the
/// builders fold — and the result no longer distinguishes bit positions.  What
/// comes out is a mask-free expression that equals \p E on the column's bits.
SymRef restrictToColumn(SymContext &Ctx, SymRef E, const llvm::APInt &ColMask,
                        llvm::ArrayRef<llvm::APInt> Masks) {
  llvm::DenseMap<uint32_t, SymRef> Done;
  for (uint32_t Index : reachableInOrder(Ctx, E)) {
    SymRef R(Index);
    llvm::ArrayRef<SymRef> Ops = Ctx.operands(R);
    if (Ops.empty()) {
      Done[Index] = R;
      continue;
    }
    SymOp Op = Ctx.op(R);
    const bool Bitwise =
        Op == SymOp::And || Op == SymOp::Or || Op == SymOp::Xor;
    llvm::SmallVector<SymRef, 8> NewOps;
    NewOps.reserve(Ops.size());
    // Masks were collected at the column's width, so a bitwise node at another
    // width holds none of them; leaving it alone also keeps the width-matched
    // APInt comparisons below from asserting.
    const bool SameWidth = Ctx.width(R) == ColMask.getBitWidth();
    for (SymRef C : Ops) {
      SymRef NC = Done.lookup(C.index());
      if (Bitwise && SameWidth && Ctx.isConst(C) &&
          llvm::any_of(Masks, [&](const llvm::APInt &M) {
            return M == Ctx.constValue(C);
          })) {
        // Uniform on the column by construction, so this is exact rather than a
        // choice: the mask either covers every position the column holds or
        // none of them.
        bool AllSet = (Ctx.constValue(C) & ColMask) == ColMask;
        NC = AllSet ? Ctx.mkOnes(Ctx.width(R)) : Ctx.mkZero(Ctx.width(R));
      }
      NewOps.push_back(NC);
    }
    Done[Index] = Ctx.rebuild(R, NewOps);
  }
  return Done.lookup(E.index());
}

/// Prove that changing bits outside one mask column cannot affect bits inside
/// it before the root is reached.
///
/// Replacing a constant mask by zero or all-ones is exact at the positions of
/// one column.  The replacement may differ elsewhere, though, and arithmetic
/// above that mask could carry the difference back into the column.  A path
/// made only of whole-word bitwise operators cannot: each output bit depends
/// only on the input bits at the same position.  Requiring every path from a
/// collected mask to the root to have that form makes column reassembly a
/// derivation rather than a sample-backed guess.
bool maskColumnsAreIndependent(const SymContext &Ctx, SymRef E, uint32_t Width,
                               llvm::ArrayRef<llvm::APInt> Masks) {
  std::vector<uint32_t> Order = reachableInOrder(Ctx, E);
  llvm::DenseMap<uint32_t, bool> SafeToRoot;
  SafeToRoot[E.index()] = true;

  for (auto It = Order.rbegin(); It != Order.rend(); ++It) {
    SymRef R(*It);
    const bool Safe = SafeToRoot.lookup(R.index());
    const SymOp Op = Ctx.op(R);

    bool HoldsMask = false;
    if (Ctx.width(R) == Width && isBitwise(Op))
      for (SymRef C : Ctx.operands(R))
        if (Ctx.isConst(C) && llvm::any_of(Masks, [&](const llvm::APInt &M) {
              return M == Ctx.constValue(C);
            })) {
          HoldsMask = true;
          break;
        }
    if (HoldsMask && !Safe)
      return false;

    const bool ChildSafe = Safe && Ctx.width(R) == Width && isBitwise(Op);
    for (SymRef C : Ctx.operands(R)) {
      auto [Pos, Inserted] = SafeToRoot.try_emplace(C.index(), ChildSafe);
      if (!Inserted)
        Pos->second &= ChildSafe;
    }
  }
  return true;
}

/// Solve a region a constant mask has made unmeasurable, by measuring each
/// mask-uniform column of the word on its own.
///
/// A mask defeats the corner measurement, but only across a column boundary:
/// on the positions where every mask is constant the expression is an ordinary
/// linear MBA, and there it can be measured.  Solving each column and keeping
/// the bits it owns reassembles the whole — which is what recovers, say,
/// `((x ^ y) + 2 * (x & y)) & 0xff` as `(x + y) & 0xff`.
///
/// The split is exact only when no arithmetic carry crosses a column boundary.
/// A low mask over a sum discards the carry it would have produced, so the
/// common case holds, but two summands masked to the same nibble do not: the
/// carry between them lands in a position the split has already decided.  The
/// path from every mask to the root is therefore proved to contain only
/// position-wise bitwise operators before any column is measured.
SymRef solveMasked(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                   WorkBudget &Budget, SolveReport &Rep) {
  const uint32_t Width = Ctx.width(E);
  llvm::SmallVector<llvm::APInt, 8> Masks;
  collectBitwiseMasks(Ctx, E, Width, Masks);
  if (Masks.empty())
    return E;
  if (!maskColumnsAreIndependent(Ctx, E, Width, Masks))
    return E;

  llvm::SmallVector<llvm::APInt, 8> Cols = maskColumns(Width, Masks);
  if (Cols.size() < 2)
    return E;

  llvm::SmallVector<SymRef, 8> Parts;
  Parts.reserve(Cols.size());
  unsigned Widest = 0;
  bool AnySolved = false;
  for (const llvm::APInt &Col : Cols) {
    SymRef Ecol = restrictToColumn(Ctx, E, Col, Masks);
    SolveReport ColRep;
    SymRef Scol = solveOneRegion(Ctx, Ecol, Opts, Budget, ColRep);
    Rep.BudgetExhausted |= ColRep.BudgetExhausted;
    if (Scol != Ecol) {
      AnySolved = true;
      Widest = std::max(Widest, ColRep.NumAtoms);
    }
    // Clip each column to the bits it owns.  The columns are disjoint, so the
    // clipped parts share no bit and combining them with `|` is exact.
    Parts.push_back(Ctx.mkAnd(Scol, Ctx.mkConst(Col)));
  }
  if (!AnySolved)
    return E;

  SymRef Rebuilt = Ctx.mkOr(Parts);
  if (Rebuilt == E)
    return E;

  // Independence above made the split exact.  Sampling remains a defect net
  // for the implementation of the derivation, never the reason to accept it.
  bool Verified = agreeOnSamples(Ctx, E, Rebuilt, Opts.VerifySamples);
  assert(Verified && "a proved mask split disagreed with what it replaces");
  if (!Verified)
    return E;
  if (!Opts.AllowGrowth && !doesNotGrow(Ctx, Rebuilt, E))
    return E;

  Rep.NumAtoms = Widest;
  Rep.Outcome = MBAOutcome::Rewritten;
  Rep.Evidence = MBAEvidence::Derivation;
  return Rebuilt;
}

SymRef tryFastPartitionedSum(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                             WorkBudget &Budget, SolveReport &Rep);

/// Measure \p E as one region, splitting a wide one into independent parts and
/// a masked one into mask-uniform columns.
SymRef solveRegionOrSplit(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                          WorkBudget &Budget, SolveReport &Rep) {
  if (SymRef Fast = tryFastPartitionedSum(Ctx, E, Opts, Budget, Rep); Fast != E)
    return Fast;
  SymRef Solved = solveOneRegion(Ctx, E, Opts, Budget, Rep);
  return Solved == E ? solveMasked(Ctx, E, Opts, Budget, Rep) : Solved;
}

// Child rewrites can hide a shared arithmetic input in one bitwise use while
// its other uses retain the original spelling. Keep a whole-region reading
// there. A product retains the arithmetic reading for distribution and
// cancellation; remeasuring its already simplified factors as one bitwise
// region often repeats their work. Complements alone use that same route.
bool hasBitwiseInteraction(const SymContext &Ctx, SymRef R) {
  auto Bitwise = [&](SymRef N) {
    SymOp Op = Ctx.op(N);
    return Op == SymOp::And || Op == SymOp::Or || Op == SymOp::Xor;
  };
  return Bitwise(R) ||
         (Ctx.op(R) == SymOp::Add && llvm::any_of(Ctx.operands(R), Bitwise));
}

// A child may improve a complement of a product into an arithmetic sum and
// thereby hide its relation to the product's other bitwise uses. Preserve one
// original reading at sums of bitwise products, including a complemented sum.
// This only inspects the immediate sum/product frontier; it does not revisit
// the graph or broaden the emitted-candidate refinement below.
bool hasBitwiseProductSum(const SymContext &Ctx, SymRef R) {
  if (Ctx.op(R) == SymOp::Not)
    R = Ctx.operand(R, 0);
  if (Ctx.op(R) != SymOp::Add)
    return false;
  for (SymRef Term : Ctx.operands(R)) {
    if (Ctx.op(Term) != SymOp::Mul)
      continue;
    unsigned BitwiseFactors = 0;
    for (SymRef Factor : Ctx.operands(Term)) {
      SymOp Op = Ctx.op(Factor);
      if ((Op == SymOp::And || Op == SymOp::Or || Op == SymOp::Xor) &&
          ++BitwiseFactors == 2)
        return true;
    }
  }
  return false;
}

// A candidate can introduce a factored sum that was absent from the original
// postorder. Visit its unmeasured nodes once, reusing exact child replacements.
// The snapshot below is deliberately not extended with nodes emitted by this
// visit: this is one bounded refinement, not a whole-graph fixed point.
SymRef refineCandidate(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                       llvm::ArrayRef<uint32_t> OriginalOrder,
                       llvm::DenseMap<uint32_t, SymRef> &Solved,
                       WorkBudget &Budget, size_t &Storage, SolveReport &Rep) {
  auto Known = [&](SymRef R) {
    auto It = Solved.find(R.index());
    if (It != Solved.end())
      return It->second;
    if (R.index() <= OriginalOrder.back() &&
        std::binary_search(OriginalOrder.begin(), OriginalOrder.end(),
                           R.index()))
      return R;
    return SymRef();
  };
  if (SymRef R = Known(E); R.isValid())
    return readingScore(Ctx, R) < readingScore(Ctx, E) ? R : E;
  // An emitted root whose children already have their final spelling has no
  // new internal region to visit. Constants and scaled variables are also
  // builder-normalized leaves of this traversal.
  auto Finished = [&](SymRef R) {
    if (SymRef K = Known(R); K.isValid())
      return K == R;
    return Ctx.numOperands(R) == 0 ||
           (Ctx.op(R) == SymOp::Mul && Ctx.numOperands(R) == 2 &&
            Ctx.isConst(Ctx.operand(R, 0)) && Ctx.isVar(Ctx.operand(R, 1)));
  };
  if (llvm::all_of(Ctx.operands(E), Finished))
    return E;
  auto Stop = [&]() {
    Rep.BudgetExhausted = true;
    return E;
  };
  llvm::DenseSet<uint32_t> Seen;
  llvm::SmallVector<SymRef, 16> Pending{E};
  llvm::SmallVector<uint32_t, 16> Order;
  while (!Pending.empty()) {
    SymRef R = Pending.pop_back_val();
    if (!Budget.consume())
      return Stop();
    if (Known(R).isValid() || Seen.contains(R.index()))
      continue;
    const size_t Children = Ctx.numOperands(R);
    // Charge cumulative scratch and memo storage, including edge slots,
    // before extending either collection.
    constexpr size_t NodeBytes = 128;
    constexpr size_t EdgeBytes = 4 * sizeof(SymRef);
    if (Storage < NodeBytes || Children > (Storage - NodeBytes) / EdgeBytes)
      return Stop();
    if (!Budget.consume(Children))
      return Stop();
    // Both the pending and rebuilt-operand buffers can retain spare capacity.
    Storage -= NodeBytes + Children * EdgeBytes;
    Seen.insert(R.index());
    Order.push_back(R.index());
    Pending.append(Ctx.operands(R).begin(), Ctx.operands(R).end());
  }
  llvm::sort(Order);
  for (uint32_t Index : Order) {
    SymRef R(Index);
    llvm::SmallVector<SymRef, 8> Ops;
    bool Changed = false;
    for (SymRef Child : Ctx.operands(R)) {
      auto It = Solved.find(Child.index());
      SymRef Next = It == Solved.end() ? Child : It->second;
      Changed |= Next != Child;
      Ops.push_back(Next);
    }
    SymRef Rebuilt = Changed ? Ctx.rebuild(R, Ops) : R;
    SymRef Best =
        readingScore(Ctx, Rebuilt) <= readingScore(Ctx, R) ? Rebuilt : R;
    // The final region can restore a product into an additive bitwise
    // relation without having measured that new root. Finish that exposed
    // relation once within this fixed frontier, even if its children stayed
    // unchanged. Already-known roots and finished children returned above;
    // expressions emitted by this reading are not added to the frontier.
    const bool ExposedRoot = R == E && Ctx.op(Rebuilt) == SymOp::Add &&
                             hasBitwiseInteraction(Ctx, Rebuilt);
    if ((R != E || Changed || ExposedRoot) && canMeasureAtRoot(Ctx, Rebuilt)) {
      if (Budget.exhausted() || !Budget.consume(Ctx.dagSize(Rebuilt)))
        return Stop();
      SolveReport Local;
      SymRef Measured = solveRegionOrSplit(Ctx, Rebuilt, Opts, Budget, Local);
      Rep.BudgetExhausted |= Local.BudgetExhausted;
      if (readingScore(Ctx, Measured) < readingScore(Ctx, Best)) {
        Best = Measured;
        Rep.NumAtoms = std::max(Rep.NumAtoms, Local.NumAtoms);
        if (Local.Evidence == MBAEvidence::Samples)
          Rep.Evidence = MBAEvidence::Samples;
      }
      if (Budget.exhausted())
        return Stop();
    }
    Solved[Index] = Best;
  }
  SymRef Best = Solved.lookup(E.index());
  return readingScore(Ctx, Best) < readingScore(Ctx, E) ? Best : E;
}

bool isDirectComplement(const SymContext &Ctx, SymRef A, SymRef B) {
  return (Ctx.op(A) == SymOp::Not && Ctx.operand(A, 0) == B) ||
         (Ctx.op(B) == SymOp::Not && Ctx.operand(B, 0) == A);
}

bool isBitwiseComplement(const SymContext &Ctx, SymRef A, SymRef B) {
  if (isDirectComplement(Ctx, A, B))
    return true;
  if (Ctx.op(A) == SymOp::And)
    std::swap(A, B);
  if (Ctx.op(A) != SymOp::Or || Ctx.op(B) != SymOp::And)
    return false;
  llvm::ArrayRef<SymRef> OrTerms = Ctx.operands(A);
  llvm::ArrayRef<SymRef> AndTerms = Ctx.operands(B);
  if (OrTerms.size() != AndTerms.size() || OrTerms.size() > 8)
    return false;
  return llvm::all_of(OrTerms, [&](SymRef R) {
    return llvm::any_of(
        AndTerms, [&](SymRef S) { return isDirectComplement(Ctx, R, S); });
  });
}

SymRef foldComplementaryAdd(SymContext &Ctx, SymRef R) {
  if (Ctx.op(R) != SymOp::Add)
    return R;
  llvm::ArrayRef<SymRef> Terms = Ctx.operands(R);
  llvm::APInt Offset(Ctx.width(R), 0);
  SymRef A, B;
  for (SymRef Term : Terms) {
    if (Ctx.isConst(Term)) {
      Offset += Ctx.constValue(Term);
    } else if (!A.isValid()) {
      A = Term;
    } else if (!B.isValid()) {
      B = Term;
    } else {
      return R;
    }
  }
  if (!A.isValid() || !B.isValid())
    return R;
  if (isBitwiseComplement(Ctx, A, B))
    return Ctx.mkConst(Offset - llvm::APInt(Ctx.width(R), 1));
  SymRef Partitioned = foldPartitionedMaskSum(Ctx, A, B, Offset);
  return Partitioned.isValid() ? Partitioned : R;
}

SymRef tryFastPartitionedSum(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                             WorkBudget &Budget, SolveReport &Rep) {
  if (Ctx.op(E) != SymOp::Add || Opts.MaxTableBytes < 4096 ||
      !Budget.canConsume(16))
    return E;
  llvm::ArrayRef<SymRef> Terms = Ctx.operands(E);
  if (Terms.size() < 2 || Terms.size() > 3)
    return E;
  SymRef A, B;
  llvm::APInt Offset(Ctx.width(E), 0);
  for (SymRef Term : Terms) {
    if (Ctx.isConst(Term)) {
      Offset += Ctx.constValue(Term);
    } else if (!A.isValid()) {
      A = Term;
    } else if (!B.isValid()) {
      B = Term;
    } else {
      return E;
    }
  }
  if (!A.isValid() || !B.isValid() || Ctx.op(A) != SymOp::And ||
      Ctx.op(B) != SymOp::And || Ctx.numOperands(A) != 2 ||
      Ctx.numOperands(B) != 2)
    return E;
  llvm::ArrayRef<SymRef> AF = Ctx.operands(A), BF = Ctx.operands(B);
  bool Shared = false;
  for (SymRef F : AF)
    Shared |= llvm::is_contained(BF, F);
  if (!Shared)
    return E;

  // The pair is a plausible partition. Bound the linear matcher and its
  // scratch before considering it, including on the deep walk's first visit.
  const size_t Nodes = Ctx.dagSize(E);
  if (Nodes > Opts.MaxTableBytes / 64 ||
      Nodes > std::numeric_limits<size_t>::max() / 4 ||
      !Budget.canConsume(4 * Nodes))
    return E;
  Budget.consume(4 * Nodes);
  SymRef Fast = foldPartitionedMaskSum(Ctx, A, B, Offset);
  if (!Fast.isValid() || readingScore(Ctx, Fast) >= readingScore(Ctx, E))
    return E;
  Rep.Outcome = MBAOutcome::Rewritten;
  Rep.Evidence = MBAEvidence::Derivation;
  return Fast;
}

} // namespace

SymRef detail::completeComplementarySums(SymContext &Ctx, SymRef Root,
                                         const MBAOptions &Opts,
                                         WorkBudget &Budget) {
  // This runs only on a completed answer. A local builder rule would change
  // earlier search choices, while a second region search would repeat them.
  constexpr size_t MaxNodes = 64;
  constexpr size_t MaxEdges = 128;
  const size_t Cost = readingCost(Ctx, Root);
  if (Cost > MaxNodes || Opts.MaxTableBytes < 4096 ||
      !Budget.canConsume(2 * MaxNodes + MaxEdges))
    return Root;
  llvm::SmallVector<uint32_t, MaxNodes> Order;
  llvm::SmallVector<SymRef, MaxEdges> Work{Root};
  size_t Edges = 0;
  while (!Work.empty()) {
    SymRef R = Work.pop_back_val();
    if (llvm::is_contained(Order, R.index()))
      continue;
    if (Order.size() == MaxNodes)
      return Root;
    Order.push_back(R.index());
    llvm::ArrayRef<SymRef> Ops = Ctx.operands(R);
    if (Ops.size() > MaxEdges - Edges)
      return Root;
    Edges += Ops.size();
    Work.append(Ops.begin(), Ops.end());
  }
  llvm::sort(Order);
  Budget.consume(2 * Order.size() + Edges);

  bool HasPair = false;
  for (uint32_t Index : Order)
    if (foldComplementaryAdd(Ctx, SymRef(Index)) != SymRef(Index)) {
      HasPair = true;
      break;
    }
  if (!HasPair)
    return Root;

  llvm::DenseMap<uint32_t, SymRef> Done;
  for (uint32_t Index : Order) {
    SymRef R(Index);
    llvm::ArrayRef<SymRef> OldOps = Ctx.operands(R);
    llvm::SmallVector<SymRef, 8> NewOps;
    bool Changed = false;
    for (SymRef Child : OldOps) {
      SymRef Replaced = Done.lookup(Child.index());
      if (!Replaced.isValid())
        Replaced = Child;
      Changed |= Replaced != Child;
      NewOps.push_back(Replaced);
    }
    SymRef Rebuilt = Changed ? Ctx.rebuild(R, NewOps) : R;
    SymRef Folded = foldComplementaryAdd(Ctx, Rebuilt);
    if (Folded != R)
      Done[Index] = Folded;
  }
  SymRef Answer = Done.lookup(Root.index());
  return Answer.isValid() && readingScore(Ctx, Answer) < readingScore(Ctx, Root)
             ? Answer
             : Root;
}

namespace {

SymRef finishCompletedSums(SymContext &Ctx, SymRef Root, const MBAOptions &Opts,
                           WorkBudget &Budget, SolveReport &Rep) {
  SymRef Completed = completeComplementarySums(Ctx, Root, Opts, Budget);
  if (Completed == Root || Budget.exhausted())
    return Completed;

  // Removing a partition can expose a new whole-region relation. Read that
  // region once with the remaining budget, then keep only a strict improvement.
  SolveReport Refined;
  SymRef Candidate = solveRegionOrSplit(Ctx, Completed, Opts, Budget, Refined);
  Rep.BudgetExhausted |= Refined.BudgetExhausted;
  if (readingScore(Ctx, Candidate) >= readingScore(Ctx, Completed))
    return Completed;
  Rep.NumAtoms = std::max(Rep.NumAtoms, Refined.NumAtoms);
  if (Rep.Evidence != MBAEvidence::Samples)
    Rep.Evidence = Refined.Evidence;
  return Candidate;
}

} // namespace

const char *mbaOutcomeName(MBAOutcome Outcome) {
  switch (Outcome) {
  case MBAOutcome::NotApplicable:
    return "not-applicable";
  case MBAOutcome::AlreadyShortest:
    return "already-shortest";
  case MBAOutcome::TooManyInputs:
    return "too-many-inputs";
  case MBAOutcome::BudgetExhausted:
    return "budget-exhausted";
  case MBAOutcome::Rewritten:
    return "rewritten";
  }
  llvm_unreachable("unhandled MBA outcome");
}

const char *mbaEvidenceName(MBAEvidence Evidence) {
  switch (Evidence) {
  case MBAEvidence::None:
    return "none";
  case MBAEvidence::Derivation:
    return "derivation";
  case MBAEvidence::Samples:
    return "samples";
  }
  llvm_unreachable("unhandled MBA evidence");
}

MBAResult simplifyMBA(SymContext &Ctx, SymRef E, const MBAOptions &Opts) {
  MBAResult Result;
  Result.Expr = E;
  if (!E.isValid())
    return Result;

  Result.SizeBefore = readingCost(Ctx, E);
  Result.SizeAfter = Result.SizeBefore;
  WorkBudget Budget(Opts.MaxWork);
  if (!Budget.consume(Ctx.dagSize(E))) {
    Result.Work = Budget.used();
    Result.Outcome = MBAOutcome::BudgetExhausted;
    return Result;
  }

  SolveReport Rep;
  Result.Expr = solveRegionOrSplit(Ctx, E, Opts, Budget, Rep);
  if (!Budget.exhausted() && readingCost(Ctx, Result.Expr) > 2) {
    SolveReport Factored;
    SymRef Candidate = solveStructuralFactors(Ctx, E, Opts, Budget, Factored);
    if (readingScore(Ctx, Candidate) < readingScore(Ctx, Result.Expr)) {
      Result.Expr = Candidate;
      Rep.NumAtoms = Factored.NumAtoms;
      Rep.Outcome = Factored.Outcome;
      Rep.Evidence = Factored.Evidence;
    }
    Rep.BudgetExhausted |= Factored.BudgetExhausted;
    if (Result.Expr == E && Factored.BudgetExhausted)
      Rep.Outcome = MBAOutcome::BudgetExhausted;
  }
  if (!Budget.exhausted()) {
    SolveReport Final;
    SymRef Factored =
        solveCoefficientFactors(Ctx, Result.Expr, Opts, Budget, Final);
    if (Factored != Result.Expr) {
      Result.Expr = Factored;
      Rep.NumAtoms = std::max(Rep.NumAtoms, Final.NumAtoms);
      Rep.Outcome = MBAOutcome::Rewritten;
      if (Rep.Evidence != MBAEvidence::Samples)
        Rep.Evidence = Final.Evidence;
    } else if (Final.BudgetExhausted && Result.Expr == E) {
      Rep.Outcome = MBAOutcome::BudgetExhausted;
    }
  }
  if (Result.Expr != E && !Budget.exhausted())
    Result.Expr = finishCompletedSums(Ctx, Result.Expr, Opts, Budget, Rep);
  if (Result.Expr != E && !Opts.AllowGrowth &&
      !doesNotGrow(Ctx, Result.Expr, E)) {
    Result.Expr = E;
    Rep.Outcome = Budget.exhausted() ? MBAOutcome::BudgetExhausted
                                     : MBAOutcome::AlreadyShortest;
    Rep.Evidence = MBAEvidence::None;
  }
  Result.Work = Budget.used();
  Result.NumAtoms = Rep.NumAtoms;
  Result.Outcome = Rep.Outcome;
  Result.Evidence = Rep.Evidence;
  if (Result.Expr == E)
    return Result;

  Result.SizeAfter = readingCost(Ctx, Result.Expr);
  Result.Changed = true;
  return Result;
}

MBAResult simplifyMBADeep(SymContext &Ctx, SymRef E, const MBAOptions &Opts) {
  MBAResult Result;
  Result.Expr = E;
  if (!E.isValid())
    return Result;
  Result.SizeBefore = readingCost(Ctx, E);
  Result.SizeAfter = Result.SizeBefore;
  WorkBudget Budget(Opts.MaxWork);
  SolveReport FastRep;
  if (SymRef Fast = tryFastPartitionedSum(Ctx, E, Opts, Budget, FastRep);
      Fast != E) {
    Result.Expr = Fast;
    Result.SizeAfter = readingCost(Ctx, Fast);
    Result.Changed = true;
    Result.Outcome = MBAOutcome::Rewritten;
    Result.Evidence = MBAEvidence::Derivation;
    Result.Work = Budget.used();
    return Result;
  }

  // Walking children before parents means that by the time a node is reached,
  // everything below it has already been shortened, so each layer is measured
  // over the result of the one beneath rather than over the obfuscation that
  // was wrapped around it.  It also keeps the walk iterative: obfuscated
  // expressions nest deeply enough that recursion is a real hazard.
  const std::vector<uint32_t> Order = reachableInOrder(Ctx, E);
  llvm::DenseMap<uint32_t, SymRef> Solved;
  bool Skipped = false;
  size_t RefinementStorage = Opts.MaxTableBytes;
  auto Remember = [&](uint32_t Index, SymRef Value, bool Complete) {
    Solved[Index] = Value;
    // A replacement root was also visited by this layer. Remember it under
    // its own identity so a later candidate does not measure that same root
    // again merely because it was emitted under another original node.
    constexpr size_t EntryBytes = 128;
    if (Complete && !Budget.exhausted() && !Solved.contains(Value.index()) &&
        RefinementStorage >= EntryBytes && Budget.consume()) {
      RefinementStorage -= EntryBytes;
      Solved.try_emplace(Value.index(), Value);
    }
  };
  // The weakest evidence any layer rested on is what the whole answer rests on,
  // because the layers above were measured over what it produced.
  bool AnySampled = false;

  for (uint32_t Index : Order) {
    SymRef R(Index);
    llvm::ArrayRef<SymRef> Ops = Ctx.operands(R);

    SymRef Rebuilt = R;
    if (!Ops.empty()) {
      llvm::SmallVector<SymRef, 8> NewOps;
      NewOps.reserve(Ops.size());
      bool Changed = false;
      for (SymRef C : Ops) {
        SymRef S = Solved.lookup(C.index());
        Changed |= S != C;
        NewOps.push_back(S);
      }
      if (Changed)
        Rebuilt = Ctx.rebuild(R, NewOps);
    }

    if (!canMeasureAtRoot(Ctx, Rebuilt)) {
      Solved[Index] = Rebuilt;
      continue;
    }

    // Do not compute a subtree size after the budget has gone: the argument to
    // consume() would otherwise repeat the very traversal the exhausted budget
    // is meant to stop paying for at every remaining node.
    bool Complete = false;
    if (!Budget.exhausted() && Budget.consume(Ctx.dagSize(Rebuilt))) {
      const bool ChildrenChanged = Rebuilt != R;
      SolveReport Rep;
      SymRef Measured = solveRegionOrSplit(Ctx, Rebuilt, Opts, Budget, Rep);
      Complete = !Rep.BudgetExhausted;
      if (Measured != Rebuilt) {
        SolveReport Refined;
        Rebuilt = refineCandidate(Ctx, Measured, Opts, Order, Solved, Budget,
                                  RefinementStorage, Refined);
        Result.NumAtoms = std::max(Result.NumAtoms, Rep.NumAtoms);
        Result.NumAtoms = std::max(Result.NumAtoms, Refined.NumAtoms);
        AnySampled |= Rep.Evidence == MBAEvidence::Samples;
        AnySampled |= Refined.Evidence == MBAEvidence::Samples;
        Skipped |= Refined.BudgetExhausted;
        Complete &= !Refined.BudgetExhausted;
      } else if (Rep.Outcome == MBAOutcome::BudgetExhausted) {
        Skipped = true;
      } else if (Rep.Outcome == MBAOutcome::TooManyInputs &&
                 Result.Outcome == MBAOutcome::NotApplicable) {
        Result.Outcome = MBAOutcome::TooManyInputs;
      } else if (Rep.Outcome == MBAOutcome::AlreadyShortest &&
                 Result.Outcome == MBAOutcome::NotApplicable) {
        Result.Outcome = MBAOutcome::AlreadyShortest;
      }
      // A shorter child can hide arithmetic behind an opaque operator. Keep
      // the original region as a second exact reading before discarding it.
      // The main reading gets first use of the shared work budget.
      SolveReport OriginalRep;
      const bool WholeRegion =
          ChildrenChanged &&
          (hasBitwiseInteraction(Ctx, R) || hasBitwiseProductSum(Ctx, R));
      SymRef Original = R;
      if (ChildrenChanged && !Budget.exhausted())
        Original = WholeRegion
                       ? solveRegionOrSplit(Ctx, R, Opts, Budget, OriginalRep)
                       : solveArithmetic(Ctx, R, Opts, Budget, OriginalRep);
      Skipped |= OriginalRep.BudgetExhausted;
      Complete &= !OriginalRep.BudgetExhausted;
      if (Original != R && !Budget.exhausted()) {
        SolveReport Refined;
        Original = refineCandidate(Ctx, Original, Opts, Order, Solved, Budget,
                                   RefinementStorage, Refined);
        OriginalRep.NumAtoms = std::max(OriginalRep.NumAtoms, Refined.NumAtoms);
        if (Refined.Evidence == MBAEvidence::Samples)
          OriginalRep.Evidence = MBAEvidence::Samples;
        Skipped |= Refined.BudgetExhausted;
        Complete &= !Refined.BudgetExhausted;
      }
      if (Original != R &&
          readingScore(Ctx, Original) < readingScore(Ctx, Rebuilt)) {
        // Arithmetic cancellation may expose a fresh linear MBA. It was not
        // in the original postorder, so finish that region before selecting it.
        SolveReport Refined;
        if (!WholeRegion && !Budget.exhausted())
          Original = solveRegionOrSplit(Ctx, Original, Opts, Budget, Refined);
        Rebuilt = Original;
        Result.NumAtoms =
            std::max({Result.NumAtoms, OriginalRep.NumAtoms, Refined.NumAtoms});
        AnySampled |= Refined.Evidence == MBAEvidence::Samples;
        AnySampled |= OriginalRep.Evidence == MBAEvidence::Samples;
        Skipped |= Refined.BudgetExhausted;
        Complete &= !Refined.BudgetExhausted;
      }
    } else {
      Skipped = true;
    }
    if (!Budget.exhausted() && readingCost(Ctx, Rebuilt) > 2) {
      SolveReport Factored;
      // Use the original node: a child rewrite may already have expanded one
      // occurrence of a complemented factor while leaving its products whole.
      SymRef Candidate = solveStructuralFactors(Ctx, R, Opts, Budget, Factored);
      if (readingScore(Ctx, Candidate) < readingScore(Ctx, Rebuilt)) {
        Rebuilt = Candidate;
        Result.NumAtoms = std::max(Result.NumAtoms, Factored.NumAtoms);
      }
      Skipped |= Factored.BudgetExhausted;
      Complete &= !Factored.BudgetExhausted;
    }
    Remember(Index, Rebuilt, Complete);
  }

  SymRef Out = Solved.lookup(E.index());
  if (!Budget.exhausted()) {
    SolveReport Final;
    Out = solveCoefficientFactors(Ctx, Out, Opts, Budget, Final);
    Result.NumAtoms = std::max(Result.NumAtoms, Final.NumAtoms);
    AnySampled |= Final.Evidence == MBAEvidence::Samples;
    Skipped |= Final.BudgetExhausted;
  }
  if (Out != E && !Budget.exhausted()) {
    SolveReport Completed;
    Out = finishCompletedSums(Ctx, Out, Opts, Budget, Completed);
    Result.NumAtoms = std::max(Result.NumAtoms, Completed.NumAtoms);
    AnySampled |= Completed.Evidence == MBAEvidence::Samples;
    Skipped |= Completed.BudgetExhausted;
  }
  Result.Work = Budget.used();

  // Every layer refused to grow and every layer was checked on its own, but
  // what the caller receives is the composition of all of them, and that is
  // what its options were about.  Applying both gates once more over the whole
  // rewrite costs a cached cost lookup and the sample count that was asked
  // for, and it is the only place either gate sees the expression the caller
  // actually handed over rather than a layer of it.
  bool Kept = Out != E;
  if (Kept && !Opts.AllowGrowth && !doesNotGrow(Ctx, Out, E))
    Kept = false;
  if (Kept) {
    const bool Verified = agreeOnSamples(Ctx, E, Out, Opts.VerifySamples);
    assert(Verified && "a layered MBA rewrite disagreed with what it replaces");
    Kept = Verified;
  }
  if (!Kept) {
    // Running out of budget with regions left unvisited is the one refusal that
    // says nothing about the expression, so it outranks the others.
    if (Skipped)
      Result.Outcome = MBAOutcome::BudgetExhausted;
    return Result;
  }

  Result.Expr = Out;
  Result.SizeAfter = readingCost(Ctx, Out);
  Result.Changed = true;
  Result.Outcome = MBAOutcome::Rewritten;
  Result.Evidence = AnySampled ? MBAEvidence::Samples : MBAEvidence::Derivation;
  return Result;
}

} // namespace neverd::symbolic
