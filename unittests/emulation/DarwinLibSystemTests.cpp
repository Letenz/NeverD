//===- DarwinLibSystemTests.cpp - C call and kernel argument boundaries ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Optional mode carriers, defined stack bytes and failure atomicity.
#include "DarwinFileTestData.h"
#include "DarwinLibSystemFileTestData.h"
#include "gtest/gtest.h"
#include "os/darwin/kernel/DarwinEntropy.h"
#include "os/darwin/kernel/DarwinFiles.h"
#include "os/darwin/kernel/DarwinMemory.h"
#include "os/darwin/process/DarwinLibSystem.h"

namespace neverd::emulation::darwin_model {
namespace {
class LibSystemCPU final : public ExecutionBackend {
  std::shared_ptr<AddressSpace> Space;
  GuestArchitecture ISA;

public:
  std::map<CPURegister, RegisterValue> Registers;
  uint64_t ModeAddress = 0;
  bool FailMode = false;
  unsigned ModeReads = 0;
  LibSystemCPU(std::shared_ptr<AddressSpace> Space, GuestArchitecture ISA)
      : Space(std::move(Space)), ISA(ISA) {}
  GuestArchitecture architecture() const override { return ISA; }
  std::shared_ptr<AddressSpace> addressSpace() const override { return Space; }
  llvm::Error bindAddressSpace(std::shared_ptr<AddressSpace>) override {
    return failure("unexpected bind");
  }
  llvm::Expected<RegisterValue> readRegister(CPURegister R) override {
    if (ISA == GuestArchitecture::X64 && R == CPURegister::X64DX) {
      ++ModeReads;
      if (FailMode)
        return failure("open mode transport");
    }
    return Registers[R];
  }
  llvm::Error writeRegister(CPURegister R, const RegisterValue &V) override {
    Registers[R] = V;
    return llvm::Error::success();
  }
  llvm::Error map(uint64_t A, uint64_t N, unsigned P) override {
    return Space->map(A, N, P);
  }
  llvm::Error protect(uint64_t A, uint64_t N, unsigned P) override {
    return Space->protect(A, N, P);
  }
  llvm::Expected<bool> canAccess(uint64_t A, uint64_t N,
                                 unsigned P) const override {
    return Space->canAccess(A, N, P);
  }
  llvm::Error read(uint64_t A, llvm::MutableArrayRef<uint8_t> B) override {
    if (ISA == GuestArchitecture::AArch64 && A == ModeAddress) {
      ++ModeReads;
      if (FailMode)
        return failure("open mode transport");
      if (B.size() != 4)
        return failure("open read undefined variadic padding");
    }
    return Space->read(A, B);
  }
  llvm::Error write(uint64_t A, llvm::ArrayRef<uint8_t> B) override {
    return Space->write(A, B);
  }
  llvm::Error fetch(uint64_t, llvm::MutableArrayRef<uint8_t>) override {
    return failure("unexpected fetch");
  }
  llvm::Expected<std::unique_ptr<BackendContext>> saveContext() override {
    return failure("unexpected save");
  }
  llvm::Error saveContext(BackendContext &) override {
    return failure("unexpected save");
  }
  llvm::Error restoreContext(const BackendContext &) override {
    return failure("unexpected restore");
  }
  llvm::Error installHooks(BackendHooks) override {
    return failure("unexpected hooks");
  }
  bool timedOut() const override { return false; }
  void stop() override {}
  bool hasMemoryFault() const override { return false; }
  bool hasDeviceError() const override { return false; }
  std::optional<BackendFault> fault() const override { return std::nullopt; }
  std::optional<BackendFault> takeRecoverableFault() override {
    return std::nullopt;
  }
  bool executable(uint64_t) const override { return false; }
};

class DarwinLibSystemTest : public testing::TestWithParam<bool> {
protected:
  static constexpr uint64_t Data = 0x10000, Code = 0x20000, Stack = 0x30000;
  std::shared_ptr<AddressSpace> Space;
  std::unique_ptr<LibSystemCPU> CPU;
  std::unique_ptr<DarwinMemory> Memory;
  std::unique_ptr<DarwinFiles> Files;
  ProcessOptions Options;
  ProcessImage Image{};
  ProcessResult Result{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn, "C boundary"};
  uint64_t SP = 0, Errno = 0;
  void SetUp() override {
    const auto ISA =
        GetParam() ? GuestArchitecture::X64 : GuestArchitecture::AArch64;
    const uint64_t Page = GetParam() ? 4096 : 16384;
    auto Physical = PhysicalMemory::create(1 << 20);
    ASSERT_TRUE(bool(Physical));
    auto Created = AddressSpace::create(*Physical, 1 << 20);
    ASSERT_TRUE(bool(Created));
    Space = *Created;
    Image.Architecture = ISA;
    Image.Memory = {Page, 0x100000000ULL, {}};
    Image.Imports[value::LibSystemGate] = LibSystemGateKind::Open;
    llvm::cantFail(prepareLibSystem(*Space, Image));
    Errno = value::LibSystemGate + Page;
    llvm::cantFail(Space->writeInteger(Errno, 71, 4));
    llvm::cantFail(Space->map(Data, Page, Read | Write | UserAccessible));
    llvm::cantFail(Space->map(Code, Page, Read | Execute | UserAccessible));
    llvm::cantFail(Space->map(Stack, Page, Read | Write | UserAccessible));
    const uint8_t Path[] = {'/', 'c', 'r', 'e', 'a', 't', 'e', 'd', 0};
    llvm::cantFail(Space->write(Data, Path));
    Options.DarwinFiles = darwin_test::libSystemFileOptions();
    Options.DarwinFiles->Metadata["/"] = darwin_test::creationParentMetadata();
    Options.DarwinFiles->CreationPolicy = darwin_test::CreationPolicy;
    Memory = std::make_unique<DarwinMemory>(*Space, Image.Memory, Options);
    Files = std::make_unique<DarwinFiles>(*Space, Options.DarwinFiles);
    CPU = std::make_unique<LibSystemCPU>(Space, ISA);
    SP = Stack + (GetParam() ? 8 : 0);
    auto ABI = llvm::cantFail(
        IntegerABI::get(GetParam() ? IntegerCallingConvention::SysVAMD64
                                   : IntegerCallingConvention::AAPCS64));
    CPU->Registers[ABI.info().StackPointer] = {SP, 0};
    CPU->Registers[ABI.info().Arguments[0]] = {Data, 0};
    CPU->Registers[ABI.info().Arguments[1]] = {0x1122334400000a02ULL, 0};
    CPU->Registers[ABI.info().Arguments[2]] = {
        GetParam() ? 0xaabbccdd123401a1ULL : 0x5566778899abcdefULL, 0};
    if (GetParam())
      llvm::cantFail(Space->writeInteger(SP, Code, 8));
    else {
      CPU->Registers[ABI.info().Link] = {Code, 0};
      CPU->ModeAddress = SP;
      llvm::cantFail(Space->writeInteger(SP, 0xdeadbeef123401a1ULL, 8));
    }
    Result.Architecture = ISA;
  }
  llvm::Expected<bool> invoke() {
    DarwinEntropy Entropy(Options.DarwinSystem);
    ServiceRequest Request{GetParam() ? ServiceRequestKind::X64Syscall
                                      : ServiceRequestKind::AArch64SVC,
                           value::LibSystemGate, 0,
                           uint16_t(value::LibSystemSVC)};
    return invokeLibSystem(*CPU, *Memory, *Files, Entropy, Image,
                           LibSystemGateKind::Open, Request, Options, Result);
  }
  void expectAbsent() {
    EXPECT_TRUE(Result.NativeCalls.empty());
    EXPECT_TRUE(Result.Services.empty());
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Errno, 4)), 71u);
    auto R = Files->handle(ServiceKind::Open, {0, 0, {Data, 0}, std::nullopt},
                           Result);
    ASSERT_TRUE(bool(R));
    ASSERT_TRUE(bool(*R));
    EXPECT_TRUE((**R).Error);
    EXPECT_EQ((**R).Value, 2u);
  }
};

