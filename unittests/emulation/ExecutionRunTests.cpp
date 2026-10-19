//===- ExecutionRunTests.cpp - Persistent state and resource supply -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Persistent state and resource supply.
///

#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionRun.h"
#include "neverd/emulation/ExecutionRunJSON.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

using namespace neverd;
using namespace neverd::emulation;
using namespace neverd::modules;
namespace {
constexpr uint64_t Page = 4096, Code = 0x10000, Missing = 0x50000;
constexpr uint64_t Output = 0x60000, Stack = 0x80000;
constexpr uint64_t CallCode = Code + 0x100, CallReturn = Code + 0x300,
                   CallStack = 0xA0000;
llvm::Error failure() {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "injected environment failure");
}
struct SceneState {
  unsigned Initialized = 0, NextCalls = 0, ModelCalls = 0, Exits = 0,
           Destroyed = 0;
  uint64_t PC = Code, Return = 0;
  bool Complete = false, StageSent = false, BuildFails = false;
  std::optional<ExecutionRunReason> FailureReason;
  bool FailureOnNonfailedAction = false;
  std::string Dependency;
  std::vector<uint8_t> Bytes{0x48, 0x8B, 0x03, 0x90};
  bool Admit = true;
  ExecutionBackend *CPU = nullptr;
  std::function<void(ExecutionSession &, ModuleBindings &)> OnInitialize,
      OnNext;
  std::function<void(ExecutionSession &, const SessionExit &)> OnExit;
  std::function<void()> OnInstruction;
  std::function<InstructionAdmission(uint64_t, uint32_t)> BeforeInstruction;
  bool Calls = false, BeginFails = false, FinishFails = false, Yield = false;
  unsigned CallsBegun = 0, CallsFinished = 0;
  uint64_t CallPC = 0, AllowedEntry = CallCode, ParentPC = 0;
  std::optional<GuestInvocationFrame> Invocation;
  GuestInvocationPlan CallPlan{CallStack, Page, CallReturn};
  std::vector<uint8_t> CallBytes{0x48, 0x89, 0xC8, 0x48, 0x01, 0xD0, 0xC3};
  std::function<llvm::Error(ExecutionSession &, const SessionExit &)> CallExit;
  std::function<llvm::Error(ExecutionSession &)> OnPlan;
  std::function<llvm::Error(ExecutionSession &, ModuleBindings &)> OnPrepare;
  unsigned Preparations = 0;
  std::function<llvm::Error(ExecutionSession &)> OnCPUEntry;
  unsigned CPUEntries = 0;
};
/// A small scene over raw CPU buffers. It models one stopped host action and a
/// named stage; it is not a Windows/Ring3 loader or a second driver executor.
class Scene final : public ExecutionEnvironment {
public:
  explicit Scene(std::shared_ptr<SceneState> State) : S(std::move(State)) {}
  ~Scene() override { ++S->Destroyed; }
  std::string name() const override { return "bounded-test-scene"; }
  std::vector<std::string> stageNames() const override {
    return {"after-entry"};
  }
  SessionModelHooks modelHooks() override {
    SessionModelHooks H;
    H.BeforeInstruction = S->BeforeInstruction;
    return H;
  }
  llvm::Error initialize(ExecutionSession &Session,
                         ModuleBindings &Modules) override {
    ++S->Initialized;
    S->CPU = &Session.cpu();
    for (const auto Address : {Code, Output, Stack})
      if (auto E = Session.cpu().map(
              Address, Page, Read | Write | (Address == Code ? Execute : 0)))
        return E;
    if (auto E = Session.cpu().write(Code, S->Bytes))
      return E;
    if (S->Calls) {
      if (auto E = Session.cpu().map(CallStack, Page, Read | Write))
        return E;
      if (auto E = Session.cpu().write(CallCode, S->CallBytes))
        return E;
      const uint8_t Trap[] = {0x0F, 0x0B};
      if (auto E = Session.cpu().write(CallReturn, Trap))
        return E;
    }
    if (auto E = Session.cpu().setReg(X64Register::BX, Missing))
      return E;
    if (auto E = Session.cpu().setReg(X64Register::AX, 73))
      return E;
    if (auto E = Session.cpu().setReg(X64Register::SP, Stack + Page - 128))
      return E;
    if (auto E = Session.cpu().setGSBase(0x90000))
      return E;
    if (auto E = Session.watchExecution({{Code + S->Bytes.size() - 1, 1}}))
      return E;
    if (S->OnInitialize)
      S->OnInitialize(Session, Modules);
    return llvm::Error::success();
  }
  llvm::Expected<EnvironmentAction> next(ExecutionSession &Session,
                                         ModuleBindings &Modules) override {
    ++S->NextCalls;
    if (S->OnNext)
      S->OnNext(Session, Modules);
    if (S->FailureReason)
      return EnvironmentAction{S->FailureOnNonfailedAction
                                   ? EnvironmentActionKind::Completed
                                   : EnvironmentActionKind::Failed,
                               0, "entry", "scene failure", S->FailureReason};
    if (S->Invocation)
      return EnvironmentAction{
          EnvironmentActionKind::RunCPU, S->CallPC, "invocation", {}};
    if (!S->Dependency.empty()) {
      const auto Found = Modules.find("dependency");
      if (!Found || !Found->mapped()) {
        auto Request = Modules.requestDependency(Session.cpu().addressSpace(),
                                                 S->Dependency, S->PC);
        if (!Request)
          return Request.takeError();
        return EnvironmentAction{
            EnvironmentActionKind::AwaitingResource, 0, "loading", {}};
      }
    }
    if (S->Complete) {
      if (!S->StageSent) {
        S->StageSent = true;
        return EnvironmentAction{
            EnvironmentActionKind::StageBoundary, 0, "after-entry", {}};
      }
      return EnvironmentAction{
          EnvironmentActionKind::Completed, 0, "finished", {}};
    }
    if (!S->ModelCalls) {
      ++S->ModelCalls;
      if (auto E = Session.cpu().writeInteger(Output, S->ModelCalls, 8))
        return std::move(E);
    }
    return EnvironmentAction{EnvironmentActionKind::RunCPU, S->PC, "entry", {}};
  }
  llvm::Error prepareExecution(ExecutionSession &Session,
                               ModuleBindings &Modules) override {
    ++S->Preparations;
    return S->OnPrepare ? S->OnPrepare(Session, Modules)
                        : llvm::Error::success();
  }
  llvm::Error prepareCPUEntry(ExecutionSession &Session) override {
    ++S->CPUEntries;
    return S->OnCPUEntry ? S->OnCPUEntry(Session) : llvm::Error::success();
  }
  llvm::Error afterCPUExit(ExecutionSession &Session,
                           const SessionExit &Exit) override {
    ++S->Exits;
    if (S->OnExit)
      S->OnExit(Session, Exit);
    auto PC = Session.cpu().reg(X64Register::PC);
    if (!PC)
      return PC.takeError();
    S->PC = *PC;
    if (S->Invocation) {
      S->PC = S->ParentPC;
      S->CallPC = *PC;
      if (S->CallExit)
        return S->CallExit(Session, Exit);
      return llvm::Error::success();
    }
    if (Exit.Kind == SessionExitKind::ExecutionWatch) {
      S->Complete = true;
      auto Result = Session.cpu().reg(X64Register::AX);
      if (!Result)
        return Result.takeError();
      S->Return = *Result;
    }
    return llvm::Error::success();
  }
  llvm::Expected<ModuleMappingPlan> buildModule(const ModuleBinding &B,
                                                const ModuleImage &I) override {
    if (S->BuildFails)
      return failure();
    const auto &Source = I.image().Segments.front().Data;
    if (!B.Base || Source.size() != 8)
      return failure();
    std::vector<uint8_t> Bytes(Page, 0);
    std::copy(Source.begin(), Source.end(), Bytes.begin());
    const auto Base = *B.Base;
    return ModuleMappingPlan{
        ImageMappingPlan{
            0,
            Base,
            Page,
            {{Base,
              Read | (B.GuestName == "invocation.module" ? Execute : Write),
              std::move(Bytes)}}},
        Page,
        {}};
  }
  bool observeInstruction(ExecutionBackend &, uint64_t, uint32_t) override {
    if (S->OnInstruction)
      S->OnInstruction();
    return std::exchange(S->Yield, false);
  }
  bool admitResourcePause(const ResourcePause &) const override {
    return S->Admit;
  }
  llvm::Expected<GuestInvocationPlan>
  planInvocation(ExecutionSession &Session, ModuleBindings &,
                 const GuestInvocationRequest &Request, uint64_t Entry,
                 llvm::StringRef Stage) override {
    if (!S->Calls || Stage != "after-entry" || Entry != S->AllowedEntry ||
        Request.ABI != IntegerCallingConvention::Win64)
      return failure();
    if (S->OnPlan)
      if (auto E = S->OnPlan(Session))
        return std::move(E);
    return S->CallPlan;
  }
  llvm::Error beginInvocation(ExecutionSession &,
                              const GuestInvocationFrame &Frame) override {
    ++S->CallsBegun;
    if (S->BeginFails)
      return failure();
    S->ParentPC = S->PC;
    S->Invocation = Frame;
    S->CallPC = Frame.Entry;
    return llvm::Error::success();
  }
  llvm::Error finishInvocation(ExecutionSession &,
                               const GuestInvocationResult &) override {
    ++S->CallsFinished;
    if (S->FinishFails)
      return failure();
    S->PC = S->ParentPC;
    S->Invocation.reset();
    return llvm::Error::success();
  }

private:
  std::shared_ptr<SceneState> S;
};
class PersistentRun : public testing::Test {
protected:
  std::filesystem::path Root, File;
  std::shared_ptr<ModuleSet> Sources;
  std::shared_ptr<AddressSpace> Space;
  std::shared_ptr<SceneState> State;
  std::unique_ptr<ExecutionRun> Run;
  ExecutionRunOptions Options;
  ExecutionContract Contract = ExecutionContract::Software;
  void SetUp() override {
    auto Available = queryExecutionBackendBuild(ExecutionBackendKind::Unicorn,
                                                GuestArchitecture::X64);
    ASSERT_TRUE(bool(Available));
    if (Available->Availability != BackendAvailability::Available)
      GTEST_SKIP() << Available->Reason;
    llvm::SmallString<128> Directory;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-run-test", Directory));
    Root = std::u8string(Directory.begin(), Directory.end());
    File = Root / "data.elf";
    writeFile();
    catalogue();
    freshScene();
    Options.PauseStages = {"after-entry"};
    Options.MemoryTransferLimit = Page;
  }
  void TearDown() override {
    Run.reset();
    Sources.reset();
    Space.reset();
    std::error_code EC;
    std::filesystem::remove_all(Root, EC);
  }
  void freshScene() {
    Space = llvm::cantFail(AddressSpace::create(
        llvm::cantFail(PhysicalMemory::create(32 * Page)), 32 * Page));
    State = std::make_shared<SceneState>();
  }
  void writeFile(uint8_t Value = 42) {
    using ELF = llvm::object::ELF64LE;
    ELF::Ehdr H{};
    std::memcpy(H.e_ident, "\177ELF", 4);
    H.e_ident[llvm::ELF::EI_CLASS] = llvm::ELF::ELFCLASS64;
    H.e_ident[llvm::ELF::EI_DATA] = llvm::ELF::ELFDATA2LSB;
    H.e_ident[llvm::ELF::EI_VERSION] = llvm::ELF::EV_CURRENT;
    H.e_type = llvm::ELF::ET_EXEC;
    H.e_machine = llvm::ELF::EM_X86_64;
    H.e_version = llvm::ELF::EV_CURRENT;
    H.e_entry = 0x401000;
    H.e_ehsize = sizeof(H);
    H.e_phoff = sizeof(H);
    H.e_phentsize = sizeof(ELF::Phdr);
    H.e_phnum = 1;
    H.e_shentsize = sizeof(ELF::Shdr);
    ELF::Phdr P{};
    P.p_type = llvm::ELF::PT_LOAD;
    P.p_flags = llvm::ELF::PF_R | llvm::ELF::PF_X;
    P.p_offset = Page;
    P.p_vaddr = 0x401000;
    P.p_filesz = 8;
    P.p_memsz = 8;
    P.p_align = Page;
    std::vector<uint8_t> Bytes(Page + 8);
    std::memcpy(Bytes.data(), &H, sizeof(H));
    std::memcpy(Bytes.data() + sizeof(H), &P, sizeof(P));
    Bytes[Page] = Value;
    std::ofstream OS(File, std::ios::binary | std::ios::trunc);
    OS.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
  }
  void catalogue(LoadPolicy Policy = LoadPolicy::Lazy,
                 std::filesystem::path Path = {}) {
    ModuleOptions M;
    M.Modules.push_back({"data", Path.empty() ? File : Path, {}, Policy});
    Sources = llvm::cantFail(ModuleSet::create(std::move(M)));
  }
  ModuleBinding binding(std::string ID = "data-instance",
                        uint64_t Base = Missing) {
    return {ID,
            "data",
            ID == "dependency" ? "dependency.module" : "support.module",
            "test-scene",
            Space,
            Base,
            Page};
  }
  llvm::Expected<std::unique_ptr<ExecutionRun>>
  construct(std::shared_ptr<ExecutionBudget> SharedBudget = {}) {
    auto CPU =
        createExecutionBackend(ExecutionBackendKind::Unicorn, Contract, Space);
    if (!CPU)
      return CPU.takeError();
    return ExecutionRun::create(std::move(CPU->CPU), Sources,
                                std::make_unique<Scene>(State), Options,
                                std::move(SharedBudget));
  }
  void start() {
    auto Created = construct();
    ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
    Run = std::move(*Created);
  }
  ModuleRequest missing() {
    auto Event = Run->run();
    EXPECT_TRUE(bool(Event));
    if (!Event) {
      llvm::consumeError(Event.takeError());
      return {};
    }
    EXPECT_EQ(Event->State, ExecutionRunState::AwaitingResource)
        << Event->Diagnostic;
    EXPECT_TRUE(Event->Request);
    return Event->Request.value_or(ModuleRequest{});
  }
  void stage() {
    auto Event = Run->run();
    ASSERT_TRUE(bool(Event)) << llvm::toString(Event.takeError());
    ASSERT_EQ(Event->State, ExecutionRunState::Paused) << Event->Diagnostic;
    EXPECT_EQ(Event->Reason, ExecutionRunReason::Stage);
    EXPECT_EQ(Event->Stage, "after-entry");
    EXPECT_EQ(State->Return, 42u);
  }
  void callStage() {
    State->Calls = true;
    catalogue(LoadPolicy::Eager);
    Options.Bindings = {binding()};
    start();
    ASSERT_TRUE(Run);
    stage();
  }
  GuestInvocationRequest callRequest() {
    GuestInvocationRequest R;
    R.Entry = CallCode;
    R.ABI = IntegerCallingConvention::Win64;
    R.Arguments = {{40, {}}, {2, {}}};
    return R;
  }
};
TEST_F(PersistentRun, SuppliedBudgetKeepsConsumedCreditsAndOwnership) {
  State->Bytes = {0x90};
  State->OnInitialize = [](ExecutionSession &S, ModuleBindings &) {
    llvm::cantFail(S.watchExecution({{Code + 1, 1}}));
  };
  std::shared_ptr<ExecutionBudget> Budget =
      llvm::cantFail(ExecutionBudget::createWithTimePolicy(Options.Limits,
                                                           Options.TimePolicy));
  ASSERT_TRUE(Budget->consumeInstructions(3));
  ASSERT_TRUE(Budget->consumeEvents(2));
  std::weak_ptr<ExecutionBudget> Lifetime = Budget;
  auto Created = construct(Budget);
  ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
  Run = std::move(*Created);
  Budget.reset();
  EXPECT_FALSE(Lifetime.expired());
  EXPECT_EQ(Run->inspect()->Instructions, 3u);
  EXPECT_EQ(Run->inspect()->Events, 2u);
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  EXPECT_EQ(Event->Reason, ExecutionRunReason::Stage);
  EXPECT_EQ(Run->inspect()->Instructions, 4u);
  EXPECT_EQ(Run->inspect()->Events, 2u);
  Run.reset();
  EXPECT_TRUE(Lifetime.expired());
}
TEST_F(PersistentRun, PreparationDependencyRetainsTheOriginalCpuAction) {
  State->OnPrepare = [](ExecutionSession &,
                        ModuleBindings &Modules) -> llvm::Error {
    const auto I = Modules.find("data-instance");
    if (!I || !I->mapped())
      return llvm::make_error<ModuleDependencyError>("support.module");
    return llvm::Error::success();
  };
  start();
  ASSERT_TRUE(Run);
  const auto R = missing();
  EXPECT_EQ(R.Kind, ModuleRequestKind::MissingDependency);
  EXPECT_EQ(State->Preparations, 1u);
  EXPECT_EQ(State->NextCalls, 1u);
  EXPECT_EQ(State->ModelCalls, 1u);
  EXPECT_EQ(State->Exits, 0u);
  EXPECT_EQ(Run->inspect()->Instructions, 0u);
  ASSERT_EQ(llvm::toString(Run->supply(R.Token, {binding()})), "");
  stage();
  EXPECT_EQ(State->Preparations, 2u);
  EXPECT_EQ(State->ModelCalls, 1u);
  EXPECT_EQ(State->Exits, 1u);
}
TEST_F(PersistentRun, RepairedCpuContinuationDoesNotRepeatStoppedPreparation) {
  State->OnPrepare = [this](ExecutionSession &,
                            ModuleBindings &) -> llvm::Error {
    if (State->Preparations != 1)
      return failure();
    return llvm::Error::success();
  };
  State->OnCPUEntry = [](ExecutionSession &S) -> llvm::Error {
    EXPECT_FALSE(S.cpu().pendingResourcePause());
    EXPECT_EQ(*S.cpu().reg(X64Register::AX), 73u);
    EXPECT_EQ(*S.cpu().reg(X64Register::BX), Missing);
    return llvm::Error::success();
  };
  start();
  ASSERT_TRUE(Run);
  const auto R = missing();
  EXPECT_EQ(R.Kind, ModuleRequestKind::UnmappedCandidate);
  EXPECT_EQ(State->CPUEntries, 1u);
  ASSERT_EQ(llvm::toString(Run->supply(R.Token, {binding()})), "");
  EXPECT_TRUE(Run->inspect()->Continuation);
  EXPECT_EQ(State->CPUEntries, 1u);
  stage();
  EXPECT_EQ(State->Preparations, 1u);
  EXPECT_EQ(State->ModelCalls, 1u);
  EXPECT_EQ(State->CPUEntries, 2u);
}
TEST_F(PersistentRun, CpuEntryProjectionObservesCommittedEagerMemory) {
  catalogue(LoadPolicy::Eager);
  Options.Bindings = {binding()};
  State->OnCPUEntry = [this](ExecutionSession &S) -> llvm::Error {
    EXPECT_EQ(State->Preparations, 1u);
    auto Value = S.cpu().readInteger(Missing, 8);
    if (!Value)
      return Value.takeError();
    EXPECT_EQ(*Value, 42u);
    return llvm::Error::success();
  };
  start();
  ASSERT_TRUE(Run);
  stage();
  EXPECT_EQ(State->CPUEntries, 1u);
}
TEST_F(PersistentRun, FailedProjectionAfterRepairDoesNotReplayTheScene) {
  State->OnCPUEntry = [this](ExecutionSession &) -> llvm::Error {
    return State->CPUEntries == 2 ? failure() : llvm::Error::success();
  };
  start();
  ASSERT_TRUE(Run);
  const auto Request = missing();
  const auto Instructions = Run->inspect()->Instructions;
  ASSERT_EQ(llvm::toString(Run->supply(Request.Token, {binding()})), "");
  auto E = Run->run();
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(E->Reason, ExecutionRunReason::EnvironmentFailure);
  EXPECT_FALSE(E->Request);
  EXPECT_EQ(State->NextCalls, 1u);
  EXPECT_EQ(State->ModelCalls, 1u);
  EXPECT_EQ(State->Preparations, 1u);
  EXPECT_EQ(State->Exits, 1u);
  EXPECT_EQ(Run->inspect()->Instructions, Instructions);
  auto Mutation = State->CPU->setReg(X64Register::AX, 99);
  EXPECT_TRUE(bool(Mutation));
  llvm::consumeError(std::move(Mutation));
}
TEST_F(PersistentRun,
       PauseDuringRepairedProjectionKeepsTheOriginalContinuation) {
  State->OnCPUEntry = [this](ExecutionSession &) -> llvm::Error {
    if (State->CPUEntries == 2)
      EXPECT_EQ(llvm::toString(Run->requestPause()), "");
    return llvm::Error::success();
  };
  start();
  ASSERT_TRUE(Run);
  const auto Request = missing();
  ASSERT_EQ(llvm::toString(Run->supply(Request.Token, {binding()})), "");
  auto E = Run->run();
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(E->Reason, ExecutionRunReason::UserPause);
  EXPECT_TRUE(Run->inspect()->Continuation);
  EXPECT_EQ(State->NextCalls, 1u);
  EXPECT_EQ(State->Exits, 1u);
  ASSERT_EQ(llvm::toString(Run->resume()), "");
  stage();
  EXPECT_EQ(State->CPUEntries, 3u);
  EXPECT_EQ(State->Preparations, 1u);
  EXPECT_EQ(State->ModelCalls, 1u);
}
TEST_F(PersistentRun, CpuEntryProjectionFailuresNeverBecomeResourceRequests) {
  for (bool Dependency : {false, true}) {
    Run.reset();
    freshScene();
    State->OnCPUEntry = [Dependency](ExecutionSession &) -> llvm::Error {
      if (Dependency)
        return llvm::make_error<ModuleDependencyError>("support.module");
      return failure();
    };
    start();
    ASSERT_TRUE(Run);
    auto E = Run->run();
    ASSERT_TRUE(bool(E));
    EXPECT_EQ(E->Reason, ExecutionRunReason::EnvironmentFailure);
    EXPECT_EQ(E->State, ExecutionRunState::Failed);
    EXPECT_FALSE(E->Request);
    EXPECT_EQ(State->CPUEntries, 1u);
    EXPECT_EQ(State->Exits, 0u);
    EXPECT_EQ(Run->inspect()->Instructions, 0u);
    EXPECT_FALSE(Sources->prepared("data"));
  }
}
TEST_F(PersistentRun, CpuEntryProjectionCannotConcealTheSharedTimeout) {
  for (bool Fails : {false, true}) {
    Run.reset();
    freshScene();
    State->OnCPUEntry = [Fails](ExecutionSession &S) -> llvm::Error {
      S.budget().remainingMicroseconds(ExecutionBudget::Clock::now() +
                                       std::chrono::hours(1));
      return Fails ? failure() : llvm::Error::success();
    };
    start();
    ASSERT_TRUE(Run);
    auto E = Run->run();
    ASSERT_TRUE(bool(E));
    EXPECT_EQ(E->Reason, ExecutionRunReason::Timeout);
    EXPECT_FALSE(E->Request);
    EXPECT_EQ(State->Exits, 0u);
    EXPECT_EQ(Run->inspect()->Instructions, 0u);
  }
}
TEST_F(PersistentRun, PauseDuringCpuEntryProjectionRetainsTheCpuAction) {
  catalogue(LoadPolicy::Eager);
  Options.Bindings = {binding()};
  State->OnCPUEntry = [this](ExecutionSession &) -> llvm::Error {
    if (State->CPUEntries == 1)
      EXPECT_EQ(llvm::toString(Run->requestPause()), "");
    return llvm::Error::success();
  };
  start();
  ASSERT_TRUE(Run);
  auto E = Run->run();
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(E->Reason, ExecutionRunReason::UserPause);
  EXPECT_EQ(State->CPUEntries, 1u);
  EXPECT_EQ(State->NextCalls, 1u);
  EXPECT_EQ(State->Exits, 0u);
  EXPECT_EQ(Run->inspect()->Instructions, 0u);
  ASSERT_EQ(llvm::toString(Run->resume()), "");
  stage();
  EXPECT_EQ(State->CPUEntries, 2u);
  EXPECT_EQ(State->ModelCalls, 1u);
}
TEST_F(PersistentRun, StoppedPreparationCannotConcealTheSharedTimeout) {
  for (unsigned Outcome = 0; Outcome < 3; ++Outcome) {
    Run.reset();
    freshScene();
    State->OnPrepare = [Outcome](ExecutionSession &S,
                                 ModuleBindings &) -> llvm::Error {
      S.budget().remainingMicroseconds(ExecutionBudget::Clock::now() +
                                       std::chrono::hours(1));
      if (Outcome == 1)
        return failure();
      if (Outcome == 2)
        return llvm::make_error<ModuleDependencyError>("support.module");
      return llvm::Error::success();
    };
    start();
    ASSERT_TRUE(Run);
    auto E = Run->run();
    ASSERT_TRUE(bool(E));
    EXPECT_EQ(E->Reason, ExecutionRunReason::Timeout);
    EXPECT_FALSE(E->Request);
    EXPECT_EQ(State->Exits, 0u);
    EXPECT_EQ(Run->inspect()->Instructions, 0u);
    EXPECT_FALSE(Sources->prepared("data"));
  }
}
TEST_F(PersistentRun,
       SuppliedBudgetMismatchFailsBeforeEnvironmentInitialization) {
  for (unsigned Index = 0; Index < 4; ++Index) {
    auto Limits = Options.Limits;
    auto Policy = Options.TimePolicy;
    if (Index == 0)
      ++Limits.Instructions;
    if (Index == 1)
      ++Limits.Events;
    if (Index == 2)
      ++Limits.TimeoutMicroseconds;
    if (Index == 3)
      Policy = ExecutionTimePolicy::AbsoluteDeadline;
    std::shared_ptr<ExecutionBudget> Budget =
        llvm::cantFail(ExecutionBudget::createWithTimePolicy(Limits, Policy));
    auto Created = construct(Budget);
    EXPECT_FALSE(bool(Created));
    if (!Created)
      EXPECT_NE(llvm::toString(Created.takeError()).find("budget"),
                std::string::npos);
    EXPECT_EQ(State->Initialized, 0u);
  }
}
TEST_F(PersistentRun,
       SuppliedExhaustedBudgetCannotAcquireFreshInstructionCredits) {
  std::shared_ptr<ExecutionBudget> Budget =
      llvm::cantFail(ExecutionBudget::createWithTimePolicy(Options.Limits,
                                                           Options.TimePolicy));
  ASSERT_TRUE(Budget->consumeInstructions(Options.Limits.Instructions));
  auto Created = construct(Budget);
  ASSERT_TRUE(bool(Created));
  Run = std::move(*Created);
  auto E = Run->run();
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(E->State, ExecutionRunState::Failed);
  EXPECT_EQ(E->Reason, ExecutionRunReason::InstructionLimit);
  EXPECT_EQ(Budget->instructions(), Options.Limits.Instructions);
  EXPECT_EQ(State->Exits, 0u);
}
TEST_F(PersistentRun, ExplicitEnvironmentFailureReasonsStayTyped) {
  for (auto Reason :
       {ExecutionRunReason::GuestFailure,
        ExecutionRunReason::EnvironmentFailure,
        ExecutionRunReason::InstructionLimit, ExecutionRunReason::EventLimit,
        ExecutionRunReason::Timeout}) {
    Run.reset();
    freshScene();
    State->FailureReason = Reason;
    start();
    auto E = Run->run();
    ASSERT_TRUE(bool(E));
    EXPECT_EQ(E->State, ExecutionRunState::Failed);
    EXPECT_EQ(E->Reason, Reason);
    EXPECT_EQ(E->Diagnostic, "scene failure");
    EXPECT_EQ(Run->inspect()->Instructions, 0u);
  }
}
TEST_F(PersistentRun,
       InvalidEnvironmentFailureReasonsCannotPublishSuccessOrResourceStates) {
  for (auto Reason :
       {ExecutionRunReason::Created, ExecutionRunReason::Completed,
        ExecutionRunReason::Resource, ExecutionRunReason::Quantum,
        static_cast<ExecutionRunReason>(255)}) {
    Run.reset();
    freshScene();
    State->FailureReason = Reason;
    start();
    auto E = Run->run();
    ASSERT_TRUE(bool(E));
    EXPECT_EQ(E->State, ExecutionRunState::Failed);
    EXPECT_EQ(E->Reason, ExecutionRunReason::EnvironmentFailure);
    EXPECT_NE(E->Diagnostic.find("invalid environment failure"),
              std::string::npos);
  }
  Run.reset();
  freshScene();
  State->FailureReason = ExecutionRunReason::Timeout;
  State->FailureOnNonfailedAction = true;
  start();
  auto E = Run->run();
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(E->State, ExecutionRunState::Failed);
  EXPECT_EQ(E->Reason, ExecutionRunReason::EnvironmentFailure);
  EXPECT_NE(E->Diagnostic.find("nonfailed environment action"),
            std::string::npos);
}
TEST_F(PersistentRun,
       ModelReturnBoundariesCompleteWithoutFreshInstructionCredits) {
  State->Bytes = {0x90, 0x90, 0x90};
  State->OnInitialize = [](ExecutionSession &S, ModuleBindings &) {
    llvm::cantFail(S.watchExecution({}));
  };
  Options.Limits.Instructions = 1;
  Options.InstructionQuantum = 1;
  Options.ActionsPerRun = 1;
  State->BeforeInstruction = [](uint64_t PC, uint32_t) {
    return PC == Code ? InstructionAdmission::Execute
                      : InstructionAdmission::Stop;
  };
  State->OnExit = [&](ExecutionSession &S, const SessionExit &E) {
    if (E.Kind != SessionExitKind::ModelBoundary)
      return;
    const auto PC = *S.cpu().reg(X64Register::PC);
    if (PC == Code + 1)
      llvm::cantFail(S.cpu().setReg(X64Register::PC, Code + 2));
    else {
      State->Complete = true;
      llvm::cantFail(S.cpu().setReg(X64Register::AX, 42));
    }
  };
  start();
  bool Reached = false;
  for (unsigned I = 0; I < 10; ++I) {
    auto E = Run->run();
    ASSERT_TRUE(bool(E));
    ASSERT_EQ(E->State, ExecutionRunState::Paused) << E->Diagnostic;
    if (E->Reason == ExecutionRunReason::Stage) {
      Reached = true;
      break;
    }
    ASSERT_EQ(E->Reason, ExecutionRunReason::Quantum);
    ASSERT_EQ(llvm::toString(Run->resume()), "");
  }
  EXPECT_TRUE(Reached);
  EXPECT_EQ(Run->inspect()->Instructions, 1u);
  EXPECT_EQ(*State->CPU->reg(X64Register::AX), 42u);
}
TEST_F(PersistentRun, ManualSupplyPreservesCPUEnvironmentMemoryAndBudgets) {
  start();
  ASSERT_TRUE(Run);
  const auto Request = missing();
  EXPECT_EQ(Request.Kind, ModuleRequestKind::UnmappedCandidate);
  EXPECT_FALSE(Request.ModuleID);
  EXPECT_EQ(State->Initialized, 1u);
  EXPECT_EQ(State->ModelCalls, 1u);
  EXPECT_EQ(State->NextCalls, 1u);
  EXPECT_EQ(*State->CPU->reg(X64Register::GSBase), 0x90000u);
  EXPECT_EQ(*Space->readInteger(Output, 8), 1u);
  auto Before = Run->inspect();
  ASSERT_TRUE(bool(Before));
  EXPECT_EQ(Before->Instructions, 1u);
  auto Invalid = Run->run();
  EXPECT_FALSE(bool(Invalid));
  if (!Invalid)
    llvm::consumeError(Invalid.takeError());
  ASSERT_EQ(llvm::toString(Run->supply(Request.Token, {binding()})), "");
  auto Ready = Run->inspect();
  ASSERT_TRUE(bool(Ready));
  EXPECT_EQ(Ready->Event.State, ExecutionRunState::Ready);
  EXPECT_EQ(Ready->Instructions, Before->Instructions);
  EXPECT_TRUE(Ready->Continuation);
  EXPECT_NE(llvm::toString(State->CPU->setReg(X64Register::AX, 99)), "");
  stage();
  EXPECT_EQ(State->Initialized, 1u);
  EXPECT_EQ(State->ModelCalls, 1u);
  EXPECT_EQ(*Space->readInteger(Output, 8), 1u);
  EXPECT_EQ(*State->CPU->reg(X64Register::GSBase), 0x90000u);
  auto After = Run->inspect();
  ASSERT_TRUE(bool(After));
  EXPECT_EQ(After->Instructions, 2u);
  EXPECT_EQ(After->Events, 2u);
  ASSERT_EQ(llvm::toString(Run->resume()), "");
  auto End = Run->run();
  ASSERT_TRUE(bool(End));
  EXPECT_EQ(End->State, ExecutionRunState::Completed);
  EXPECT_NE(llvm::toString(Run->resume()), "");
  EXPECT_NE(llvm::toString(Run->supply(Request.Token, {binding()})), "");
}
TEST_F(PersistentRun,
       DeclaredLazyModuleLoadsAtFirstAccessWithoutAUserRoundTrip) {
  Options.Bindings = {binding()};
  start();
  ASSERT_TRUE(Run);
  EXPECT_FALSE(Sources->prepared("data"));
  EXPECT_FALSE(Run->modules().find("data-instance")->mapped());
  stage();
  EXPECT_TRUE(Run->modules().find("data-instance")->mapped());
  EXPECT_EQ(State->Exits, 2u);
  EXPECT_EQ(State->ModelCalls, 1u);
}
TEST_F(PersistentRun, EagerModulesMapBeforeCPUAdmission) {
  catalogue(LoadPolicy::Eager);
  Options.Bindings = {binding()};
  start();
  ASSERT_TRUE(Run);
  EXPECT_FALSE(Sources->prepared("data"));
  stage();
  EXPECT_EQ(State->Exits, 1u);
  auto Info = Run->inspect();
  ASSERT_TRUE(bool(Info));
  EXPECT_EQ(Info->Instructions, 1u);
  EXPECT_EQ(Info->Events, 0u);
}
TEST_F(PersistentRun,
       MissingEagerInputCanBeCorrectedWithoutRepeatingTheSceneAction) {
  catalogue(LoadPolicy::Eager, Root / "omitted.elf");
  Options.Bindings = {binding()};
  start();
  ASSERT_TRUE(Run);
  const auto Request = missing();
  EXPECT_EQ(Request.Kind, ModuleRequestKind::DeclaredModule);
  EXPECT_EQ(State->Exits, 0u);
  ASSERT_EQ(llvm::toString(Sources->reviseUnprepared("data", File)), "");
  ASSERT_EQ(llvm::toString(Run->supply(Request.Token, {binding()})), "");
  stage();
  EXPECT_EQ(State->ModelCalls, 1u);
  EXPECT_EQ(State->Exits, 1u);
}
TEST_F(PersistentRun, MissingLazyInputKeepsItsActualCPUContinuationForRetry) {
  catalogue(LoadPolicy::Lazy, Root / "omitted.elf");
  Options.Bindings = {binding()};
  start();
  ASSERT_TRUE(Run);
  const auto Request = missing();
  EXPECT_EQ(State->Exits, 1u);
  auto Info = Run->inspect();
  ASSERT_TRUE(bool(Info));
  EXPECT_TRUE(Info->Event.Resource);
  EXPECT_FALSE(State->CPU->fault());
  ASSERT_EQ(llvm::toString(Sources->reviseUnprepared("data", File)), "");
  ASSERT_EQ(llvm::toString(Run->supply(Request.Token, {binding()})), "");
  stage();
  EXPECT_EQ(State->ModelCalls, 1u);
}
TEST_F(PersistentRun, ResolverCanRegisterAnOmittedResourceAndUseTheSameSupply) {
  start();
  ASSERT_TRUE(Run);
  missing();
  auto Resolved = Run->resolve(
      [&](const ModuleRequest &Request) -> llvm::Expected<ModuleResolution> {
        EXPECT_FALSE(Request.ModuleID);
        if (auto E = Sources->add({"late", File, {}, LoadPolicy::Lazy}))
          return std::move(E);
        auto B = binding();
        B.ModuleID = "late";
        return ModuleResolution{ModuleResolutionKind::Found, {B}};
      });
  ASSERT_TRUE(bool(Resolved));
  EXPECT_EQ(*Resolved, ModuleResolutionKind::Found);
  stage();
  EXPECT_TRUE(Run->modules().find("data-instance")->mapped());
}
TEST_F(PersistentRun, ResolverWaitingAndErrorsPreserveThePendingRequest) {
  start();
  ASSERT_TRUE(Run);
  const auto Request = missing();
  for (auto Kind :
       {ModuleResolutionKind::NeedInput, ModuleResolutionKind::NotFound}) {
    auto Result = Run->resolve(
        [&](const ModuleRequest &) -> llvm::Expected<ModuleResolution> {
          return ModuleResolution{Kind, {}};
        });
    ASSERT_TRUE(bool(Result));
    EXPECT_EQ(*Result, Kind);
  }
  auto Result = Run->resolve(
      [](const ModuleRequest &) -> llvm::Expected<ModuleResolution> {
        return failure();
      });
  ASSERT_FALSE(bool(Result));
  llvm::consumeError(Result.takeError());
  auto Info = Run->inspect();
  ASSERT_TRUE(bool(Info));
  EXPECT_EQ(Info->Event.Request->Token, Request.Token);
  ASSERT_EQ(llvm::toString(Run->supply(Request.Token, {binding()})), "");
  stage();
}
TEST_F(PersistentRun, CallbackReentryCannotExecuteSupplyOrMutateTheRun) {
  start();
  ASSERT_TRUE(Run);
  const auto Request = missing();
  auto Result = Run->resolve(
      [&](const ModuleRequest &) -> llvm::Expected<ModuleResolution> {
        auto Nested = Run->run();
        EXPECT_FALSE(bool(Nested));
        if (!Nested)
          llvm::consumeError(Nested.takeError());
        EXPECT_NE(llvm::toString(Run->supply(Request.Token, {binding()})), "");
        EXPECT_NE(llvm::toString(Run->resume()), "");
        auto Inspection = Run->inspect();
        EXPECT_FALSE(bool(Inspection));
        if (!Inspection)
          llvm::consumeError(Inspection.takeError());
        return ModuleResolution{ModuleResolutionKind::Found, {binding()}};
      });
  ASSERT_TRUE(bool(Result));
  stage();
}
TEST_F(PersistentRun, FailedSupplyRetainsTokenAndGuestState) {
  start();
  ASSERT_TRUE(Run);
  const auto Request = missing();
  EXPECT_NE(llvm::toString(
                Run->supply(Request.Token, {binding("wrong", Missing + Page)})),
            "");
  EXPECT_EQ(Space->mappedBytes(), 3 * Page);
  EXPECT_EQ(State->ModelCalls, 1u);
  EXPECT_FALSE(State->CPU->fault());
  auto Info = Run->inspect();
  ASSERT_TRUE(bool(Info));
  EXPECT_EQ(Info->Event.Request->Token, Request.Token);
  ASSERT_EQ(llvm::toString(Run->supply(Request.Token, {binding()})), "");
  EXPECT_NE(llvm::toString(Run->supply(Request.Token, {binding()})), "");
  stage();
}
TEST_F(PersistentRun, SourceAdditionAloneDoesNotMapOrResumeAnExecution) {
  start();
  ASSERT_TRUE(Run);
  const auto Request = missing();
  ASSERT_EQ(
      llvm::toString(Sources->add({"extra", File, {}, LoadPolicy::Eager})), "");
  EXPECT_EQ(Run->state(), ExecutionRunState::AwaitingResource);
  EXPECT_FALSE(*Space->canAccess(Missing, 1, Read));
  EXPECT_EQ(State->Exits, 1u);
  EXPECT_NE(llvm::toString(Run->resume()), "");
  ASSERT_EQ(llvm::toString(Run->supply(Request.Token, {binding()})), "");
  stage();
  EXPECT_FALSE(Sources->prepared("extra"));
}
TEST_F(PersistentRun, SharedSourcesHaveIndependentRunsAndForeignTokensFail) {
  start();
  ASSERT_TRUE(Run);
  auto First = std::move(Run);
  auto FirstState = State;
  auto A = First->run();
  ASSERT_TRUE(bool(A));
  ASSERT_TRUE(A->Request);
  freshScene();
  start();
  ASSERT_TRUE(Run);
  const auto B = missing();
  EXPECT_NE(llvm::toString(Run->supply(A->Request->Token, {binding()})), "");
  EXPECT_FALSE(Sources->prepared("data"));
  ASSERT_EQ(llvm::toString(Run->supply(B.Token, {binding()})), "");
  stage();
  EXPECT_EQ(First->state(), ExecutionRunState::AwaitingResource);
  EXPECT_EQ(FirstState->Exits, 1u);
  EXPECT_EQ(*FirstState->CPU->reg(X64Register::AX), 73u);
}
TEST_F(PersistentRun, MetadataDependencyCanPauseBeforeCPUExecution) {
  State->Dependency = "dependency.module";
  start();
  ASSERT_TRUE(Run);
  const auto Request = missing();
  EXPECT_EQ(Request.Kind, ModuleRequestKind::MissingDependency);
  EXPECT_EQ(State->Exits, 0u);
  auto B = binding("dependency");
  ASSERT_EQ(llvm::toString(Run->supply(Request.Token, {B})), "");
  stage();
  EXPECT_EQ(State->ModelCalls, 1u);
}
TEST_F(PersistentRun, UnknownAddressPolicyCanKeepFaultsTerminal) {
  Options.PauseUnknownAddresses = false;
  start();
  ASSERT_TRUE(Run);
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  EXPECT_EQ(Event->State, ExecutionRunState::Failed);
  EXPECT_EQ(Event->Reason, ExecutionRunReason::GuestFailure);
  EXPECT_FALSE(Run->modules().pendingRequest());
  EXPECT_TRUE(State->CPU->fault());
  EXPECT_NE(llvm::toString(Run->resume()), "");
}
TEST_F(PersistentRun, SceneAdmissionCanRejectKnownObjectStateFailures) {
  State->Admit = false;
  start();
  ASSERT_TRUE(Run);
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  EXPECT_EQ(Event->State, ExecutionRunState::Failed);
  EXPECT_EQ(State->Exits, 1u);
  EXPECT_FALSE(Event->Request);
}
TEST_F(PersistentRun, UnknownStageNamesFailBeforeEnvironmentInitialization) {
  Options.PauseStages = {"not-a-stage"};
  auto Created = construct();
  ASSERT_FALSE(bool(Created));
  llvm::consumeError(Created.takeError());
  EXPECT_EQ(State->Initialized, 0u);
  EXPECT_EQ(State->Destroyed, 1u);
}
TEST_F(PersistentRun, StageCanBeSkippedWithoutInventingACompletion) {
  Options.PauseStages.clear();
  Options.Bindings = {binding()};
  start();
  ASSERT_TRUE(Run);
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  EXPECT_EQ(Event->State, ExecutionRunState::Completed);
  EXPECT_EQ(State->Return, 42u);
  EXPECT_TRUE(State->StageSent);
}
TEST_F(PersistentRun, InstructionLimitsAreCumulativeAndTerminal) {
  State->Bytes = {0xEB, 0xFE, 0x90};
  Options.Limits.Instructions = 3;
  Options.InstructionQuantum = 1;
  start();
  ASSERT_TRUE(Run);
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  EXPECT_EQ(Event->State, ExecutionRunState::Failed);
  EXPECT_EQ(Event->Reason, ExecutionRunReason::InstructionLimit);
  ASSERT_TRUE(Event->CPU);
  EXPECT_EQ(Event->CPU->Kind, SessionExitKind::InstructionLimit);
  auto Info = Run->inspect();
  ASSERT_TRUE(bool(Info));
  EXPECT_EQ(Info->Instructions, 3u);
  EXPECT_NE(llvm::toString(Run->resume()), "");
}
TEST_F(PersistentRun, ExactLastInstructionMayReachACompletedStage) {
  catalogue(LoadPolicy::Eager);
  Options.Bindings = {binding()};
  Options.Limits.Instructions = 1;
  Options.PauseStages.clear();
  State->OnInitialize = [](ExecutionSession &Session, ModuleBindings &) {
    llvm::cantFail(Session.watchExecution({}));
  };
  State->BeforeInstruction = [](uint64_t PC, uint32_t) {
    return PC == Code + 3 ? InstructionAdmission::Stop
                          : InstructionAdmission::Execute;
  };
  State->OnExit = [&](ExecutionSession &Session, const SessionExit &Exit) {
    if (Exit.Kind == SessionExitKind::ModelBoundary) {
      State->Complete = true;
      State->Return = *Session.cpu().reg(X64Register::AX);
    }
  };
  start();
  ASSERT_TRUE(Run);
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  EXPECT_EQ(Event->State, ExecutionRunState::Completed) << Event->Diagnostic;
  EXPECT_EQ(State->Return, 42u);
}
TEST_F(PersistentRun, ResourceSupplyDoesNotReplenishTheEventBudget) {
  Options.Limits.Events = 1;
  start();
  ASSERT_TRUE(Run);
  const auto Request = missing();
  EXPECT_NE(llvm::toString(Run->supply(Request.Token, {binding()})), "");
  auto Info = Run->inspect();
  ASSERT_TRUE(bool(Info));
  EXPECT_EQ(Info->Event.State, ExecutionRunState::Failed);
  EXPECT_EQ(Info->Event.Reason, ExecutionRunReason::EventLimit);
  EXPECT_EQ(Info->Events, 1u);
  EXPECT_FALSE(Sources->prepared("data"));
}
TEST_F(PersistentRun, HostExceptionsTerminateTheWorkload) {
  State->OnNext = [](ExecutionSession &, ModuleBindings &) {
    throw std::runtime_error("scene failure");
  };
  start();
  ASSERT_TRUE(Run);
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  EXPECT_EQ(Event->State, ExecutionRunState::Failed);
  EXPECT_EQ(Event->Reason, ExecutionRunReason::EnvironmentFailure);
  EXPECT_NE(llvm::toString(Run->resume()), "");
}
TEST_F(PersistentRun, InvalidMachineCodeCannotBecomeAResourceRequest) {
  State->Bytes = {0x0F, 0x0B, 0x90};
  start();
  ASSERT_TRUE(Run);
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  EXPECT_EQ(Event->State, ExecutionRunState::Failed);
  EXPECT_EQ(Event->Reason, ExecutionRunReason::GuestFailure);
  EXPECT_FALSE(Event->Request);
}
TEST_F(PersistentRun, PausedMemoryOperationsAreBoundedAndPreserveFaultState) {
  start();
  ASSERT_TRUE(Run);
  const auto Request = missing();
  const uint8_t Value[] = {73};
  ASSERT_EQ(llvm::toString(Run->writeMemory(Output, Value)), "");
  auto Bytes = Run->readMemory(Output, 1);
  ASSERT_TRUE(bool(Bytes));
  EXPECT_EQ((*Bytes)[0], 73);
  auto Bad = Run->readMemory(Output, Page + 1);
  ASSERT_FALSE(bool(Bad));
  llvm::consumeError(Bad.takeError());
  Bad = Run->readMemory(Missing, 1);
  ASSERT_FALSE(bool(Bad));
  llvm::consumeError(Bad.takeError());
  EXPECT_FALSE(State->CPU->fault());
  ASSERT_EQ(llvm::toString(Run->supply(Request.Token, {binding()})), "");
  stage();
  EXPECT_EQ(*Space->readInteger(Output, 1), 73u);
}
TEST_F(PersistentRun, UserPausePreservesTheEnvironmentAcrossResume) {
  bool Sent = false;
  State->Bytes = {0xB8, 42, 0, 0, 0, 0x90};
  State->OnInstruction = [&] {
    if (!Sent) {
      Sent = true;
      EXPECT_EQ(llvm::toString(Run->requestPause()), "");
    }
  };
  start();
  ASSERT_TRUE(Run);
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  EXPECT_EQ(Event->State, ExecutionRunState::Paused);
  EXPECT_EQ(Event->Reason, ExecutionRunReason::UserPause);
  EXPECT_FALSE(State->Complete);
  ASSERT_EQ(llvm::toString(Run->resume()), "");
  stage();
  EXPECT_EQ(State->ModelCalls, 1u);
}
TEST_F(PersistentRun,
       PauseRequestedWhileAwaitingResourceSurvivesAutomaticReadiness) {
  start();
  ASSERT_TRUE(Run);
  const auto Request = missing();
  ASSERT_EQ(llvm::toString(Run->requestPause()), "");
  ASSERT_EQ(llvm::toString(Run->supply(Request.Token, {binding()})), "");
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  EXPECT_EQ(Event->Reason, ExecutionRunReason::UserPause);
  EXPECT_EQ(State->Exits, 1u);
  ASSERT_EQ(llvm::toString(Run->resume()), "");
  stage();
  EXPECT_EQ(State->Exits, 2u);
}
TEST_F(PersistentRun, FiniteControlQuantaDoNotResetInstructionCredits) {
  Options.ActionsPerRun = 1;
  catalogue(LoadPolicy::Eager);
  Options.Bindings = {binding()};
  start();
  ASSERT_TRUE(Run);
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  EXPECT_EQ(Event->Reason, ExecutionRunReason::Quantum);
  auto Before = Run->inspect();
  ASSERT_TRUE(bool(Before));
  EXPECT_EQ(Before->Instructions, 1u);
  ASSERT_EQ(llvm::toString(Run->resume()), "");
  Event = Run->run();
  ASSERT_TRUE(bool(Event));
  EXPECT_EQ(Event->Reason, ExecutionRunReason::Stage);
  auto After = Run->inspect();
  ASSERT_TRUE(bool(After));
  EXPECT_EQ(After->Instructions, Before->Instructions);
}
TEST_F(PersistentRun, ActiveTimeExcludesUserWaitingAfterAResourceStop) {
  start();
  ASSERT_TRUE(Run);
  missing();
  auto First = Run->inspect();
  ASSERT_TRUE(bool(First));
  auto Second = Run->inspect();
  ASSERT_TRUE(bool(Second));
  EXPECT_EQ(First->TimePolicy, ExecutionTimePolicy::ActiveWork);
  EXPECT_EQ(First->RemainingMicroseconds, Second->RemainingMicroseconds);
  EXPECT_GT(First->RemainingMicroseconds, 0u);
}

