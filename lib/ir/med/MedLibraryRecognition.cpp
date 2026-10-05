//===- MedLibraryRecognition.cpp - Read-only library expression matching -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedLibraryRecognition.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Demangle/Demangle.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <tuple>

using namespace neverd;
using namespace neverd::sigs;

namespace {

using VarKey = std::tuple<unsigned, int, int>;
VarKey key(const MedVar &V) { return {V.Kind, V.Id, V.SSAVer}; }

bool same(const MedVar &A, const MedVar &B) {
  return A == B && A.Size == B.Size;
}

bool sameWidthOrZero(const MedVar &Value, unsigned Bytes) {
  return Value.Size == Bytes || (Value.isConst() && Value.ConstVal == 0 &&
                                 Value.Size > 0 && Value.Size < Bytes);
}

std::optional<NdOp> opcode(LibraryFeatureOp Op) {
  switch (Op) {
  case LibraryFeatureOp::Load:
    return NdOp::LOAD;
  case LibraryFeatureOp::Zext:
    return NdOp::INT_ZEXT;
  case LibraryFeatureOp::Sext:
    return NdOp::INT_SEXT;
  case LibraryFeatureOp::Select:
    return NdOp::SELECT;
  case LibraryFeatureOp::Add:
    return NdOp::INT_ADD;
  case LibraryFeatureOp::Sub:
    return NdOp::INT_SUB;
  case LibraryFeatureOp::Mul:
    return NdOp::INT_MULT;
  case LibraryFeatureOp::UDiv:
    return NdOp::INT_DIV;
  case LibraryFeatureOp::And:
    return NdOp::INT_AND;
  case LibraryFeatureOp::Or:
    return NdOp::INT_OR;
  case LibraryFeatureOp::Xor:
    return NdOp::INT_XOR;
  case LibraryFeatureOp::Shl:
    return NdOp::INT_LEFT;
  case LibraryFeatureOp::LShr:
    return NdOp::INT_RIGHT;
  case LibraryFeatureOp::AShr:
    return NdOp::INT_ASHR;
  case LibraryFeatureOp::Eq:
    return NdOp::INT_EQUAL;
  case LibraryFeatureOp::Ne:
    return NdOp::INT_NOTEQUAL;
  case LibraryFeatureOp::ULT:
    return NdOp::INT_LESS;
  case LibraryFeatureOp::ULE:
    return NdOp::INT_LESSEQUAL;
  case LibraryFeatureOp::SLT:
    return NdOp::INT_SLESS;
  default:
    return std::nullopt;
  }
}

bool commutative(LibraryFeatureOp Op) {
  return Op == LibraryFeatureOp::Add || Op == LibraryFeatureOp::Mul ||
         Op == LibraryFeatureOp::And || Op == LibraryFeatureOp::Or ||
         Op == LibraryFeatureOp::Xor || Op == LibraryFeatureOp::Eq ||
         Op == LibraryFeatureOp::Ne;
}

bool ordinaryScalar(NdOp Op) {
  switch (Op) {
  case NdOp::COPY:
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_MULT:
  case NdOp::INT_AND:
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
  case NdOp::INT_LEFT:
  case NdOp::INT_RIGHT:
  case NdOp::INT_ASHR:
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESS:
  case NdOp::INT_SLESSEQUAL:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
  case NdOp::INT_CARRY:
  case NdOp::INT_SBOR:
  case NdOp::INT_SOVF:
  case NdOp::BOOL_AND:
  case NdOp::BOOL_OR:
  case NdOp::BOOL_XOR:
  case NdOp::BOOL_NOT:
  case NdOp::SUBBYTES:
  case NdOp::CONCAT:
  case NdOp::SELECT:
    return true;
  default:
    return false;
  }
}

struct MatchState {
  std::vector<std::optional<MedVar>> Bindings;
  std::set<const MedOp *> Operations;
  std::optional<MedVar> Receiver;
};

class ExpressionMatcher {
public:
  ExpressionMatcher(const MedFunc &F, size_t &Work, bool &Exhausted)
      : Function(F), Work(Work), Exhausted(Exhausted) {
    for (const MedCallClobber &Clobber : F.CallClobbers) {
      if (!spend())
        return;
      Definitions[key(Clobber.Value)] = nullptr;
    }
    for (const MedBlock &B : F.Blocks) {
      for (const PhiNode &P : B.Phis) {
        if (!spend())
          return;
        Definitions[key(P.Output)] = nullptr;
      }
      for (const MedOp &O : B.Ops) {
        if (!spend())
          return;
        if (O.NumInputs > O.Inputs.size()) {
          Valid = false;
          return;
        }
        for (const MedVar &V : O.IntrinsicOutputs) {
          if (!spend())
            return;
          Definitions[key(V)] = nullptr;
        }
        if (O.Dead || !O.Output.Size || O.Output.isConst())
          continue;
        // Synthetic entry copies retain the incoming version as both sides.
        if (O.Opcode == NdOp::COPY && O.NumInputs == 1 &&
            same(O.Output, O.Inputs[0]))
          continue;
        auto [I, Added] = Definitions.emplace(key(O.Output), &O);
        if (!Added)
          I->second = nullptr;
      }
    }
  }

