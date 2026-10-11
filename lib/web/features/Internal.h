//===- Internal.h - Bounded client syntax pattern context --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared indexes, admission and source-only relationship construction.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/SourceFeatures.h"
#include "neverd/web/SourceNavigation.h"

namespace neverd::web::client_features {

inline constexpr auto None = NoSourceIndex;
struct Context {
  const SourceAnalysis &S;
  const SourceBindingAnalysis &B;
  const SourceNavigation &N;
  SourceFeatures A;
  std::vector<uint32_t> Parents, Functions, FunctionNodes, Calls, References;
  std::vector<uint32_t> BindingFunctions;

  void step(uint64_t Count = 1);
  void prepare();
  uint32_t child(uint32_t Node, std::string_view Field);
  const std::u16string *key(uint32_t Node, bool Computed);
  bool ordinaryProperty(uint32_t Node) const;
  void callback(SourceFeature &Record);
  void publish(SourceFeature Record);
};

void recognizeCall(Context &C, uint32_t Index);
void recognizeObject(Context &C, uint32_t Index);
void recognizeProperty(Context &C, uint32_t Index);

} // namespace neverd::web::client_features