TEST_F(PersistentRun, AnotherThreadCanRequestAPauseDuringCPUExecution) {
  std::mutex Mutex;
  std::condition_variable CV;
  bool Entered = false, Released = false;
  State->Bytes = {0xB8, 42, 0, 0, 0, 0x90};
  State->OnInstruction = [&] {
    std::unique_lock Lock(Mutex);
    if (Entered)
      return;
    Entered = true;
    CV.notify_one();
    CV.wait_for(Lock, std::chrono::seconds(5), [&] { return Released; });
  };
  start();
  ASSERT_TRUE(Run);
  std::optional<ExecutionRunEvent> Result;
  std::string Error;
  std::thread Worker([&] {
    auto Event = Run->run();
    if (Event)
      Result = std::move(*Event);
    else
      Error = llvm::toString(Event.takeError());
  });
  bool InCPU;
  {
    std::unique_lock Lock(Mutex);
    InCPU = CV.wait_for(Lock, std::chrono::seconds(5), [&] { return Entered; });
  }
  if (InCPU) {
    EXPECT_EQ(Run->state(), ExecutionRunState::Running);
    EXPECT_EQ(llvm::toString(Run->requestPause()), "");
  }
  {
    std::unique_lock Lock(Mutex);
    Released = true;
    CV.notify_one();
  }
  Worker.join();
  ASSERT_TRUE(InCPU);
  ASSERT_TRUE(Result) << Error;
  EXPECT_EQ(Result->State, ExecutionRunState::Paused);
  EXPECT_EQ(Result->Reason, ExecutionRunReason::UserPause);
  ASSERT_EQ(llvm::toString(Run->resume()), "");
  stage();
  EXPECT_EQ(State->ModelCalls, 1u);
}