  bool valid() const { return Valid; }

  bool spend() {
    if (!Work) {
      Exhausted = true;
      return false;
    }
    --Work;
    return true;
  }

  const MedOp *definition(const MedVar &V) const {
    auto I = Definitions.find(key(V));
    return I == Definitions.end() ? nullptr : I->second;
  }

  std::optional<MedVar> canonical(MedVar V) {
    for (unsigned Depth = 0; Depth < 64; ++Depth) {
      if (!spend())
        return std::nullopt;
      if (V.isConst())
        return V;
      const MedOp *D = definition(V);
      if (!D || D->Opcode != NdOp::COPY)
        return V;
      if (D->NumInputs != 1 || D->Output.Size != V.Size ||
          D->Inputs[0].Size != V.Size)
        return std::nullopt;
      V = D->Inputs[0];
    }
    return std::nullopt;
  }

  bool entryRegister(const MedVar &V, uint64_t Register) const {
    if (V.Size != 8 || (V.Kind != MedVar::Reg && V.Kind != MedVar::Param) ||
        V.RegOff != Register)
      return false;
    if (V.Kind == MedVar::Param)
      return llvm::any_of(Function.Params,
                          [&](const MedVar &P) { return same(P, V); });
    return V.SSAVer == 0 && !Definitions.contains(key(V));
  }

  // Undo only a literal low-byte round trip through a wider register. A
  // truncation of an arbitrary value is not an identity operation.
  std::optional<MedVar> lowRoundTrip(const MedVar &Value, MatchState &State) {
    const MedOp *Trunc = definition(Value);
    if (!Trunc || Trunc->Opcode != NdOp::SUBBYTES || Trunc->NumInputs != 2 ||
        !Trunc->Inputs[1].isConst() || Trunc->Inputs[1].ConstVal != 0 ||
        Trunc->Output.Size != Value.Size || Trunc->Inputs[0].Size <= Value.Size)
      return std::nullopt;
    auto Wide = canonical(Trunc->Inputs[0]);
    const MedOp *Extend = Wide ? definition(*Wide) : nullptr;
    if (!Extend ||
        (Extend->Opcode != NdOp::INT_ZEXT &&
         Extend->Opcode != NdOp::INT_SEXT) ||
        Extend->NumInputs != 1 || Extend->Inputs[0].Size != Value.Size ||
        Extend->Output.Size <= Value.Size)
      return std::nullopt;
    State.Operations.insert(Trunc);
    State.Operations.insert(Extend);
    return Extend->Inputs[0];
  }

  bool unsignedFits(MedVar Value, unsigned Bits, unsigned Depth = 0) {
    if (!spend() || Depth >= 32 || !Bits || Bits > 64)
      return false;
    auto C = canonical(Value);
    if (!C)
      return false;
    Value = *C;
    if (Value.Size * 8 <= Bits)
      return true;
    if (Value.isConst())
      return Bits == 64 || Value.ConstVal < (uint64_t{1} << Bits);
    const MedOp *D = definition(Value);
    if (!D)
      return false;
    if (D->Opcode == NdOp::INT_ZEXT && D->NumInputs == 1 &&
        D->Inputs[0].Size < Value.Size)
      return unsignedFits(D->Inputs[0], Bits, Depth + 1);
    if (D->Opcode == NdOp::SUBBYTES && D->NumInputs == 2 &&
        D->Inputs[1].isConst() && D->Inputs[1].ConstVal == 0 &&
        D->Inputs[0].Size > Value.Size)
      return unsignedFits(D->Inputs[0], Bits, Depth + 1);
    return false;
  }

