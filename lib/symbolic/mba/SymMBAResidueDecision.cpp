//===- SymMBAResidueDecision.cpp - Exact bounded low-word decisions -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Construct reduced ordered Boolean decisions for the observed low bits of
/// a scaled product. Every supported operation is evaluated exactly; other
/// words remain independent opaque inputs. Only a complete smaller word is
/// published, and all attempts spend the caller's cumulative resources.
///
//===----------------------------------------------------------------------===//

#include "Limits.h"
#include "SymMBADetail.h"

#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace neverd::symbolic::detail {
namespace {

struct Decision {
  uint64_t Variable;
  uint32_t Low;
  uint32_t High;
  bool operator==(const Decision &) const = default;
};

struct DecisionHash {
  size_t operator()(const Decision &D) const {
    return size_t(llvm::hash_combine(D.Variable, D.Low, D.High));
  }
};

class LowWordDecisions {
  using Bits = llvm::SmallVector<uint32_t, 8>;
  enum Operation : uint64_t { And, Or, Xor };
  // A known parity input precedes the other words. The remaining words keep
  // bit-interleaved order, which bounds the carries of their own products.
  static constexpr uint64_t SecondaryWord = uint64_t(1) << 63;

  static unsigned variableBit(uint64_t Variable) {
    return unsigned((Variable & ~SecondaryWord) >> 32);
  }

  SymContext &Ctx;
  WorkBudget &Budget;
  size_t &Storage;
  size_t MaxWork;
  bool *StorageLimited;
  size_t Work = 0;
  bool Failed = false;
  std::optional<std::pair<uint64_t, bool>> Assumption;
  std::optional<uint32_t> PriorityInput;
  // The low edge bit denotes complement. Negation never duplicates a graph;
  // zero and one are the two edges of the single terminal node.
  std::vector<Decision> Nodes{{UINT64_MAX, 0, 0}};
  std::unordered_map<Decision, uint32_t, DecisionHash> Unique, Applied;
  struct CarryKey {
    uint32_t A, B, C;
    bool operator==(const CarryKey &) const = default;
  };
  struct CarryHash {
    size_t operator()(const CarryKey &K) const {
      return size_t(llvm::hash_combine(K.A, K.B, K.C));
    }
  };
  std::unordered_map<CarryKey, uint32_t, CarryHash> Carries;
  std::unordered_map<uint64_t, Bits> Words;
  std::unordered_map<uint32_t, unsigned> Zeros;
  struct WordOperation {
    SymOp Op;
    Bits A, B;
    bool operator==(const WordOperation &) const = default;
  };
  struct WordOperationHash {
    size_t operator()(const WordOperation &K) const {
      return size_t(llvm::hash_combine(
          K.Op, llvm::hash_combine_range(K.A.begin(), K.A.end()),
          llvm::hash_combine_range(K.B.begin(), K.B.end())));
    }
  };
  std::unordered_map<WordOperation, Bits, WordOperationHash> Arithmetic;

  bool charge(size_t Units = 1, size_t Bytes = 0) {
    if (Failed)
      return false;
    if (Bytes > Storage && StorageLimited)
      *StorageLimited = true;
    if (Units > MaxWork - Work || Bytes > Storage ||
        !Budget.canConsume(Units)) {
      Failed = true;
      return false;
    }
    Work += Units;
    Budget.consume(Units);
    Storage -= Bytes;
    return true;
  }

  Decision decision(uint32_t Edge) const {
    auto D = Nodes[Edge >> 1];
    D.Low ^= Edge & 1;
    D.High ^= Edge & 1;
    return D;
  }

  std::optional<llvm::APInt> literal(SymRef R) {
    const size_t Words = (size_t(Ctx.width(R)) - 1) / 64 + 1;
    if (!charge(Words, sizeof(llvm::APInt) + Words * sizeof(uint64_t)))
      return std::nullopt;
    return Ctx.constValue(R);
  }

  bool isOne(SymRef R) {
    if (!Ctx.isConst(R))
      return false;
    if (Ctx.width(R) <= 64)
      return Ctx.node(R).Aux == 1;
    auto Value = literal(R);
    return Value && Value->isOne();
  }

  uint32_t node(uint64_t Variable, uint32_t Low, uint32_t High) {
    if (!charge() || Low == High)
      return Low;
    if (Assumption && Variable == Assumption->first)
      return Assumption->second ? High : Low;
    const uint32_t Complement = Low & 1;
    Low ^= Complement;
    High ^= Complement;
    const Decision D{Variable, Low, High};
    if (auto It = Unique.find(D); It != Unique.end())
      return It->second ^ Complement;
    if (Nodes.size() >= limits::kMaxResidueDecisionNodes ||
        !charge(1,
                2 * sizeof(Decision) + sizeof(uint32_t) + 6 * sizeof(void *))) {
      Failed = true;
      return 0;
    }
    const auto Id = uint32_t(Nodes.size() << 1);
    Nodes.push_back(D);
    Unique.emplace(D, Id);
    return Id ^ Complement;
  }

  uint32_t apply(Operation Op, uint32_t A, uint32_t B, unsigned Depth = 0) {
    if (!charge())
      return 0;
    uint32_t Complement = 0;
    if (Op == Or) {
      Op = And;
      A ^= 1;
      B ^= 1;
      Complement = 1;
    } else if (Op == Xor) {
      Complement = (A ^ B) & 1;
      A &= ~uint32_t(1);
      B &= ~uint32_t(1);
    }
    if (A > B)
      std::swap(A, B);
    if (Op == And) {
      if ((A ^ B) == 1)
        return Complement;
      if (!A || A == B)
        return A ^ Complement;
      if (A == 1)
        return B ^ Complement;
    } else {
      if (!A)
        return B ^ Complement;
      if (A == B)
        return Complement;
    }
    if (Depth >= limits::kMaxResidueDecisionDepth) {
      Failed = true;
      return 0;
    }
    const Decision Key{Op, A, B};
    if (auto It = Applied.find(Key); It != Applied.end())
      return It->second ^ Complement;
    // Recursive insertion may reallocate Nodes; copy both decisions first.
    const auto Left = decision(A), Right = decision(B);
    const auto Variable = std::min(Left.Variable, Right.Variable);
    const auto Low =
        apply(Op, Left.Variable == Variable ? Left.Low : A,
              Right.Variable == Variable ? Right.Low : B, Depth + 1);
    const auto High =
        apply(Op, Left.Variable == Variable ? Left.High : A,
              Right.Variable == Variable ? Right.High : B, Depth + 1);
    const auto Result = node(Variable, Low, High);
    if (!charge(1, sizeof(Decision) + sizeof(uint32_t) + 4 * sizeof(void *)))
      return 0;
    Applied.emplace(Key, Result);
    return Result ^ Complement;
  }