TEST_F(PersistentRun, TerminalStateAllowsInspectionButRejectsMutation) {
  State->Admit = false;
  start();
  ASSERT_TRUE(Run);
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  ASSERT_EQ(Event->State, ExecutionRunState::Failed);
  auto Bytes = Run->readMemory(Output, 8);
  ASSERT_TRUE(bool(Bytes));
  EXPECT_EQ((*Bytes)[0], 1u);
  const uint8_t Value[] = {99};
  EXPECT_NE(llvm::toString(Run->writeMemory(Output, Value)), "");
  EXPECT_NE(llvm::toString(Run->declareModule(binding())), "");
  EXPECT_NE(llvm::toString(Run->requestPause()), "");
  EXPECT_TRUE(State->CPU->fault());
  EXPECT_FALSE(Run->modules().pendingRequest());
}

TEST_F(PersistentRun, EagerMappingFailureTerminatesBeforeCPUAdmission) {
  catalogue(LoadPolicy::Eager);
  Options.Bindings = {binding()};
  State->BuildFails = true;
  start();
  ASSERT_TRUE(Run);
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  EXPECT_EQ(Event->State, ExecutionRunState::Failed);
  EXPECT_EQ(Event->Reason, ExecutionRunReason::EnvironmentFailure);
  EXPECT_EQ(State->Exits, 0u);
  EXPECT_TRUE(Sources->prepared("data"));
  EXPECT_FALSE(Run->modules().find("data-instance")->mapped());
  EXPECT_FALSE(*Space->canAccess(Missing, 1, Read));
}

