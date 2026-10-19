//===- ExecutionRunJSONTests.cpp - Public workload protocol contracts ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Public workload protocol contracts.
///

#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionRunJSON.h"

#include "llvm/Support/JSON.h"

using namespace neverd::emulation;
namespace {
template <typename T> void rejected(llvm::Expected<T> Result) {
  EXPECT_FALSE(bool(Result));
  if (!Result)
    EXPECT_FALSE(llvm::toString(Result.takeError()).empty());
}
llvm::json::Value parsed(llvm::Expected<std::string> Text) {
  EXPECT_TRUE(bool(Text));
  if (!Text) {
    llvm::consumeError(Text.takeError());
    return nullptr;
  }
  auto V = llvm::json::parse(*Text);
  EXPECT_TRUE(bool(V));
  if (!V) {
    llvm::consumeError(V.takeError());
    return nullptr;
  }
  return std::move(*V);
}
TEST(ExecutionJSON, DefaultControlsMatchTheEngine) {
  auto R = executionRunOptionsFromJSON("{}");
  ASSERT_TRUE(bool(R));
  ExecutionRunOptions Default;
  EXPECT_EQ(R->Limits.Instructions, Default.Limits.Instructions);
  EXPECT_EQ(R->Limits.Events, Default.Limits.Events);
  EXPECT_EQ(R->Limits.TimeoutMicroseconds, Default.Limits.TimeoutMicroseconds);
  EXPECT_EQ(R->TimePolicy, ExecutionTimePolicy::ActiveWork);
  EXPECT_EQ(R->InstructionQuantum, Default.InstructionQuantum);
  EXPECT_EQ(R->ActionsPerRun, Default.ActionsPerRun);
  EXPECT_EQ(R->InstanceLimit, Default.InstanceLimit);
  EXPECT_EQ(R->MemoryTransferLimit, Default.MemoryTransferLimit);
  EXPECT_TRUE(R->EnableResourcePauses);
  EXPECT_TRUE(R->PauseUnknownAddresses);
  EXPECT_TRUE(R->Bindings.empty());
}
TEST(ExecutionJSON, ExplicitControlsAndDecimalOrHexValues) {
  auto R = executionRunOptionsFromJSON(R"({
    "limits":{"instructions":"0xFFFFFFFFFFFFFFFF","events":"17",
              "timeout_microseconds":123},
    "time_policy":"absolute_deadline", "instruction_quantum":"0x2A",
    "actions_per_run":7,"instance_limit":3,"memory_transfer_limit":4096,
    "enable_resource_pauses":false,"pause_unknown_addresses":false,
    "pause_stages":["after-entry","自定义阶段"]})");
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Limits.Instructions, UINT64_MAX);
  EXPECT_EQ(R->Limits.Events, 17u);
  EXPECT_EQ(R->Limits.TimeoutMicroseconds, 123u);
  EXPECT_EQ(R->TimePolicy, ExecutionTimePolicy::AbsoluteDeadline);
  EXPECT_EQ(R->InstructionQuantum, 42u);
  EXPECT_EQ(R->ActionsPerRun, 7u);
  EXPECT_EQ(R->InstanceLimit, 3u);
  EXPECT_EQ(R->MemoryTransferLimit, 4096u);
  EXPECT_FALSE(R->EnableResourcePauses);
  EXPECT_FALSE(R->PauseUnknownAddresses);
  ASSERT_EQ(R->PauseStages.size(), 2u);
  EXPECT_EQ(R->PauseStages[1], "自定义阶段");
}
TEST(ExecutionJSON, RejectsMalformedUnknownAndWrongTypes) {
  for (const auto *JSON : {"",
                           "[]",
                           "null",
                           "{} trailing",
                           R"({"driver_entry":true})",
                           R"({"limits":null})",
                           R"({"limits":{"timeout":1}})",
                           R"({"limits":{"instructions":0}})",
                           R"({"time_policy":true})",
                           R"({"time_policy":"wall"})",
                           R"({"enable_resource_pauses":1})",
                           R"({"pause_unknown_addresses":null})",
                           R"({"pause_stages":{}})",
                           R"({"pause_stages":[null]})",
                           R"({"pause_stages":[""]})",
                           R"({"pause_stages":["a","a"]})",
                           R"({"pause_stages":["a\u0000b"]})",
                           R"({"instruction_quantum":0})",
                           R"({"actions_per_run":1000001})",
                           R"({"instance_limit":4097})",
                           R"({"memory_transfer_limit":67108865})"})
    rejected(executionRunOptionsFromJSON(JSON));
}
TEST(ExecutionJSON, RejectsLossyNumbersSignsWhitespaceAndOverflow) {
  for (const auto *Number :
       {"-1", "1.0", "1e0", "true", "null", "\"\"", "\"0x\"", "\"+1\"",
        "\"-1\"", "\" 1\"", "\"1 \"", "\"0b10\"", "\"0xG0\"",
        "18446744073709551616", "\"18446744073709551616\"",
        "\"0x10000000000000000\""})
    rejected(executionRunOptionsFromJSON(
        std::string("{\"instruction_quantum\":") + Number + "}"));
  auto R = executionRunOptionsFromJSON(
      R"({"instruction_quantum":18446744073709551615})");
  ASSERT_TRUE(bool(R));
  EXPECT_EQ(R->InstructionQuantum, UINT64_MAX);
  R = executionRunOptionsFromJSON(R"({"instruction_quantum":"010"})");
  ASSERT_TRUE(bool(R));
  EXPECT_EQ(R->InstructionQuantum, 10u);
}
TEST(ExecutionJSON, RejectsDuplicateKeysIncludingEscapedSpellings) {
  for (const auto *JSON :
       {R"({"instruction_quantum":1,"instruction_quantum":2})",
        R"({"limits":{"events":1,"\u0065vents":2}})",
        R"({"pause_stages":[],"pause_stages":["a"]})"})
    rejected(executionRunOptionsFromJSON(JSON));
  rejected(guestInvocationFromJSON(
      R"({"entry":1,"abi":"win64","arguments":[{"value":1,"value":2}]})"));
}
TEST(ExecutionJSON, RejectsAmbiguousUnicodeButAcceptsPairedSurrogates) {
  for (const auto *Stage :
       {R"("\uD800")", R"("\uDC00")", R"("\uD800x")", R"("\uD800\u0041")"})
    rejected(executionRunOptionsFromJSON(std::string("{\"pause_stages\":[") +
                                         Stage + "]}"));
  auto R = executionRunOptionsFromJSON(
      R"({"pause_stages":["\uD83D\uDE00","quote\"comma,:{}[]\\tail"]})");
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->PauseStages[0], "\xF0\x9F\x98\x80");
}
TEST(ExecutionJSON, BoundsBytesNestingAndNames) {
  rejected(executionRunOptionsFromJSON(
      std::string(execution_limit::JSONBytes + 1, ' ')));
  rejected(executionRunOptionsFromJSON(std::string("{\"pause_stages\":[\"") +
                                       std::string(129, 'a') + "\"]}"));
  rejected(executionRunOptionsFromJSON(std::string("{\"pause_stages\":[\"") +
                                       char(0xFF) + "\"]}"));
  rejected(executionRunOptionsFromJSON(std::string("{}\0", 3)));
  rejected(executionBindingsFromJSON(std::string(65, '[') + "0" +
                                     std::string(65, ']')));
  std::string Stages = "{\"pause_stages\":[";
  for (unsigned I = 0; I < 65; ++I)
    Stages += (I ? ",\"" : "\"") + std::to_string(I) + "\"";
  rejected(executionRunOptionsFromJSON(Stages + "]}"));
}
TEST(ExecutionJSON, BindingParsingDoesNotChooseOrCreateAddressSpaces) {
  auto R = executionBindingsFromJSON(R"([
    {"id":"i","module_id":"m","guest_name":"helper.sys",
     "scope":"kernel","address_space":"system","base":"0xFFFFF80000000000",
     "size":"4096"},
    {"id":"j","module_id":"m","guest_name":"helper.sys"}])");
  ASSERT_TRUE(bool(R));
  ASSERT_EQ(R->size(), 2u);
  EXPECT_EQ((*R)[0].Base, 0xFFFFF80000000000u);
  EXPECT_EQ((*R)[0].Size, 4096u);
  EXPECT_EQ((*R)[0].AddressSpace, "system");
  EXPECT_EQ((*R)[1].AddressSpace, "");
  EXPECT_EQ((*R)[1].Scope, "");
  EXPECT_FALSE((*R)[1].Base);
  EXPECT_FALSE((*R)[1].Size);
  rejected(resolveExecutionBindings(*R, {}));
  unsigned Calls = 0;
  rejected(resolveExecutionBindings(
      *R,
      [&](llvm::StringRef) -> llvm::Expected<std::shared_ptr<AddressSpace>> {
        ++Calls;
        return std::shared_ptr<AddressSpace>();
      }));
  EXPECT_EQ(Calls, 1u);
  auto Empty = resolveExecutionBindings({}, {});
  ASSERT_TRUE(bool(Empty));
  EXPECT_TRUE(Empty->empty());
}
TEST(ExecutionJSON, BindingNamesAndFieldsAreStrict) {
  for (const auto *JSON :
       {"{}", "[null]", "[{}]",
        R"([{"id":"i","module_id":"m","guest_name":"g","base":null}])",
        R"([{"id":"i","module_id":"m","guest_name":"g","address_space":7}])",
        R"([{"id":"i","module_id":"m","guest_name":"g","scope":null}])",
        R"([{"id":"i","module_id":"m","guest_name":"g","load":"lazy"}])",
        R"([{"id":"i","module_id":"m","guest_name":"g\u0000"}])"})
    rejected(executionBindingsFromJSON(JSON));
  std::string Many = "[";
  for (unsigned I = 0; I < 4097; ++I)
    Many += I ? ",{}" : "{}";
  rejected(executionBindingsFromJSON(Many + "]"));
}
TEST(ExecutionJSON, TokensPreserveFullWidthAndZeroRevision) {
  auto R = executionSupplyFromJSON(R"({"token":{
    "owner":"0xFFFFFFFFFFFFFFFF","version":"0x0","sequence":"18446744073709551615"},
    "bindings":[]})");
  ASSERT_TRUE(bool(R));
  EXPECT_EQ(R->Token, (ModuleRequestToken{UINT64_MAX, 0, UINT64_MAX}));
  EXPECT_TRUE(R->Bindings.empty());
}
TEST(ExecutionJSON, RejectsMissingExtraAndInvalidTokenParts) {
  for (
      const auto *JSON :
      {"{}", "[]", R"({"token":null,"bindings":[]})",
       R"({"token":{"owner":1,"version":0},"bindings":[]})",
       R"({"token":{"owner":0,"version":0,"sequence":1},"bindings":[]})",
       R"({"token":{"owner":1,"version":0,"sequence":0},"bindings":[]})",
       R"({"token":{"owner":1,"version":-1,"sequence":1},"bindings":[]})",
       R"({"token":{"owner":1,"version":0,"sequence":1,"pc":1},"bindings":[]})",
       R"({"token":{"owner":1,"version":0,"sequence":1},"bindings":[],"resume":true})"})
    rejected(executionSupplyFromJSON(JSON));
}
TEST(ExecutionJSON, InvocationRequiresAnExplicitAbiAndEntry) {
  for (const auto *JSON :
       {"{}", "[]", R"({"entry":1})", R"({"abi":"win64"})",
        R"({"entry":1,"abi":null})", R"({"entry":1,"abi":"auto"})",
        R"({"entry":1,"abi":"cdecl"})",
        R"({"entry":1,"abi":"win64","signature":"guess"})"})
    rejected(guestInvocationFromJSON(JSON));
  for (const auto &[Name, ABI] :
       {std::pair{"win64", IntegerCallingConvention::Win64},
        {"sysv_amd64", IntegerCallingConvention::SysVAMD64},
        {"aapcs64", IntegerCallingConvention::AAPCS64}}) {
    auto R = guestInvocationFromJSON(
        std::string("{\"entry\":\"0xFFFFFFFFFFFFFFFF\",\"abi\":\"") + Name +
        "\"}");
    ASSERT_TRUE(bool(R));
    EXPECT_EQ(R->Entry, UINT64_MAX);
    EXPECT_EQ(R->ABI, ABI);
    EXPECT_TRUE(R->InstanceID.empty());
  }
}
TEST(ExecutionJSON, InvocationPreservesScalarPointerBufferAndQuotaInputs) {
  auto R = guestInvocationFromJSON(
      R"({"instance_id":"helper-instance","entry":"0x0",
    "abi":"win64", "arguments":[{"value":"0xFFFFFFFFFFFFFFFF"},
      {"buffer":0,"offset":2},{"buffer":0,"offset":4},{"buffer":1}],
    "buffers":[{"size":4,"input_hex":"0080FF01","capture":false},{"size":0}],
    "limits":{"instructions":123,"events":17,"timeout_microseconds":12345}})",
      16);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->InstanceID, "helper-instance");
  EXPECT_EQ(R->Entry, 0u);
  ASSERT_EQ(R->Arguments.size(), 4u);
  EXPECT_EQ(R->Arguments[0].Value, UINT64_MAX);
  EXPECT_FALSE(R->Arguments[0].Buffer);
  EXPECT_EQ(R->Arguments[1].Buffer, 0u);
  EXPECT_EQ(R->Arguments[1].Value, 2u);
  EXPECT_EQ(R->Arguments[2].Value, 4u);
  EXPECT_EQ(R->Arguments[3].Value, 0u);
  ASSERT_EQ(R->Buffers.size(), 2u);
  EXPECT_EQ(R->Buffers[0].Input, (std::vector<uint8_t>{0, 128, 255, 1}));
  EXPECT_FALSE(R->Buffers[0].Capture);
  EXPECT_EQ(R->Buffers[1].Size, 0u);
  EXPECT_TRUE(R->Buffers[1].Capture);
  EXPECT_TRUE(R->Buffers[1].Input.empty());
  EXPECT_EQ(R->Limits.Instructions, 123u);
  EXPECT_EQ(R->Limits.Events, 17u);
  EXPECT_EQ(R->Limits.TimeoutMicroseconds, 12345u);
}
TEST(ExecutionJSON, InvocationRejectsAmbiguousArgumentsAndOutOfBoundsBuffers) {
  for (const auto *Fields :
       {R"("arguments":[42])", R"("arguments":[{}])",
        R"("arguments":[{"value":1.0}])", R"("arguments":[{"buffer":0}])",
        R"("arguments":[{"value":1,"offset":0}])",
        R"("arguments":[{"buffer":0,"value":1}],"buffers":[{"size":1}])",
        R"("arguments":[{"buffer":0,"offset":2}],"buffers":[{"size":1}])",
        R"("arguments":[{"buffer":4294967296}],"buffers":[{"size":1}])",
        R"("buffers":[{"size":1,"input_hex":"00FF"}])",
        R"("buffers":[{"size":1,"input_hex":"F"}])",
        R"("buffers":[{"size":1,"input_hex":"GG"}])",
        R"("buffers":[{"size":1,"input_hex":null}])",
        R"("buffers":[{"size":1,"capture":1}])",
        R"("buffers":[{"size":1,"extra":true}])", R"("buffers":[{"size":17}])",
        R"("buffers":[{"size":1},{"size":1}])"})
    rejected(guestInvocationFromJSON(
        std::string("{\"entry\":1,\"abi\":\"win64\",") + Fields + "}", 16));
  rejected(guestInvocationFromJSON(R"({"entry":1,"abi":"win64"})", 0));
  rejected(guestInvocationFromJSON(R"({"entry":1,"abi":"win64"})",
                                   execution_limit::TransferBytes + 1));
}
TEST(ExecutionJSON, InvocationCountsAreBoundedBeforePayloadAllocation) {
  for (const auto *Key : {"arguments", "buffers"}) {
    std::string JSON =
        std::string("{\"entry\":1,\"abi\":\"win64\",\"") + Key + "\":[";
    for (unsigned I = 0; I < 65; ++I)
      JSON += I ? ",{}" : "{}";
    rejected(guestInvocationFromJSON(JSON + "]}"));
  }
}
TEST(ExecutionJSON, EventDistinguishesStateReasonAndAbsentFacts) {
  ExecutionRunEvent E;
  auto V = parsed(executionRunEventJSON(E));
  const auto *O = V.getAsObject();
  ASSERT_TRUE(O);
  EXPECT_EQ(O->getInteger("schema_version"), 1);
  EXPECT_EQ(O->getString("state"), "ready");
  EXPECT_EQ(O->getString("reason"), "created");
  EXPECT_FALSE(O->get("request"));
  EXPECT_FALSE(O->get("resource"));
  EXPECT_FALSE(O->get("cpu"));
  EXPECT_FALSE(O->get("invocation"));
}
TEST(ExecutionJSON, EventRetainsTheIndependentCpuFaultAndRestartFacts) {
  BackendFault F{BackendFaultKind::UnmappedMemory, UINT64_MAX, uint64_t(0), 8,
                 BackendAccessKind::Read};
  F.ErrorCode = 0;
  ResourcePause P{F,    0xAB,
                  0xAA, ResourceRestartKind::ArchitecturalRetry,
                  true, Read | UserAccessible};
  ExecutionRunEvent E;
  E.State = ExecutionRunState::AwaitingResource;
  E.Reason = ExecutionRunReason::Resource;
  E.Resource = P;
  E.CPU = SessionExit{SessionExitKind::CPU,
                      ExecutionExit{ExecutionExitKind::ResourcePause,
                                    F,
                                    "repair needed",
                                    true,
                                    true,
                                    {},
                                    P}};
  auto V = parsed(executionRunEventJSON(E));
  auto *O = V.getAsObject();
  ASSERT_TRUE(O);
  const auto *R = O->getObject("resource");
  ASSERT_TRUE(R);
  EXPECT_EQ(R->getString("resume_pc"), "0xAB");
  EXPECT_EQ(R->getString("instruction_pc"), "0xAA");
  EXPECT_EQ(R->getBoolean("memory_effects_may_exist"), true);
  const auto *Fault = R->getObject("fault");
  ASSERT_TRUE(Fault);
  EXPECT_EQ(Fault->getString("pc"), "0xFFFFFFFFFFFFFFFF");
  EXPECT_EQ(Fault->getString("address"), "0x0");
  EXPECT_EQ(Fault->getString("error_code"), "0x0");
  EXPECT_FALSE(Fault->get("interrupt"));
  const auto *C = O->getObject("cpu")->getObject("cpu");
  ASSERT_TRUE(C);
  EXPECT_EQ(C->getBoolean("stop_requested"), true);
  EXPECT_EQ(C->getBoolean("deadline_reached"), true);
  EXPECT_EQ(C->getString("diagnostic"), "repair needed");
  EXPECT_TRUE(C->getObject("fault"));
  EXPECT_TRUE(C->getObject("resource"));
}
TEST(ExecutionJSON, InvocationResultAndProgressRetainUnsignedAndBinaryValues) {
  ExecutionRunSnapshot S{};
  S.Environment = "driver";
  S.TimePolicy = ExecutionTimePolicy::ActiveWork;
  S.Instructions = UINT64_MAX;
  S.Events = UINT64_MAX - 1;
  S.RemainingMicroseconds = 3;
  S.ModuleRevision = 0;
  S.Event.State = ExecutionRunState::Paused;
  S.Event.Reason = ExecutionRunReason::InvocationComplete;
  S.Event.Invocation = GuestInvocationResult{
      UINT64_MAX, UINT64_MAX, 4, 5, 6, {{UINT64_MAX, {0, 128, 255}}, {0, {}}}};
  S.Invocation =
      GuestInvocationProgress{{1, 2, 3, 4, 5, 6}, 7, 8, 9, 10, 11, 12};
  auto V = parsed(executionRunSnapshotJSON(S));
  const auto *O = V.getAsObject();
  ASSERT_TRUE(O);
  EXPECT_EQ(O->getString("instructions"), "0xFFFFFFFFFFFFFFFF");
  EXPECT_EQ(O->getString("events"), "0xFFFFFFFFFFFFFFFE");
  EXPECT_EQ(O->getString("module_revision"), "0x0");
  EXPECT_EQ(O->getString("time_policy"), "active_work");
  const auto *I = O->getObject("event")->getObject("invocation");
  ASSERT_TRUE(I);
  EXPECT_EQ(I->getString("value"), "0xFFFFFFFFFFFFFFFF");
  const auto *B = I->getArray("buffers");
  ASSERT_TRUE(B);
  ASSERT_EQ(B->size(), 2u);
  EXPECT_EQ((*B)[0].getAsObject()->getString("bytes_hex"), "0080ff");
  EXPECT_EQ((*B)[1].getAsObject()->getString("bytes_hex"), "");
  const auto *P = O->getObject("invocation");
  ASSERT_TRUE(P);
  EXPECT_EQ(P->getString("remaining_events"), "0xB");
  EXPECT_EQ(P->getObject("frame")->getString("return_stack_pointer"), "0x6");
}
TEST(ExecutionJSON, ReportRejectsInvalidUtf8InsteadOfReplacingIdentities) {
  ExecutionRunEvent E;
  E.Stage = std::string(1, char(0xFF));
  rejected(executionRunEventJSON(E));
  E.Stage = "";
  E.Diagnostic = std::string("a\0b", 3);
  rejected(executionRunEventJSON(E));
  E.Diagnostic.assign(execution_limit::JSONBytes + 1, 'a');
  rejected(executionRunEventJSON(E));
  E.Diagnostic.clear();
  E.Invocation.emplace();
  E.Invocation->Buffers.resize(65);
  rejected(executionRunEventJSON(E));
}
TEST(ExecutionJSON, AddressSpaceReportsRequireExplicitSceneIdentity) {
  auto Physical = PhysicalMemory::create(8192);
  ASSERT_TRUE(bool(Physical));
  auto Space = AddressSpace::create(std::move(*Physical), 8192);
  ASSERT_TRUE(bool(Space));
  ExecutionRunEvent E;
  E.State = ExecutionRunState::AwaitingResource;
  E.Reason = ExecutionRunReason::Resource;
  E.Request = ModuleRequest{};
  E.Request->Space = *Space;
  E.Request->Token = {1, 0, 1};
  rejected(executionRunEventJSON(E));
  rejected(
      executionRunEventJSON(E, [](const auto &) -> llvm::Expected<std::string> {
        return std::string();
      }));
  rejected(
      executionRunEventJSON(E, [](const auto &) -> llvm::Expected<std::string> {
        return std::string(1, char(0xFF));
      }));
  const ExecutionSpaceIdentifier ID =
      [&](const auto &S) -> llvm::Expected<std::string> {
    EXPECT_EQ(S, *Space);
    return std::string("scene/system");
  };
  auto V = parsed(executionRunEventJSON(E, ID));
  const auto *R = V.getAsObject()->getObject("request");
  ASSERT_TRUE(R);
  EXPECT_EQ(R->getString("address_space"), "scene/system");
  EXPECT_EQ(R->getObject("token")->getString("version"), "0x0");
  E.Request->Space.reset();
  rejected(executionRunEventJSON(E, ID));
}
TEST(ExecutionJSON, ExplicitBindingResolutionPreservesSpaceAndDeclaration) {
  auto Physical = PhysicalMemory::create(8192);
  ASSERT_TRUE(bool(Physical));
  auto Space = AddressSpace::create(std::move(*Physical), 8192);
  ASSERT_TRUE(bool(Space));
  auto Inputs = executionBindingsFromJSON(R"([
    {"id":"i","module_id":"m","guest_name":"g","scope":"s","base":"0x0","size":4096},
    {"id":"j","module_id":"m","guest_name":"g","scope":"s","address_space":"other"}])");
  ASSERT_TRUE(bool(Inputs));
  unsigned Calls = 0;
  auto Failed = resolveExecutionBindings(
      *Inputs,
      [&](llvm::StringRef Name)
          -> llvm::Expected<std::shared_ptr<AddressSpace>> {
        ++Calls;
        if (!Name.empty())
          return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                         "unknown scene space");
        return *Space;
      });
  EXPECT_FALSE(bool(Failed));
  if (!Failed)
    EXPECT_EQ(llvm::toString(Failed.takeError()), "unknown scene space");
  EXPECT_EQ(Calls, 2u);
  Inputs->resize(1);
  auto Resolved = resolveExecutionBindings(
      *Inputs,
      [&](llvm::StringRef Name)
          -> llvm::Expected<std::shared_ptr<AddressSpace>> {
        EXPECT_TRUE(Name.empty());
        return *Space;
      });
  ASSERT_TRUE(bool(Resolved));
  ASSERT_EQ(Resolved->size(), 1u);
  EXPECT_EQ((*Resolved)[0].Space, *Space);
  EXPECT_EQ((*Resolved)[0].Base, 0u);
  EXPECT_EQ((*Resolved)[0].Size, 4096u);
  EXPECT_EQ((*Resolved)[0].Scope, "s");
  auto Rows = (*Space)->mappings();
  ASSERT_TRUE(bool(Rows));
  EXPECT_TRUE(Rows->empty());
  ModuleInstance I;
  I.Binding = (*Resolved)[0];
  const ExecutionSpaceIdentifier ID =
      [](const auto &) -> llvm::Expected<std::string> {
    return std::string("default");
  };
  auto V = parsed(executionInstancesJSON({I}, ID));
  const auto *R = (*V.getAsObject()->getArray("instances"))[0].getAsObject();
  ASSERT_TRUE(R);
  EXPECT_EQ(R->getBoolean("mapped"), false);
  EXPECT_EQ(R->getString("declared_base"), "0x0");
  EXPECT_FALSE(R->get("base"));
  EXPECT_FALSE(R->get("image_sha256"));
  EXPECT_FALSE(R->get("executable"));
}
} // namespace
