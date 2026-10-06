//===- ParallelExecutionTests.cpp - Concurrent entries and atomic commits ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "arch/aarch64/CheckedAArch64Backend.h"
#include "arch/x86_64/CheckedX64Backend.h"
#include "backends/MachineFactories.h"
#include "core/ExecutionDiagnostics.h"
#include "core/RAMTransaction.h"
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Endian.h"

#include <condition_variable>
#include <future>

namespace neverd::emulation {
namespace {
#define NEVERD_PARALLEL_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_PARALLEL_BYTES(Name, ...)                                       \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#define NEVERD_PARALLEL_TEXT(Name, Text) constexpr char Name[] = Text;
#include "ParallelExecutionCases.def"
#undef NEVERD_PARALLEL_VALUE
#undef NEVERD_PARALLEL_BYTES
#undef NEVERD_PARALLEL_TEXT
using Clock = std::chrono::steady_clock;
constexpr auto Wait = std::chrono::microseconds(WaitMicroseconds);

struct Meeting {
  std::mutex Mutex;
  std::condition_variable Changed;
  unsigned Arrived = 0;
  bool enter() {
    std::unique_lock Lock(Mutex);
    ++Arrived;
    Changed.notify_all();
    return Changed.wait_for(Lock, Wait,
                            [&] { return Arrived == Participants; });
  }
};

class ParallelMemory : public testing::Test {
protected:
  std::shared_ptr<AddressSpace> Space;
  std::array<std::unique_ptr<MemoryProjection>, Participants> Memories;
  void SetUp() override {
    auto RAM = llvm::cantFail(PhysicalMemory::create(Limit));
    Space = llvm::cantFail(AddressSpace::create(RAM, Limit));
    llvm::cantFail(Space->map(Code, memory::PageSize, Read | Write | Execute));
    llvm::cantFail(Space->map(Data, memory::PageSize, Read | Write));
    llvm::cantFail(
        Space->mapAlias(Alias, Data, memory::PageSize, Read | Write));
    llvm::cantFail(Space->writeInteger(Data, Initial, sizeof(uint64_t)));
    for (auto &M : Memories) {
      M = llvm::cantFail(MemoryProjection::create(Space));
      llvm::cantFail(M->enableParallel());
    }
  }
  llvm::Error reader(MemoryProjection &Memory,
                     llvm::function_ref<llvm::Error()> Body) {
    if (auto E = Memory.beginParallelRun({Clock::now() + Wait}))
      return E;
    auto End = llvm::scope_exit([&] { Memory.endRun(); });
    auto Instruction = Memory.beginInstruction();
    if (!Instruction)
      return Instruction.takeError();
    auto T = RAMTransaction::create(Memory, {}, sizeof(uint64_t));
    if (!T)
      return T.takeError();
    if (auto E = (*T)->execute(Body, {}))
      return E;
    if (auto E = (*T)->stage())
      return E;
    return (*T)->commit();
  }
};

TEST_F(ParallelMemory,
       ReadOnlyEntriesActuallyOverlapAndFailuresRetireTheirLease) {
  Meeting Meet;
  std::array<std::future<std::string>, Participants> Runs;
  for (unsigned I = 0; I < Participants; ++I)
    Runs[I] = std::async(std::launch::async, [&, I] {
      return llvm::toString(reader(*Memories[I], [&] {
        if (!Meet.enter())
          return diagnostic::error(MeetingFailure);
        return I ? diagnostic::error(NativeFailure) : llvm::Error::success();
      }));
    });
  EXPECT_EQ(Runs[0].get(), "");
  EXPECT_EQ(Runs[1].get(), NativeFailure);
  EXPECT_EQ(Meet.Arrived, Participants);
  EXPECT_EQ(
      llvm::toString(Space->writeInteger(Data, Updated, sizeof(uint64_t))), "");
}

TEST_F(ParallelMemory,
       CancelledWriterCannotBlockReaderRetirementOrLaterAdmission) {
  std::promise<void> Entered, Release;
  auto Arrived = Entered.get_future();
  auto Finish = Release.get_future().share();
  auto Reading = std::async(std::launch::async, [&] {
    return llvm::toString(reader(*Memories[0], [&]() -> llvm::Error {
      Entered.set_value();
      if (Finish.wait_for(Wait) != std::future_status::ready)
        return diagnostic::error(MeetingFailure);
      return llvm::Error::success();
    }));
  });
  ASSERT_EQ(Arrived.wait_for(Wait), std::future_status::ready);
  EXPECT_NE(llvm::toString(Space->protect(Data, memory::PageSize, Read)), "");
  std::atomic<bool> Stop{false};
  std::promise<void> Attempted;
  auto Attempt = Attempted.get_future();
  auto Writing = std::async(std::launch::async, [&]() -> llvm::Error {
    auto &M = *Memories[1];
    if (auto E = M.beginParallelRun({Clock::now() + Wait, &Stop}))
      return E;
    auto End = llvm::scope_exit([&] { M.endRun(); });
    auto Instruction = M.beginInstruction();
    if (!Instruction)
      return Instruction.takeError();
    Attempted.set_value();
    auto T = RAMTransaction::create(M, RAMWriteRange{Alias, sizeof(uint64_t)},
                                    sizeof(uint64_t));
    return T ? diagnostic::error(MeetingFailure) : T.takeError();
  });
  EXPECT_EQ(Attempt.wait_for(Wait), std::future_status::ready);
  Stop = true;
  const auto Cancelled = Writing.wait_for(Wait);
  if (Cancelled != std::future_status::ready)
    Release.set_value();
  EXPECT_EQ(Cancelled, std::future_status::ready);
  bool Interrupted = false;
  auto Remaining =
      llvm::handleErrors(Writing.get(), [&](const MachineInterruptedError &E) {
        Interrupted = E.stopRequested();
      });
  EXPECT_EQ(llvm::toString(std::move(Remaining)), "");
  EXPECT_TRUE(Interrupted);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Data, sizeof(uint64_t))),
            Initial);
  EXPECT_EQ(llvm::toString(
                reader(*Memories[1], [] { return llvm::Error::success(); })),
            "");
  if (Cancelled == std::future_status::ready)
    Release.set_value();
  EXPECT_EQ(Reading.get(), "");
  EXPECT_EQ(llvm::toString(Space->protect(Data, memory::PageSize, Read)), "");
}