TEST_F(PersistentRun, DeclaredMappedModuleHolesCannotRequestAnotherBinary) {
  Options.Bindings = {binding()};
  start();
  ASSERT_TRUE(Run);
  stage();
  // Remove a page from an already published module. Its declaration still
  // owns that range; retrying another file cannot repair its runtime state.
  ASSERT_EQ(llvm::toString(Space->unmap(Missing, Page)), "");
  State->Complete = false;
  State->PC = Code;
  ASSERT_EQ(llvm::toString(Run->resume()), "");
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  EXPECT_EQ(Event->State, ExecutionRunState::Failed);
  EXPECT_EQ(Event->Reason, ExecutionRunReason::GuestFailure);
  EXPECT_FALSE(Event->Request);
}

TEST_F(PersistentRun, ResourceRepairDoesNotAllowNewDeclarationsBeforeRestart) {
  start();
  ASSERT_TRUE(Run);
  const auto Request = missing();
  ASSERT_EQ(llvm::toString(Run->supply(Request.Token, {binding()})), "");
  auto Added = binding("another", Missing + Page);
  Added.GuestName = "another.module";
  EXPECT_NE(llvm::toString(Run->declareModule(Added)), "");
  stage();
  EXPECT_EQ(llvm::toString(Run->declareModule(Added)), "");
  EXPECT_FALSE(Run->modules().find("another")->mapped());
}