  bool match(const LibraryFeatureRule &Rule, unsigned NodeIndex, MedVar Value,
             MatchState &State, unsigned Depth = 0) {
    if (!spend() || Depth >= 128)
      return false;
    const LibraryFeatureNode &Node = Rule.Nodes[NodeIndex];
    auto Canonical = canonical(Value);
    if (!Canonical)
      return false;
    Value = *Canonical;
    if (Node.Op == LibraryFeatureOp::Constant)
      if (const MedOp *Extend = definition(Value);
          Extend && Extend->Opcode == NdOp::INT_ZEXT &&
          Extend->NumInputs == 1 && Extend->Output.Size == Value.Size &&
          Extend->Inputs[0].isConst() && Extend->Inputs[0].Size < Value.Size &&
          Extend->Inputs[0].Size > 0 && Extend->Inputs[0].Size <= 8 &&
          Value.Size <= 8) {
        const unsigned Bits = Extend->Inputs[0].Size * 8;
        Value = MedVar::makeConst(
            Extend->Inputs[0].ConstVal &
                (Bits == 64 ? UINT64_MAX : ((uint64_t{1} << Bits) - 1)),
            Value.Size);
        State.Operations.insert(Extend);
      }
    const MatchState BeforeCarriers = State;
    if (auto Narrow = lowRoundTrip(Value, State)) {
      if (match(Rule, NodeIndex, *Narrow, State, Depth + 1))
        return true;
      State = BeforeCarriers;
    }
    if (Value.Size * 8 > Node.Bits) {
      if (const MedOp *Extend = definition(Value);
          Extend && Extend->Opcode == NdOp::INT_ZEXT &&
          Extend->NumInputs == 1 && Extend->Inputs[0].Size < Value.Size) {
        if (match(Rule, NodeIndex, Extend->Inputs[0], State, Depth + 1)) {
          State.Operations.insert(Extend);
          return true;
        }
        State = BeforeCarriers;
      }
    }
    bool WidenedAnd = false;
    if (Node.Op == LibraryFeatureOp::And && Value.Size * 8 > Node.Bits)
      if (const MedOp *And = definition(Value);
          And && And->Opcode == NdOp::INT_AND && And->NumInputs == 2 &&
          And->Inputs[0].Size == Value.Size &&
          And->Inputs[1].Size == Value.Size)
        WidenedAnd = unsignedFits(And->Inputs[0], Node.Bits) &&
                     unsignedFits(And->Inputs[1], Node.Bits);
    if (Value.Size * 8 != Node.Bits && !WidenedAnd &&
        // A nonnegative narrow immediate has the same value under either
        // signed or unsigned extension. A set sign bit requires an explicit
        // extension operation instead of guessing the arithmetic convention.
        !(Node.Op == LibraryFeatureOp::Constant && Value.isConst() &&
          Value.Size > 0 && Value.Size * 8 < Node.Bits &&
          Value.ConstVal < (uint64_t{1} << (Value.Size * 8 - 1))) &&
        !(Node.Op == LibraryFeatureOp::Constant && Value.Size * 8 > Node.Bits &&
          unsignedFits(Value, Node.Bits)))
      return false;
    if (State.Bindings[NodeIndex])
      return same(*State.Bindings[NodeIndex], Value);
    if (Node.Op == LibraryFeatureOp::Input) {
      if (Value.isConst())
        return false;
      if (Node.Receiver) {
        if (State.Receiver && !same(*State.Receiver, Value))
          return false;
        State.Receiver = Value;
      }
      State.Bindings[NodeIndex] = Value;
      return true;
    }
    if (Node.Op == LibraryFeatureOp::Constant) {
      if (!Value.isConst() || Value.ConstVal != Node.Constant)
        return false;
      State.Bindings[NodeIndex] = Value;
      return true;
    }
    if (Value.isConst())
      return false;
    const MedOp *D = definition(Value);
    // A compiler can test the tag's sign bit by sign-extending the byte and
    // comparing it with zero. Prove that exact finite equivalence; do not
    // interpret an arbitrary signed comparison as an SSO discriminator.
    if (Node.Op == LibraryFeatureOp::Ne && D && D->Opcode == NdOp::INT_SLESS &&
        D->NumInputs == 2 && D->Inputs[1].isConst() &&
        D->Inputs[1].ConstVal == 0 && D->Inputs[0].Size == D->Inputs[1].Size &&
        D->Output.Size == Value.Size &&
        D->MemoryOrdering == NdMemoryOrdering::None &&
        D->MemoryAddressSpace == NdMemoryAddressSpace::Default) {
      for (unsigned Swap = 0; Swap != 2; ++Swap) {
        const auto &And = Rule.Nodes[Node.Args[Swap]];
        const auto &Zero = Rule.Nodes[Node.Args[1 - Swap]];
        if (And.Op != LibraryFeatureOp::And ||
            Zero.Op != LibraryFeatureOp::Constant || Zero.Constant != 0)
          continue;
        for (unsigned MaskSide = 0; MaskSide != 2; ++MaskSide) {
          const auto &Mask = Rule.Nodes[And.Args[MaskSide]];
          if (Mask.Op != LibraryFeatureOp::Constant ||
              Mask.Constant != (uint64_t{1} << (And.Bits - 1)))
            continue;
          MatchState Candidate = State;
          auto Signed = canonical(D->Inputs[0]);
          if (const MedOp *Sub = Signed ? definition(*Signed) : nullptr;
              Sub && Sub->Opcode == NdOp::INT_SUB && Sub->NumInputs == 2 &&
              Sub->Inputs[1].isConst() && Sub->Inputs[1].ConstVal == 0 &&
              Sub->Inputs[0].Size == Signed->Size &&
              Sub->Inputs[1].Size == Signed->Size) {
            Candidate.Operations.insert(Sub);
            Signed = canonical(Sub->Inputs[0]);
          }
          if (Signed)
            if (auto Narrow = lowRoundTrip(*Signed, Candidate))
              Signed = canonical(*Narrow);
          const MedOp *Extend = Signed ? definition(*Signed) : nullptr;
          if (!Extend || Extend->Opcode != NdOp::INT_SEXT ||
              Extend->NumInputs != 1 ||
              Extend->Inputs[0].Size * 8 != And.Bits ||
              Extend->Output.Size != D->Inputs[0].Size ||
              !match(Rule, And.Args[1 - MaskSide], Extend->Inputs[0], Candidate,
                     Depth + 1))
            continue;
          Candidate.Operations.insert(Extend);
          Candidate.Operations.insert(D);
          Candidate.Bindings[NodeIndex] = Value;
          State = std::move(Candidate);
          return true;
        }
      }
    }
    const auto Expected = opcode(Node.Op);
    if (!D || !Expected || D->Opcode != *Expected ||
        D->Output.Size != Value.Size ||
        D->MemoryOrdering != NdMemoryOrdering::None ||
        D->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        D->NumInputs != Node.Args.size() || D->Addr == InvalidVA ||
        (Node.Op == LibraryFeatureOp::Load && D->OriginSeq < 0))
      return false;
    MatchState Original = State;
    bool Matched = false;
    if (Node.Op == LibraryFeatureOp::Load) {
      auto Address = canonical(D->Inputs[0]);
      if (Address) {
        if (Node.Offset == 0) {
          Matched = match(Rule, Node.Args[0], *Address, State, Depth + 1);
        } else if (const MedOp *Add = definition(*Address);
                   Add && Add->Opcode == NdOp::INT_ADD && Add->NumInputs == 2 &&
                   Add->Output.Size == 8 && Add->OriginSeq >= 0) {
          for (unsigned C = 0; C != 2 && !Matched; ++C) {
            auto Offset = canonical(Add->Inputs[C]);
            if (!Offset || !Offset->isConst() || Offset->Size != 8 ||
                Offset->ConstVal != static_cast<uint64_t>(Node.Offset))
              continue;
            State = Original;
            Matched =
                match(Rule, Node.Args[0], Add->Inputs[1 - C], State, Depth + 1);
            if (Matched)
              State.Operations.insert(Add);
          }
        }
      }
    } else if (Node.Op == LibraryFeatureOp::Eq ||
               Node.Op == LibraryFeatureOp::Ne) {
      // Flags often retain `(a - b) == 0`, while a source-level comparison
      // has two direct operands. Equality under modular subtraction is exact
      // at the same width; no signed-order or overflow inference is made.
      for (unsigned C = 0; C != 2 && !Matched; ++C) {
        auto Zero = canonical(D->Inputs[C]);
        auto Difference = canonical(D->Inputs[1 - C]);
        const MedOp *Sub = Difference ? definition(*Difference) : nullptr;
        if (!Zero || !Zero->isConst() || Zero->ConstVal != 0 || !Sub ||
            Sub->Opcode != NdOp::INT_SUB || Sub->NumInputs != 2 ||
            Sub->Output.Size != Zero->Size ||
            !sameWidthOrZero(Sub->Inputs[0], Zero->Size) ||
            !sameWidthOrZero(Sub->Inputs[1], Zero->Size) || Sub->OriginSeq < 0)
          continue;
        for (unsigned Swap = 0; Swap != 2 && !Matched; ++Swap) {
          State = Original;
          Matched =
              match(Rule, Node.Args[0], Sub->Inputs[Swap], State, Depth + 1) &&
              match(Rule, Node.Args[1], Sub->Inputs[1 - Swap], State,
                    Depth + 1);
        }
        if (Matched)
          State.Operations.insert(Sub);
      }
      if (!Matched) {
        State = Original;
        Matched = match(Rule, Node.Args[0], D->Inputs[0], State, Depth + 1) &&
                  match(Rule, Node.Args[1], D->Inputs[1], State, Depth + 1);
      }
      if (!Matched) {
        State = Original;
        Matched = match(Rule, Node.Args[0], D->Inputs[1], State, Depth + 1) &&
                  match(Rule, Node.Args[1], D->Inputs[0], State, Depth + 1);
      }
    } else {
      Matched = true;
      for (unsigned I = 0; I < Node.Args.size() && Matched; ++I)
        Matched = match(Rule, Node.Args[I], D->Inputs[I], State, Depth + 1);
      if (!Matched && commutative(Node.Op)) {
        State = Original;
        Matched = match(Rule, Node.Args[0], D->Inputs[1], State, Depth + 1) &&
                  match(Rule, Node.Args[1], D->Inputs[0], State, Depth + 1);
      }
    }
    if (!Matched) {
      State = std::move(Original);
      return false;
    }
    State.Operations.insert(D);
    State.Bindings[NodeIndex] = Value;
    return true;
  }

