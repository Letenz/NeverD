#ifndef NEVERD_SDK_CAPI_SOURCERECEIPTOCCURRENCES_H
#define NEVERD_SDK_CAPI_SOURCERECEIPTOCCURRENCES_H

#include "neverd/ir/SourceCallTypeHint.h"
#include "neverd/ir/high/HighIR.h"

#include <algorithm>
#include <optional>
#include <set>
#include <vector>

namespace neverd::sdk {
// Inventory current ordinary-call receipts, including every exception arm.
// This adds no annotation or exception authority. An empty inventory creates
// no obligation for this receipt owner; a nonempty one still requires its own
// current producer/body proof and cannot combine with exception regions.
template <class Receipt>
inline std::optional<std::set<SourceCallOccurrenceKey>>
ordinarySourceReceiptOccurrences(
    const HighFunc &Function,
    std::optional<Receipt> SourceCallTypeHint::*Member) {
  std::set<SourceCallOccurrenceKey> Sites;
  bool Invalid = false;
  bool HasExceptions = Function.StructuredExceptionRegions ||
                       Function.UnstructuredExceptionRegions;
  size_t Budget = 100000;
  std::vector<std::pair<const HighStmt *, unsigned>> Statements;
  const auto Append = [&](const std::vector<HighStmt> &Body, unsigned Depth) {
    if (Depth >= 64 ||
        Body.size() > Budget - std::min(Budget, Statements.size())) {
      Budget = 0;
      return;
    }
    for (const auto &S : Body)
      Statements.emplace_back(&S, Depth);
  };
  Append(Function.Body, 0);
  while (Budget && !Statements.empty()) {
    --Budget;
    const auto [Statement, Depth] = Statements.back();
    Statements.pop_back();
    const auto &S = *Statement;
    forEachExpr(S, [&](const ExprPtr &Root) {
      std::vector<ExprPtr> Pending{Root};
      while (!Pending.empty() && Budget) {
        --Budget;
        auto E = Pending.back();
        Pending.pop_back();
        if (!E)
          continue;
        if (E->SourceCallHint) {
          const auto &R = E->SourceCallHint.get()->*Member;
          if (R)
            Invalid |= E->Kind != ExprKind::Call || E->IsIndirectCall ||
                       E->IndirectTarget ||
                       R->FunctionEntry != Function.Entry ||
                       !Sites.insert(R->Site).second;
        }
        // Count evaluations, not shared pointers: a copied call remains two
        // occurrences, and expression cycles exhaust the bounded inventory.
        E->forEachChildExpr([&](const ExprPtr &Child) {
          if (Pending.size() >= Budget)
            Budget = 0;
          else
            Pending.push_back(Child);
        });
      }
    });
    HasExceptions |= S.Kind == StmtKind::SEHTry || S.Kind == StmtKind::CxxTry ||
                     S.Kind == StmtKind::ItaniumTry || S.EHIsReducible ||
                     !S.EHClauses.empty() || !S.EHClauseBodies.empty();
    Append(S.Body, Depth + 1);
    Append(S.ElseBody, Depth + 1);
    Append(S.DefaultBody, Depth + 1);
    for (const auto &Case : S.Cases)
      Append(Case.Body, Depth + 1);
    for (const auto &Clause : S.EHClauseBodies)
      Append(Clause, Depth + 1);
  }
  return Budget && !Invalid && (!HasExceptions || Sites.empty())
             ? std::optional(Sites)
             : std::nullopt;
}
} // namespace neverd::sdk
#endif