TEST_F(PersistentRun, ResolverHostExceptionsRetainTheResourceToken) {
  start();
  ASSERT_TRUE(Run);
  const auto Request = missing();
  auto Resolved = Run->resolve(
      [](const ModuleRequest &) -> llvm::Expected<ModuleResolution> {
        throw std::runtime_error("resolver failure");
      });
  ASSERT_FALSE(bool(Resolved));
  EXPECT_NE(llvm::toString(Resolved.takeError()), "");
  auto Info = Run->inspect();
  ASSERT_TRUE(bool(Info));
  ASSERT_TRUE(Info->Event.Request);
  EXPECT_EQ(Info->Event.Request->Token, Request.Token);
  ASSERT_EQ(llvm::toString(Run->supply(Request.Token, {binding()})), "");
  stage();
}
TEST_F(PersistentRun, CheckedContractUsesTheSameRetainedWorkloadAndSupply) {
  Contract = ExecutionContract::CheckedX64;
  start();
  ASSERT_TRUE(Run);
  const auto Request = missing();
  ASSERT_EQ(llvm::toString(Run->supply(Request.Token, {binding()})), "");
  stage();
  EXPECT_EQ(State->ModelCalls, 1u);
  EXPECT_EQ(State->Initialized, 1u);
}

TEST_F(PersistentRun, DisablingResourcePausesLeavesGuestFaultsTerminal) {
  Options.EnableResourcePauses = false;
  start();
  ASSERT_TRUE(Run);
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  EXPECT_EQ(Event->State, ExecutionRunState::Failed);
  EXPECT_EQ(Event->Reason, ExecutionRunReason::GuestFailure);
  EXPECT_TRUE(State->CPU->fault());
  EXPECT_FALSE(Run->modules().pendingRequest());
}

TEST_F(PersistentRun, InvalidBoundsAreRejectedBeforeSceneInitialization) {
  Options.InstructionQuantum = 0;
  auto Created = construct();
  ASSERT_FALSE(bool(Created));
  llvm::consumeError(Created.takeError());
  EXPECT_EQ(State->Initialized, 0u);
  Options.InstructionQuantum = 1;
  Options.MemoryTransferLimit = 64 * 1024 * 1024 + 1;
  Created = construct();
  ASSERT_FALSE(bool(Created));
  llvm::consumeError(Created.takeError());
  EXPECT_EQ(State->Initialized, 0u);
}

TEST_F(PersistentRun, EnvironmentWorkCannotPublishCompletionAfterTimeout) {
  State->Complete = State->StageSent = true;
  State->OnNext = [](ExecutionSession &Session, ModuleBindings &) {
    EXPECT_EQ(Session.budget().remainingMicroseconds(
                  ExecutionBudget::Clock::now() + std::chrono::hours(1)),
              0u);
  };
  start();
  ASSERT_TRUE(Run);
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  EXPECT_EQ(Event->State, ExecutionRunState::Failed);
  EXPECT_EQ(Event->Reason, ExecutionRunReason::Timeout);
  EXPECT_EQ(State->Exits, 0u);
}

TEST_F(PersistentRun, EnvironmentWorkCannotAwaitAResourceAfterTimeout) {
  State->OnExit = [](ExecutionSession &Session, const SessionExit &) {
    EXPECT_EQ(Session.budget().remainingMicroseconds(
                  ExecutionBudget::Clock::now() + std::chrono::hours(1)),
              0u);
  };
  start();
  ASSERT_TRUE(Run);
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  EXPECT_EQ(Event->State, ExecutionRunState::Failed);
  EXPECT_EQ(Event->Reason, ExecutionRunReason::Timeout);
  ASSERT_TRUE(Event->CPU);
  ASSERT_TRUE(Event->CPU->CPU);
  EXPECT_EQ(Event->CPU->CPU->Kind, ExecutionExitKind::ResourcePause);
  EXPECT_FALSE(Run->modules().pendingRequest());
}

TEST_F(PersistentRun, ResourceStopWithNoRemainingInstructionsIsTerminal) {
  Options.Limits.Instructions = 1;
  start();
  ASSERT_TRUE(Run);
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  EXPECT_EQ(Event->State, ExecutionRunState::Failed);
  EXPECT_EQ(Event->Reason, ExecutionRunReason::InstructionLimit);
  EXPECT_FALSE(Run->modules().pendingRequest());
  EXPECT_FALSE(Sources->prepared("data"));
}

