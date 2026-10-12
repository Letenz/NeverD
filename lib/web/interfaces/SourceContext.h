//===- SourceContext.h - Bounded passive source evidence context -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared model admission, indexes and publication for syntax adapters.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Interfaces.h"

#include <set>

namespace neverd::web::interfaces {

inline constexpr auto None = NoSourceIndex;

struct SourceContext {
  const SourceAnalysis &S;
  const SourceBindingAnalysis &B;
  const SourceValueAnalysis &V;
  SourceInterfaces A;
  std::vector<uint32_t> References, Declarations, Parents;
  std::vector<bool> WrittenBindings;
  std::set<std::u16string> WrittenGlobals;

  void prepare();
  void step(uint64_t N = 1);
  uint32_t child(uint32_t I, std::string_view Field, uint32_t Ordinal = 0);
  const std::u16string *constant(uint32_t I) const;
  const std::u16string *member(uint32_t I);
  std::optional<std::string> utf8(std::u16string_view Text);
  bool external(uint32_t I, std::u16string_view Name) const;
  uint32_t binding(uint32_t I) const;
  bool spreadArguments(uint32_t I);
  void url(SourceInterface &R);
  void publish(SourceInterface R);
};

void inspectRequestSyntax(SourceContext &C, uint32_t I);
std::vector<uint32_t> xhrConstructors(SourceContext &C);
void inspectXHR(SourceContext &C, const std::vector<uint32_t> &Constructors,
                uint32_t I);

} // namespace neverd::web::interfaces
