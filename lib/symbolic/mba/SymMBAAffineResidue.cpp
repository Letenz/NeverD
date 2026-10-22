//===- SymMBAAffineResidue.cpp - Normalize modular factor residues --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Remove fixed powers of two and odd units from affine factors before
/// proving a product zero. Bounded low-bit prefixes give conservative
/// valuations; complete small-domain tables can also synthesize nonzero
/// products after checking every coefficient of their common affine base.
///
//===----------------------------------------------------------------------===//

#include "Limits.h"
#include "SymMBADetail.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace neverd::symbolic::detail {
namespace {

using Atom = std::vector<uint32_t>;
using Coefficients = std::map<Atom, llvm::APInt>;

struct Factor {
  Coefficients Terms;
  llvm::APInt Offset;
  unsigned Zeros = 0;
  bool FixedValuation = false;
};

// Keep the existing unit-offset proof cheap. This bounded hint does not
// inspect bitwise interiors or grant any equality.
bool hasScaledOffsets(const SymContext &Ctx, SymRef Root) {
  unsigned Scaled = 0;
  for (auto F : Ctx.operands(Root)) {
    if (Ctx.op(F) != SymOp::Add ||
        Ctx.numOperands(F) > limits::kMaxResidueBaseTerms ||
        !Ctx.isConst(Ctx.operand(F, 0)))
      continue;
    for (auto T : Ctx.operands(F)) {
      if (Ctx.op(T) == SymOp::Mul && Ctx.isConst(Ctx.operand(T, 0))) {
        if (++Scaled == limits::kMinAffineResidueScaledFactors)
          return true;
        break;
      }
    }
  }
  return false;
}

struct Resources {
  WorkBudget &Budget;
  size_t &Storage;
  size_t Words;
  size_t SourceWords;

  bool charge(size_t Work, size_t Bytes = 0) {
    if (Bytes > Storage || !Budget.consume(Work))
      return false;
    Storage -= Bytes;
    return true;
  }

