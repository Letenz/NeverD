//===- DriverShadowStackTests.cpp - Disabled shadow-stack reads ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"
#include "neverd/emulation/ProcessObserver.h"
#include "neverd/emulation/ProcessRuntimeState.h"

#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <tuple>

namespace neverd::emulation {
namespace {
using Parameter = std::tuple<ExecutionBackendKind, ExecutionContract>;
constexpr uint64_t Seed = 0x1234567887654321;
constexpr std::array<X64Register, 17> Registers = {
    X64Register::AX,   X64Register::CX,  X64Register::DX,  X64Register::BX,
    X64Register::SP,   X64Register::BP,  X64Register::SI,  X64Register::DI,
    X64Register::R8,   X64Register::R9,  X64Register::R10, X64Register::R11,
    X64Register::R12,  X64Register::R13, X64Register::R14, X64Register::R15,
    X64Register::FLAGS};

struct Snapshot {
  std::array<uint64_t, Registers.size()> Values{};
  std::array<uint8_t, 40> Stack{};
  std::shared_ptr<const ProcessRuntimeState> Runtime;
};

class ShadowObserver final : public ProcessObserver {
public:
  std::vector<uint64_t> Offsets;
  std::vector<Snapshot> States;
  std::vector<ExecutionWatch> Watches;
  bool Stop = false;
  llvm::Expected<std::vector<ExecutionWatch>> started(ProcessView &P) override {
    for (uint64_t Offset : Offsets)
      Watches.push_back({P.inputModule()->Entry + Offset, 1});
    return Watches;
  }
  llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
  watched(ProcessView &P, uint64_t) override {
    Snapshot State;
    for (unsigned I = 0; I < Registers.size(); ++I) {
      auto Value = P.readRegister(cpuRegister(Registers[I]));
      if (!Value)
        return Value.takeError();
      State.Values[I] = (*Value)[0];
    }
    if (auto E = P.read(State.Values[4], State.Stack))
      return E;
    auto Runtime = P.runtimeState();
    if (!Runtime)
      return Runtime.takeError();
    State.Runtime = std::move(*Runtime);
    States.push_back(std::move(State));
    if (Stop)
      return std::nullopt;
    return Watches;
  }
};

class DriverShadowStack : public testing::TestWithParam<Parameter> {
protected:
  std::filesystem::path Directory;
  DriverOptions Options;
  void SetUp() override {
    std::tie(Options.Backend, Options.Contract) = GetParam();
    auto Probe = createExecutionBackend(Options.Backend, Options.Contract,
                                        Options.MemoryLimit);
    if (!Probe) {
      auto E = Probe.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Text = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Text;
      FAIL() << Text;
    }
    llvm::SmallString<128> Temporary;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(
        "neverd-driver-shadow-read", Temporary));
    Directory = Temporary.str().str();
  }
  void TearDown() override {
    if (!Directory.empty()) {
      std::error_code Ignored;
      std::filesystem::remove_all(Directory, Ignored);
    }
  }
  std::filesystem::path image(llvm::ArrayRef<uint8_t> Code) {
    const auto Input =
        std::filesystem::path(NEVERD_DRIVER_FIXTURES) / "success.sys";
    auto Buffer = llvm::MemoryBuffer::getFile(Input.string());
    if (!Buffer) {
      ADD_FAILURE() << Buffer.getError().message();
      return {};
    }
    auto Object =
        llvm::object::COFFObjectFile::create((*Buffer)->getMemBufferRef());
    if (!Object) {
      ADD_FAILURE() << llvm::toString(Object.takeError());
      return {};
    }
    uintptr_t Entry = 0;
    if (auto E = (*Object)->getRvaPtr(
            (*Object)->getPE32PlusHeader()->AddressOfEntryPoint, Entry)) {
      ADD_FAILURE() << llvm::toString(std::move(E));
      return {};
    }
    const auto Data = (*Buffer)->getBuffer();
    const auto Offset = Entry - reinterpret_cast<uintptr_t>(Data.data());
    if (Offset > Data.size() || Code.size() > Data.size() - Offset) {
      ADD_FAILURE() << "fixture entry has insufficient file bytes";
      return {};
    }
    std::vector<uint8_t> Bytes(Data.bytes_begin(), Data.bytes_end());
    const uint32_t EntryRVA =
        (*Object)->getPE32PlusHeader()->AddressOfEntryPoint;
    const llvm::object::coff_section *EntrySection = nullptr;
    for (const auto &Section : (*Object)->sections()) {
      const auto *Header = (*Object)->getCOFFSection(Section);
      if (EntryRVA >= Header->VirtualAddress &&
          EntryRVA - Header->VirtualAddress < Header->SizeOfRawData)
        EntrySection = Header;
    }
    if (!EntrySection ||
        Code.size() > EntrySection->SizeOfRawData -
                          (EntryRVA - EntrySection->VirtualAddress)) {
      ADD_FAILURE() << "fixture entry has insufficient section bytes";
      return {};
    }
    // File padding alone is not mapped executable code. Extend this fixture's
    // existing text extent to cover the complete independently authored body.
    const auto HeaderOffset =
        reinterpret_cast<const char *>(EntrySection) - Data.data();
    const uint32_t Extent =
        EntryRVA - EntrySection->VirtualAddress + Code.size();
    llvm::support::endian::write32le(
        Bytes.data() + HeaderOffset +
            offsetof(llvm::object::coff_section, VirtualSize),
        std::max(uint32_t(EntrySection->VirtualSize), Extent));
    std::copy(Code.begin(), Code.end(), Bytes.begin() + Offset);
    const auto Path = Directory / "shadow-read.sys";
    std::ofstream Out(Path, std::ios::binary);
    Out.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    Out.close();
    EXPECT_TRUE(Out);
    return Path;
  }
  struct Program {
    std::vector<uint8_t> Code;
    std::vector<uint64_t> Watches;
  };
  Program program() {
    Program P;
    for (uint8_t Rex : {0x48, 0x49}) { // Seed RAX and R8, including high bits.
      P.Code.insert(P.Code.end(), {Rex, 0xb8});
      for (unsigned I = 0; I != 8; ++I)
        P.Code.push_back(uint8_t(Seed >> (I * 8)));
    }
    P.Code.push_back(0xf9); // STC.
    P.Watches.push_back(P.Code.size());
    for (bool Wide : {false, true})
      for (unsigned Register = 0; Register != 16; ++Register) {
        P.Code.push_back(0xf3);
        if (Wide || Register >= 8)
          P.Code.push_back(
              uint8_t(0x40 | (Wide ? 8 : 0) | (Register >= 8 ? 1 : 0)));
        P.Code.insert(P.Code.end(),
                      {0x0f, 0x1e, uint8_t(0xc8 | (Register & 7))});
      }
    P.Watches.push_back(P.Code.size());
    // Ordinary write after the reads must still execute exactly once.
    P.Code.insert(P.Code.end(), {0x48, 0x89, 0x44, 0x24, 8});
    P.Watches.push_back(P.Code.size());
    P.Code.insert(P.Code.end(), {0x31, 0xc0, 0xc3});
    return P;
  }
};

TEST_P(DriverShadowStack, PreservesAllRegistersFlagsAndMemory) {
  const auto Program = program();
  const auto Path = image(Program.Code);
  ASSERT_FALSE(HasFailure());
  for (uint64_t Quantum : {0u, 1u, 3u, 64u}) {
    SCOPED_TRACE(Quantum);
    Options.Scheduling.reset();
    if (Quantum)
      Options.Scheduling = DriverScheduling{Quantum, 7};
    // An advertised feature bit is not a request to enable architectural CET.
    Options.CPUID = {{7, 0, {0, 0, 1u << 7, 0}}};
    ShadowObserver Observer;
    Observer.Offsets = Program.Watches;
    auto Run = observeDriver(Path, Options, Observer);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    ASSERT_EQ(Run->Stop, DriverStopReason::Returned) << Run->Diagnostic;
    EXPECT_EQ(Run->NTStatus, 0u);
    EXPECT_EQ(Run->Instructions, 38u);
    ASSERT_EQ(Observer.States.size(), 3u);
    const auto &Before = Observer.States[0];
    const auto &After = Observer.States[1];
    EXPECT_EQ(Before.Values[0], Seed);
    EXPECT_EQ(Before.Values[8], Seed);
    EXPECT_NE(Before.Values[16] & 1, 0u);
    EXPECT_EQ(After.Values, Before.Values);
    EXPECT_EQ(After.Stack, Before.Stack);
    EXPECT_EQ(Observer.States[2].Values, Before.Values);
    auto Expected = Before.Stack;
    llvm::support::endian::write64le(Expected.data() + 8, Seed);
    EXPECT_EQ(Observer.States[2].Stack, Expected);
    ASSERT_TRUE(Before.Runtime);
    ASSERT_TRUE(After.Runtime);
    const auto HasEffect = [](const auto &State) {
      const auto Reasons = State->additionalDependencyReasons();
      return std::find(Reasons.begin(), Reasons.end(),
                       "unreconstructed kernel or environment effects") !=
             Reasons.end();
    };
    EXPECT_FALSE(HasEffect(Before.Runtime));
    EXPECT_TRUE(After.Runtime->hasAdditionalDependencies());
    EXPECT_TRUE(HasEffect(After.Runtime));
    // The earlier owned snapshot must remain unchanged after all later reads.
    EXPECT_FALSE(HasEffect(Before.Runtime));
  }
}

TEST_P(DriverShadowStack, BudgetAndObserverStopBeforeUnadmittedEffects) {
  const auto Program = program();
  const auto Path = image(Program.Code);
  ASSERT_FALSE(HasFailure());
  Options.Scheduling = DriverScheduling{1, 7};
  for (uint64_t Limit : {3u, 4u, 35u, 37u, 38u}) {
    SCOPED_TRACE(Limit);
    Options.InstructionLimit = Limit;
    ShadowObserver Observer;
    Observer.Offsets = Program.Watches;
    auto Run = observeDriver(Path, Options, Observer);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    EXPECT_EQ(Run->Stop, Limit == 38 ? DriverStopReason::Returned
                                     : DriverStopReason::InstructionLimit)
        << Run->Diagnostic;
    EXPECT_EQ(Run->Instructions, Limit);
    if (Limit < 38)
      EXPECT_FALSE(Run->NTStatus);
    ASSERT_FALSE(Observer.States.empty());
    EXPECT_EQ(Observer.States.front().Values[0], Seed);
    if (Limit <= 35)
      EXPECT_TRUE(Run->Writes.empty());
  }
  Options.InstructionLimit = 38;
  ShadowObserver Observer;
  Observer.Offsets = Program.Watches;
  Observer.Stop = true;
  auto Run = observeDriver(Path, Options, Observer);
  ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
  EXPECT_EQ(Run->Stop, DriverStopReason::Observer) << Run->Diagnostic;
  EXPECT_EQ(Run->Instructions, 3u);
  EXPECT_FALSE(Run->NTStatus);
  ASSERT_EQ(Observer.States.size(), 1u);
  EXPECT_EQ(Observer.States.front().Values[0], Seed);
  EXPECT_TRUE(Run->Writes.empty());
}

TEST_P(DriverShadowStack, UnsupportedControlWritesRemainRejected) {
  for (const std::vector<uint8_t> Bytes :
       {std::vector<uint8_t>{0xf3, 0x48, 0x0f, 0xae, 0xe8}, // INCSSPQ.
        std::vector<uint8_t>{0x0f, 0x22, 0xe0},             // MOV CR4,RAX.
        std::vector<uint8_t>{0x0f, 0x30}}) {                // WRMSR.
    auto Code = Bytes;
    Code.insert(Code.end(), {0x31, 0xc0, 0xc3});
    const auto Path = image(Code);
    ASSERT_FALSE(HasFailure());
    auto Run = emulateDriver(Path, Options);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    EXPECT_EQ(Run->Stop, DriverStopReason::UnsupportedInstruction)
        << Run->Diagnostic;
    EXPECT_EQ(Run->Instructions, 0u);
    EXPECT_FALSE(Run->NTStatus);
    EXPECT_TRUE(Run->Writes.empty());
  }
}

std::string parameterName(const testing::TestParamInfo<Parameter> &P) {
  const auto &[Backend, Contract] = P.param;
  return std::string(executionBackendName(Backend)) +
         (Contract == ExecutionContract::Legacy ? "_driver" : "_checked");
}
INSTANTIATE_TEST_SUITE_P(
    Native, DriverShadowStack,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP),
                     testing::Values(ExecutionContract::Legacy,
                                     ExecutionContract::CheckedX64)),
    parameterName);
} // namespace
} // namespace neverd::emulation