TEST_F(PersistentRun, ResolverFailureCannotConcealItsActiveTimeExhaustion) {
  ExecutionBudget *Budget = nullptr;
  State->OnInitialize = [&](ExecutionSession &Session, ModuleBindings &) {
    Budget = &Session.budget();
  };
  start();
  ASSERT_TRUE(Run);
  missing();
  auto Result = Run->resolve(
      [&](const ModuleRequest &) -> llvm::Expected<ModuleResolution> {
        EXPECT_EQ(Budget->remainingMicroseconds(ExecutionBudget::Clock::now() +
                                                std::chrono::hours(1)),
                  0u);
        return failure();
      });
  ASSERT_FALSE(bool(Result));
  llvm::consumeError(Result.takeError());
  auto Info = Run->inspect();
  ASSERT_TRUE(bool(Info));
  EXPECT_EQ(Info->Event.State, ExecutionRunState::Failed);
  EXPECT_EQ(Info->Event.Reason, ExecutionRunReason::Timeout);
  EXPECT_FALSE(Run->modules().pendingRequest());
}

TEST_F(PersistentRun, ResolverIsNotCalledWhenActiveTimeIsAlreadyExhausted) {
  ExecutionBudget *Budget = nullptr;
  State->OnInitialize = [&](ExecutionSession &Session, ModuleBindings &) {
    Budget = &Session.budget();
  };
  start();
  ASSERT_TRUE(Run);
  missing();
  auto Work = Budget->enterWork();
  ASSERT_TRUE(bool(Work));
  ASSERT_EQ(llvm::toString((*Work)->close(ExecutionBudget::Clock::now() +
                                          std::chrono::hours(1))),
            "");
  unsigned Calls = 0;
  auto Result = Run->resolve(
      [&](const ModuleRequest &) -> llvm::Expected<ModuleResolution> {
        ++Calls;
        return ModuleResolution{ModuleResolutionKind::NeedInput, {}};
      });
  ASSERT_FALSE(bool(Result));
  llvm::consumeError(Result.takeError());
  EXPECT_EQ(Calls, 0u);
  auto Info = Run->inspect();
  ASSERT_TRUE(bool(Info));
  EXPECT_EQ(Info->Event.Reason, ExecutionRunReason::Timeout);
}

TEST_F(PersistentRun, InvocationReturnsToTheSamePausedStageAndRestoresCPU) {
  callStage();
  ASSERT_TRUE(Run);
  const auto OriginalSP = *State->CPU->reg(X64Register::SP);
  const auto OriginalPC = *State->CPU->reg(X64Register::PC);
  const RegisterValue Vector{0x1234, 0x5678};
  ASSERT_EQ(llvm::toString(State->CPU->setXmm(6, Vector)), "");
  const uint8_t Function[] = {0xBB, 99,   0,    0,    0,    0x66, 0x0F, 0xEF,
                              0xF6, 0x48, 0x89, 0xC8, 0x48, 0x01, 0xD0, 0xC3};
  ASSERT_EQ(llvm::toString(Run->writeMemory(CallCode, Function)), "");
  const auto Before = *Run->inspect();
  ASSERT_EQ(llvm::toString(Run->invoke(callRequest())), "");
  EXPECT_EQ(Run->state(), ExecutionRunState::Ready);
  EXPECT_TRUE(Run->inspect()->Invocation);
  EXPECT_EQ(State->CallsBegun, 1u);
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  ASSERT_EQ(Event->Reason, ExecutionRunReason::InvocationComplete)
      << Event->Diagnostic;
  ASSERT_TRUE(Event->Invocation);
  EXPECT_EQ(Event->Invocation->Value, 42u);
  EXPECT_EQ(Event->Stage, "after-entry");
  EXPECT_EQ(Event->State, ExecutionRunState::Paused);
  EXPECT_EQ(*State->CPU->reg(X64Register::SP), OriginalSP);
  EXPECT_EQ(*State->CPU->reg(X64Register::PC), OriginalPC);
  EXPECT_EQ(*State->CPU->reg(X64Register::BX), Missing);
  EXPECT_EQ(*State->CPU->reg(X64Register::AX), 42u);
  EXPECT_EQ(*State->CPU->reg(X64Register::GSBase), 0x90000u);
  EXPECT_EQ(*State->CPU->xmm(6), Vector);
  EXPECT_EQ(State->CallsFinished, 1u);
  EXPECT_EQ(State->Initialized, 1u);
  EXPECT_EQ(State->ModelCalls, 1u);
  auto After = Run->inspect();
  ASSERT_TRUE(bool(After));
  EXPECT_FALSE(After->Invocation);
  EXPECT_GT(After->Instructions, Before.Instructions);
  EXPECT_EQ(After->Events, Before.Events + 2);
  ASSERT_EQ(llvm::toString(Run->resume()), "");
  auto End = Run->run();
  ASSERT_TRUE(bool(End));
  EXPECT_EQ(End->State, ExecutionRunState::Completed);
}

TEST_F(PersistentRun, InvocationBuffersPreserveOutputsAndGuestMemoryEffects) {
  State->CallBytes = {0x48, 0x8B, 0x01, 0x48, 0x01,
                      0xD0, 0x48, 0x89, 0x01, 0xC3};
  callStage();
  ASSERT_TRUE(Run);
  auto R = callRequest();
  R.Buffers = {{16, {40}, true}, {7, {9}, false}};
  R.Arguments[0] = {0, 0};
  ASSERT_EQ(llvm::toString(Run->invoke(R)), "");
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  ASSERT_TRUE(Event->Invocation) << Event->Diagnostic;
  EXPECT_EQ(Event->Invocation->Value, 42u);
  const auto &Buffers = Event->Invocation->Buffers;
  ASSERT_EQ(Buffers.size(), 2u);
  ASSERT_EQ(Buffers[0].Bytes.size(), 16u);
  EXPECT_EQ(Buffers[0].Bytes[0], 42u);
  EXPECT_EQ(Buffers[0].Bytes[15], 0u);
  EXPECT_TRUE(Buffers[1].Bytes.empty());
  auto Memory = Run->readMemory(Buffers[0].Address, 16);
  ASSERT_TRUE(bool(Memory));
  EXPECT_EQ(*Memory, Buffers[0].Bytes);
}

TEST_F(PersistentRun, InvocationUsesWin64StackArgumentsAndFiniteCredits) {
  State->CallBytes = {0x48, 0x89, 0xC8, 0x48, 0x01, 0xD0, 0x4C, 0x01,
                      0xC0, 0x4C, 0x01, 0xC8, 0x48, 0x03, 0x44, 0x24,
                      0x28, 0x48, 0x03, 0x44, 0x24, 0x30, 0xC3};
  callStage();
  ASSERT_TRUE(Run);
  auto R = callRequest();
  R.Arguments = {{1, {}}, {2, {}}, {3, {}}, {4, {}}, {5, {}}, {27, {}}};
  R.Limits.Instructions = 7;
  R.Limits.Events = 2;
  ASSERT_EQ(llvm::toString(Run->invoke(R)), "");
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  ASSERT_TRUE(Event->Invocation) << Event->Diagnostic;
  EXPECT_EQ(Event->Invocation->Value, 42u);
  EXPECT_EQ(Event->Invocation->Instructions, 7u);
  EXPECT_EQ(Event->Invocation->Events, 2u);
}

TEST_F(PersistentRun, InvocationCanRepeatWithoutResettingBudgetsOrScene) {
  callStage();
  ASSERT_TRUE(Run);
  auto Before = *Run->inspect();
  for (unsigned I = 0; I < 2; ++I) {
    ASSERT_EQ(llvm::toString(Run->invoke(callRequest())), "");
    auto Event = Run->run();
    ASSERT_TRUE(bool(Event));
    ASSERT_TRUE(Event->Invocation) << Event->Diagnostic;
    EXPECT_EQ(Event->Invocation->Value, 42u);
  }
  auto After = Run->inspect();
  ASSERT_TRUE(bool(After));
  EXPECT_EQ(After->Events, Before.Events + 4);
  EXPECT_EQ(State->CallsBegun, 2u);
  EXPECT_EQ(State->CallsFinished, 2u);
  EXPECT_EQ(State->ModelCalls, 1u);
}

TEST_F(PersistentRun, InvocationInvalidABIAndBuffersHaveNoGuestEffects) {
  callStage();
  ASSERT_TRUE(Run);
  const auto SP = *State->CPU->reg(X64Register::SP);
  auto Before = *Run->readMemory(CallStack, Page);
  auto R = callRequest();
  R.ABI.reset();
  EXPECT_NE(llvm::toString(Run->invoke(R)), "");
  R.ABI = IntegerCallingConvention::AAPCS64;
  EXPECT_NE(llvm::toString(Run->invoke(R)), "");
  R.ABI = IntegerCallingConvention::SysVAMD64;
  EXPECT_NE(llvm::toString(Run->invoke(R)), "");
  R = callRequest();
  R.Arguments[0].Buffer = 0;
  EXPECT_NE(llvm::toString(Run->invoke(R)), "");
  R.Buffers = {{1, {1, 2}, true}};
  EXPECT_NE(llvm::toString(Run->invoke(R)), "");
  R.Buffers = {{Page + 1, {}, true}};
  EXPECT_NE(llvm::toString(Run->invoke(R)), "");
  R.Buffers = {{1, {}, true}};
  R.Arguments[0].Value = 2;
  EXPECT_NE(llvm::toString(Run->invoke(R)), "");
  EXPECT_EQ(State->CallsBegun, 0u);
  EXPECT_EQ(Run->state(), ExecutionRunState::Paused);
  EXPECT_EQ(*State->CPU->reg(X64Register::SP), SP);
  EXPECT_EQ(*Run->readMemory(CallStack, Page), Before);
  EXPECT_EQ(Run->inspect()->Events, 0u);
}

TEST_F(PersistentRun, InvocationRejectsNonStageAndUnsupportedEnvironment) {
  State->Calls = true;
  start();
  ASSERT_TRUE(Run);
  EXPECT_NE(llvm::toString(Run->invoke(callRequest())), "");
  auto Request = missing();
  EXPECT_NE(llvm::toString(Run->invoke(callRequest())), "");
  ASSERT_EQ(llvm::toString(Run->supply(Request.Token, {binding()})), "");
  stage();
  State->Calls = false;
  EXPECT_NE(llvm::toString(Run->invoke(callRequest())), "");
  EXPECT_EQ(Run->state(), ExecutionRunState::Paused);
}

TEST_F(PersistentRun, InvocationRejectsBadStackAndReturnPlansBeforeEffects) {
  callStage();
  ASSERT_TRUE(Run);
  const auto Before = *Run->readMemory(CallStack, Page);
  for (const auto &Plan :
       {GuestInvocationPlan{CallStack, 16, CallReturn},
        GuestInvocationPlan{Stack, Page, CallReturn},
        GuestInvocationPlan{CallStack, Page, CallCode},
        GuestInvocationPlan{CallStack, Page, Missing + Page},
        GuestInvocationPlan{CallStack, Page - 1, CallReturn}}) {
    State->CallPlan = Plan;
    EXPECT_NE(llvm::toString(Run->invoke(callRequest())), "");
    EXPECT_EQ(Run->state(), ExecutionRunState::Paused);
  }
  EXPECT_EQ(State->CallsBegun, 0u);
  EXPECT_EQ(*Run->readMemory(CallStack, Page), Before);
}

TEST_F(PersistentRun, InvocationUnbalancedReturnTerminatesTheWorkload) {
  State->CallBytes = {0xC2, 8, 0};
  callStage();
  ASSERT_TRUE(Run);
  ASSERT_EQ(llvm::toString(Run->invoke(callRequest())), "");
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  EXPECT_EQ(Event->State, ExecutionRunState::Failed);
  EXPECT_EQ(Event->Reason, ExecutionRunReason::GuestFailure);
  EXPECT_NE(Event->Diagnostic.find("unbalanced"), std::string::npos);
  EXPECT_EQ(State->CallsFinished, 0u);
  EXPECT_NE(llvm::toString(Run->resume()), "");
}