  Bits calculate(SymOp Op, const Bits &A, const Bits &B) {
    if (!charge(1 + A.size()))
      return Bits(A.size(), 0);
    auto Zero = [](const Bits &Value) {
      return std::all_of(Value.begin(), Value.end(),
                         [](uint32_t V) { return !V; });
    };
    if (Zero(A))
      return Op == SymOp::Add ? B : A;
    if (Zero(B))
      return Op == SymOp::Add ? A : B;
    auto Zeros = [](const Bits &Value) {
      return size_t(std::find_if(Value.begin(), Value.end(),
                                 [](uint32_t V) { return V != 0; }) -
                    Value.begin());
    };
    const size_t ZA = Zeros(A), ZB = Zeros(B);
    const size_t Shift = Op == SymOp::Mul ? ZA + ZB : std::min(ZA, ZB);
    if (Shift) {
      Bits Result(A.size(), 0);
      if (Shift >= A.size())
        return Result;
      const size_t Count = A.size() - Shift;
      const size_t StartA = Op == SymOp::Mul ? ZA : Shift;
      const size_t StartB = Op == SymOp::Mul ? ZB : Shift;
      Bits Left(A.begin() + StartA, A.begin() + StartA + Count);
      Bits Right(B.begin() + StartB, B.begin() + StartB + Count);
      auto Narrow = calculate(Op, Left, Right);
      std::copy(Narrow.begin(), Narrow.end(), Result.begin() + Shift);
      return Result;
    }
    if (Op == SymOp::Add && A == B) {
      Bits Result(A.size(), 0);
      std::copy(A.begin(), A.end() - 1, Result.begin() + 1);
      return Result;
    }
    if (Op == SymOp::Mul) {
      auto One = [](const Bits &V) {
        return V.front() == 1 && std::all_of(V.begin() + 1, V.end(),
                                             [](uint32_t B) { return !B; });
      };
      if (One(A))
        return B;
      if (One(B))
        return A;
    }
    const WordOperation Key =
        A < B ? WordOperation{Op, A, B} : WordOperation{Op, B, A};
    if (auto It = Arithmetic.find(Key); It != Arithmetic.end())
      return It->second;
    Bits Result;
    if (Op == SymOp::Add) {
      Result = add(A, B);
    } else if (A == B) {
      Result = square(A);
    } else {
      size_t Common = 0;
      while (Common < A.size() && A[Common] == B[Common])
        ++Common;
      if (2 * Common >= A.size()) {
        // A=L+2^K*U and B=L+2^K*V. Their unobserved cross product
        // vanishes when 2K covers the observed word. Exact decision identity,
        // not sampled equality, establishes the shared low word L.
        Bits Low(A.size(), 0);
        std::copy(A.begin(), A.begin() + Common, Low.begin());
        Result = calculate(SymOp::Mul, Low, Low);
        Bits UpperA(A.begin() + Common, A.end());
        Bits UpperB(B.begin() + Common, B.end());
        Bits NarrowLow(A.begin(), A.begin() + A.size() - Common);
        auto Upper = calculate(SymOp::Mul, NarrowLow,
                               calculate(SymOp::Add, UpperA, UpperB));
        Bits Shifted(A.size(), 0);
        std::copy(Upper.begin(), Upper.end(), Shifted.begin() + Common);
        Result = calculate(SymOp::Add, Result, Shifted);
      } else {
        Result = multiply(A, B);
      }
    }
    if (!charge(1, sizeof(WordOperation) + sizeof(Bits) + 4 * sizeof(void *)))
      return Bits(A.size(), 0);
    Arithmetic.emplace(Key, Result);
    return Result;
  }

  // A full adder's carry is the majority of its three input bits. Compute
  // that one function directly instead of retaining three temporary graphs.
  uint32_t majority(uint32_t A, uint32_t B, uint32_t C, unsigned Depth = 0) {
    if (!charge())
      return 0;
    if (A == B || A == C)
      return A;
    if (B == C)
      return B;
    if ((A ^ B) == 1)
      return C;
    if ((A ^ C) == 1)
      return B;
    if ((B ^ C) == 1)
      return A;
    if (A > B)
      std::swap(A, B);
    if (B > C)
      std::swap(B, C);
    if (A > B)
      std::swap(A, B);
    if (A <= 1)
      return apply(A ? Or : And, B, C);
    const uint32_t Complement = A & 1;
    A ^= Complement;
    B ^= Complement;
    C ^= Complement;
    const CarryKey Key{A, B, C};
    if (auto It = Carries.find(Key); It != Carries.end())
      return It->second ^ Complement;
    if (Depth >= limits::kMaxResidueDecisionDepth) {
      Failed = true;
      return 0;
    }
    const auto DA = decision(A), DB = decision(B), DC = decision(C);
    const auto V = std::min({DA.Variable, DB.Variable, DC.Variable});
    auto L =
        majority(DA.Variable == V ? DA.Low : A, DB.Variable == V ? DB.Low : B,
                 DC.Variable == V ? DC.Low : C, Depth + 1);
    auto H =
        majority(DA.Variable == V ? DA.High : A, DB.Variable == V ? DB.High : B,
                 DC.Variable == V ? DC.High : C, Depth + 1);
    auto R = node(V, L, H);
    if (!charge(1, sizeof(CarryKey) + sizeof(uint32_t) + 4 * sizeof(void *)))
      return 0;
    Carries.emplace(Key, R);
    return R ^ Complement;
  }

  Bits add(const Bits &A, const Bits &B) {
    Bits Result;
    uint32_t Carry = 0;
    for (size_t I = 0; I < A.size(); ++I) {
      Result.push_back(apply(Xor, apply(Xor, A[I], B[I]), Carry));
      if (I + 1 != A.size())
        Carry = majority(A[I], B[I], Carry);
    }
    return Result;
  }

  Bits square(const Bits &A) {
    Bits Result(A.size(), 0);
    for (size_t I = 0; I < A.size() && !Failed; ++I) {
      Bits Row(A.size(), 0);
      if (2 * I < A.size())
        Row[2 * I] = A[I];
      for (size_t J = I + 1; I + J + 1 < A.size(); ++J)
        Row[I + J + 1] = apply(And, A[I], A[J]);
      Result = calculate(SymOp::Add, Result, Row);
    }
    return Result;
  }