  bool scalar(size_t Work) {
    // Charge copies and temporaries cumulatively, even after scratch dies.
    return charge(Work,
                  4 * (sizeof(llvm::APInt) + SourceWords * sizeof(uint64_t)));
  }
};

bool readFactor(const SymContext &Ctx, SymRef R, const llvm::APInt &Scale,
                Factor &Out, Resources &Res, size_t &Edges,
                unsigned Depth = 0) {
  if (++Edges > limits::kMaxAffineResidueEdges || !Res.scalar(1 + Res.Words))
    return false;
  if (Ctx.isConst(R)) {
    if (!Res.charge(Scale.isOne() ? Res.Words
                                  : Res.Words * Res.Words + Res.Words))
      return false;
    auto Value = Ctx.constValue(R).zextOrTrunc(Scale.getBitWidth());
    if (!Scale.isOne())
      Value *= Scale;
    Out.Offset += Value;
    return true;
  }
  if (Depth < limits::kMaxAffineResidueDepth && Ctx.op(R) == SymOp::Add &&
      Ctx.numOperands(R) <= limits::kMaxResidueBaseTerms) {
    for (auto Term : Ctx.operands(R))
      if (!readFactor(Ctx, Term, Scale, Out, Res, Edges, Depth + 1))
        return false;
    return true;
  }

  auto Coeff = Scale;
  auto Ops = Ctx.operands(R);
  const bool Product =
      Ctx.op(R) == SymOp::Mul && Ops.size() <= limits::kMaxResidueBaseTerms;
  if (Product && Ctx.isConst(Ops.front())) {
    auto Value = Ctx.constValue(Ops.front()).zextOrTrunc(Scale.getBitWidth());
    if (Scale.isOne()) {
      if (!Res.charge(Res.Words))
        return false;
      Coeff = std::move(Value);
    } else {
      if (!Res.charge(Res.Words))
        return false;
      const bool Small = Value.getActiveBits() <= 64;
      if (!Res.charge(Small ? 2 * Res.Words : Res.Words * Res.Words))
        return false;
      Coeff = Small ? Scale * Value.getZExtValue() : Scale * Value;
    }
    Ops = Ops.drop_front();
    if (Ops.size() == 1 && Depth < limits::kMaxAffineResidueDepth)
      return readFactor(Ctx, Ops.front(), Coeff, Out, Res, Edges, Depth + 1);
  }
  const size_t Count = Product ? Ops.size() : 1;
  const size_t Bytes = sizeof(Coefficients::value_type) + 4 * sizeof(void *) +
                       Count * sizeof(uint32_t) + Res.Words * sizeof(uint64_t);
  if (!Res.charge(Count * (1 + std::bit_width(limits::kMaxResidueBaseTerms)) +
                      Res.Words,
                  Bytes))
    return false;
  Atom Key;
  Key.reserve(Count);
  if (Product) {
    for (auto Op : Ops)
      Key.push_back(Op.index());
  } else {
    Key.push_back(R.index());
  }
  // A multi-element atom is the complete flattened product of its handles;
  // a singleton is that exact opaque expression. No new DAG nodes are built.
  auto [It, Inserted] = Out.Terms.try_emplace(std::move(Key), Coeff);
  if (!Inserted)
    It->second += Coeff;
  if (It->second.isZero())
    Out.Terms.erase(It);
  return Out.Terms.size() <= limits::kMaxResidueBaseTerms;
}

// Nonzero neighbors can still be much simpler functions of one affine word.
// Check every coefficient before using that word, then enumerate its complete
// observed domain. Independent opaque atoms need not be independent inputs.
SymRef synthesizeResidueTable(SymContext &Ctx, SymRef Root,
                              const std::vector<Factor> &Factors,
                              unsigned Observed, Resources &Res) {
  if (Observed > limits::kMaxAffineResidueTableBits)
    return Root;
  const Factor *Basis = nullptr;
  const Atom *Pivot = nullptr;
  unsigned Shift = Ctx.width(Root);
  for (const auto &F : Factors)
    for (const auto &[Key, Coeff] : F.Terms) {
      if (!Res.charge(Res.Words))
        return Root;
      const unsigned Zeros = Coeff.countr_zero() - F.Zeros;
      if (Zeros < Shift) {
        Shift = Zeros;
        Basis = &F;
        Pivot = &Key;
      }
    }
  if (!Basis || Shift >= Observed)
    return Root;
  const uint64_t Mask = (uint64_t(1) << Observed) - 1;
  auto Low = [&](const llvm::APInt &V, unsigned Zeros) {
    return V.lshr(Zeros).zextOrTrunc(Observed).getZExtValue();
  };
  if (!Res.scalar(Res.Words))
    return Root;
  const uint64_t Odd = Low(Basis->Terms.at(*Pivot), Basis->Zeros + Shift);
  uint64_t Inverse = 1;
  for (unsigned Bits = 1; Bits < Observed; Bits *= 2) {
    if (!Res.charge(3))
      return Root;
    Inverse *= 2 - Odd * Inverse;
  }
  if ((Odd * Inverse & Mask) != 1)
    return Root;
  std::map<Atom, uint64_t> Base;
  for (const auto &[Key, Coeff] : Basis->Terms) {
    if (!Res.scalar(Res.Words) ||
        !Res.charge(Key.size(), sizeof(decltype(Base)::value_type) +
                                    4 * sizeof(void *) +
                                    Key.size() * sizeof(uint32_t)))
      return Root;
    const auto C = Low(Coeff, Basis->Zeros + Shift) * Inverse & Mask;
    if (C)
      Base.emplace(Key, C);
  }
  std::vector<std::pair<uint64_t, uint64_t>> Rows;
  if (!Res.charge(Factors.size(), Factors.size() * sizeof(Rows.front())))
    return Root;
  Rows.reserve(Factors.size());
  for (const auto &F : Factors) {
    if (!Res.scalar(Res.Words))
      return Root;
    auto It = F.Terms.find(*Pivot);
    const uint64_t A = It == F.Terms.end() ? 0 : Low(It->second, F.Zeros);
    // Base's pivot is exactly one. Compare the complete union of atom keys;
    // absent terms and extra nonzero terms must also agree modulo Observed.
    for (const auto &[Key, C] : Base) {
      if (!Res.scalar(Res.Words +
                      Key.size() * (1 + std::bit_width(F.Terms.size()))))
        return Root;
      auto Term = F.Terms.find(Key);
      const auto Actual =
          Term == F.Terms.end() ? 0 : Low(Term->second, F.Zeros);
      if ((A * C & Mask) != Actual)
        return Root;
    }
    for (const auto &[Key, C] : F.Terms) {
      if (!Res.scalar(Res.Words +
                      Key.size() * (1 + std::bit_width(Base.size()))))
        return Root;
      if (!Base.contains(Key) && Low(C, F.Zeros))
        return Root;
    }
    Rows.emplace_back(A, Low(F.Offset, F.Zeros));
  }

  const size_t Count = size_t(1) << Observed;
  // Reserve all table, decision, lookup, polynomial and factored candidates
  // before emission. Output constants retain the source word width even when
  // the coefficient proof only needed a narrow prefix.
  const size_t NodeBytes = sizeof(SymNode) + 4 * sizeof(SymRef) +
                           sizeof(llvm::APInt) +
                           Res.SourceWords * sizeof(uint64_t);
  if (Res.Storage / Count < sizeof(uint64_t) ||
      NodeBytes > (Res.Storage / Count - sizeof(uint64_t)) / 20 ||
      !Res.charge(Count * (3 * Rows.size() + Observed + 20),
                  Count * (sizeof(uint64_t) + 20 * NodeBytes)) ||
      !Res.charge(Ctx.dagSize(Root) + 10 * Rows.size(),
                  2 * Rows.size() * sizeof(SymRef) +
                      (Observed + 1) * sizeof(uint64_t)))
    return Root;
  std::vector<uint64_t> Values(Count, 1);
  for (size_t X = 0; X < Count; ++X)
    for (auto [A, B] : Rows)
      Values[X] = Values[X] * (A * X + B) & Mask;

  const unsigned Width = Ctx.width(Root);
  llvm::SmallVector<SymRef, 16> Terms;
  for (const auto &[Key, C] : Base) {
    llvm::SmallVector<SymRef, 16> Ops;
    for (auto Id : Key)
      Ops.push_back(SymRef(Id));
    auto Coeff = Ctx.mkConst(llvm::APInt(Observed, C).sextOrTrunc(Width));
    Terms.push_back(Ctx.mkMul(Coeff, Ctx.mkMul(Ops)));
  }
  const auto Word = Ctx.mkAdd(Terms);
  // Remove repeated periods and a common low-bit support condition. The
  // remaining output bits often fit in one native-width lookup constant.
  size_t Period = Count;
  while (Period > 1 && std::equal(Values.begin(), Values.begin() + Period / 2,
                                  Values.begin() + Period / 2))
    Period /= 2;
  size_t First = 0;
  while (First < Period && !Values[First])
    ++First;
  if (First == Period)
    return Ctx.mkZero(Width);
  unsigned Prefix = 0;
  while ((size_t(1) << (Prefix + 1)) <= Period &&
         (size_t(1) << (Prefix + 1)) <= Width) {
    const size_t LowMask = (size_t(1) << (Prefix + 1)) - 1;
    bool Same = true;
    for (size_t I = 0; I < Period; ++I)
      Same &= !Values[I] || (I & LowMask) == (First & LowMask);
    if (!Same)
      break;
    ++Prefix;
  }
  // Each output bit can have a smaller period than the whole word. Native
  // lookup words may widen to 64 bits so byte-sized inputs need not expand
  // a 64-entry bit table into dozens of arithmetic selectors.
  llvm::SmallVector<SymRef, 8> OutputBits;
  const unsigned LookupWidth = std::max(Width, 64u);
  bool Packed = true;
  for (unsigned Bit = 0; Bit < Observed; ++Bit) {
    size_t BitPeriod = Period;
    auto BitAt = [&](size_t I) { return (Values[I] >> Bit) & 1; };
    while (BitPeriod > 1) {
      bool Same = true;
      for (size_t I = 0; I < BitPeriod / 2; ++I)
        Same &= BitAt(I) == BitAt(I + BitPeriod / 2);
      if (!Same)
        break;
      BitPeriod /= 2;
    }
    if (BitPeriod > LookupWidth) {
      Packed = false;
      break;
    }
    llvm::APInt Pattern(LookupWidth, 0);
    for (size_t I = 0; I < BitPeriod; ++I)
      if (BitAt(I))
        Pattern.setBit(I);
    if (Pattern.isZero())
      continue;
    auto Index = Ctx.mkZExt(Ctx.mkAnd(Word, Ctx.mkConst(Width, BitPeriod - 1)),
                            LookupWidth);
    auto Value = Ctx.mkZExt(
        Ctx.mkExtract(Ctx.mkLShr(Ctx.mkConst(Pattern), Index), 0, 1), Width);
    OutputBits.push_back(Ctx.mkMul(
        Ctx.mkConst(llvm::APInt(Width, 1).shl(Width - Observed + Bit)), Value));
  }
  SymRef Result = Packed ? Ctx.mkOr(OutputBits) : SymRef{};
  auto Build = [&](auto &&Self, size_t Start, size_t Step,
                   unsigned Bit) -> SymRef {
    const auto V = Values[Start];
    bool Constant = true;
    for (size_t I = Start; I < Count; I += Step)
      Constant &= Values[I] == V;
    if (Constant)
      return Ctx.mkConst(llvm::APInt(Width, V).shl(Width - Observed));
    auto Lo = Self(Self, Start, 2 * Step, Bit + 1);
    auto Hi = Self(Self, Start + Step, 2 * Step, Bit + 1);
    if (Lo == Hi)
      return Lo;
    auto Input = Ctx.mkAnd(Ctx.mkLShr(Word, Ctx.mkConst(Width, Bit)),
                           Ctx.mkConst(Width, 1));
    return Ctx.mkAdd(Lo, Ctx.mkMul(Input, Ctx.mkAdd(Hi, Ctx.mkNeg(Lo))));
  };
  if (!Result)
    Result = Build(Build, 0, 1, 0);
  // On the common support, Word = Support + 2^Prefix*T. Every degree D
  // coefficient then contains 2^(Prefix*D), so higher terms vanish exactly.
  if (Prefix) {
    const size_t Support = First & ((size_t(1) << Prefix) - 1);
    const unsigned Degree = (Observed - 1) / Prefix;
    std::vector<uint64_t> Poly(Degree + 1, 0);
    Poly[0] = 1;
    for (auto [A, B] : Rows) {
      B = A * Support + B;
      A <<= Prefix;
      for (unsigned I = Degree; I; --I)
        Poly[I] = (B * Poly[I] + A * Poly[I - 1]) & Mask;
      Poly[0] = B * Poly[0] & Mask;
    }
    auto T = Ctx.mkLShr(Word, Ctx.mkConst(Width, Prefix));
    auto Polynomial = Ctx.mkZero(Width);
    for (unsigned I = Degree + 1; I--;) {
      auto C = Ctx.mkConst(llvm::APInt(Width, Poly[I]).shl(Width - Observed));
      Polynomial = Ctx.mkAdd(C, Ctx.mkMul(T, Polynomial));
    }
    auto Index = Ctx.mkAnd(Word, Ctx.mkConst(Width, (size_t(1) << Prefix) - 1));
    auto Gate = Ctx.mkAnd(
        Ctx.mkLShr(Ctx.mkConst(llvm::APInt(Width, 1).shl(Support)), Index),
        Ctx.mkConst(Width, 1));
    Polynomial = Ctx.mkMul(Polynomial, Gate);
    if (readingScore(Ctx, Polynomial) < readingScore(Ctx, Result))
      Result = Polynomial;
  }
  llvm::SmallVector<SymRef, 32> Normalized;
  uint64_t Unit = 1;
  for (auto [A, B] : Rows) {
    if (!A) {
      Unit = Unit * B & Mask;
      continue;
    }
    const auto Odd = A >> std::countr_zero(A);
    uint64_t Inv = 1;
    for (unsigned I = 1; I < Observed; I *= 2)
      Inv *= 2 - Odd * Inv;
    Unit = Unit * Odd & Mask;
    auto Slope = Ctx.mkConst(llvm::APInt(Observed, A * Inv).sextOrTrunc(Width));
    auto Offset =
        Ctx.mkConst(llvm::APInt(Observed, B * Inv).sextOrTrunc(Width));
    Normalized.push_back(Ctx.mkAdd(Ctx.mkMul(Slope, Word), Offset));
  }
  Normalized.push_back(
      Ctx.mkConst(llvm::APInt(Width, Unit).shl(Width - Observed)));
  auto Factored = Ctx.mkMul(Normalized);
  return readingScore(Ctx, Result) <
                 std::min(readingScore(Ctx, Root), readingScore(Ctx, Factored))
             ? Result
             : Root;
}

} // namespace