TEST_F(ParallelMemory, AnUnrelatedProjectionCannotBorrowAnotherCPUsAuthority) {
  auto &M = *Memories[0];
  llvm::cantFail(M.beginParallelRun({Clock::now() + Wait}));
  auto End = llvm::scope_exit([&] { M.endRun(); });
  auto Instruction = llvm::cantFail(M.beginInstruction());
  auto Foreign = Memories[1]->executionLock();
  ASSERT_FALSE(bool(Foreign));
  llvm::consumeError(Foreign.takeError());
  EXPECT_NE(
      llvm::toString(Memories[1]->beginParallelRun({Clock::now() + Wait})), "");
  EXPECT_NE(
      llvm::toString(Space->writeInteger(Data, Updated, sizeof(uint64_t))), "");
}

TEST_F(ParallelMemory,
       PrivateTransportBytesStageOutputsAndRetainExceptionPrefixes) {
  auto M = llvm::cantFail(MemoryProjection::create(Space));
  llvm::cantFail(M->enableParallel(true));
  const auto &Page = M->mappings().at(Data);
  const auto Registration = M->registrations()[1];
  auto *Private = Registration.Backing + Page.Physical - Registration.Physical;
  EXPECT_NE(Private, M->physicalPointer(Page.Physical));
  llvm::cantFail(M->beginParallelRun({Clock::now() + Wait}));
  auto End = llvm::scope_exit([&] { M->endRun(); });
  auto Instruction = llvm::cantFail(M->beginInstruction());
  for (bool Commit : {false, true}) {
    auto T = llvm::cantFail(RAMTransaction::create(
        *M, RAMWriteRange{Alias, sizeof(uint64_t)}, sizeof(uint64_t)));
    auto E = T->execute(
        [&] {
          EXPECT_EQ(llvm::support::endian::read64le(Private), Initial);
          llvm::support::endian::write64le(Private, Updated);
          EXPECT_EQ(llvm::cantFail(Space->readInteger(Data, sizeof(uint64_t))),
                    Initial);
          return diagnostic::error(NativeFailure);
        },
        RAMWriteRange{Data, sizeof(uint64_t)});
    EXPECT_EQ(llvm::toString(std::move(E)), NativeFailure);
    llvm::cantFail(T->stage());
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Data, sizeof(uint64_t))),
              Initial);
    if (Commit)
      llvm::cantFail(T->commit());
  }
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Data, sizeof(uint64_t))),
            Updated);
}

