//===- InterfaceRelations.h - Request source relationships -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independently bounded syntax relationships, never runtime response flow.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Interfaces.h"
#include "neverd/web/Limits.h"
#include "neverd/web/SourceNavigation.h"

namespace neverd::web {

inline constexpr std::string_view InterfaceSourceRelationProfile =
    "request-source-relations-v2";

struct InterfaceRelationLimits {
  uint64_t Records = MaxInterfaceSourceRelations;
  uint64_t Steps = MaxInterfaceRelationSteps;
};

/// Indices name the exact input models. MemberKind is a fixed spelling class;
/// neither it nor a lexical result binding authenticates a response API.
struct InterfaceSourceRelation {
  std::string ID, Kind;
  uint32_t Request = NoSourceIndex, Node = NoSourceIndex;
  uint32_t FunctionNode = NoSourceIndex, ReceiverNode = NoSourceIndex;
  uint32_t Binding = NoSourceIndex, HandlerNode = NoSourceIndex;
  uint32_t InputNode = NoSourceIndex;
  uint32_t ContainmentDepth = 0;
  std::string MemberKind = "not_applicable";
  std::string ArgumentsStatus = "not_applicable";
  std::string HandlerStatus = "not_applicable";
};

struct InterfaceSourceRelations {
  std::string ID, InterfaceID, SourceID, BindingID, NavigationID;
  std::string Status = "unavailable", Reason;
  std::vector<InterfaceSourceRelation> Records;
  uint64_t Steps = 0;
};

/// Consume the authoritative navigation model without inferring caller
/// targets, wrapper argument roles, execution, response values or ordering.
/// Failure clears these relations and never modifies request evidence.
InterfaceSourceRelations analyzeInterfaceSourceRelations(
    const SourceAnalysis &Source, const SourceBindingAnalysis &Bindings,
    const SourceInterfaces &Interfaces, const SourceNavigation &Navigation);
InterfaceSourceRelations analyzeInterfaceSourceRelations(
    const SourceAnalysis &Source, const SourceBindingAnalysis &Bindings,
    const SourceInterfaces &Interfaces, const SourceNavigation &Navigation,
    InterfaceRelationLimits Limits);

} // namespace neverd::web
