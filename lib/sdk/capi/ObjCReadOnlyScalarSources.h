#ifndef NEVERD_SDK_CAPI_OBJC_READONLYSCALARSOURCES_H
#define NEVERD_SDK_CAPI_OBJC_READONLYSCALARSOURCES_H

#include "BorrowedByteSources.h"

#include "neverd/ir/high/HighSourceFlow.h"

#include <algorithm>
#include <map>
#include <set>

namespace neverd::sdk {
namespace readonly_scalar_detail {
struct LoadPlan {
  va_t Base = 0;
  uint32_t Extent = 0;
  uint64_t Stride = 1;
  uint64_t SignLimit = UINT64_MAX;
  ExprPtr Index;
  ExprPtr Offset;
  ExprPtr BaseExpression;
};
inline bool ordinary(const ExprPtr &E) {
  return E && E->Type && E->IntrinsicId == Intrinsic::None &&
         E->IntrinsicOutputs.empty() &&
         E->MemoryOrdering == NdMemoryOrdering::None &&
         E->MemoryAddressSpace == NdMemoryAddressSpace::Default;
}
inline std::optional<LoadPlan> loadPlan(const ExprPtr &E, bool Bound) {
  if (!ordinary(E) || E->Kind != ExprKind::Load || E->Operands.size() != 1 ||
      !((E->Type->Kind == NdTypeKind::Int &&
         (E->Type->Size == 1 || E->Type->Size == 2 || E->Type->Size == 4 ||
          E->Type->Size == 8)) ||
        (E->Type->Kind == NdTypeKind::Float &&
         (E->Type->Size == 4 || E->Type->Size == 8))))
    return std::nullopt;
  const auto &Address = E->Operands[0];
  if (!ordinary(Address) || Address->Type->Size != 8 ||
      Address->Kind != ExprKind::BinOp || Address->Op != NdOp::INT_ADD ||
      Address->Operands.size() != 2)
    return std::nullopt;
  LoadPlan P;
  for (unsigned Side = 0; Side < 2; ++Side) {
    const auto &Base = Address->Operands[Side];
    if (!ordinary(Base) || Base->Type->Size != 8 || !Base->Operands.empty())
      continue;
    if (!Bound && Base->Kind == ExprKind::Const &&
        Base->Type->Kind == NdTypeKind::Int) {
      P.Base = Base->ConstVal;
    } else if (Bound && Base->Kind == ExprKind::Call && Base->SourceCallHint &&
               Base->SourceCallHint->CallKind ==
                   SourceCallTypeHint::Kind::RuntimeReadOnlyBytes) {
      P.Base = Base->SourceCallHint->TargetAddress;
      P.Extent = Base->SourceCallHint->ByteCount;
    } else {
      continue;
    }
    P.BaseExpression = Base;
    P.Offset = Address->Operands[1 - Side];
    break;
  }
  if (!P.Base || !ordinary(P.Offset) || P.Offset->Type->Size != 8 ||
      P.Offset->Type->Kind != NdTypeKind::Int)
    return std::nullopt;
  P.Index = P.Offset;
  if (P.Index->Kind == ExprKind::BinOp &&
      (P.Index->Op == NdOp::INT_LEFT || P.Index->Op == NdOp::INT_MULT) &&
      P.Index->Operands.size() == 2) {
    const auto &Scale = P.Index->Operands[1];
    if (!ordinary(Scale) || Scale->Kind != ExprKind::Const ||
        !Scale->Operands.empty() || Scale->Type->Kind != NdTypeKind::Int ||
        !Scale->Type->Size || Scale->Type->Size > 8 ||
        (Scale->Type->Size < 8 &&
         Scale->ConstVal >= (uint64_t{1} << (Scale->Type->Size * 8))))
      return std::nullopt;
    if (P.Index->Op == NdOp::INT_LEFT) {
      if (Scale->ConstVal > 16)
        return std::nullopt;
      P.Stride = uint64_t{1} << Scale->ConstVal;
    } else {
      if (Scale->Type->Size != 8 || !Scale->ConstVal || Scale->ConstVal > 65536)
        return std::nullopt;
      P.Stride = Scale->ConstVal;
    }
    P.Index = P.Index->Operands[0];
    if (!ordinary(P.Index) || P.Index->Type->Kind != NdTypeKind::Int ||
        P.Index->Type->Size != 8)
      return std::nullopt;
  }
  if (P.Index->Kind == ExprKind::UnaryOp && P.Index->Operands.size() == 1 &&
      (P.Index->Op == NdOp::INT_ZEXT || P.Index->Op == NdOp::INT_SEXT)) {
    const auto &Narrow = P.Index->Operands[0];
    if (!ordinary(Narrow) || Narrow->Type->Kind != NdTypeKind::Int ||
        !Narrow->Type->Size || Narrow->Type->Size > 8)
      return std::nullopt;
    if (P.Index->Op == NdOp::INT_SEXT)
      P.SignLimit = uint64_t{1} << (Narrow->Type->Size * 8 - 1);
    P.Index = Narrow;
  }
  return P;
}
} // namespace readonly_scalar_detail

/// Every occurrence of a shared load must fit the same immutable byte copy.
/// A pointer-valued or escaping consumer is separately excluded by projection.
inline std::map<const HighExpr *, readonly_scalar_detail::LoadPlan>
readOnlyScalarLoadPlans(const HighFunc &Function, const BinaryImage &Image,
                        bool Bound = false) {
  using namespace readonly_scalar_detail;
  struct Occurrence {
    ExprPtr Load;
    LoadPlan Plan;
  };
  std::vector<Occurrence> Occurrences;
  std::vector<HighSourceUnsignedRangeQuery> Queries;
  size_t Budget = 100000;
  walkStmts(Function.Body, [&](const HighStmt &S) {
    if (!Budget)
      return;
    --Budget;
    std::set<const HighExpr *> Seen;
    forEachExpr(S, [&](const ExprPtr &Root) {
      std::vector<ExprPtr> Pending{Root};
      while (!Pending.empty() && Budget) {
        --Budget;
        auto E = Pending.back();
        Pending.pop_back();
        if (!E || !Seen.insert(E.get()).second)
          continue;
        if (auto P = loadPlan(E, Bound)) {
          if (Queries.size() == 128) {
            Budget = 0;
            return;
          }
          Queries.push_back({&S, P->Index});
          Occurrences.push_back({E, std::move(*P)});
        }
        Pending.insert(Pending.end(), E->Operands.begin(), E->Operands.end());
      }
    });
  });
  std::map<const HighExpr *, LoadPlan> Plans;
  if (!Budget || Queries.empty())
    return Plans;
  const auto Bounds = highSourceUnsignedUpperBounds(Function, Queries);
  std::set<const HighExpr *> Rejected;
  for (size_t I = 0; I < Occurrences.size(); ++I) {
    auto &[Load, P] = Occurrences[I];
    const auto Upper = Bounds[I];
    // Bound both the scalar inventory and the byte copy; sparse strides do
    // not permit unbounded reads or source helper growth.
    if (!Upper || *Upper >= 4096 || *Upper >= P.SignLimit ||
        *Upper > (65536 - Load->Type->Size) / P.Stride) {
      Rejected.insert(Load.get());
      continue;
    }
    const auto Extent = uint32_t(*Upper * P.Stride + Load->Type->Size);
    if (Bound && (P.Extent < Extent || P.Extent > 65536)) {
      Rejected.insert(Load.get());
      continue;
    }
    if (!Bound)
      P.Extent = Extent;
    const auto Bytes = readImmutableImageBytes(Image, P.Base, P.Extent);
    if (!Bytes) {
      Rejected.insert(Load.get());
      continue;
    }
    // Scalar copies do not relocate pointers, including unmarked values that
    // happen to point into this image. The existing pointer reader owns them.
    bool Scalar = true;
    for (uint64_t Entry = 0; Entry <= *Upper && Scalar; ++Entry) {
      uint64_t Bits = 0;
      for (unsigned B = 0; B < Load->Type->Size; ++B)
        Bits |= uint64_t((*Bytes)[Entry * P.Stride + B]) << (B * 8);
      Scalar = !isImagePointerBitPattern(Image, Bits, Load->Type->Size);
    }
    if (!Scalar) {
      Rejected.insert(Load.get());
      continue;
    }
    auto [It, Fresh] = Plans.emplace(Load.get(), P);
    if (!Fresh)
      It->second.Extent = std::max(It->second.Extent, P.Extent);
  }
  for (const auto *E : Rejected)
    Plans.erase(E);
  return Plans;
}

/// Revalidate current bounds, bytes and every occurrence of each helper.
/// A table address has no authority outside its proven scalar loads.
inline std::set<const HighExpr *>
readOnlyScalarSourceHelpers(const HighFunc &Function,
                            const BinaryImage &Image) {
  const auto Plans = readOnlyScalarLoadPlans(Function, Image, true);
  std::set<const HighExpr *> Allowed, Escaped;
  size_t Budget = 100000;
  walkStmts(Function.Body, [&](const HighStmt &S) {
    if (!Budget)
      return;
    --Budget;
    forEachExpr(S, [&](const ExprPtr &Root) {
      std::vector<ExprPtr> Pending{Root};
      std::set<const HighExpr *> Seen;
      while (!Pending.empty() && Budget) {
        --Budget;
        auto E = Pending.back();
        Pending.pop_back();
        if (!E || !Seen.insert(E.get()).second)
          continue;
        if (const auto P = Plans.find(E.get()); P != Plans.end()) {
          Allowed.insert(P->second.BaseExpression.get());
          Pending.push_back(P->second.Offset);
          continue;
        }
        if (E->SourceCallHint &&
            E->SourceCallHint->CallKind ==
                SourceCallTypeHint::Kind::RuntimeReadOnlyBytes)
          Escaped.insert(E.get());
        Pending.insert(Pending.end(), E->Operands.begin(), E->Operands.end());
      }
    });
  });
  if (!Budget)
    return {};
  for (const auto *E : Escaped)
    Allowed.erase(E);
  return Allowed;
}
} // namespace neverd::sdk
#endif