  Bits multiply(const Bits &A, const Bits &B) {
    Bits Result(A.size(), 0);
    for (size_t I = 0; I < B.size() && !Failed; ++I) {
      if (!B[I])
        continue;
      Bits Row(A.size(), 0);
      for (size_t J = 0; I + J < A.size(); ++J)
        Row[I + J] = apply(And, A[J], B[I]);
      Result = calculate(SymOp::Add, Result, Row);
    }
    return Result;
  }

  std::optional<unsigned> lookupWidth(SymRef R) {
    if (Ctx.op(R) != SymOp::LShr || !Ctx.isConst(Ctx.operand(R, 0)))
      return std::nullopt;
    auto Index = Ctx.operand(R, 1);
    if (Ctx.op(Index) == SymOp::ZExt)
      Index = Ctx.operand(Index, 0);
    if (Ctx.op(Index) != SymOp::And || !Ctx.isConst(Ctx.operand(Index, 0)))
      return std::nullopt;
    const auto Mask = literal(Ctx.operand(Index, 0));
    if (!Mask)
      return std::nullopt;
    const unsigned Bits = Mask->getActiveBits();
    return Bits && Bits <= limits::kMaxAffineResidueTableBits
               ? std::optional<unsigned>(Bits)
               : std::nullopt;
  }

  bool transparent(SymRef R, unsigned Count) {
    const auto Op = Ctx.op(R);
    if (Op == SymOp::Add || Op == SymOp::Mul || Op == SymOp::And ||
        Op == SymOp::Or || Op == SymOp::Xor || Op == SymOp::Not ||
        Op == SymOp::ZExt || Op == SymOp::SExt)
      return true;
    if (Op == SymOp::Extract)
      return Ctx.node(R).Aux <= limits::kMaxAffineResidueTableBits - Count;
    if (Op == SymOp::Shl)
      return Ctx.isConst(Ctx.operand(R, 1));
    if (Op != SymOp::LShr)
      return false;
    if (Ctx.isConst(Ctx.operand(R, 1))) {
      const auto Value = literal(Ctx.operand(R, 1));
      if (!Value)
        return false;
      const auto Shift = Value->getLimitedValue();
      return Shift >= Ctx.width(R) ||
             Shift <= limits::kMaxAffineResidueTableBits - Count;
    }
    return lookupWidth(R).has_value();
  }

  unsigned knownZeros(SymRef R, unsigned Depth = 0) {
    if (!charge() || Depth >= limits::kMaxResidueDecisionDepth)
      return 0;
    if (auto It = Zeros.find(R.index()); It != Zeros.end())
      return It->second;
    const unsigned Ceiling =
        std::min(Ctx.width(R), limits::kMaxAffineResidueTableBits);
    const auto Op = Ctx.op(R);
    unsigned Result = 0;
    if (Ctx.isConst(R)) {
      const auto Value = literal(R);
      if (!Value)
        return 0;
      Result = std::min(Ceiling, Value->countr_zero());
    } else if (Op == SymOp::ZExt || Op == SymOp::SExt) {
      Result = knownZeros(Ctx.operand(R, 0), Depth + 1);
    } else if (Op == SymOp::Shl && Ctx.isConst(Ctx.operand(R, 1))) {
      const auto Shift = literal(Ctx.operand(R, 1));
      if (!Shift)
        return 0;
      Result = std::min<uint64_t>(Ceiling,
                                  Shift->getLimitedValue(Ceiling) +
                                      knownZeros(Ctx.operand(R, 0), Depth + 1));
    } else if (Op == SymOp::Add || Op == SymOp::Or || Op == SymOp::Xor) {
      Result = Ceiling;
      for (auto Child : Ctx.operands(R)) {
        Result = std::min(Result, knownZeros(Child, Depth + 1));
        if (!Result)
          break;
      }
    } else if (Op == SymOp::Mul || Op == SymOp::And) {
      for (auto Child : Ctx.operands(R)) {
        const auto ChildZeros = knownZeros(Child, Depth + 1);
        Result = Op == SymOp::Mul ? std::min(Ceiling, Result + ChildZeros)
                                  : std::max(Result, ChildZeros);
        if (Result == Ceiling)
          break;
      }
    }
    if (charge(1, 2 * sizeof(uint32_t) + 4 * sizeof(void *)))
      Zeros.emplace(R.index(), Result);
    return Result;
  }