  bool isolated(const MatchState &State, const MedVar &Root) {
    const MedBlock *Owner = nullptr;
    size_t First = 0, Last = 0;
    for (const MedBlock &B : Function.Blocks)
      for (size_t I = 0; I < B.Ops.size(); ++I) {
        if (!spend())
          return false;
        if (State.Operations.contains(&B.Ops[I])) {
          if (Owner && Owner != &B)
            return false;
          if (!Owner) {
            Owner = &B;
            First = I;
          }
          Last = I;
        }
      }
    if (!Owner || !Owner->ExceptionalPreds.empty() ||
        !Owner->ExceptionalSuccs.empty())
      return false;
    for (size_t I = First; I <= Last; ++I) {
      if (!spend())
        return false;
      const MedOp &O = Owner->Ops[I];
      if (!O.Dead && !State.Operations.contains(&O) && O.Opcode != NdOp::COPY)
        return false;
    }
    // An intermediate with another consumer is still recognizable, but its
    // region cannot hide that separate use. Transparent copies do not add a
    // new observation; every non-copy use is checked at its canonical input.
    for (const MedBlock &B : Function.Blocks) {
      for (const PhiNode &P : B.Phis)
        for (const auto &[Pred, V] : P.Args) {
          (void)Pred;
          auto C = canonical(V);
          if (!C ||
              (State.Operations.contains(definition(*C)) && !same(*C, Root)))
            return false;
        }
      for (const MedOp &O : B.Ops) {
        if (!spend())
          return false;
        if (O.Dead || O.Opcode == NdOp::COPY || State.Operations.contains(&O))
          continue;
        for (unsigned I = 0; I < O.NumInputs; ++I) {
          auto C = canonical(O.Inputs[I]);
          if (!C ||
              (State.Operations.contains(definition(*C)) && !same(*C, Root)))
            return false;
        }
      }
    }
    return true;
  }