TEST_P(DarwinLibSystemTest, OpenSeparatesModeCarrierFromKernelModeAndMetadata) {
  auto ABI = llvm::cantFail(
      IntegerABI::get(GetParam() ? IntegerCallingConvention::SysVAMD64
                                 : IntegerCallingConvention::AAPCS64));
  auto Arguments =
      readLibSystemArguments(*CPU, ABI, SP, LibSystemGateKind::Open);
  ASSERT_TRUE(bool(Arguments)) << llvm::toString(Arguments.takeError());
  EXPECT_EQ(Arguments->Count, 3u);
  const uint64_t Carrier = GetParam() ? 0xaabbccdd123401a1ULL : 0x123401a1ULL;
  EXPECT_EQ(Arguments->Carriers[2], Carrier);
  EXPECT_EQ(Arguments->Kernel[2], 0641u);
  EXPECT_EQ(Arguments->Kernel[1], 0x1122334400000a02ULL);
  EXPECT_EQ(CPU->ModeReads, 1u);
  CPU->ModeReads = 0;
  auto R = invoke();
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  ASSERT_TRUE(*R);
  EXPECT_EQ(CPU->ModeReads, 1u);
  ASSERT_EQ(Result.NativeCalls.size(), 1u);
  EXPECT_EQ(Result.NativeCalls[0].Arguments[2], Carrier);
  EXPECT_EQ(Result.NativeCalls[0].ArgumentCount, 3u);
  EXPECT_EQ(Result.NativeCalls[0].Result, 3u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Errno, 4)), 71u);
  auto Stat = Files->handle(ServiceKind::Fstat64,
                            {0, 0, {3, Data + 256}, std::nullopt}, Result);
  ASSERT_TRUE(bool(Stat));
  ASSERT_TRUE(bool(*Stat));
  EXPECT_FALSE((**Stat).Error);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Data + 260, 2)), 0100640u);
}
TEST_P(DarwinLibSystemTest, OpenWithoutCreateDoesNotReadOptionalStorage) {
  auto ABI = llvm::cantFail(
      IntegerABI::get(GetParam() ? IntegerCallingConvention::SysVAMD64
                                 : IntegerCallingConvention::AAPCS64));
  CPU->Registers[ABI.info().Arguments[1]] = {0x0000020000000000ULL, 0};
  CPU->FailMode = true;
  if (!GetParam())
    llvm::cantFail(Space->unmap(Stack, Image.Memory.PageSize));
  auto Arguments =
      readLibSystemArguments(*CPU, ABI, SP, LibSystemGateKind::Open);
  ASSERT_TRUE(bool(Arguments)) << llvm::toString(Arguments.takeError());
  EXPECT_EQ(Arguments->Count, 2u);
  EXPECT_EQ(Arguments->Kernel[2], 0u);
  auto R = invoke();
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_TRUE(*R);
  EXPECT_EQ(CPU->ModeReads, 0u);
  ASSERT_EQ(Result.NativeCalls.size(), 1u);
  EXPECT_EQ(Result.NativeCalls[0].ArgumentCount, 2u);
  EXPECT_EQ(Result.NativeCalls[0].Result, UINT64_MAX);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Errno, 4)), 2u);
}
TEST_P(DarwinLibSystemTest, OptionalModeTransportFailureCannotPublishEffects) {
  CPU->FailMode = true;
  auto R = invoke();
  ASSERT_FALSE(bool(R));
  EXPECT_EQ(llvm::toString(R.takeError()), "open mode transport");
  expectAbsent();
  CPU->FailMode = false;
  auto Retry = invoke();
  ASSERT_TRUE(bool(Retry)) << llvm::toString(Retry.takeError());
  ASSERT_TRUE(*Retry);
  ASSERT_EQ(Result.NativeCalls.size(), 1u);
  EXPECT_EQ(Result.NativeCalls.front().Result, 3u);
}
TEST_P(DarwinLibSystemTest, MissingStackBytesKeepTheirDistinctCallBoundary) {
  llvm::cantFail(Space->unmap(Stack, Image.Memory.PageSize));
  auto R = invoke();
  ASSERT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  if (!GetParam())
    expectAbsent(); // Required ARM64 mode was unavailable before call entry.
  else {
    ASSERT_EQ(Result.NativeCalls.size(), 1u); // Required return address failed.
    EXPECT_FALSE(Result.NativeCalls[0].Result);
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Errno, 4)), 71u);
  }
}
INSTANTIATE_TEST_SUITE_P(Architectures, DarwinLibSystemTest,
                         testing::Values(false, true));
} // namespace
} // namespace neverd::emulation::darwin_model