  // References into unordered_map elements survive recursive insertions.
  // Cached words are immutable until the parity assumption changes; no view
  // escapes that phase. Cache hits pay for lookup, without copying each bit.
  const Bits &word(SymRef R, unsigned Count, unsigned Depth = 0) {
    static const auto EmptyWords = [] {
      std::array<Bits, limits::kMaxAffineResidueTableBits + 1> Result;
      for (unsigned I = 0; I < Result.size(); ++I)
        Result[I].resize(I, 0);
      return Result;
    }();
    const auto &Empty = EmptyWords[Count];
    if (!charge())
      return Empty;
    const uint64_t Key = (uint64_t(R.index()) << 8) | Count;
    if (auto It = Words.find(Key); It != Words.end())
      return It->second;
    if (!charge(Count) || Depth >= limits::kMaxResidueDecisionDepth) {
      Failed = true;
      return Empty;
    }
    const auto Op = Ctx.op(R);
    Bits Result(Count, 0);
    if (Ctx.isConst(R)) {
      const auto Value = literal(R);
      if (!Value)
        return Empty;
      for (unsigned I = 0; I < Count; ++I)
        Result[I] = (*Value)[I];
    } else if (Op == SymOp::ZExt || Op == SymOp::SExt) {
      const auto Child = Ctx.operand(R, 0);
      const auto ChildBits = std::min(Count, Ctx.width(Child));
      const auto &Value = word(Child, ChildBits, Depth + 1);
      std::copy(Value.begin(), Value.end(), Result.begin());
      if (Op == SymOp::SExt)
        std::fill(Result.begin() + ChildBits, Result.end(), Value.back());
    } else if (Op == SymOp::Extract && transparent(R, Count)) {
      const auto Offset = unsigned(Ctx.node(R).Aux);
      const auto &Value = word(Ctx.operand(R, 0), Count + Offset, Depth + 1);
      std::copy(Value.begin() + Offset, Value.end(), Result.begin());
    } else if (Op == SymOp::Not) {
      Result = word(Ctx.operand(R, 0), Count, Depth + 1);
      for (auto &Bit : Result)
        Bit = apply(Xor, Bit, 1);
    } else if (Op == SymOp::Shl && Ctx.isConst(Ctx.operand(R, 1))) {
      const auto Value = literal(Ctx.operand(R, 1));
      if (!Value)
        return Empty;
      const auto Shift = Value->getLimitedValue(Count);
      if (Shift < Count) {
        const auto &Value = word(Ctx.operand(R, 0), Count - Shift, Depth + 1);
        std::copy(Value.begin(), Value.end(), Result.begin() + Shift);
      }
    } else if (Op == SymOp::LShr && transparent(R, Count)) {
      if (Ctx.isConst(Ctx.operand(R, 1))) {
        const auto Value = literal(Ctx.operand(R, 1));
        if (!Value)
          return Empty;
        const auto Shift = Value->getLimitedValue();
        if (Shift < Ctx.width(R)) {
          const unsigned Take = std::min<uint64_t>(Count, Ctx.width(R) - Shift);
          const auto &Value = word(Ctx.operand(R, 0), Take + Shift, Depth + 1);
          std::copy(Value.begin() + Shift, Value.end(), Result.begin());
        }
      } else {
        const auto Bounded = lookupWidth(R);
        if (!Bounded)
          return Empty;
        const unsigned IndexBits = *Bounded;
        const size_t Entries = size_t(1) << IndexBits;
        if (!charge(Count * Entries, Entries * sizeof(uint32_t)))
          return Empty;
        const auto &Index = word(Ctx.operand(R, 1), IndexBits, Depth + 1);
        const auto Pattern = literal(Ctx.operand(R, 0));
        if (!Pattern)
          return Empty;
        for (unsigned I = 0; I < Count && !Failed; ++I) {
          std::vector<uint32_t> Values(Entries);
          for (size_t J = 0; J < Entries; ++J)
            Values[J] = I + J < Pattern->getBitWidth() ? (*Pattern)[I + J] : 0;
          size_t Size = Entries;
          for (unsigned Bit = 0; Bit < IndexBits; ++Bit) {
            for (size_t J = 0; J < Size / 2; ++J) {
              const auto Low = Values[2 * J], High = Values[2 * J + 1];
              Values[J] = apply(Xor, Low,
                                apply(And, Index[Bit], apply(Xor, Low, High)));
            }
            Size /= 2;
          }
          Result[I] = Values.front();
        }
      }
    } else if (Op == SymOp::Add || Op == SymOp::Mul || Op == SymOp::And ||
               Op == SymOp::Or || Op == SymOp::Xor) {
      if (Op == SymOp::Mul)
        Result.front() = 1;
      if (Op == SymOp::And)
        std::fill(Result.begin(), Result.end(), 1);
      // Emission and recursive interning must not invalidate operand views.
      llvm::SmallVector<SymRef, 16> Operands(Ctx.operands(R));
      unsigned TotalZeros = 0;
      llvm::SmallVector<unsigned, 16> FactorZeros;
      if (Op == SymOp::Mul && Count > 1) {
        for (auto Child : Operands) {
          const auto Z = std::min(Count, knownZeros(Child));
          FactorZeros.push_back(Z);
          TotalZeros = std::min(Count, TotalZeros + Z);
        }
        if (Failed)
          return Empty;
        if (TotalZeros == Count)
          return Empty;
      }
      if (TotalZeros) {
        Bits Narrow(Count - TotalZeros, 0);
        Narrow.front() = 1;
        for (size_t I = 0; I < Operands.size(); ++I) {
          const auto Z = FactorZeros[I];
          const auto &Value =
              word(Operands[I], Count - TotalZeros + Z, Depth + 1);
          Narrow = calculate(SymOp::Mul, Narrow,
                             Bits(Value.begin() + Z, Value.end()));
        }
        std::copy(Narrow.begin(), Narrow.end(), Result.begin() + TotalZeros);
        Result.front() = 0;
      } else {
        for (auto Child : Operands) {
          const auto &Value = word(Child, Count, Depth + 1);
          if (Op == SymOp::Add)
            Result = calculate(SymOp::Add, Result, Value);
          else if (Op == SymOp::Mul)
            Result = calculate(SymOp::Mul, Result, Value);
          else
            for (unsigned I = 0; I < Count; ++I)
              Result[I] = apply(Op == SymOp::And  ? And
                                : Op == SymOp::Or ? Or
                                                  : Xor,
                                Result[I], Value[I]);
          if (Failed)
            return Empty;
          if ((Op == SymOp::Mul || Op == SymOp::And) &&
              std::all_of(Result.begin(), Result.end(),
                          [](uint32_t Bit) { return Bit == 0; }))
            break;
          if (Op == SymOp::Or &&
              std::all_of(Result.begin(), Result.end(),
                          [](uint32_t Bit) { return Bit == 1; }))
            break;
        }
      }
    } else {
      // Treat an unsupported word as an exact opaque input, without assuming
      // a relation between it and any other input word or path condition.
      for (unsigned I = 0; I < Count; ++I)
        Result[I] =
            node(((PriorityInput && R.index() != *PriorityInput) ? SecondaryWord
                                                                 : 0) |
                     (uint64_t(I) << 32) | R.index(),
                 0, 1);
    }
    if (!charge(Count, sizeof(uint64_t) + sizeof(Bits) +
                           2 * Count * sizeof(uint32_t) + 4 * sizeof(void *)))
      return Empty;
    return Words.emplace(Key, std::move(Result)).first->second;
  }