  bool consistentMemory(const MatchState &State) {
    const MedBlock *Owner = nullptr;
    size_t First = 0, Last = 0;
    for (const MedBlock &B : Function.Blocks)
      for (size_t I = 0; I < B.Ops.size(); ++I) {
        if (!spend())
          return false;
        if (State.Operations.contains(&B.Ops[I])) {
          if (Owner && Owner != &B)
            return false;
          if (!Owner) {
            Owner = &B;
            First = I;
          }
          Last = I;
        }
      }
    if (!Owner)
      return false;
    for (size_t I = First; I <= Last; ++I) {
      if (!spend())
        return false;
      const MedOp &O = Owner->Ops[I];
      if (!O.Dead && !State.Operations.contains(&O) &&
          !ordinaryScalar(O.Opcode))
        return false;
    }
    return true;
  }

  bool wholeFunction(const MatchState &State, const MedVar &Root,
                     uint64_t ReturnRegister) {
    if (Function.Blocks.size() != 1)
      return false;
    std::optional<MedVar> Result;
    bool Returned = false;
    for (const MedOp &O : Function.Blocks.front().Ops) {
      if (!spend())
        return false;
      if (O.Dead)
        continue;
      if (O.Output.Kind == MedVar::Reg && O.Output.RegOff == ReturnRegister &&
          O.Output.Size)
        Result = canonical(O.Output);
      if (O.Opcode == NdOp::RETURN) {
        if (!Result || (!same(*Result, Root) && !sameBoolean(*Result, Root)))
          return false;
        Returned = true;
      } else if (!State.Operations.contains(&O) && !ordinaryScalar(O.Opcode)) {
        return false;
      }
    }
    return Returned;
  }

private:
  struct Comparison {
    NdOp Opcode;
    MedVar Left;
    MedVar Right;
  };

