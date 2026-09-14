//===- SourceProjectionEvidenceJSON.h - Source evidence serialization -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_SOURCEPROJECTIONEVIDENCEJSON_H
#define NEVERD_SDK_CAPI_SOURCEPROJECTIONEVIDENCEJSON_H

#include "JSONText.h"
#include "ObjCNativeDependencies.h"
#include "SourceProjectionDiagnostics.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd::sdk {

inline llvm::json::Object nativeSourceDependencyEvidenceJSON(
    const NativeSourceDependencyEvidence &Evidence) {
  auto Addresses = [](const std::set<va_t> &Values) {
    llvm::json::Array Array;
    for (va_t Address : Values)
      Array.push_back("0x" + llvm::utohexstr(Address, true));
    return Array;
  };
  llvm::json::Array Calls;
  for (const auto &Call : Evidence.Calls)
    Calls.push_back(llvm::json::Object{
        {"caller", "0x" + llvm::utohexstr(Call.Caller, true)},
        {"block_address", "0x" + llvm::utohexstr(Call.Block, true)},
        {"instruction_address", "0x" + llvm::utohexstr(Call.Instruction, true)},
        {"target_address",
         Call.Indirect
             ? llvm::json::Value(nullptr)
             : llvm::json::Value("0x" + llvm::utohexstr(Call.Target, true))},
        {"indirect", Call.Indirect}});
  return llvm::json::Object{
      {"scope", "supported_objc_roots_and_direct_native_dependencies"},
      {"inventory_complete", Evidence.InventoryComplete},
      {"targets_complete", Evidence.TargetsComplete},
      {"roots", Addresses(Evidence.Roots)},
      {"missing_functions", Addresses(Evidence.MissingFunctions)},
      {"calls", std::move(Calls)}};
}

inline const char *sourceProjectionIssueName(SourceProjectionIssue Issue) {
  switch (Issue) {
  case SourceProjectionIssue::Signature:
    return "signature";
  case SourceProjectionIssue::ABI:
    return "abi";
  case SourceProjectionIssue::Audit:
    return "native_coverage";
  case SourceProjectionIssue::Body:
    return "body";
  case SourceProjectionIssue::Exception:
    return "exception";
  case SourceProjectionIssue::ControlFlow:
    return "control_flow";
  case SourceProjectionIssue::DefiniteAssignment:
    return "definite_assignment";
  case SourceProjectionIssue::UnresolvedValue:
    return "unresolved_value";
  case SourceProjectionIssue::CallBinding:
    return "call_binding";
  case SourceProjectionIssue::ParameterBinding:
    return "parameter_binding";
  case SourceProjectionIssue::IncomingValue:
    return "incoming_value";
  case SourceProjectionIssue::LocalDefinition:
    return "local_definition";
  case SourceProjectionIssue::MalformedExpression:
    return "malformed_expression";
  case SourceProjectionIssue::Budget:
    return "budget";
  case SourceProjectionIssue::DataBinding:
    return "data_binding";
  case SourceProjectionIssue::Dependency:
    return "dependency";
  }
  return "unknown";
}

inline llvm::json::Object sourceCallEvidenceJSON(const HighExpr &Call) {
  llvm::json::Object Object{
      {"target_name", jsonSafeText(Call.CallTarget)},
      {"target_address", "0x" + llvm::utohexstr(Call.CallAddr, true)},
      {"indirect", Call.IsIndirectCall},
      {"recovered_arguments", static_cast<int64_t>(Call.Operands.size())}};
  if (Call.SourceCallHint) {
    const auto &Binding = *Call.SourceCallHint;
    Object["binding_name"] = jsonSafeText(Binding.TargetName);
    Object["expected_arguments"] =
        static_cast<int64_t>(Binding.Signature.Parameters.size());
  }
  return Object;
}

inline llvm::json::Object
sourceProjectionEvidenceJSON(const SourceProjectionDiagnostics &Diagnostics) {
  llvm::json::Array Items;
  for (const auto &Item : Diagnostics.Items) {
    llvm::json::Object Object{{"code", sourceProjectionIssueName(Item.Issue)},
                              {"reason", jsonSafeText(Item.Reason)}};
    if (Item.StatementAddress && Item.StatementAddress != InvalidVA)
      Object["statement_address"] =
          "0x" + llvm::utohexstr(Item.StatementAddress, true);
    if (Item.RelatedAddress && Item.RelatedAddress != InvalidVA)
      Object["related_address"] =
          "0x" + llvm::utohexstr(Item.RelatedAddress, true);
    if (const auto *Expression = Item.Expression) {
      if (Expression->Kind == ExprKind::Call)
        Object["call"] = sourceCallEvidenceJSON(*Expression);
      if (Expression->Kind == ExprKind::Var ||
          Expression->Kind == ExprKind::Phi) {
        const auto &Variable = Expression->Var;
        Object["value"] = llvm::json::Object{
            {"kind", static_cast<int64_t>(Variable.Kind)},
            {"id", Variable.Id},
            {"ssa_version", Variable.SSAVer},
            {"rename_tag", Variable.RenameTag},
            {"register_offset", "0x" + llvm::utohexstr(Variable.RegOff, true)},
            {"stack_offset", Variable.StackOff}};
      }
    }
    Items.push_back(std::move(Object));
  }
  return llvm::json::Object{{"checks_complete", Diagnostics.Complete},
                            {"items", std::move(Items)}};
}

} // namespace neverd::sdk

#endif