  // Count equal and complementary bits before carry propagation. A pair of
  // complements contributes one, while repeated copies move to higher columns.
  Bits sumWords(llvm::ArrayRef<Bits> Values, unsigned Count) {
    using Column = std::map<uint32_t, size_t>;
    if (!charge(Count, Count * sizeof(Column)))
      return Bits(Count, 0);
    std::vector<Column> Columns(Count);
    auto Insert = [&](unsigned Bit, uint32_t Edge) {
      if (!charge())
        return false;
      if (!Edge)
        return true;
      auto &C = Columns[Bit];
      if (!charge(1 + std::bit_width(C.size())))
        return false;
      auto It = C.find(Edge);
      if (It == C.end()) {
        if (!charge(1, sizeof(Column::value_type) + 4 * sizeof(void *)))
          return false;
        It = C.emplace(Edge, 0).first;
      }
      ++It->second;
      return true;
    };
    for (const auto &Value : Values)
      for (unsigned I = 0; I < Count; ++I)
        if (!Insert(I, Value[I]))
          return Bits(Count, 0);
    Bits Left(Count, 0), Right(Count, 0);
    for (unsigned I = 0; I < Count && !Failed; ++I) {
      auto &C = Columns[I];
      if (!charge(C.size(), 2 * C.size() * sizeof(uint32_t)))
        break;
      Bits Remaining;
      Remaining.reserve(C.size());
      size_t Constants = 0;
      for (auto It = C.begin(); It != C.end(); ++It) {
        auto [Id, Copies] = *It;
        if (!charge(1 + std::bit_width(Copies)))
          break;
        if (!Copies)
          continue;
        if (Id == 1) {
          Constants += Copies;
          continue;
        }
        if (!(Id & 1)) {
          auto Other = C.find(Id ^ 1);
          if (Other != C.end()) {
            const auto Common = std::min(Copies, Other->second);
            Copies -= Common;
            Other->second -= Common;
            Constants += Common;
          }
        }
        if (Copies & 1)
          Remaining.push_back(Id);
        Copies >>= 1;
        for (unsigned J = I + 1; Copies && J < Count; ++J, Copies >>= 1)
          if ((Copies & 1) && !Insert(J, Id))
            break;
      }
      if (Constants & 1)
        Remaining.push_back(1);
      Constants >>= 1;
      for (unsigned J = I + 1; Constants && J < Count; ++J, Constants >>= 1)
        if ((Constants & 1) && !Insert(J, 1))
          break;
      while (Remaining.size() > 2 && !Failed) {
        const auto A = Remaining.pop_back_val();
        const auto B = Remaining.pop_back_val();
        const auto C = Remaining.pop_back_val();
        const auto Sum = apply(Xor, apply(Xor, A, B), C);
        if (Sum)
          Remaining.push_back(Sum);
        if (I + 1 < Count && !Insert(I + 1, majority(A, B, C)))
          break;
      }
      if (!Remaining.empty())
        Left[I] = Remaining[0];
      if (Remaining.size() > 1)
        Right[I] = Remaining[1];
    }
    return calculate(SymOp::Add, Left, Right);
  }

  Bits power(Bits Value, size_t Exponent) {
    Bits Term(Value.size(), 0);
    Term.front() = 1;
    for (; Exponent && !Failed; Exponent >>= 1) {
      if (Exponent & 1)
        Term = calculate(SymOp::Mul, Term, Value);
      if (Exponent > 1)
        Value = calculate(SymOp::Mul, Value, Value);
    }
    return Term;
  }

public:
  LowWordDecisions(SymContext &Ctx, WorkBudget &Budget, size_t &Storage,
                   size_t MaxWork, bool *StorageLimited)
      : Ctx(Ctx), Budget(Budget), Storage(Storage),
        MaxWork(std::min(MaxWork, limits::kMaxResidueDecisionWork)),
        StorageLimited(StorageLimited) {}

