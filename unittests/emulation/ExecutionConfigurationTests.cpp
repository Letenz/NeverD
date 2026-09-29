//===- ExecutionConfigurationTests.cpp - CPU requirements and live probes ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation {
namespace {
#define NEVERD_CONFIGURATION_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_CONFIGURATION_X64(Name, ...)                                    \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#define NEVERD_CONFIGURATION_ARM(Name, ...)                                    \
  constexpr uint32_t Name[] = {__VA_ARGS__};
#include "ExecutionConfigurationCases.def"
#undef NEVERD_CONFIGURATION_ARM
#undef NEVERD_CONFIGURATION_X64
#undef NEVERD_CONFIGURATION_VALUE

ExecutionConfiguration checked(GuestArchitecture ISA) {
  ExecutionConfiguration Config;
  Config.Backend = ExecutionBackendKind::Unicorn;
  Config.Architecture = ISA;
  Config.Contract = ISA == GuestArchitecture::X64
                        ? ExecutionContract::CheckedX64
                        : ExecutionContract::CheckedAArch64;
  return Config;
}
TEST(ExecutionConfiguration, UnsupportedRequirementsDoNotChangeAddressSpace) {
  auto RAM = llvm::cantFail(PhysicalMemory::create(MemoryLimit));
  auto Space = llvm::cantFail(AddressSpace::create(RAM, MemoryLimit));
  ASSERT_EQ(llvm::toString(Space->map(Code, PageSize, Read | Write)), "");
  const auto Generation = Space->mappingGeneration();
  for (auto ISA : {GuestArchitecture::X64, GuestArchitecture::AArch64}) {
#define NEVERD_CONFIGURATION_REJECTION(Name, Field, Value)                     \
  {                                                                            \
    auto Config = checked(ISA);                                                \
    Config.Field = Value;                                                      \
    auto Result = createExecutionBackend(Config, Space);                       \
    ASSERT_FALSE(bool(Result));                                                \
    auto E = Result.takeError();                                               \
    EXPECT_FALSE(E.isA<BackendUnavailableError>());                            \
    llvm::consumeError(std::move(E));                                          \
    EXPECT_EQ(RAM->allocatedBytes(), PageSize);                                \
    EXPECT_EQ(Space->mappedBytes(), PageSize);                                 \
    EXPECT_EQ(Space->mappingGeneration(), Generation);                         \
  }
#include "ExecutionConfigurationCases.def"
#undef NEVERD_CONFIGURATION_REJECTION
  }
}
TEST(ExecutionConfiguration,
     RegisterStorageDoesNotAdvertiseInstructionSupport) {
  for (auto ISA : {GuestArchitecture::X64, GuestArchitecture::AArch64}) {
    const auto Config = checked(ISA);
    auto Capabilities =
        llvm::cantFail(executionCapabilities(Config.Contract, ISA));
    EXPECT_TRUE(Capabilities.supports(ExecutionFeature::VectorRegisterState));
    EXPECT_FALSE(Capabilities.supports(ExecutionFeature::FloatingPoint));
    EXPECT_FALSE(Capabilities.supports(ExecutionFeature::SIMD));
    EXPECT_TRUE(Capabilities.supports(ExecutionFeature::CooperativeCPUs));
    EXPECT_FALSE(Capabilities.supports(ExecutionFeature::ParallelCPUs));
    EXPECT_TRUE(Capabilities.HasInstructionAllowlist);
    EXPECT_FALSE(Capabilities.InstructionFamilies.empty());
    EXPECT_EQ(Capabilities.MemoryObservation,
              ExecutionMemoryObservation::InstructionPreflight);
    EXPECT_EQ(Capabilities.ControlPrecision,
              ExecutionControlPrecision::InstructionBoundary);
    EXPECT_FALSE(Capabilities.HardWallClockBound);
    auto Resolved = llvm::cantFail(resolveExecutionConfiguration(Config));
    EXPECT_EQ(Resolved.Configuration.Privilege, ExecutionPrivilege::Supervisor);
    EXPECT_EQ(Resolved.Configuration.PageSize, Capabilities.PageSize);
    EXPECT_EQ(Resolved.Configuration.VirtualAddressBits,
              Capabilities.VirtualAddressBits);
  }
}
TEST(ExecutionConfiguration,
     UnknownISAAndMismatchedContractAreConfigurationErrors) {
  auto Config = checked(GuestArchitecture::X64);
  Config.Architecture = GuestArchitecture::AArch64;
  auto Mismatch = probeExecutionBackend(Config);
  ASSERT_FALSE(bool(Mismatch));
  llvm::consumeError(Mismatch.takeError());
  Config.Architecture = static_cast<GuestArchitecture>(-1);
  auto Unknown = resolveExecutionConfiguration(Config);
  ASSERT_FALSE(bool(Unknown));
  llvm::consumeError(Unknown.takeError());
}
TEST(ExecutionConfiguration,
     StaticSupportIsSeparateFromHostPlatformAvailability) {
  auto Config = checked(GuestArchitecture::X64);
#if defined(_WIN32)
  Config.Backend = ExecutionBackendKind::KVM;
#else
  Config.Backend = ExecutionBackendKind::WHP;
#endif
  auto Resolved = resolveExecutionConfiguration(Config);
  ASSERT_TRUE(bool(Resolved));
  EXPECT_TRUE(Resolved->Capabilities.SupportsNativeExecution);
  auto Probe = probeExecutionBackend(Config);
  ASSERT_TRUE(bool(Probe));
  EXPECT_EQ(Probe->Availability, BackendAvailability::HostPlatformMismatch);
  auto CPU = createExecutionBackend(Config, MemoryLimit);
  ASSERT_FALSE(bool(CPU));
  llvm::handleAllErrors(CPU.takeError(), [&](const BackendUnavailableError &E) {
    EXPECT_EQ(E.availability(), Probe->Availability);
    EXPECT_EQ(E.reason(), Probe->Reason);
  });
}
TEST(ExecutionConfiguration,
     ReportedSoftwareMMIOExecutesThroughGuestInstructions) {
  for (auto ISA : {GuestArchitecture::X64, GuestArchitecture::AArch64}) {
    ExecutionConfiguration Config;
    Config.Backend = ExecutionBackendKind::Unicorn;
    Config.Architecture = ISA;
    Config.RequiredFeatures = ExecutionFeature::MMIO;
    const auto Caps =
        llvm::cantFail(executionCapabilities(Config.Contract, ISA));
    EXPECT_TRUE(Caps.supports(Config.RequiredFeatures));
    EXPECT_FALSE(Caps.SupportsNativeExecution);
    EXPECT_FALSE(Caps.HasInstructionAllowlist);
    EXPECT_EQ(Caps.MemoryObservation,
              ExecutionMemoryObservation::EngineCallbacks);
    EXPECT_EQ(Caps.ControlPrecision, ExecutionControlPrecision::EngineRequest);
    EXPECT_FALSE(Caps.HardWallClockBound);
    auto Probe = probeExecutionBackend(Config);
    ASSERT_TRUE(bool(Probe));
#ifndef NEVERD_TEST_UNICORN
    EXPECT_EQ(Probe->Availability, BackendAvailability::BuildDisabled);
    GTEST_SKIP() << Probe->Reason;
#else
    ASSERT_EQ(Probe->Availability, BackendAvailability::Available)
        << Probe->Reason;
#endif
    auto Created = createExecutionBackend(Config, MemoryLimit);
    ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
    auto &CPU = *Created->CPU;
    unsigned DeviceReads = 0;
    GuestMMIOCallbacks IO;
    IO.Validate = [](uint64_t, uint64_t, bool) {
      return llvm::Error::success();
    };
    IO.Read = [&](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
      ++DeviceReads;
      return DeviceValue;
    };
    IO.Write = [](uint64_t, unsigned, uint64_t) {
      return llvm::Error::success();
    };
    ASSERT_EQ(llvm::toString(CPU.map(Code, PageSize, Read | Write | Execute)),
              "");
    ASSERT_EQ(llvm::toString(CPU.mapMMIO(Device, PageSize, std::move(IO))), "");
    if (ISA == GuestArchitecture::X64) {
      ASSERT_EQ(llvm::toString(CPU.write(Code, LoadDeviceX64)), "");
      ASSERT_EQ(llvm::toString(CPU.setReg(X64Register::CX, Device)), "");
    } else {
      std::vector<uint8_t> Bytes(sizeof(LoadDeviceARM));
      for (size_t I = 0; I < std::size(LoadDeviceARM); ++I)
        llvm::support::endian::write32le(Bytes.data() + I * sizeof(uint32_t),
                                         LoadDeviceARM[I]);
      ASSERT_EQ(llvm::toString(CPU.write(Code, Bytes)), "");
      ASSERT_EQ(llvm::toString(CPU.setReg(AArch64Register::X1, Device)), "");
    }
    unsigned Instructions = 0;
    BackendHooks Hooks;
    Hooks.Instruction = [&](uint64_t, uint32_t) {
      if (Instructions++)
        CPU.stop();
    };
    ASSERT_EQ(llvm::toString(CPU.installHooks(std::move(Hooks))), "");
    ASSERT_EQ(llvm::toString(CPU.run(Code, Timeout)), "");
    EXPECT_FALSE(CPU.fault());
    const auto Result = llvm::cantFail(CPU.readRegister(
        ISA == GuestArchitecture::X64 ? CPURegister::X64AX
                                      : CPURegister::AArch64X0));
    EXPECT_EQ(Result[0], DeviceValue);
    EXPECT_EQ(DeviceReads, 1u);
  }
}
} // namespace
} // namespace neverd::emulation