  std::optional<Comparison> comparison(MedVar Value, unsigned Depth = 0) {
    if (Depth > 32)
      return std::nullopt;
    auto C = canonical(Value);
    const MedOp *D = C ? definition(*C) : nullptr;
    if (!D)
      return std::nullopt;
    if (D->Opcode == NdOp::INT_ZEXT && D->NumInputs == 1 &&
        D->Inputs[0].Size < D->Output.Size)
      return comparison(D->Inputs[0], Depth + 1);
    if (D->Opcode == NdOp::SELECT && D->NumInputs == 3 &&
        D->Inputs[1].isConst() && D->Inputs[1].ConstVal == 1 &&
        D->Inputs[2].isConst() && D->Inputs[2].ConstVal == 0 &&
        D->Inputs[1].Size == D->Output.Size &&
        D->Inputs[2].Size == D->Output.Size)
      return comparison(D->Inputs[0], Depth + 1);
    if ((D->Opcode != NdOp::INT_EQUAL && D->Opcode != NdOp::INT_NOTEQUAL) ||
        D->NumInputs != 2 ||
        (!sameWidthOrZero(D->Inputs[0], D->Inputs[1].Size) &&
         !sameWidthOrZero(D->Inputs[1], D->Inputs[0].Size)))
      return std::nullopt;
    auto A = canonical(D->Inputs[0]), B = canonical(D->Inputs[1]);
    if (!A || !B)
      return std::nullopt;
    if (A->isConst())
      std::swap(A, B);
    if (B->isConst() && B->ConstVal == 0)
      if (const MedOp *Sub = definition(*A);
          Sub && Sub->Opcode == NdOp::INT_SUB && Sub->NumInputs == 2 &&
          sameWidthOrZero(Sub->Inputs[0], B->Size) &&
          sameWidthOrZero(Sub->Inputs[1], B->Size)) {
        A = canonical(Sub->Inputs[0]);
        B = canonical(Sub->Inputs[1]);
      }
    if (!A || !B)
      return std::nullopt;
    if (B->isConst() && B->ConstVal == 0 && B->Size < A->Size)
      B->Size = A->Size;
    return Comparison{D->Opcode, *A, *B};
  }

  bool sameBoolean(const MedVar &A, const MedVar &B) {
    auto Left = comparison(A), Right = comparison(B);
    return Left && Right && Left->Opcode == Right->Opcode &&
           ((same(Left->Left, Right->Left) &&
             same(Left->Right, Right->Right)) ||
            (same(Left->Left, Right->Right) && same(Left->Right, Right->Left)));
  }

  const MedFunc &Function;
  std::map<VarKey, const MedOp *> Definitions;
  size_t &Work;
  bool &Exhausted;
  bool Valid = true;
};

LibraryRecognition recognition(const MedFunc &Function,
                               const LibraryFeaturePack &Pack,
                               const LibraryFeatureRule &Rule) {
  LibraryRecognition R;
  R.Function = Function.Entry;
  R.Pack = Pack.Id;
  R.PackSHA256 = Pack.SHA256;
  R.ProfileSHA256 = Pack.ProfileSHA256;
  R.EvidenceSHA256 = Pack.EvidenceSHA256;
  R.Rule = Rule.Id;
  R.RuleRevision = Rule.Revision;
  R.Family = Rule.Family;
  R.Operation = Rule.Operation;
  R.ReceiverType = Rule.ReceiverType;
  R.SourceOrigin = Pack.SourceOrigin;
  R.SourceRevision = Pack.SourceRevision;
  R.DisplayName = Rule.ReceiverType + "::" + Rule.Operation;
  return R;
}

} // namespace

