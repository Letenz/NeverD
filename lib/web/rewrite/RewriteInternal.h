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
#include "neverd/web/SourceRewrite.h"
#include "neverd/web/SourceValues.h"

namespace neverd::web::rewrite {

void charge(uint64_t &Steps, uint64_t Count = 1);
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

} // namespace neverd::web::rewrite
