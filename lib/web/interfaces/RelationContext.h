//===- RelationContext.h - Bounded request relationship context --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared model admission and publication for request syntax relationships.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/InterfaceRelations.h"

namespace neverd::web::interface_relations {

inline constexpr auto None = NoSourceIndex;

struct Context {
  const SourceAnalysis &S;
  const SourceBindingAnalysis &B;
  const SourceInterfaces &I;
  const SourceNavigation &N;
  InterfaceRelationLimits Limits;
  InterfaceSourceRelations A;
  std::vector<uint32_t> Parents, Calls, Requests, Declarations, References;
  std::vector<uint32_t> ResultBindings;
  std::vector<bool> WrittenBindings;

  void step(uint64_t Count = 1);
  void prepare();
  uint32_t child(uint32_t Node, std::string_view Field, uint32_t Ordinal = 0);
  const std::u16string *member(uint32_t Node);
  uint32_t binding(uint32_t Node) const;
  uint32_t function(uint32_t Node) const;
  bool spread(uint32_t Node);
  void publish(InterfaceSourceRelation Relation);
};

void requestLinks(Context &C, uint32_t Request);
void resultLinks(Context &C, uint32_t Node);

} // namespace neverd::web::interface_relations
