//===- RewriteInternal.h - Local rewrite implementation boundary --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared work accounting, primitive replacement and lexical validation.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/SourceBindings.h"
#include "neverd/web/SourceEffects.h"
#include "neverd/web/SourceRewrite.h"
#include "neverd/web/SourceValues.h"

#include <bit>

namespace neverd::web::rewrite {

void charge(uint64_t &Steps, uint64_t Count = 1);

// Receipts publish the step count, so it must not depend on how a C++
// library searches or sorts. Charge ordered searches and sorts by their
// deterministic comparison bounds, never per comparator call.
inline uint64_t searchSteps(uint64_t Size) { return std::bit_width(Size); }
inline uint64_t sortSteps(uint64_t Size) {
  return Size * std::bit_width(Size ? Size - 1 : 0);
}
void append(std::string &Output, std::string_view Bytes, uint64_t &Steps);
std::string primitive(const SourcePrimitive &Value, uint64_t &Steps);
bool sameValue(const SourcePrimitive &Left, const SourcePrimitive &Right,
               uint64_t &Steps);
void apply(SourceRewrite &Result, std::string_view Original);
void validate(SourceRewrite &Result, const SourceAnalysis &Original,
              const SourceValueAnalysis &Values);
void planRename(SourceRewrite &Result, const SourceAnalysis &Original,
                const SourceBindingAnalysis &Bindings, std::string_view Bytes);
void validateRename(SourceRewrite &Result, const SourceAnalysis &Original,
                    const SourceBindingAnalysis &Bindings);
void planBranches(SourceRewrite &Result, const SourceAnalysis &Original,
                  const SourceBindingAnalysis &Bindings,
                  const SourceValueAnalysis &Values,
                  const SourceEffectAnalysis &Effects, std::string_view Bytes);
void validateBranches(SourceRewrite &Result, const SourceAnalysis &Original,
                      const SourceBindingAnalysis &Bindings);
std::string branchRecordIdentity(const SourceRewrite &Result,
                                 const SourceRewriteRecord &Record);

} // namespace neverd::web::rewrite