TEST_F(PersistentRun, InvocationGuestFaultIsTerminalAndCannotRestoreAnOldCPU) {
  State->CallBytes = {0x0F, 0x0B};
  callStage();
  ASSERT_TRUE(Run);
  ASSERT_EQ(llvm::toString(Run->invoke(callRequest())), "");
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  EXPECT_EQ(Event->State, ExecutionRunState::Failed);
  EXPECT_EQ(Event->Reason, ExecutionRunReason::GuestFailure);
  EXPECT_TRUE(State->CPU->fault());
  EXPECT_EQ(*State->CPU->reg(X64Register::SP), State->Invocation->StackPointer);
  EXPECT_NE(llvm::toString(Run->resume()), "");
}

TEST_F(PersistentRun,
       InvocationInstructionLimitDoesNotUseTheWholeRunAllowance) {
  State->CallBytes = {0xEB, 0xFE};
  callStage();
  ASSERT_TRUE(Run);
  auto R = callRequest();
  R.Limits.Instructions = 3;
  ASSERT_EQ(llvm::toString(Run->invoke(R)), "");
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  EXPECT_EQ(Event->State, ExecutionRunState::Failed);
  EXPECT_EQ(Event->Reason, ExecutionRunReason::InstructionLimit);
  ASSERT_TRUE(Run->inspect()->Invocation);
  EXPECT_EQ(Run->inspect()->Invocation->Instructions, 3u);
  EXPECT_EQ(Run->inspect()->Instructions, 4u);
}

TEST_F(PersistentRun, InvocationReturnAtItsExactLocalInstructionBound) {
  for (auto C : {ExecutionContract::Software, ExecutionContract::CheckedX64}) {
    Run.reset();
    freshScene();
    Contract = C;
    State->CallBytes = {0x48, 0x89, 0xC8, 0xC3}; // mov rax,rcx; ret
    callStage();
    ASSERT_TRUE(Run);
    auto R = callRequest();
    R.Limits.Instructions = 2;
    ASSERT_EQ(llvm::toString(Run->invoke(R)), "");
    auto Event = Run->run();
    ASSERT_TRUE(bool(Event));
    ASSERT_EQ(Event->Reason, ExecutionRunReason::InvocationComplete)
        << Event->Diagnostic;
    ASSERT_TRUE(Event->Invocation);
    EXPECT_EQ(Event->Invocation->Value, 40u);
    EXPECT_EQ(Event->Invocation->Instructions, 2u);
    EXPECT_EQ(State->CallsFinished, 1u);
  }
}

TEST_F(PersistentRun, InvocationResourceSupplyRetainsTheActiveCallFrame) {
  State->CallBytes = {0x48, 0x8B, 0x02, 0xC3};
  callStage();
  ASSERT_TRUE(Run);
  auto R = callRequest();
  R.Arguments[1].Value = Missing + Page;
  ASSERT_EQ(llvm::toString(Run->invoke(R)), "");
  auto Request = missing();
  EXPECT_TRUE(Run->inspect()->Invocation);
  EXPECT_NE(llvm::toString(Run->invoke(R)), "");
  auto B = binding("call-data", Missing + Page);
  B.GuestName = "call-data.module";
  ASSERT_EQ(llvm::toString(Run->supply(Request.Token, {B})), "");
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  ASSERT_TRUE(Event->Invocation) << Event->Diagnostic;
  EXPECT_EQ(Event->Invocation->Value, 42u);
  EXPECT_EQ(State->CallsBegun, 1u);
  EXPECT_EQ(State->CallsFinished, 1u);
  EXPECT_EQ(State->ModelCalls, 1u);
}

TEST_F(PersistentRun, InvocationLazyInstanceCanLoadAtItsFirstInstructionFetch) {
  callStage();
  ASSERT_TRUE(Run);
  const auto CalleeFile = Root / "callee.elf";
  std::filesystem::copy_file(File, CalleeFile);
  {
    const uint8_t Function[] = {0xB8, 42, 0, 0, 0, 0xC3, 0x90, 0x90};
    std::fstream OS(CalleeFile,
                    std::ios::binary | std::ios::in | std::ios::out);
    OS.seekp(Page);
    OS.write(reinterpret_cast<const char *>(Function), sizeof(Function));
  }
  ASSERT_EQ(llvm::toString(
                Sources->add({"callee", CalleeFile, {}, LoadPolicy::Lazy})),
            "");
  ModuleBinding B{
      "callee-instance", "callee", "invocation.module", "test-scene", Space,
      Missing + Page,    Page};
  ASSERT_EQ(llvm::toString(Run->declareModule(B)), "");
  State->AllowedEntry = Missing + Page;
  auto R = callRequest();
  R.InstanceID = B.ID;
  R.Entry = 0;
  ASSERT_EQ(llvm::toString(Run->invoke(R)), "");
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  ASSERT_TRUE(Event->Invocation) << Event->Diagnostic;
  EXPECT_EQ(Event->Invocation->Value, 42u);
  EXPECT_TRUE(Run->modules().find(B.ID)->mapped());
  EXPECT_EQ(State->CallsBegun, 1u);
}

TEST_F(PersistentRun, InvocationCanPauseAndResumeWithoutAdmittingNestedInvoke) {
  callStage();
  ASSERT_TRUE(Run);
  bool Sent = false;
  State->OnInstruction = [&] {
    if (!Sent && State->Invocation) {
      Sent = true;
      EXPECT_NE(llvm::toString(Run->invoke(callRequest())), "");
      EXPECT_EQ(llvm::toString(Run->requestPause()), "");
    }
  };
  ASSERT_EQ(llvm::toString(Run->invoke(callRequest())), "");
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  EXPECT_EQ(Event->Reason, ExecutionRunReason::UserPause);
  EXPECT_TRUE(Run->inspect()->Invocation);
  EXPECT_NE(llvm::toString(Run->invoke(callRequest())), "");
  ASSERT_EQ(llvm::toString(Run->resume()), "");
  Event = Run->run();
  ASSERT_TRUE(bool(Event));
  ASSERT_TRUE(Event->Invocation) << Event->Diagnostic;
  EXPECT_EQ(Event->Invocation->Value, 42u);
  EXPECT_EQ(State->CallsBegun, 1u);
}

TEST_F(PersistentRun, InvocationAPIsAndNestedCallbacksUseTheSameSessionPump) {
  constexpr auto API = Code + 0x200, Nested = Code + 0x180,
                 NestedReturn = Code + 0x280;
  State->CallBytes = {0x48, 0x83, 0xEC, 0x28, 0xE8, 0xF7, 0,    0,    0,
                      0x48, 0x83, 0xC0, 2,    0x48, 0x83, 0xC4, 0x28, 0xC3};
  callStage();
  ASSERT_TRUE(Run);
  const uint8_t Callback[] = {0xB8, 40, 0, 0, 0, 0xC3};
  ASSERT_EQ(llvm::toString(Run->writeMemory(Nested, Callback)), "");
  std::unique_ptr<SessionContext> Parent;
  unsigned APIs = 0, Callbacks = 0;
  State->OnInstruction = [&] {
    if (*State->CPU->reg(X64Register::PC) == API)
      State->Yield = true;
  };
  State->CallExit = [&](ExecutionSession &Session,
                        const SessionExit &Exit) -> llvm::Error {
    auto &CPU = Session.cpu();
    const auto PC = llvm::cantFail(CPU.reg(X64Register::PC));
    const auto ABI =
        llvm::cantFail(IntegerABI::get(IntegerCallingConvention::Win64));
    if (PC == API) {
      ++APIs;
      auto Saved = Session.saveContext();
      if (!Saved)
        return Saved.takeError();
      Parent = std::move(*Saved);
      auto Layout = ABI.prepareCall(CPU, CallStack, Page / 2, NestedReturn, {});
      if (!Layout)
        return Layout.takeError();
      if (auto E = Session.watchExecution({{NestedReturn, 1}}))
        return E;
      State->CallPC = Nested;
    } else if (PC == NestedReturn &&
               Exit.Kind == SessionExitKind::ExecutionWatch) {
      ++Callbacks;
      const auto Value = llvm::cantFail(CPU.reg(X64Register::AX));
      if (auto E = Session.restoreContext(*Parent))
        return E;
      Parent.reset();
      const auto SP = llvm::cantFail(CPU.reg(X64Register::SP));
      auto Return = ABI.readReturnAddress(CPU, SP);
      if (!Return)
        return Return.takeError();
      if (auto E = CPU.setReg(X64Register::AX, Value))
        return E;
      if (auto E = CPU.setReg(X64Register::SP, SP + 8))
        return E;
      State->CallPC = *Return;
    }
    return llvm::Error::success();
  };
  ASSERT_EQ(llvm::toString(Run->invoke(callRequest())), "");
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  ASSERT_TRUE(Event->Invocation) << Event->Diagnostic;
  EXPECT_EQ(Event->Invocation->Value, 42u);
  EXPECT_EQ(APIs, 1u);
  EXPECT_EQ(Callbacks, 1u);
  EXPECT_EQ(State->CallsBegun, 1u);
  EXPECT_EQ(State->CallsFinished, 1u);
  EXPECT_FALSE(Parent);
}

TEST_F(PersistentRun, InvocationCheckedCPUUsesTheSameCallAndReturnContract) {
  Contract = ExecutionContract::CheckedX64;
  callStage();
  ASSERT_TRUE(Run);
  ASSERT_EQ(llvm::toString(Run->invoke(callRequest())), "");
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  ASSERT_TRUE(Event->Invocation) << Event->Diagnostic;
  EXPECT_EQ(Event->Invocation->Value, 42u);
}

TEST_F(PersistentRun, InvocationEventsAndTimeoutRemainTerminal) {
  callStage();
  ASSERT_TRUE(Run);
  auto R = callRequest();
  R.Limits.Events = 2;
  State->CallExit = [](ExecutionSession &Session, const SessionExit &) {
    EXPECT_TRUE(Session.budget().consumeEvents());
    return llvm::Error::success();
  };
  ASSERT_EQ(llvm::toString(Run->invoke(R)), "");
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  EXPECT_EQ(Event->State, ExecutionRunState::Failed);
  EXPECT_EQ(Event->Reason, ExecutionRunReason::EventLimit);
  EXPECT_EQ(State->CallsFinished, 0u);
}

TEST_F(PersistentRun, InvocationModelTimeConsumesTheCallSpecificAllowance) {
  callStage();
  ASSERT_TRUE(Run);
  auto R = callRequest();
  R.Limits.TimeoutMicroseconds = 100000;
  State->CallExit = [](ExecutionSession &Session, const SessionExit &) {
    Session.budget().remainingMicroseconds(ExecutionBudget::Clock::now() +
                                           std::chrono::seconds(1));
    return llvm::Error::success();
  };
  ASSERT_EQ(llvm::toString(Run->invoke(R)), "");
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  EXPECT_EQ(Event->State, ExecutionRunState::Failed);
  EXPECT_EQ(Event->Reason, ExecutionRunReason::Timeout);
  EXPECT_NE(Event->Diagnostic.find("invocation"), std::string::npos);
  EXPECT_GT(Run->inspect()->RemainingMicroseconds, 0u);
}

TEST_F(PersistentRun, InvocationFinishModelFailureDoesNotRestoreTheOldCPU) {
  State->FinishFails = true;
  callStage();
  ASSERT_TRUE(Run);
  ASSERT_EQ(llvm::toString(Run->invoke(callRequest())), "");
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  EXPECT_EQ(Event->State, ExecutionRunState::Failed);
  EXPECT_EQ(Event->Reason, ExecutionRunReason::EnvironmentFailure);
  EXPECT_EQ(*State->CPU->reg(X64Register::PC), CallReturn);
  EXPECT_EQ(State->CallsFinished, 1u);
}

TEST_F(PersistentRun, InvocationZeroSizeBuffersAndOnePastPointersAreExplicit) {
  callStage();
  ASSERT_TRUE(Run);
  auto R = callRequest();
  R.Buffers = {{0, {}, true}};
  R.Arguments = {{0, 0}, {0, {}}};
  ASSERT_EQ(llvm::toString(Run->invoke(R)), "");
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  ASSERT_TRUE(Event->Invocation) << Event->Diagnostic;
  EXPECT_EQ(Event->Invocation->Value, CallStack + Page);
  ASSERT_EQ(Event->Invocation->Buffers.size(), 1u);
  EXPECT_TRUE(Event->Invocation->Buffers[0].Bytes.empty());
}