MedLibraryIdentityIndex::MedLibraryIdentityIndex(const BinaryImage &Image)
    : Image(&Image) {
  for (const Symbol &S : Image.Symbols)
    if (S.IsFunc && S.Origin >= NameOrigin::Stated && !S.Name.empty())
      Names[S.Addr].push_back(S.Name);
  for (const Export &E : Image.Exports)
    if (!E.Name.empty())
      Names[E.Addr].push_back(E.Name);
  for (auto &[Entry, At] : Names) {
    (void)Entry;
    std::sort(At.begin(), At.end());
    At.erase(std::unique(At.begin(), At.end()), At.end());
  }
}

llvm::ArrayRef<std::string>
MedLibraryIdentityIndex::names(const BinaryImage &Image, va_t Entry) const {
  if (this->Image != &Image)
    return {};
  auto I = Names.find(Entry);
  return I == Names.end() ? llvm::ArrayRef<std::string>{}
                          : llvm::ArrayRef<std::string>(I->second);
}

MedLibraryRecognitionResult neverd::recognizeMedLibraryOperations(
    const MedFunc &Function, const BinaryImage &Image,
    const std::map<std::string, LibraryFeaturePack> &Packs,
    llvm::ArrayRef<MedLibraryReceiver> Receivers, size_t MaxWork,
    const MedLibraryIdentityIndex *Identities) {
  MedLibraryRecognitionResult Result;
  if (Image.Arch != Arch::X64 && Image.Arch != Arch::AArch64)
    return Result;
  if (Function.SkippedSSA)
    return Result;
  ExpressionMatcher Matcher(Function, MaxWork, Result.BudgetExhausted);
  if (!Matcher.valid() || Result.BudgetExhausted)
    return Result;
  // Read stated names once, before inspecting rules. The same bounded work
  // allowance covers symbol lookup and structural proof.
  std::set<std::string> StatedNames;
  if (Identities) {
    for (const std::string &Name : Identities->names(Image, Function.Entry)) {
      if (!Matcher.spend())
        return Result;
      StatedNames.insert(Name);
    }
  } else {
    for (const Symbol &S : Image.Symbols) {
      if (!Matcher.spend())
        return Result;
      if (S.IsFunc && S.Addr == Function.Entry &&
          S.Origin >= NameOrigin::Stated)
        StatedNames.insert(S.Name);
    }
    for (const Export &E : Image.Exports) {
      if (!Matcher.spend())
        return Result;
      if (E.Addr == Function.Entry)
        StatedNames.insert(E.Name);
    }
  }
  const auto Registers =
      getTargetRegInfo(Image.Arch).integerParamRegs(Image.Format);
  std::set<std::tuple<std::string, std::string, VarKey>> Seen;
  for (const auto &[ID, Pack] : Packs) {
    if (Result.BudgetExhausted)
      break;
    (void)ID;
    if (!Pack.accepts(Image.Arch, Image.Format, Image.Bits))
      continue;
    for (const LibraryFeatureRule &Rule : Pack.Rules) {
      if (!Matcher.spend())
        break;
      if (Rule.PatternKind != LibraryFeatureRule::Kind::Expression ||
          Rule.Identity != LibraryFeatureIdentity::Receiver)
        continue;
      std::string Symbol;
      for (const std::string &Name : Rule.WholeFunctionSymbols) {
        if (!Matcher.spend())
          break;
        if (StatedNames.contains(Name)) {
          Symbol = Name;
          break;
        }
      }
      const auto Layout = Pack.Layouts.find(Rule.Layout);
      if (Layout == Pack.Layouts.end())
        continue;
      for (const MedBlock &Block : Function.Blocks) {
        if (Result.BudgetExhausted)
          break;
        for (const MedOp &Op : Block.Ops) {
          if (!Matcher.spend())
            break;
          if (Op.Dead || !Op.Output.Size || Op.Opcode == NdOp::COPY ||
              Op.OriginSeq < 0 || Op.Addr == InvalidVA)
            continue;
          MatchState State;
          State.Bindings.resize(Rule.Nodes.size());
          if (!Matcher.match(Rule, Rule.Result, Op.Output, State) ||
              !State.Receiver)
            continue;
          if (!Matcher.consistentMemory(State))
            continue;
          std::string Evidence;
          bool Contradiction = false;
          for (const MedLibraryReceiver &Receiver : Receivers)
            if (!Receiver.Evidence.empty() &&
                Matcher.entryRegister(*State.Receiver, Receiver.Register) &&
                (Receiver.Type != Rule.ReceiverType ||
                 Receiver.ObjectBytes != Layout->second.ObjectBytes))
              Contradiction = true;
          if (Contradiction)
            continue;
          if (!Symbol.empty() && !Registers.empty() &&
              Matcher.entryRegister(*State.Receiver, Registers.front())) {
            Evidence = "stated-member-symbol";
          } else {
            for (const MedLibraryReceiver &Receiver : Receivers)
              if (Receiver.Type == Rule.ReceiverType &&
                  Receiver.ObjectBytes == Layout->second.ObjectBytes &&
                  !Receiver.Evidence.empty() &&
                  Matcher.entryRegister(*State.Receiver, Receiver.Register)) {
                Evidence = Receiver.Evidence;
                break;
              }
          }
          if (Evidence.empty())
            continue;
          const MedVar Root = *State.Bindings[Rule.Result];
          const bool Whole =
              !Symbol.empty() &&
              Rule.supports(LibraryFeatureScope::WholeFunction) &&
              Matcher.wholeFunction(State, Root,
                                    getTargetRegInfo(Image.Arch).IntReturnReg);
          if (!Whole && !Rule.supports(LibraryFeatureScope::InlineExpression))
            continue;
          if (!Seen.emplace(Pack.Id, Rule.Id, key(Root)).second)
            continue;
          LibraryRecognition Match = recognition(Function, Pack, Rule);
          Match.IdentityEvidence = Evidence;
          Match.Scope = Whole ? LibraryFeatureScope::WholeFunction
                              : LibraryFeatureScope::InlineExpression;
          Match.LinkageName = Whole ? Symbol : std::string();
          if (Whole) {
            llvm::StringRef Linkage(Symbol);
            if (Image.Format == BinaryFormat::MachO &&
                Linkage.starts_with("__Z"))
              Linkage = Linkage.drop_front();
            Match.DisplayName = llvm::demangle(Linkage.str());
          }
          Match.Isolated = Matcher.isolated(State, Root);
          const MedOp *RootDefinition = Matcher.definition(Root);
          if (!RootDefinition || RootDefinition->OriginSeq < 0 ||
              RootDefinition->Addr == InvalidVA)
            continue;
          Match.ResultOccurrence = LibraryOccurrence{RootDefinition->Addr,
                                                     RootDefinition->OriginSeq};
          for (const MedOp *O : State.Operations)
            if (O->Addr != InvalidVA && O->OriginSeq >= 0)
              Match.Occurrences.push_back({O->Addr, O->OriginSeq});
          std::sort(Match.Occurrences.begin(), Match.Occurrences.end());
          Match.Occurrences.erase(
              std::unique(Match.Occurrences.begin(), Match.Occurrences.end()),
              Match.Occurrences.end());
          Result.Matches.push_back(std::move(Match));
          if (Result.Matches.size() > 128) {
            Result.BudgetExhausted = true;
            MaxWork = 0;
            break;
          }
        }
      }
    }
  }
  if (Result.BudgetExhausted)
    Result.Matches.clear();
  // A structural overlap is not a license to hide a second operation. A
  // conflicting profile cannot supply an identity at all. Keep this decision
  // independent of the directory/file enumeration order.
  std::set<size_t> Conflicts;
  for (size_t I = 0; I < Result.Matches.size(); ++I)
    for (size_t J = I + 1; J < Result.Matches.size(); ++J) {
      auto &A = Result.Matches[I];
      auto &B = Result.Matches[J];
      bool Overlap = false;
      for (const auto &Occurrence : A.Occurrences) {
        if (!Matcher.spend())
          break;
        if (std::binary_search(B.Occurrences.begin(), B.Occurrences.end(),
                               Occurrence)) {
          Overlap = true;
          break;
        }
      }
      if (!Overlap)
        continue;
      if (A.Pack != B.Pack) {
        Conflicts.insert(I);
        Conflicts.insert(J);
      }
      A.Isolated = B.Isolated = false;
    }
  if (Result.BudgetExhausted) {
    Result.Matches.clear();
    return Result;
  }
  size_t Index = 0;
  std::erase_if(Result.Matches,
                [&](const auto &) { return Conflicts.contains(Index++); });
  std::sort(Result.Matches.begin(), Result.Matches.end(),
            [](const auto &A, const auto &B) {
              return std::tie(A.Function, A.Occurrences, A.Pack, A.Rule) <
                     std::tie(B.Function, B.Occurrences, B.Pack, B.Rule);
            });
  return Result;
}
