//===- Recognize.cpp - Closed client syntax pattern vocabulary ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recognize names and shapes without evaluating keys or claiming API identity.
///
//===----------------------------------------------------------------------===//

#include "Internal.h"

namespace neverd::web::client_features {
namespace {
using Domain = SourceFeatureDomain;
using Pattern = SourceFeaturePattern;
struct CallPattern {
  std::u16string_view Name;
  Domain Role;
  Pattern Kind;
  uint32_t CallbackArity = 0;
};
// This is a spelling catalog, not an authenticated framework API table.
constexpr CallPattern CallPatterns[] = {
    {u"registerTool", Domain::Tool, Pattern::ToolRegistrationCall, 3},
    {u"executeTool", Domain::Tool, Pattern::ToolDispatchCall},
    {u"dispatchTool", Domain::Tool, Pattern::ToolDispatchCall},
    {u"loadSettings", Domain::Configuration, Pattern::ConfigurationCall},
    {u"readSettings", Domain::Configuration, Pattern::ConfigurationCall},
    {u"getSettings", Domain::Configuration, Pattern::ConfigurationCall},
    {u"checkPermission", Domain::Permission, Pattern::PermissionCall},
    {u"checkPermissions", Domain::Permission, Pattern::PermissionCall},
    {u"canUseTool", Domain::Permission, Pattern::PermissionCall},
    {u"setRequestHandler", Domain::MCP, Pattern::MCPCall, 2},
    {u"setNotificationHandler", Domain::MCP, Pattern::MCPCall, 2},
    {u"registerPrompt", Domain::MCP, Pattern::MCPCall, 3},
    {u"registerResource", Domain::MCP, Pattern::MCPCall, 3},
    {u"listTools", Domain::MCP, Pattern::MCPCall},
    {u"callTool", Domain::MCP, Pattern::MCPCall},
    {u"registerHook", Domain::Hook, Pattern::HookCall, 2},
    {u"runHooks", Domain::Hook, Pattern::HookCall},
    {u"executeHooks", Domain::Hook, Pattern::HookCall},
    {u"dispatchHooks", Domain::Hook, Pattern::HookCall}};
constexpr std::u16string_view HookEvents[] = {
    u"PreToolUse",         u"PostToolUse",
    u"PostToolUseFailure", u"PermissionRequest",
    u"UserPromptSubmit",   u"SessionStart",
    u"SessionEnd",         u"Stop",
    u"SubagentStart",      u"SubagentStop",
    u"PreCompact",         u"Notification"};

void nameStatus(Context &C, SourceFeature &R) {
  if (R.Name == None)
    R.NameStatus = "not_applicable";
  else
    R.NameStatus = C.S.Nodes[R.Name].Kind == "StringLiteral"
                       ? "literal_string_redacted"
                       : "nonliteral_expression";
}

void condition(Context &C, SourceFeature &R) {
  for (auto P = C.Parents[R.Node]; P != None; P = C.Parents[P]) {
    C.step();
    const auto &N = C.S.Nodes[P];
    if (C.FunctionNodes[P] != None)
      break;
    if (N.Kind != "IfStatement" && N.Kind != "ConditionalExpression")
      continue;
    const auto Test = C.child(P, "test");
    if (Test == None)
      continue;
    const auto &T = C.S.Nodes[Test];
    const auto &Call = C.S.Nodes[R.Node];
    if (T.Start <= Call.Start && T.End >= Call.End) {
      R.Condition = Test;
      break;
    }
  }
}

void eventArguments(Context &C, const SourceCall &Call) {
  uint32_t Arguments = 0;
  bool Spread = false;
  for (const auto &Arg : C.S.Nodes[Call.Node].Children) {
    C.step();
    if (Arg.Field == "arguments") {
      ++Arguments;
      Spread |= C.S.Nodes[Arg.Index].Kind == "SpreadElement";
    }
  }
  uint32_t Ordinal = 0;
  for (const auto &Arg : C.S.Nodes[Call.Node].Children) {
    C.step();
    if (Arg.Field != "arguments")
      continue;
    const auto *Value = C.key(Arg.Index, true);
    bool Event = false;
    if (Value)
      for (const auto Name : HookEvents) {
        C.step();
        Event |= *Value == Name;
      }
    if (Event) {
      SourceFeature R;
      R.Domain = Domain::Hook;
      R.Pattern = Pattern::HookEventArgument;
      R.Node = Call.Node;
      R.Trigger = R.Name = Arg.Index;
      R.ArgumentOrdinal = Ordinal;
      R.Arguments = Arguments;
      R.SpreadArguments = Spread;
      R.Basis = "hook_event_literal_call_argument";
      R.Status = Spread ? "unresolved_shape" : "matched_syntax";
      R.Reason = Spread ? "spread_arguments" : "";
      R.NameStatus = "literal_string_redacted";
      C.publish(std::move(R));
    }
    ++Ordinal;
  }
}
} // namespace

void recognizeCall(Context &C, uint32_t Index) {
  C.step();
  const auto &Call = C.N.Calls[Index];
  if (Call.Kind != "call" && Call.Kind != "optional_call")
    return;
  eventArguments(C, Call);
  const auto &Callee = C.S.Nodes.at(Call.Callee);
  uint32_t Trigger = Call.Callee;
  const std::u16string *Name = nullptr;
  std::string Basis;
  if (Callee.Kind == "Identifier") {
    Name = C.key(Trigger, false);
    Basis = "callee_identifier_spelling";
  } else if (Callee.Kind == "MemberExpression" ||
             Callee.Kind == "OptionalMemberExpression") {
    Trigger = C.child(Call.Callee, "property");
    Name = C.key(Trigger, Callee.flag("computed"));
    if (!Name && Callee.flag("computed"))
      ++C.A.ComputedMemberCalls;
    Basis = "callee_member_spelling";
  }
  if (!Name)
    return;
  for (const auto &Rule : CallPatterns) {
    C.step();
    if (*Name != Rule.Name)
      continue;
    SourceFeature R;
    R.Domain = Rule.Role;
    R.Pattern = Rule.Kind;
    R.Node = Call.Node;
    R.Trigger = Trigger;
    R.Basis = Basis;
    R.Status = "matched_syntax";
    uint32_t First = None, Last = None;
    for (const auto &Arg : C.S.Nodes[Call.Node].Children) {
      C.step();
      if (Arg.Field != "arguments")
        continue;
      if (First == None)
        First = Arg.Index;
      Last = Arg.Index;
      ++R.Arguments;
      R.SpreadArguments |= C.S.Nodes[Arg.Index].Kind == "SpreadElement";
    }
    if (Rule.CallbackArity) {
      R.Name = First;
      if (R.SpreadArguments || R.Arguments < Rule.CallbackArity) {
        R.Status = "unresolved_shape";
        R.Reason = R.SpreadArguments ? "spread_arguments"
                                     : "callback_argument_missing";
      } else {
        R.Callback = Last;
      }
    }
    if (R.Domain == Domain::Permission)
      condition(C, R);
    nameStatus(C, R);
    C.publish(std::move(R));
    return;
  }
}

void recognizeObject(Context &C, uint32_t Index) {
  uint32_t Name = None, Schema = None, Callback = None;
  std::string Reason;
  for (const auto &Child : C.S.Nodes[Index].Children) {
    C.step();
    if (Child.Field != "properties")
      continue;
    const auto &P = C.S.Nodes[Child.Index];
    if (P.Kind == "SpreadElement") {
      Reason = "spread_object";
      continue;
    }
    if (P.Kind != "Property")
      continue;
    const auto Key = C.child(Child.Index, "key");
    const auto *Spelling = C.key(Key, P.flag("computed"));
    if (!Spelling) {
      Reason = "computed_shape_key";
      continue;
    }
    uint32_t *Slot = nullptr;
    if (*Spelling == u"name")
      Slot = &Name;
    else if (*Spelling == u"inputSchema")
      Slot = &Schema;
    else if (*Spelling == u"call" || *Spelling == u"execute" ||
             *Spelling == u"handler")
      Slot = &Callback;
    if (!Slot)
      continue;
    if (*Slot != None)
      Reason = "duplicate_shape_key";
    if (!C.ordinaryProperty(Child.Index))
      Reason = "accessor_or_method_shape_property";
    *Slot = Child.Index;
  }
  if (Name == None || Schema == None || Callback == None)
    return;
  SourceFeature R;
  R.Domain = Domain::Tool;
  R.Pattern = Pattern::ToolDefinition;
  R.Node = Index;
  R.Trigger = C.child(Name, "key");
  R.Name = C.child(Name, "value");
  R.Basis = "name_input_schema_callback_properties";
  R.Status = Reason.empty() ? "matched_syntax" : "unresolved_shape";
  R.Reason = std::move(Reason);
  if (R.Status == "matched_syntax")
    R.Callback = C.child(Callback, "value");
  nameStatus(C, R);
  C.publish(std::move(R));
}

void recognizeProperty(Context &C, uint32_t Index) {
  const auto Parent = C.Parents[Index];
  if (Parent == None || C.S.Nodes[Parent].Kind != "ObjectExpression")
    return;
  const auto &P = C.S.Nodes[Index];
  const auto Key = C.child(Index, "key");
  const auto *Name = C.key(Key, P.flag("computed"));
  if (!Name)
    return;
  SourceFeature R;
  R.Node = Index;
  R.Trigger = Key;
  R.Basis = "object_property_spelling";
  bool Section = true;
  if (*Name == u"permissions" || *Name == u"permissionMode") {
    R.Domain = Domain::Permission;
    R.Pattern = Pattern::PermissionSection;
    Section = *Name == u"permissions";
  } else if (*Name == u"mcpServers") {
    R.Domain = Domain::MCP;
    R.Pattern = Pattern::MCPSection;
  } else if (*Name == u"hooks") {
    R.Domain = Domain::Configuration;
    R.Pattern = Pattern::ConfigurationSection;
  } else {
    bool Event = false;
    for (const auto E : HookEvents) {
      C.step();
      Event |= *Name == E;
    }
    const auto Owner = C.Parents[Parent];
    if (!Event || Owner == None || C.S.Nodes[Owner].Kind != "Property")
      return;
    const auto OwnerKey = C.child(Owner, "key");
    const auto *OwnerName = C.key(OwnerKey, C.S.Nodes[Owner].flag("computed"));
    if (!OwnerName || *OwnerName != u"hooks" || !C.ordinaryProperty(Owner) ||
        C.child(Owner, "value") != Parent)
      return;
    R.Domain = Domain::Hook;
    R.Pattern = Pattern::HookEvent;
    R.Basis = "hook_event_property_in_hooks_object";
    Section = false;
  }
  R.Status = "matched_syntax";
  const auto Value = C.child(Index, "value");
  if (!C.ordinaryProperty(Index)) {
    R.Status = "unresolved_shape";
    R.Reason = "accessor_or_method_property";
  } else if (Section &&
             (Value == None || C.S.Nodes[Value].Kind != "ObjectExpression")) {
    R.Status = "unresolved_shape";
    R.Reason = "section_value_not_object";
  }
  R.NameStatus = "not_applicable";
  C.publish(std::move(R));
}

} // namespace neverd::web::client_features