TEST_F(PersistentRun, InvocationBeginModelFailureIsTerminalBeforeCPUExecution) {
  State->BeginFails = true;
  callStage();
  ASSERT_TRUE(Run);
  const auto Before = *Run->inspect();
  EXPECT_NE(llvm::toString(Run->invoke(callRequest())), "");
  EXPECT_EQ(Run->state(), ExecutionRunState::Failed);
  EXPECT_EQ(Run->inspect()->Event.Reason,
            ExecutionRunReason::EnvironmentFailure);
  EXPECT_EQ(Run->inspect()->Instructions, Before.Instructions);
  EXPECT_NE(llvm::toString(Run->resume()), "");
}

TEST_F(PersistentRun, InvocationCompletionRequiresTheSharedEventCredit) {
  Options.Limits.Events = 1;
  callStage();
  ASSERT_TRUE(Run);
  ASSERT_EQ(llvm::toString(Run->invoke(callRequest())), "");
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  EXPECT_EQ(Event->State, ExecutionRunState::Failed);
  EXPECT_EQ(Event->Reason, ExecutionRunReason::EventLimit);
  EXPECT_EQ(State->CallsFinished, 0u);
}

TEST_F(PersistentRun,
       InvocationUserWaitingDoesNotChargeTheActiveCallAllowance) {
  State->CallBytes = {0x48, 0x8B, 0x02, 0xC3};
  callStage();
  ASSERT_TRUE(Run);
  auto R = callRequest();
  R.Arguments[1].Value = Missing + Page;
  ASSERT_EQ(llvm::toString(Run->invoke(R)), "");
  missing();
  auto Before = Run->inspect();
  auto After = Run->inspect();
  ASSERT_TRUE(bool(Before));
  ASSERT_TRUE(bool(After));
  ASSERT_TRUE(Before->Invocation);
  ASSERT_TRUE(After->Invocation);
  EXPECT_EQ(Before->Invocation->RemainingMicroseconds,
            After->Invocation->RemainingMicroseconds);
  EXPECT_EQ(Before->Invocation->Instructions, After->Invocation->Instructions);
}

TEST_F(PersistentRun, InvocationPrivateStackFailureCannotRequestAnotherBinary) {
  State->CallBytes = {0x48, 0x8B, 0x01, 0xC3};
  callStage();
  ASSERT_TRUE(Run);
  auto R = callRequest();
  R.Buffers = {{8, {42}, true}};
  R.Arguments = {{0, 0}};
  ASSERT_EQ(llvm::toString(Run->invoke(R)), "");
  bool Removed = false;
  State->OnNext = [&](ExecutionSession &, ModuleBindings &) {
    if (!Removed) {
      Removed = true;
      EXPECT_EQ(llvm::toString(Space->unmap(CallStack, Page)), "");
    }
  };
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  EXPECT_EQ(Event->State, ExecutionRunState::Failed);
  EXPECT_EQ(Event->Reason, ExecutionRunReason::GuestFailure);
  EXPECT_FALSE(Event->Request);
  EXPECT_TRUE(State->CPU->fault());
}

TEST_F(PersistentRun, InvocationPlanningFailureCannotConcealTheSharedTimeout) {
  callStage();
  ASSERT_TRUE(Run);
  State->OnPlan = [](ExecutionSession &Session) {
    Session.budget().remainingMicroseconds(ExecutionBudget::Clock::now() +
                                           std::chrono::hours(1));
    return failure();
  };
  EXPECT_NE(llvm::toString(Run->invoke(callRequest())), "");
  EXPECT_EQ(Run->state(), ExecutionRunState::Failed);
  EXPECT_EQ(Run->inspect()->Event.Reason, ExecutionRunReason::Timeout);
  EXPECT_EQ(State->CallsBegun, 0u);
}

TEST_F(PersistentRun, InvocationPlanningConsumesItsCallTimeBeforeGuestEffects) {
  callStage();
  ASSERT_TRUE(Run);
  const auto Before = *Run->readMemory(CallStack, Page);
  State->OnPlan = [](ExecutionSession &Session) {
    Session.budget().remainingMicroseconds(ExecutionBudget::Clock::now() +
                                           std::chrono::seconds(1));
    return llvm::Error::success();
  };
  auto R = callRequest();
  R.Limits.TimeoutMicroseconds = 100000;
  EXPECT_NE(llvm::toString(Run->invoke(R)), "");
  EXPECT_EQ(Run->state(), ExecutionRunState::Failed);
  EXPECT_EQ(Run->inspect()->Event.Reason, ExecutionRunReason::Timeout);
  EXPECT_GT(Run->inspect()->RemainingMicroseconds, 0u);
  EXPECT_EQ(*Run->readMemory(CallStack, Page), Before);
  EXPECT_EQ(State->CallsBegun, 0u);
}

TEST_F(PersistentRun, InvocationRejectsUnknownAndOutOfExtentInstanceEntries) {
  callStage();
  ASSERT_TRUE(Run);
  auto R = callRequest();
  R.InstanceID = "unknown";
  EXPECT_NE(llvm::toString(Run->invoke(R)), "");
  R.InstanceID = "data-instance";
  R.Entry = Page;
  EXPECT_NE(llvm::toString(Run->invoke(R)), "");
  EXPECT_EQ(State->CallsBegun, 0u);
  EXPECT_EQ(Run->state(), ExecutionRunState::Paused);
}

TEST_F(PersistentRun, JsonResourceRoundTripRepairsTheSameStoppedWorkload) {
  auto Controls = executionRunOptionsFromJSON(
      R"({"pause_stages":["after-entry"],"memory_transfer_limit":4096})");
  ASSERT_TRUE(bool(Controls));
  Options = std::move(*Controls);
  start();
  ASSERT_TRUE(Run);
  const auto Request = missing();
  auto Before = Run->inspect();
  ASSERT_TRUE(bool(Before));
  const ExecutionSpaceIdentifier ID =
      [&](const auto &S) -> llvm::Expected<std::string> {
    if (S != Space)
      return failure();
    return std::string("primary");
  };
  auto Report = executionRunSnapshotJSON(*Before, ID);
  ASSERT_TRUE(bool(Report));
  auto Snapshot = llvm::json::parse(*Report);
  ASSERT_TRUE(bool(Snapshot));
  const auto *Event = Snapshot->getAsObject()->getObject("event");
  ASSERT_TRUE(Event);
  const auto *WireRequest = Event->getObject("request");
  ASSERT_TRUE(WireRequest);
  EXPECT_EQ(WireRequest->getString("address_space"), "primary");
  EXPECT_EQ(WireRequest->getString("kind"), "unmapped_candidate");
  EXPECT_FALSE(WireRequest->get("module_id"));
  const auto *Token = WireRequest->get("token");
  ASSERT_TRUE(Token);
  llvm::json::Object Supply{{"token", *Token},
                            {"bindings", llvm::json::Array{llvm::json::Object{
                                             {"id", "data-instance"},
                                             {"module_id", "data"},
                                             {"guest_name", "support.module"},
                                             {"scope", "test-scene"},
                                             {"address_space", "primary"},
                                             {"base", "0x50000"},
                                             {"size", "0x1000"}}}}};
  auto Input = executionSupplyFromJSON(
      llvm::formatv("{0}", llvm::json::Value(std::move(Supply))).str());
  ASSERT_TRUE(bool(Input));
  EXPECT_EQ(Input->Token, Request.Token);
  auto Bindings = resolveExecutionBindings(
      Input->Bindings,
      [&](llvm::StringRef Name)
          -> llvm::Expected<std::shared_ptr<AddressSpace>> {
        if (Name != "primary")
          return failure();
        return Space;
      });
  ASSERT_TRUE(bool(Bindings));
  ASSERT_EQ(llvm::toString(Run->supply(Input->Token, *Bindings)), "");
  EXPECT_EQ(Run->inspect()->Instructions, Before->Instructions);
  EXPECT_TRUE(Run->inspect()->Continuation);
  stage();
  EXPECT_EQ(State->Initialized, 1u);
  EXPECT_EQ(State->ModelCalls, 1u);
  EXPECT_EQ(*Space->readInteger(Output, 8), 1u);
  auto Instances = executionInstancesJSON(Run->modules().inspect(), ID);
  ASSERT_TRUE(bool(Instances));
  auto ParsedInstances = llvm::json::parse(*Instances);
  ASSERT_TRUE(bool(ParsedInstances));
  const auto *Rows = ParsedInstances->getAsObject()->getArray("instances");
  ASSERT_TRUE(Rows);
  ASSERT_EQ(Rows->size(), 1u);
  EXPECT_EQ((*Rows)[0].getAsObject()->getBoolean("mapped"), true);
  EXPECT_EQ((*Rows)[0].getAsObject()->getString("base"), "0x50000");
  EXPECT_EQ((*Rows)[0].getAsObject()->getString("image_sha256"),
            Sources->prepared("data")->imageDigest());
  const auto Instructions = Run->inspect()->Instructions;
  EXPECT_NE(llvm::toString(Run->supply(Input->Token, *Bindings)), "");
  EXPECT_EQ(Run->inspect()->Instructions, Instructions);
  EXPECT_EQ(Run->state(), ExecutionRunState::Paused);
}
TEST_F(PersistentRun, JsonInvocationUsesTheSameFrameAndReturnBoundary) {
  callStage();
  ASSERT_TRUE(Run);
  const auto Before = Run->inspect()->Instructions;
  auto Invalid = guestInvocationFromJSON(
      R"({"entry":"0x10100","abi":"win64","arguments":[{"value":1.0}]})");
  EXPECT_FALSE(bool(Invalid));
  if (!Invalid)
    llvm::consumeError(Invalid.takeError());
  EXPECT_EQ(Run->state(), ExecutionRunState::Paused);
  EXPECT_EQ(Run->inspect()->Instructions, Before);
  auto Input = guestInvocationFromJSON(R"({"entry":"0x10100","abi":"win64",
    "arguments":[{"value":40},{"value":2}],"buffers":[{"size":4,"input_hex":"0080FF01"}],
    "limits":{"instructions":100,"events":100,"timeout_microseconds":1000000}})",
                                       Page);
  ASSERT_TRUE(bool(Input));
  ASSERT_EQ(llvm::toString(Run->invoke(std::move(*Input))), "");
  auto Progress = executionRunSnapshotJSON(*Run->inspect());
  ASSERT_TRUE(bool(Progress));
  auto P = llvm::json::parse(*Progress);
  ASSERT_TRUE(bool(P));
  EXPECT_TRUE(P->getAsObject()->getObject("invocation"));
  auto Event = Run->run();
  ASSERT_TRUE(bool(Event));
  ASSERT_EQ(Event->Reason, ExecutionRunReason::InvocationComplete);
  auto Report = executionRunEventJSON(*Event);
  ASSERT_TRUE(bool(Report));
  auto V = llvm::json::parse(*Report);
  ASSERT_TRUE(bool(V));
  const auto *O = V->getAsObject();
  ASSERT_TRUE(O);
  EXPECT_EQ(O->getString("state"), "paused");
  EXPECT_EQ(O->getString("reason"), "invocation_complete");
  EXPECT_EQ(O->getString("stage"), "after-entry");
  const auto *Result = O->getObject("invocation");
  ASSERT_TRUE(Result);
  EXPECT_EQ(Result->getString("value"), "0x2A");
  ASSERT_EQ(Result->getArray("buffers")->size(), 1u);
  EXPECT_EQ(
      (*Result->getArray("buffers"))[0].getAsObject()->getString("bytes_hex"),
      "0080ff01");
  EXPECT_EQ(State->CallsBegun, 1u);
  EXPECT_EQ(State->CallsFinished, 1u);
  EXPECT_EQ(*State->CPU->reg(X64Register::AX), 42u);
}
} // namespace