std::optional<SymRef> foldAffineProductResidues(SymContext &Ctx, SymRef Root,
                                                WorkBudget &Budget,
                                                size_t &Storage) {
  if (Ctx.op(Root) != SymOp::Mul ||
      Ctx.numOperands(Root) < limits::kMinResidueProductFactors ||
      Ctx.numOperands(Root) > limits::kMaxResidueProductFactors ||
      !hasScaledOffsets(Ctx, Root))
    return std::nullopt;
  const unsigned Width = Ctx.width(Root);
  const size_t SourceWords = (size_t(Width) - 1) / 64 + 1;
  if (SourceWords > Storage / (4 * sizeof(uint64_t)) ||
      !Budget.consume(SourceWords))
    return Root;
  Storage -= SourceWords * 4 * sizeof(uint64_t);
  auto Ops = Ctx.operands(Root);
  const auto Scale = Ctx.isConst(Ops.front()) ? Ctx.constValue(Ops.front())
                                              : llvm::APInt(Width, 1);
  if (Scale.isZero())
    return Budget.consume() ? Ctx.mkZero(Width) : Root;
  const unsigned ReadWidth = Width - Scale.countr_zero();
  const size_t Words = (size_t(ReadWidth) - 1) / 64 + 1;
  if (Words > (std::numeric_limits<size_t>::max() - Words) / Words ||
      Words > Storage / (4 * sizeof(uint64_t)))
    return Root;
  Resources Res{Budget, Storage, Words, SourceWords};
  const size_t N = Ctx.numOperands(Root);
  if (N > Storage / sizeof(Factor) || !Res.charge(N, N * sizeof(Factor)))
    return Root;
  std::vector<Factor> Factors;
  Factors.reserve(N);
  unsigned Needed = ReadWidth;
  for (auto R : Ctx.operands(Root)) {
    if (!Res.scalar(2 * Words))
      return Root;
    Factor F{{}, llvm::APInt(ReadWidth, 0)};
    size_t Edges = 0;
    if (Ctx.isConst(R))
      F.Offset = Scale.lshr(Width - ReadWidth).zextOrTrunc(ReadWidth);
    else if (!readFactor(Ctx, R, llvm::APInt(ReadWidth, 1), F, Res, Edges))
      return Root;
    unsigned CoeffZeros = ReadWidth;
    for (const auto &[Key, Coeff] : F.Terms) {
      (void)Key;
      if (!Res.charge(Words))
        return Root;
      CoeffZeros = std::min(CoeffZeros, Coeff.countr_zero());
    }
    const unsigned OffsetZeros = F.Offset.countr_zero();
    F.Zeros = std::min(CoeffZeros, OffsetZeros);
    if (F.Zeros >= Needed)
      return Res.charge(1) ? Ctx.mkZero(Width) : Root;
    Needed -= F.Zeros;
    F.FixedValuation = OffsetZeros < CoeffZeros;
    Factors.push_back(std::move(F));
  }

  // Examining only this prefix avoids full-width inversions. It can decline
  // a proof that needs deeper residues; it never overstates a valuation.
  const unsigned Bits = std::min(Needed, unsigned(std::bit_width(N)));
  const uint64_t Mask = ~uint64_t(0) >> (64 - Bits);
  using Groups = std::map<std::vector<uint64_t>, std::vector<llvm::APInt>>;
  Groups Families;
  for (const auto &F : Factors) {
    if (F.FixedValuation)
      continue;
    size_t KeySize = 0;
    for (const auto &[Key, Coeff] : F.Terms) {
      (void)Coeff;
      KeySize += Key.size() + 2;
    }
    const size_t Bytes = 2 * sizeof(Groups::value_type) + 8 * sizeof(void *) +
                         4 * sizeof(llvm::APInt) +
                         2 * KeySize * sizeof(uint64_t);
    if (!Res.charge(KeySize * (1 + std::bit_width(N)) + F.Terms.size() * Words,
                    Bytes))
      return Root;
    uint64_t Pivot = 0;
    for (const auto &[Key, Coeff] : F.Terms) {
      (void)Key;
      auto Low = Coeff.extractBitsAsZExtValue(Bits, F.Zeros);
      if (Low & 1) {
        Pivot = Low;
        break;
      }
    }
    if (!Pivot)
      return Root;
    uint64_t Inverse = 1;
    for (unsigned Known = 1; Known < Bits; Known *= 2) {
      if (!Res.charge(3))
        return Root;
      Inverse *= 2 - Pivot * Inverse;
    }
    // Verify the unit before using its low-bit inverse in a proof key.
    if ((Pivot * Inverse & Mask) != 1)
      return Root;
    std::vector<uint64_t> Key;
    Key.reserve(KeySize);
    for (const auto &[Atom, Coeff] : F.Terms) {
      const uint64_t Low =
          Coeff.extractBitsAsZExtValue(Bits, F.Zeros) * Inverse & Mask;
      if (!Low)
        continue;
      Key.push_back(Atom.size());
      Key.insert(Key.end(), Atom.begin(), Atom.end());
      Key.push_back(Low);
    }
    const uint64_t Offset =
        F.Offset.extractBitsAsZExtValue(Bits, F.Zeros) * Inverse & Mask;
    Families[std::move(Key)].emplace_back(Bits, Offset);
  }
  const unsigned Observed = Needed;
  for (auto &[Key, Offsets] : Families) {
    (void)Key;
    if (Offsets.size() < 2)
      continue;
    auto Zeros = minimumProductResidueZeros(Offsets, Bits, Needed, Budget);
    if (!Zeros)
      return Root;
    Needed -= *Zeros;
    if (!Needed)
      return Res.charge(1) ? Ctx.mkZero(Width) : Root;
  }
  return synthesizeResidueTable(Ctx, Root, Factors, Observed, Res);
}

} // namespace neverd::symbolic::detail