  SymRef simplify(SymRef Root, const llvm::APInt &Scale, unsigned Observed,
                  bool OddProductOnly) {
    const auto Width = Ctx.width(Root);
    // Refuse broad domains before constructing decisions. This is only a
    // search admission rule; opaque words still retain all their possible bits.
    std::unordered_set<uint32_t> Seen, Inputs;
    llvm::SmallVector<SymRef, 16> Pending{Root};
    while (!Pending.empty()) {
      const auto R = Pending.pop_back_val();
      if (!charge(1, sizeof(uint32_t) + 6 * sizeof(void *)))
        return Root;
      if (!Seen.insert(R.index()).second || Ctx.isConst(R))
        continue;
      const bool Transparent = transparent(R, std::min(Observed, Ctx.width(R)));
      if (!Transparent) {
        if (Inputs.insert(R.index()).second &&
            Inputs.size() > limits::kMaxResidueDecisionInputs)
          return Root;
        continue;
      }
      if (!charge(Ctx.numOperands(R), Ctx.numOperands(R) * sizeof(SymRef)))
        return Root;
      Pending.append(Ctx.operands(R).begin(), Ctx.operands(R).end());
    }
    Bits Product(Observed, 0);
    Product.front() = 1;
    llvm::SmallVector<SymRef, 16> Factors(Ctx.operands(Root));
    uint32_t Parity = 1;
    size_t Nonconstant = Factors.size() - Ctx.isConst(Factors.front());
    std::optional<uint32_t> Selector;
    for (auto F : Factors) {
      auto Base = F;
      if (Ctx.op(Base) == SymOp::Xor && Ctx.numOperands(Base) == 2 &&
          isOne(Ctx.operand(Base, 0)))
        Base = Ctx.operand(Base, 1);
      if (Ctx.op(Base) == SymOp::And && isOne(Ctx.operand(Base, 0))) {
        Selector = word(F, 1).front();
        break;
      }
    }
    if (Selector || Nonconstant >= Observed) {
      std::optional<uint32_t> Common;
      if (Selector)
        Common = Selector;
      else
        for (auto Factor : Factors) {
          if (Ctx.isConst(Factor))
            continue;
          const auto Low = word(Factor, 1).front();
          if (Failed)
            return Root;
          if (Common && *Common != Low) {
            Common.reset();
            break;
          }
          Common = Low;
        }
      if (Common && *Common > 1) {
        const auto D = decision(*Common);
        if (D.Low <= 1 && D.High <= 1) {
          // All N factors are even in the other branch, so their product
          // is zero modulo 2^Observed when N >= Observed. Build only the
          // odd branch and restore its exact selector after multiplication.
          Parity = *Common;
          Assumption = std::make_pair(D.Variable, D.High == 1);
          PriorityInput = uint32_t(D.Variable);
          Words.clear();
          Arithmetic.clear();
          Applied.clear();
          Carries.clear();
        }
      }
    }
    if (OddProductOnly && !Assumption)
      return Root;

    // Write every group as (L + 2^Split H_i). Since 3*Split >= Observed,
    // only the constant, linear and quadratic elementary sums survive.
    // Group equality comes from complete decision words, never from samples.
    const unsigned Split = (Observed + 2) / 3;
    std::map<Bits, std::vector<Bits>> Groups;
    for (auto Factor : Factors) {
      if (Ctx.isConst(Factor))
        continue;
      const auto &Value = word(Factor, Observed);
      if (Failed ||
          !charge(Observed * (1 + std::bit_width(Groups.size())),
                  sizeof(decltype(Groups)::value_type) + 4 * sizeof(void *) +
                      (2 * Observed * sizeof(uint32_t) + 2 * sizeof(Bits))))
        return Root;
      Bits Low(Value.begin(), Value.begin() + Split);
      Groups[Low].emplace_back(Value.begin() + Split, Value.end());
    }
    std::vector<Bits> Products;
    if (!charge(Groups.size(), Groups.size() * sizeof(Bits)))
      return Root;
    for (const auto &[Low, Group] : Groups) {
      const auto Count = Group.size();
      const auto High = sumWords(Group, Observed - Split);
      if (Failed)
        return Root;
      auto Wide = Low;
      Wide.resize(Observed, 0);
      auto Term = power(Wide, Count);
      if (Observed != Split) {
        Bits Narrow(Wide.begin(), Wide.begin() + Observed - Split);
        auto Correction = calculate(SymOp::Mul, power(Narrow, Count - 1), High);
        Bits Upper(Observed, 0);
        std::copy(Correction.begin(), Correction.end(), Upper.begin() + Split);
        Term = calculate(SymOp::Add, Term, Upper);
      }
      if (2 * Split < Observed && Count > 1) {
        // 2*e2(H) = (sum H)^2 - sum(H^2). Compute one additional bit
        // before exact division by two; two is not invertible in this ring.
        const unsigned N = Observed - 2 * Split + 1;
        if (!charge(Group.size(), Group.size() * sizeof(Bits)))
          return Root;
        std::vector<Bits> Squares;
        Squares.reserve(Group.size());
        for (const auto &H : Group) {
          Bits V(H.begin(), H.begin() + N);
          Squares.push_back(calculate(SymOp::Mul, V, V));
        }
        auto SumSquares = sumWords(Squares, N);
        auto Sum = Bits(High.begin(), High.begin() + N);
        auto SumSquare = calculate(SymOp::Mul, Sum, Sum);
        for (auto &Bit : SumSquares)
          Bit = apply(Xor, Bit, 1);
        Bits One(N, 0);
        One[0] = 1;
        auto Difference = calculate(SymOp::Add, SumSquare,
                                    calculate(SymOp::Add, SumSquares, One));
        if (Failed || Difference.front() != 0)
          return Root;
        Bits Elementary(Difference.begin() + 1, Difference.end());
        Bits Narrow(Low.begin(), Low.begin() + N - 1);
        auto Correction =
            calculate(SymOp::Mul, power(Narrow, Count - 2), Elementary);
        Bits Upper(Observed, 0);
        std::copy(Correction.begin(), Correction.end(),
                  Upper.begin() + 2 * Split);
        Term = calculate(SymOp::Add, Term, Upper);
      }
      Products.push_back(std::move(Term));
    }
    // Pair equal low prefixes first. Balanced products avoid constructing
    // transient decisions for high carries that a nearby factor cancels.
    while (Products.size() > 1 && !Failed) {
      if (!charge(Products.size() * Observed *
                  (1 + std::bit_width(Products.size()))))
        return Root;
      std::sort(Products.begin(), Products.end());
      size_t Out = 0;
      for (size_t I = 0; I < Products.size(); I += 2)
        Products[Out++] =
            I + 1 == Products.size()
                ? Products[I]
                : calculate(SymOp::Mul, Products[I], Products[I + 1]);
      Products.resize(Out);
    }
    if (Failed || Products.empty()) {
      return Root;
    }
    Product = std::move(Products.front());
    Assumption.reset();
    if (Parity != 1)
      for (auto &Bit : Product)
        Bit = apply(And, Parity, Bit);

    // Equal output decisions share their complete modular coefficient.
    std::map<uint32_t, llvm::APInt> Coefficients;
    for (unsigned I = 0; I < Observed; ++I) {
      if (!Product[I])
        continue;
      if (!charge(1, sizeof(decltype(Coefficients)::value_type) +
                         4 * sizeof(void *) + ((Width - 1) / 64 + 1) * 8))
        return Root;
      Coefficients.try_emplace(Product[I], Width, 0).first->second +=
          Scale.shl(I);
    }
    const size_t FirstEmitted = Ctx.numNodes();
    std::unordered_map<uint32_t, SymRef> Emitted;
    auto Emit = [&](auto &&Self, uint32_t Id, unsigned Depth) -> SymRef {
      if (!charge())
        return {};
      if (Id <= 1)
        return Ctx.mkConst(Width, Id);
      if (auto It = Emitted.find(Id); It != Emitted.end())
        return It->second;
      const auto D = decision(Id);
      const auto Input = SymRef(uint32_t(D.Variable));
      const size_t Words =
          (size_t(std::max(Width, Ctx.width(Input))) - 1) / 64 + 1;
      if (Depth >= limits::kMaxResidueDecisionDepth ||
          !charge(1,
                  sizeof(SymRef) + 4 * sizeof(void *) +
                      8 * (sizeof(SymNode) + 4 * sizeof(SymRef) +
                           sizeof(llvm::APInt) + Words * sizeof(uint64_t)))) {
        Failed = true;
        return {};
      }
      const auto Bit = variableBit(D.Variable);
      auto Selector = Ctx.mkZExtOrTrunc(
          Ctx.mkAnd(Ctx.mkLShr(Input, Ctx.mkConst(Ctx.width(Input), Bit)),
                    Ctx.mkConst(Ctx.width(Input), 1)),
          Width);
      auto Low = Self(Self, D.Low, Depth + 1);
      auto High = Self(Self, D.High, Depth + 1);
      if (!Low || !High || Failed)
        return {};
      SymRef Result;
      const auto One = Ctx.mkConst(Width, 1);
      if (!D.Low)
        Result = Ctx.mkAnd(Selector, High);
      else if (!D.High)
        Result = Ctx.mkAnd(Ctx.mkXor(Selector, One), Low);
      else if (D.Low == 1)
        Result = Ctx.mkOr(Ctx.mkXor(Selector, One), High);
      else if (D.High == 1)
        Result = Ctx.mkOr(Selector, Low);
      else if (apply(Xor, D.Low, D.High) == 1)
        Result = Ctx.mkXor(Selector, Low);
      else
        Result = Ctx.mkXor(Low, Ctx.mkAnd(Selector, Ctx.mkXor(Low, High)));
      Emitted.emplace(Id, Result);
      return Result;
    };
    auto Synthesize = [&](uint32_t RootId, SymRef Fallback) -> SymRef {
      if (RootId <= 1 ||
          (decision(RootId).Low <= 1 && decision(RootId).High <= 1))
        return Fallback;
      std::map<uint64_t, unsigned> Variables;
      std::unordered_set<uint32_t> Reachable;
      llvm::SmallVector<uint32_t, 16> Pending{RootId};
      while (!Pending.empty()) {
        const auto Id = Pending.pop_back_val();
        if (!charge())
          return Fallback;
        if (Id <= 1 || Reachable.contains(Id))
          continue;
        if (!charge(1, sizeof(decltype(Variables)::value_type) +
                           2 * sizeof(uint32_t) + 8 * sizeof(void *)))
          return Fallback;
        Reachable.insert(Id);
        const auto D = decision(Id);
        Variables.try_emplace(D.Variable, 0);
        if (Variables.size() > limits::kMaxResidueDecisionSynthesisInputs)
          return Fallback;
        Pending.push_back(D.Low);
        Pending.push_back(D.High);
      }
      const unsigned Arity = Variables.size();
      const size_t Count = size_t(1) << Arity;
      BitwiseSynthesisLimits Options;
      const size_t SynthesisWork = Arity <= Options.MaxOptimalAtoms
                                       ? bitwiseSynthesisWork(Arity)
                                       : Count * (Arity + 4);
      // Cover, parity and recipe construction each have their own bounded
      // sweep; prepay their combined ceiling in the shared request budget.
      size_t Words = (size_t(Width) - 1) / 64 + 1;
      for (const auto &[Key, Position] : Variables) {
        (void)Position;
        Words = std::max(
            Words, (size_t(Ctx.width(SymRef(uint32_t(Key)))) - 1) / 64 + 1);
      }
      const size_t Units = Count * (3 * Arity + 1) + 3 * SynthesisWork;
      const size_t Bytes = Count * (sizeof(uint64_t) +
                                    8 * (sizeof(SymNode) + sizeof(llvm::APInt) +
                                         Words * sizeof(uint64_t)) +
                                    16 * sizeof(SymRef));
      if (Bytes > Storage && StorageLimited)
        *StorageLimited = true;
      if (Units > MaxWork - Work || Bytes > Storage ||
          !Budget.canConsume(Units))
        return Fallback;
      if (!charge(Units, Bytes))
        return Fallback;
      llvm::SmallVector<SymRef, 8> Atoms;
      for (auto &[Key, Position] : Variables) {
        Position = Atoms.size();
        const auto Input = SymRef(uint32_t(Key));
        const auto Bit = variableBit(Key);
        Atoms.push_back(Ctx.mkZExtOrTrunc(
            Ctx.mkAnd(Ctx.mkLShr(Input, Ctx.mkConst(Ctx.width(Input), Bit)),
                      Ctx.mkConst(Ctx.width(Input), 1)),
            Width));
      }
      auto Table = TruthTable::zero(Arity);
      for (size_t Pattern = 0; Pattern < Count; ++Pattern) {
        auto Id = RootId;
        while (Id > 1) {
          const auto D = decision(Id);
          Id = (Pattern >> Variables.at(D.Variable)) & 1 ? D.High : D.Low;
        }
        Table.setValue(Pattern, Id != 0);
      }
      Options.MaxWork = SynthesisWork;
      Options.MaxCost = readingCost(Ctx, Fallback);
      if (auto Value = synthesizeBitwise(Ctx, Table, Atoms, Options)) {
        const auto Bit = Ctx.mkAnd(*Value, Ctx.mkConst(Width, 1));
        if (readingScore(Ctx, Bit) < readingScore(Ctx, Fallback))
          return Bit;
      }
      return Fallback;
    };
    using Polynomial = llvm::SmallVector<uint64_t, 8>;
    std::map<uint64_t, unsigned> PolynomialVariables;
    std::unordered_map<uint32_t, Polynomial> Polynomials;
    auto ReadPolynomial = [&](auto &&Self,
                              uint32_t Id) -> std::optional<Polynomial> {
      if (!charge())
        return std::nullopt;
      if (Id <= 1)
        return Id ? Polynomial{0} : Polynomial{};
      if (Id & 1) {
        auto Result = Self(Self, Id ^ 1);
        if (!Result || !charge(Result->size() + 1))
          return std::nullopt;
        if (!Result->empty() && !Result->front())
          Result->erase(Result->begin());
        else
          Result->insert(Result->begin(), 0);
        if (Result->size() > limits::kMaxResidueDecisionPolynomialTerms)
          return std::nullopt;
        return Result;
      }
      if (auto It = Polynomials.find(Id); It != Polynomials.end()) {
        if (!charge(It->second.size(), It->second.size() * sizeof(uint64_t)))
          return std::nullopt;
        return It->second;
      }
      const auto D = decision(Id);
      auto [It, Inserted] = PolynomialVariables.try_emplace(
          D.Variable, PolynomialVariables.size());
      if (It->second >= 64)
        return std::nullopt;
      if (Inserted &&
          !charge(1, sizeof(decltype(PolynomialVariables)::value_type) +
                         4 * sizeof(void *)))
        return std::nullopt;
      const auto Variable = uint64_t(1) << It->second;
      auto Low = Self(Self, D.Low), High = Self(Self, D.High);
      if (!Low || !High)
        return std::nullopt;
      auto Xor = [&](const Polynomial &A,
                     const Polynomial &B) -> std::optional<Polynomial> {
        const auto Size = A.size() + B.size();
        if (!charge(Size, Size * sizeof(uint64_t)))
          return std::nullopt;
        Polynomial Result;
        std::set_symmetric_difference(A.begin(), A.end(), B.begin(), B.end(),
                                      std::back_inserter(Result));
        if (Result.size() > limits::kMaxResidueDecisionPolynomialTerms)
          return std::nullopt;
        return Result;
      };
      auto Difference = Xor(*Low, *High);
      if (!Difference || !charge(Difference->size()))
        return std::nullopt;
      for (auto &Term : *Difference) {
        // Ordered decisions place this variable before both children.
        // Their polynomials cannot contain it; adding its bit preserves order.
        if (Term & Variable)
          return std::nullopt;
        Term |= Variable;
      }
      auto Result = Xor(*Low, *Difference);
      if (!Result ||
          !charge(Result->size(), sizeof(decltype(Polynomials)::value_type) +
                                      4 * sizeof(void *) +
                                      Result->size() * sizeof(uint64_t)))
        return std::nullopt;
      Polynomials.emplace(Id, *Result);
      return Result;
    };
    auto SynthesizePolynomial = [&](uint32_t Id, SymRef Fallback) -> SymRef {
      auto Form = ReadPolynomial(ReadPolynomial, Id);
      if (!Form)
        return Fallback;
      size_t Edges = Form->size();
      for (auto Term : *Form)
        Edges += std::popcount(Term);
      size_t Words = (size_t(Width) - 1) / 64 + 1;
      for (const auto &[Variable, Index] : PolynomialVariables) {
        (void)Index;
        Words = std::max(
            Words,
            (size_t(Ctx.width(SymRef(uint32_t(Variable)))) - 1) / 64 + 1);
      }
      if (!charge(Edges * (1 + Words),
                  (Edges + 1) *
                      (4 * sizeof(SymNode) + 8 * sizeof(SymRef) +
                       4 * sizeof(llvm::APInt) + 4 * Words * sizeof(uint64_t))))
        return Fallback;
      llvm::SmallVector<SymRef, 16> Inputs(PolynomialVariables.size());
      for (const auto &[Variable, Index] : PolynomialVariables) {
        const auto Input = SymRef(uint32_t(Variable));
        Inputs[Index] = Ctx.mkZExtOrTrunc(
            Ctx.mkAnd(Ctx.mkLShr(Input, Ctx.mkConst(Ctx.width(Input),
                                                    variableBit(Variable))),
                      Ctx.mkConst(Ctx.width(Input), 1)),
            Width);
      }
      llvm::SmallVector<SymRef, 16> Terms;
      for (auto Term : *Form) {
        llvm::SmallVector<SymRef, 8> Factors;
        while (Term) {
          Factors.push_back(Inputs[std::countr_zero(Term)]);
          Term &= Term - 1;
        }
        Terms.push_back(Factors.empty() ? Ctx.mkConst(Width, 1)
                                        : Ctx.mkAnd(Factors));
      }
      auto Result = Terms.empty() ? Ctx.mkZero(Width) : Ctx.mkXor(Terms);
      std::map<Polynomial, SymRef> Factored;
      auto Factor = [&](auto &&Self, const Polynomial &P) -> SymRef {
        if (!charge(1 + P.size(), sizeof(decltype(Factored)::value_type) +
                                      4 * sizeof(void *) +
                                      P.size() * sizeof(uint64_t)))
          return {};
        if (P.empty())
          return Ctx.mkZero(Width);
        if (P.size() == 1 && !P.front())
          return Ctx.mkConst(Width, 1);
        if (auto It = Factored.find(P); It != Factored.end())
          return It->second;
        if (!charge(Inputs.size(), Inputs.size() * sizeof(unsigned)))
          return {};
        llvm::SmallVector<unsigned, 16> Frequencies(Inputs.size(), 0);
        for (auto Term : P)
          while (Term) {
            if (!charge())
              return {};
            ++Frequencies[std::countr_zero(Term)];
            Term &= Term - 1;
          }
        const auto Best =
            std::max_element(Frequencies.begin(), Frequencies.end());
        const unsigned Variable = Best - Frequencies.begin();
        const auto Bit = uint64_t(1) << Variable;
        Polynomial Low, High;
        for (auto Term : P) {
          if (Term & Bit)
            High.push_back(Term ^ Bit);
          else
            Low.push_back(Term);
        }
        auto A = Self(Self, Low), B = Self(Self, High);
        if (!A || !B || !charge(3, 3 * (sizeof(SymNode) + 4 * sizeof(SymRef))))
          return {};
        auto Value = Ctx.mkXor(A, Ctx.mkAnd(Inputs[Variable], B));
        Factored.emplace(P, Value);
        return Value;
      };
      if (auto Smaller = Factor(Factor, *Form);
          Smaller && readingScore(Ctx, Smaller) < readingScore(Ctx, Result))
        Result = Smaller;
      return readingScore(Ctx, Result) < readingScore(Ctx, Fallback) ? Result
                                                                     : Fallback;
    };
    llvm::SmallVector<SymRef, 8> Terms;
    llvm::SmallVector<std::pair<uint32_t, SymRef>, 8> Values;
    for (const auto &[Id, Coefficient] : Coefficients) {
      if (Coefficient.isZero())
        continue;
      auto Value = Emit(Emit, Id, 0);
      if (!Value || Failed) {
        return Root;
      }
      Values.emplace_back(Id, Value);
      Terms.push_back(Ctx.mkMul(Ctx.mkConst(Coefficient), Value));
    }
    if (!charge(1 + 3 * (Ctx.numNodes() - Ctx.numCachedReadabilityNodes()),
                (Terms.size() + 1) * (sizeof(SymNode) + 4 * sizeof(SymRef)))) {
      return Root;
    }
    auto Result = Terms.empty() ? Ctx.mkZero(Width) : Ctx.mkAdd(Terms);
    // Finish the proved reading before optional Boolean recipe search.
    // A later refusal must retain that complete result, including when one
    // output bit has a more complicated support than the others.
    for (size_t I = 0; I < Values.size(); ++I) {
      const auto [Id, Value] = Values[I];
      // Reserve publication before optional refinement. Even if a later
      // attempt refuses, an already completed smaller bit remains usable.
      if (!charge(Terms.size() + 1,
                  (Terms.size() + 2) * (sizeof(SymNode) + 4 * sizeof(SymRef) +
                                        sizeof(llvm::APInt) +
                                        ((size_t(Width) - 1) / 64 + 1) * 8)))
        break;
      auto Shorter = SynthesizePolynomial(Id, Value);
      if (!Failed && Shorter == Value)
        Shorter = Synthesize(Id, Shorter);
      if (Shorter != Value) {
        Terms[I] = Ctx.mkMul(Ctx.mkConst(Coefficients.at(Id)), Shorter);
        auto Next = Ctx.mkAdd(Terms);
        if (readingScore(Ctx, Next) < readingScore(Ctx, Result))
          Result = Next;
      }
      if (Failed)
        break;
    }

    if (readingScore(Ctx, Result) >= readingScore(Ctx, Root))
      return Root;
    recordResidueReading(Ctx, FirstEmitted, Budget, Storage);
    return Result;
  }
};

} // namespace