struct Profile {
  ExecutionBackendKind Backend;
  GuestArchitecture ISA;
};
void PrintTo(const Profile &P, std::ostream *OS) {
  *OS << executionBackendName(P.Backend) << guestArchitectureName(P.ISA);
}
const Profile Profiles[] = {
#define NEVERD_PARALLEL_PROFILE(Backend, ISA)                                  \
  {ExecutionBackendKind::Backend, GuestArchitecture::ISA},
#include "ParallelExecutionCases.def"
#undef NEVERD_PARALLEL_PROFILE
};
class ParallelCPU : public ParallelMemory,
                    public testing::WithParamInterface<Profile> {};

class MeetingX64 final : public X64Machine {
public:
  std::unique_ptr<X64Machine> Inner;
  Meeting &Meet;
  MeetingX64(std::unique_ptr<X64Machine> Inner, Meeting &Meet)
      : Inner(std::move(Inner)), Meet(Meet) {}
  bool requiresExceptionMonitor() const override {
    return Inner->requiresExceptionMonitor();
  }
  uint32_t mxcsrMask() const override { return Inner->mxcsrMask(); }
  X64BranchModel branchModel() const override { return Inner->branchModel(); }
  llvm::Error step(X64MachineState &State, uint64_t Root,
                   MachineRunControl Control) override {
    if (!Meet.enter())
      return diagnostic::error(MeetingFailure);
    return Inner->step(State, Root, Control);
  }
};
class MeetingARM final : public AArch64Machine {
public:
  std::unique_ptr<AArch64Machine> Inner;
  Meeting &Meet;
  MeetingARM(std::unique_ptr<AArch64Machine> Inner, Meeting &Meet)
      : Inner(std::move(Inner)), Meet(Meet) {}
  llvm::Error step(AArch64MachineState &State,
                   MachineRunControl Control) override {
    if (!Meet.enter())
      return diagnostic::error(MeetingFailure);
    return Inner->step(State, Control);
  }
};
llvm::Expected<std::unique_ptr<ExecutionBackend>>
meetingCPU(Profile P, std::shared_ptr<AddressSpace> Space, Meeting &Meet) {
  auto Build = queryExecutionBackendBuild(P.Backend, P.ISA);
  if (!Build)
    return Build.takeError();
  if (Build->Availability != BackendAvailability::Available)
    return llvm::make_error<BackendUnavailableError>(Build->Reason,
                                                     Build->Availability);
  auto M = llvm::cantFail(MemoryProjection::create(Space));
  llvm::cantFail(M->enableParallel(P.Backend == ExecutionBackendKind::WHP));
  if (P.ISA == GuestArchitecture::X64) {
    auto Native = P.Backend == ExecutionBackendKind::KVM ? createKvmMachine(*M)
                  : P.Backend == ExecutionBackendKind::WHP
                      ? createWhpMachine(*M)
                      : createUnicornX64Machine(*M);
    if (!Native)
      return Native.takeError();
    return CheckedX64Backend::create(
        std::move(M), std::make_unique<MeetingX64>(std::move(*Native), Meet));
  }
  auto Native = P.Backend == ExecutionBackendKind::KVM
                    ? createKvmAArch64Machine(*M)
                : P.Backend == ExecutionBackendKind::WHP
                    ? createWhpAArch64Machine(*M)
                    : createUnicornAArch64Machine(*M);
  if (!Native)
    return Native.takeError();
  return CheckedAArch64Backend::create(
      std::move(M), std::make_unique<MeetingARM>(std::move(*Native), Meet));
}
bool unavailable(llvm::Error E, std::string &Reason) {
  bool Unavailable = false;
  E = llvm::handleErrors(std::move(E), [&](const BackendUnavailableError &E) {
    Unavailable = E.availability() != BackendAvailability::InitializationFailed;
    Reason = E.reason().str();
  });
  if (E) {
    Reason = llvm::toString(std::move(E));
    ADD_FAILURE() << Reason;
  }
  return Unavailable;
}
TEST_P(ParallelCPU, ProcessorCallsOverlapAndPublishIndependentRegisters) {
  const auto P = GetParam();
  const bool X64 = P.ISA == GuestArchitecture::X64;
  llvm::cantFail(Space->write(Code, X64 ? llvm::ArrayRef(X64Add)
                                        : llvm::ArrayRef(ArmAdd)));
  Meeting Meet;
  std::array<std::unique_ptr<ExecutionBackend>, Participants> CPUs;
  for (auto &CPU : CPUs) {
    auto Next = meetingCPU(P, Space, Meet);
    if (!Next) {
      std::string Reason;
      if (unavailable(Next.takeError(), Reason) && !CPUs[0])
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(*Next);
    llvm::cantFail(
        CPU->writeRegister(X64 ? CPURegister::X64AX : CPURegister::AArch64X0,
                           RegisterValue{Initial, 0}));
    BackendHooks Hooks;
    Hooks.Instruction = [Ptr = CPU.get(), X64](uint64_t PC, unsigned) {
      if (PC == Code + (X64 ? X64InstructionBytes : ArmInstructionBytes))
        Ptr->stop();
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
  }
  std::array<std::future<llvm::Expected<ExecutionExit>>, Participants> Runs;
  for (unsigned N = 0; N < Participants; ++N)
    Runs[N] = std::async(std::launch::async, [&, N] {
      return CPUs[N]->runUntilExit(Code, WaitMicroseconds);
    });
  for (unsigned N = 0; N < Participants; ++N) {
    auto Exit = Runs[N].get();
    ASSERT_TRUE(bool(Exit)) << llvm::toString(Exit.takeError());
    EXPECT_EQ(Exit->Kind, ExecutionExitKind::Stopped) << Exit->Diagnostic;
    EXPECT_EQ(llvm::cantFail(CPUs[N]->readRegister(
                  X64 ? CPURegister::X64AX : CPURegister::AArch64X0))[0],
              Initial + 1);
  }
  EXPECT_EQ(Meet.Arrived, Participants);
}

TEST_P(ParallelCPU, ReentryCannotChangeAnActiveCPUsState) {
  const auto P = GetParam();
  const bool X64 = P.ISA == GuestArchitecture::X64;
  ExecutionConfiguration Config;
  Config.Backend = P.Backend;
  Config.Architecture = P.ISA;
  Config.Contract =
      X64 ? ExecutionContract::CheckedX64 : ExecutionContract::CheckedAArch64;
  Config.RequiredFeatures = ExecutionFeature::ParallelCPUs;
  auto Created = createExecutionBackend(Config, Space);
  if (!Created) {
    std::string Reason;
    if (unavailable(Created.takeError(), Reason))
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  auto &CPU = *Created->CPU;
  llvm::cantFail(Space->write(Code, X64 ? llvm::ArrayRef(X64Add)
                                        : llvm::ArrayRef(ArmAdd)));
  std::promise<void> Entered, Released;
  auto Arrival = Entered.get_future();
  auto Release = Released.get_future();
  BackendHooks Hooks;
  Hooks.Instruction = [&](uint64_t, unsigned) {
    Entered.set_value();
    EXPECT_EQ(Release.wait_for(Wait), std::future_status::ready);
    CPU.stop();
  };
  llvm::cantFail(CPU.installHooks(std::move(Hooks)));
  auto Active = std::async(std::launch::async, [&] {
    return CPU.runUntilExit(Code, WaitMicroseconds);
  });
  EXPECT_EQ(Arrival.wait_for(Wait), std::future_status::ready);
  auto Reentered = CPU.runUntilExit(Data, WaitMicroseconds);
  EXPECT_FALSE(bool(Reentered));
  if (!Reentered)
    llvm::consumeError(Reentered.takeError());
  Released.set_value();
  auto Result = Active.get();
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Kind, ExecutionExitKind::Stopped);
  EXPECT_EQ(llvm::cantFail(CPU.readRegister(X64 ? CPURegister::X64PC
                                                : CPURegister::AArch64PC))[0],
            Code);
}

TEST_P(ParallelCPU, AtomicContendersSharePhysicalRAMAcrossAddressSpaces) {
  for (bool Separate : {false, true}) {
    SCOPED_TRACE(Separate);
    auto Region =
        llvm::cantFail(Space->physicalMemory()->allocate(memory::PageSize));
    std::array<std::shared_ptr<AddressSpace>, Participants> Spaces;
    Spaces[0] =
        llvm::cantFail(AddressSpace::create(Space->physicalMemory(), Limit));
    Spaces[1] = Separate ? llvm::cantFail(AddressSpace::create(
                               Space->physicalMemory(), Limit))
                         : Spaces[0];
    for (unsigned N = 0; N < Participants; ++N) {
      if (!N || Separate)
        llvm::cantFail(
            Spaces[N]->map(Code, memory::PageSize, Read | Write | Execute));
      llvm::cantFail(Spaces[N]->mapRegion(N ? Alias : Data, Region, 0,
                                          memory::PageSize, Read | Write));
    }
    const auto P = GetParam();
    const bool X64 = P.ISA == GuestArchitecture::X64;
    const llvm::ArrayRef Program =
        X64 ? llvm::ArrayRef(X64Contend) : llvm::ArrayRef(ArmContend);
    for (auto &S : Spaces)
      llvm::cantFail(S->write(Code, Program));
    ExecutionConfiguration Config;
    Config.Backend = P.Backend;
    Config.Architecture = P.ISA;
    Config.Contract =
        X64 ? ExecutionContract::CheckedX64 : ExecutionContract::CheckedAArch64;
    Config.RequiredFeatures = ExecutionFeature::ParallelCPUs;
    std::array<std::unique_ptr<ExecutionBackend>, Participants> CPUs;
    for (unsigned N = 0; N < Participants; ++N) {
      auto Next = createExecutionBackend(Config, Spaces[N]);
      if (!Next) {
        std::string Reason;
        if (unavailable(Next.takeError(), Reason) && !CPUs[0])
          GTEST_SKIP() << Reason;
        FAIL() << Reason;
      }
      CPUs[N] = std::move(Next->CPU);
      auto Set = [&](CPURegister R, uint64_t V) {
        llvm::cantFail(CPUs[N]->writeRegister(R, RegisterValue{V, 0}));
      };
      Set(X64 ? CPURegister::X64BX : CPURegister::AArch64X3, N ? Alias : Data);
      Set(X64 ? CPURegister::X64CX : CPURegister::AArch64X4, Iterations);
      if (!X64)
        Set(CPURegister::AArch64X1, 1);
      BackendHooks Hooks;
      Hooks.Instruction = [Ptr = CPUs[N].get(),
                           End = Code + Program.size() -
                                 (X64 ? 1 : ArmInstructionBytes)](uint64_t PC,
                                                                  unsigned) {
        if (PC == End)
          Ptr->stop();
      };
      llvm::cantFail(CPUs[N]->installHooks(std::move(Hooks)));
    }
    Meeting Start;
    std::array<std::future<llvm::Expected<ExecutionExit>>, Participants> Runs;
    for (unsigned N = 0; N < Participants; ++N)
      Runs[N] = std::async(
          std::launch::async, [&, N]() -> llvm::Expected<ExecutionExit> {
            if (!Start.enter())
              return diagnostic::error(MeetingFailure);
            return CPUs[N]->runUntilExit(Code, WaitMicroseconds);
          });
    for (auto &Run : Runs) {
      auto Exit = Run.get();
      ASSERT_TRUE(bool(Exit)) << llvm::toString(Exit.takeError());
      EXPECT_EQ(Exit->Kind, ExecutionExitKind::Stopped) << Exit->Diagnostic;
    }
    EXPECT_EQ(llvm::cantFail(Spaces[0]->readInteger(Data, sizeof(uint64_t))),
              Participants * Iterations);
  }
}
INSTANTIATE_TEST_SUITE_P(ExplicitBackends, ParallelCPU,
                         testing::ValuesIn(Profiles),
                         [](const testing::TestParamInfo<Profile> &Info) {
                           return std::string(executionBackendName(
                                      Info.param.Backend)) +
                                  guestArchitectureName(Info.param.ISA);
                         });
} // namespace
} // namespace neverd::emulation
