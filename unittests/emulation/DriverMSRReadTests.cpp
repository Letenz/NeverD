//===- DriverMSRReadTests.cpp - Declared driver MSR reads ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independently encoded MSR reads and strict declaration boundaries.
///
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"
#include "neverd/emulation/ProcessObserver.h"

#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <tuple>

namespace neverd::emulation {
namespace {
using Parameter = ExecutionBackendKind;
constexpr uint64_t OtherValue = 0x123456789abcdef0;

class MSRObserver final : public ProcessObserver {
public:
  std::vector<uint64_t> Offsets;
  std::vector<std::array<uint64_t, 17>> States;
  std::vector<ExecutionWatch> Watches;
  llvm::Expected<std::vector<ExecutionWatch>> started(ProcessView &P) override {
    for (uint64_t Offset : Offsets)
      Watches.push_back({P.inputModule()->Entry + Offset, 1});
    return Watches;
  }
  llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
  watched(ProcessView &P, uint64_t) override {
    constexpr X64Register Registers[] = {
        X64Register::AX,  X64Register::BX,  X64Register::CX,
        X64Register::DX,  X64Register::R8,  X64Register::FLAGS,
        X64Register::SP,  X64Register::SI,  X64Register::DI,
        X64Register::BP,  X64Register::R9,  X64Register::R10,
        X64Register::R11, X64Register::R12, X64Register::R13,
        X64Register::R14, X64Register::R15};
    std::array<uint64_t, 17> Values{};
    for (unsigned I = 0; I < Values.size(); ++I) {
      auto V = P.readRegister(cpuRegister(Registers[I]));
      if (!V)
        return V.takeError();
      Values[I] = (*V)[0];
    }
    States.push_back(Values);
    return Watches;
  }
};

class DriverMSRExecution : public testing::TestWithParam<Parameter> {
protected:
  std::filesystem::path Directory;
  DriverOptions Options;
  void SetUp() override {
    Options.Backend = GetParam();
    Options.Contract = ExecutionContract::CheckedX64;
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
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-driver-msr", Temporary));
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
    std::copy(Code.begin(), Code.end(), Bytes.begin() + Offset);
    const auto Path = Directory / "msr.sys";
    std::ofstream Out(Path, std::ios::binary);
    Out.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    Out.close();
    EXPECT_TRUE(Out);
    return Path;
  }
  std::vector<uint8_t> program(uint32_t Index = 0xc0000082) {
    std::vector<uint8_t> Code{0x53}; // Preserve the entry's nonvolatile RBX.
    auto Move = [&](uint8_t Rex, uint8_t Opcode, uint64_t Value) {
      Code.insert(Code.end(), {Rex, Opcode});
      const size_t At = Code.size();
      Code.resize(At + 8);
      llvm::support::endian::write64le(Code.data() + At, Value);
    };
    Move(0x48, 0xb8, 0xa5a5a5a5deadbeefULL);
    Move(0x48, 0xbb, UINT64_MAX);
    Move(0x48, 0xb9, 0x5a5a5a5a00000000ULL | Index);
    Move(0x48, 0xba, UINT64_MAX);
    Move(0x49, 0xb8, OtherValue);
    // STC; RDMSR; NOP; RDMSR; NOP; XOR EAX,EAX; POP RBX; RET.
    Code.insert(Code.end(), {0xf9, 0x0f, 0x32, 0x90, 0x0f, 0x32, 0x90, 0x31,
                             0xc0, 0x5b, 0xc3});
    return Code;
  }
};

TEST_P(DriverMSRExecution, DeclaredReadsPreserveStateAndRepeatExactly) {
  for (uint64_t Value : {0ULL, 0xffffffffffffffffULL, 0xfffff80081234567ULL,
                         0x00000000fedcba98ULL, 0x8765432100000000ULL}) {
    SCOPED_TRACE(Value);
    Options.MSRReads = {{0xc0000082, Value}};
    const auto Path = image(program());
    ASSERT_FALSE(HasFailure());
    MSRObserver Observer;
    Observer.Offsets = {52, 54, 55, 57};
    auto Run = observeDriver(Path, Options, Observer);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    EXPECT_EQ(Run->Stop, DriverStopReason::Returned) << Run->Diagnostic;
    EXPECT_EQ(Run->Instructions, 14u);
    EXPECT_TRUE(Run->Writes.empty());
    ASSERT_EQ(Observer.States.size(), 4u);
    auto Expected = Observer.States[0];
    Expected[0] = uint32_t(Value);
    Expected[3] = Value >> 32;
    for (size_t I = 1; I != 4; ++I)
      EXPECT_EQ(Observer.States[I], Expected);
    EXPECT_EQ(Expected[2], 0x5a5a5a5ac0000082ULL);
    EXPECT_EQ(Expected[4], OtherValue);
    EXPECT_NE(Expected[5] & 1, 0u);
  }
}

TEST_P(DriverMSRExecution, MissingAndUnsupportedSelectorsStopBeforeResults) {
  for (uint32_t Index : {0xc0000082u, 0xc0000081u, 0xc0000101u, 0u}) {
    Options.MSRReads = Index == 0xc0000082
                           ? std::vector<DriverMSRRead>{}
                           : std::vector<DriverMSRRead>{{0xc0000082, 42}};
    const auto Path = image(program(Index));
    ASSERT_FALSE(HasFailure());
    MSRObserver Observer;
    Observer.Offsets = {52, 54};
    auto Run = observeDriver(Path, Options, Observer);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    EXPECT_EQ(Run->Stop, DriverStopReason::UnsupportedInstruction);
    EXPECT_NE(Run->Diagnostic.find(Index == 0xc0000082
                                       ? "undeclared selector"
                                       : "unsupported selector"),
              std::string::npos);
    ASSERT_EQ(Observer.States.size(), 1u);
    EXPECT_EQ(Observer.States[0][0], 0xa5a5a5a5deadbeefULL);
    EXPECT_EQ(Observer.States[0][3], UINT64_MAX);
    EXPECT_TRUE(Run->Writes.empty());
  }
}

TEST_P(DriverMSRExecution, InstructionBudgetsBoundEveryRead) {
  Options.MSRReads = {{0xc0000082, 0xabcdef0176543210ULL}};
  const auto Path = image(program());
  ASSERT_FALSE(HasFailure());
  for (uint64_t Limit : {7u, 8u, 9u, 10u}) {
    Options.InstructionLimit = Limit;
    MSRObserver Observer;
    Observer.Offsets = {52, 54, 55, 57};
    auto Run = observeDriver(Path, Options, Observer);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    EXPECT_EQ(Run->Stop, DriverStopReason::InstructionLimit);
    EXPECT_EQ(Run->Instructions, Limit);
    ASSERT_EQ(Observer.States.size(), Limit - 6);
    if (Limit > 7) {
      EXPECT_EQ(Observer.States[1][0], 0x76543210u);
      EXPECT_EQ(Observer.States[1][3], 0xabcdef01u);
    }
  }
}

TEST_P(DriverMSRExecution, DeclarationDoesNotAdmitWritesOrPrefixedForms) {
  Options.MSRReads = {{0xc0000082, 42}};
  for (auto Instruction : {std::vector<uint8_t>{0x0f, 0x30},
                           std::vector<uint8_t>{0xf0, 0x0f, 0x32},
                           std::vector<uint8_t>{0x66, 0x0f, 0x32}}) {
    auto Code = program();
    Code.erase(Code.begin() + 52, Code.begin() + 54);
    Code.insert(Code.begin() + 52, Instruction.begin(), Instruction.end());
    const auto Path = image(Code);
    ASSERT_FALSE(HasFailure());
    auto Run = emulateDriver(Path, Options);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    EXPECT_EQ(Run->Stop, DriverStopReason::UnsupportedInstruction)
        << Run->Diagnostic;
    EXPECT_TRUE(Run->Writes.empty());
  }
}

TEST_P(DriverMSRExecution, DeclaredAddressDoesNotCreateGuestMemory) {
  Options.MSRReads = {{0xc0000082, 0x50000000}};
  auto Code = program();
  // Replace the first post-read NOP with a load from the returned low word.
  Code.erase(Code.begin() + 54);
  Code.insert(Code.begin() + 54, {0x48, 0x8b, 0x00});
  const auto Path = image(Code);
  ASSERT_FALSE(HasFailure());
  auto Run = emulateDriver(Path, Options);
  ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
  EXPECT_EQ(Run->Stop, DriverStopReason::MemoryFault) << Run->Diagnostic;
  EXPECT_FALSE(Run->NTStatus.has_value());
}

TEST(DriverMSRDeclaration, RoundTripsFullWidthValuesAndCanBeCleared) {
  for (uint64_t Value : {0ULL, 0xffffffffffffffffULL, 0xfffff80081234567ULL}) {
    const auto Text =
        std::string(R"({"msr_reads":[{"index":3221225602,"value":")") +
        llvm::formatv("{0:x}", Value).str() + "\"}]}";
    auto Parsed = driverOptionsFromScenarioJSON(Text);
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
    ASSERT_EQ(Parsed->MSRReads.size(), 1u);
    EXPECT_EQ(Parsed->MSRReads[0].Index, 0xc0000082u);
    EXPECT_EQ(Parsed->MSRReads[0].Value, Value);
    auto Inherited = driverOptionsFromScenarioJSON("{}", *Parsed);
    ASSERT_TRUE(bool(Inherited));
    DriverResult Result;
    Result.Configuration = *Inherited;
    auto JSON = llvm::cantFail(llvm::json::parse(driverResultJSON(Result)));
    const auto *Entries =
        JSON.getAsObject()->getObject("configuration")->get("msr_reads");
    ASSERT_NE(Entries, nullptr);
    auto RoundTrip = driverOptionsFromScenarioJSON(
        "{\"msr_reads\":" + llvm::formatv("{0}", *Entries).str() + "}");
    ASSERT_TRUE(bool(RoundTrip));
    ASSERT_EQ(RoundTrip->MSRReads.size(), 1u);
    EXPECT_EQ(RoundTrip->MSRReads[0].Value, Value);
    auto Cleared =
        driverOptionsFromScenarioJSON(R"({"msr_reads":[]})", *Parsed);
    ASSERT_TRUE(bool(Cleared));
    EXPECT_TRUE(Cleared->MSRReads.empty());
  }
  // JSON numeric spellings must retain the same exact unsigned value.
  for (const char *Index : {"3221225602.0", "3.221225602e9"}) {
    auto Parsed = driverOptionsFromScenarioJSON(
        std::string("{\"msr_reads\":[{\"index\":") + Index +
        ",\"value\":\"0x0\"}]}");
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
    ASSERT_EQ(Parsed->MSRReads.size(), 1u);
    EXPECT_EQ(Parsed->MSRReads[0].Index, 0xc0000082u);
  }
}

TEST(DriverMSRDeclaration, RejectsMalformedDuplicateAndUnownedInputs) {
  for (
      const char *Entries :
      {"null",
       "false",
       "{}",
       "[null]",
       "[{}]",
       R"([{"index":3221225602}])",
       R"([{"index":3221225602,"value":0}])",
       R"([{"index":3221225602,"value":null}])",
       R"([{"index":3221225602,"value":"0x"}])",
       R"([{"index":3221225602,"value":"1"}])",
       R"([{"index":3221225602,"value":"0x-1"}])",
       R"([{"index":3221225602,"value":"0x10000000000000000"}])",
       R"([{"index":3221225602,"value":"0xg"}])",
       R"([{"index":3221225602,"value":"0x0","extra":0}])",
       R"([{"index":3221225602,"value":"0x0","value":"0x1"}])",
       R"([{"index":3221225602,"value":"0x0"},{"index":3221225602,"value":"0x0"}])",
       R"([{"index":3221225602.5,"value":"0x0"}])",
       R"([{"index":-1,"value":"0x0"}])",
       R"([{"index":4294967296,"value":"0x0"}])",
       R"([{"index":true,"value":"0x0"}])",
       R"([{"index":3221225729,"value":"0x0"}])"}) {
    auto Parsed = driverOptionsFromScenarioJSON(std::string("{\"msr_reads\":") +
                                                Entries + "}");
    ASSERT_FALSE(bool(Parsed)) << Entries;
    llvm::consumeError(Parsed.takeError());
  }
  for (auto Entries :
       {std::vector<DriverMSRRead>{{0, 0}},
        std::vector<DriverMSRRead>{{0xc0000082, 0}, {0xc0000082, 1}},
        std::vector<DriverMSRRead>(profile::MaxMSRReadEntries + 1)}) {
    DriverOptions Options;
    Options.Contract = ExecutionContract::CheckedX64;
    Options.MSRReads = std::move(Entries);
    auto Run = emulateDriver("absent.sys", Options);
    ASSERT_FALSE(bool(Run));
    EXPECT_NE(llvm::toString(Run.takeError()).find("driver msr_reads:"),
              std::string::npos);
  }
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::DirectDriverX64}) {
    DriverOptions Options;
    Options.Contract = Contract;
    Options.TraceMemoryWrites = false;
    Options.MSRReads = {{0xc0000082, 0}};
    auto Run = emulateDriver("absent.sys", Options);
    ASSERT_FALSE(bool(Run));
    EXPECT_EQ(llvm::toString(Run.takeError()),
              "driver msr_reads requires checked-x64 execution");
  }
}

INSTANTIATE_TEST_SUITE_P(Native, DriverMSRExecution,
                         testing::Values(ExecutionBackendKind::Unicorn,
                                         ExecutionBackendKind::KVM,
                                         ExecutionBackendKind::WHP,
                                         ExecutionBackendKind::HVF),
                         [](const testing::TestParamInfo<Parameter> &P) {
                           return std::string(executionBackendName(P.param));
                         });
} // namespace
} // namespace neverd::emulation