SymRef foldResidueDecisions(SymContext &Ctx, SymRef Root, WorkBudget &Budget,
                            size_t &Storage, bool OddProductOnly,
                            size_t MaxWork, bool *StorageLimited) {
  if (Ctx.op(Root) != SymOp::Mul || Ctx.numOperands(Root) < 2)
    return Root;
  const size_t Words = (size_t(Ctx.width(Root)) - 1) / 64 + 1;
  const size_t Bytes = sizeof(llvm::APInt) + Words * sizeof(uint64_t);
  if (Bytes > Storage && StorageLimited)
    *StorageLimited = true;
  if (Words > MaxWork || Bytes > Storage || !Budget.canConsume(Words))
    return Root;
  Budget.consume(Words);
  Storage -= Bytes;
  MaxWork -= Words;
  const auto First = Ctx.operand(Root, 0);
  const auto Scale = Ctx.isConst(First) ? Ctx.constValue(First)
                                        : llvm::APInt(Ctx.width(Root), 1);
  const unsigned Observed = Ctx.width(Root) - Scale.countr_zero();
  if (!Observed || Observed > limits::kMaxAffineResidueTableBits)
    return Root;
  return LowWordDecisions(Ctx, Budget, Storage, MaxWork, StorageLimited)
      .simplify(Root, Scale, Observed, OddProductOnly);
}

} // namespace neverd::symbolic::detail
