//===- AndroidThreadAttributeTests.cpp - Independent API 28 workloads -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "os/linux/android/AndroidInternal.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"

#include <array>
#include <tuple>

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000;
class AndroidThreadAttribute : public testing::TestWithParam<const char *> {
protected:
  std::filesystem::path Path;
  ProcessOptions Options;
  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Clang and ld.lld Android fixtures unavailable";
#else
    Path = std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
           (std::string(GetParam()) + ".so");
    Options.Backend = ExecutionBackendKind::Unicorn;
    ExecutionConfiguration Configuration;
    Configuration.Backend = Options.Backend;
    Configuration.Architecture = GuestArchitecture::AArch64;
    Configuration.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(Configuration);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      GTEST_SKIP() << Probe->Reason;
    Options.Android.emplace();
    Options.Android->Initialize = false;
    Options.Android->TraceLimit = 4096;
    Options.Android->Memory.push_back(
        {Buffer, 8192, std::vector<uint8_t>(8192, 0xa5), false});
    Options.Android->ReadMemory.push_back({Buffer, 512});
    Options.InstructionQuantum = 7;
    Options.Limits.Instructions = 500000;
#endif
  }
  ProcessResult run(llvm::StringRef Entry, std::vector<uint64_t> Arguments) {
    Options.Android->EntrySymbol = Entry.str();
    Options.Android->Arguments = std::move(Arguments);
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return {ProcessProfile::AndroidNativeAArch64,
              GuestArchitecture::AArch64,
              Options.Backend,
              {}};
    }
    return std::move(*R);
  }
  ProcessResult call(unsigned Operation, uint64_t Value = 0, uint64_t Extra = 0,
                     uint64_t Attribute = Buffer) {
    return run("thread_attribute_call", {Attribute, Operation, Value, Extra});
  }
  void returned(const ProcessResult &R, uint64_t Value = 0) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, Value);
  }
  std::vector<uint8_t> before() {
    const auto &B = Options.Android->Memory[0].Bytes;
    return {B.begin(), B.begin() + 512};
  }
  void word(unsigned Offset, unsigned Size, uint64_t Value) {
    auto *P = Options.Android->Memory[0].Bytes.data() + Offset;
    if (Size == 4)
      llvm::support::endian::write32le(P, Value);
    else
      llvm::support::endian::write64le(P, Value);
  }
  void equal(const ProcessResult &R, const std::vector<uint8_t> &Expected) {
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  }
};
TEST_P(AndroidThreadAttribute,
       InitializationPreservesPaddingAndDestroyFillsWholeObject) {
  auto R = call(0);
  returned(R);
  auto Expected = before();
  std::fill(Expected.begin(), Expected.begin() + 4, 0);
  std::fill(Expected.begin() + 8, Expected.begin() + 40, 0);
  llvm::support::endian::write64le(Expected.data() + 16, 0xfc000);
  llvm::support::endian::write64le(Expected.data() + 24, 4096);
  equal(R, Expected);
  R = call(1);
  returned(R);
  Expected = before();
  std::fill(Expected.begin(), Expected.begin() + 56, 0x42);
  equal(R, Expected);
}
TEST_P(AndroidThreadAttribute, SettersKeepOtherFieldsAndAcceptFullWidthValues) {
  for (auto [Operation, Input, Offset, Size, ExpectedValue] :
       {std::tuple<unsigned, uint64_t, unsigned, unsigned, uint64_t>{
            2, 0x100000000, 0, 4, 0xa5a5a5a4},
        {2, 0x100000001, 0, 4, 0xa5a5a5a5},
        {4, 0x100000000, 0, 4, 0xa5a5a5a9},
        {4, 0x100000001, 0, 4, 0xa5a5a5a5},
        {6, UINT64_MAX, 32, 4, UINT32_MAX},
        {6, 0x100000001, 32, 4, 1},
        {10, 16385, 16, 8, 16385},
        {10, UINT64_MAX, 16, 8, UINT64_MAX},
        {12, 3, 24, 8, 3},
        {12, UINT64_MAX, 24, 8, UINT64_MAX}}) {
    SCOPED_TRACE(Operation);
    auto R = call(Operation, Input);
    returned(R);
    auto Expected = before();
    if (Size == 4)
      llvm::support::endian::write32le(Expected.data() + Offset, ExpectedValue);
    else
      llvm::support::endian::write64le(Expected.data() + Offset, ExpectedValue);
    equal(R, Expected);
  }
  word(96, 4, 0xdeadbeef);
  auto R = call(8, Buffer + 96);
  returned(R);
  auto Expected = before();
  llvm::support::endian::write32le(Expected.data() + 36, 0xdeadbeef);
  equal(R, Expected);
}
TEST_P(AndroidThreadAttribute, GettersWriteOnlyTheirDeclaredWidth) {
  word(16, 8, 0xfedcba9876543210);
  word(24, 8, UINT64_MAX);
  word(32, 4, 0x81234567);
  word(36, 4, 0xffffffff);
  for (auto [Operation, Size, Value] :
       {std::tuple<unsigned, unsigned, uint64_t>{3, 4, 1},
        {5, 4, 1},
        {7, 4, 0x81234567},
        {9, 4, 0xffffffff},
        {11, 8, 0xfedcba9876543210},
        {13, 8, UINT64_MAX},
        {17, 4, 0}}) {
    auto R = call(Operation, Buffer + 96);
    returned(R);
    auto Expected = before();
    if (Size == 4)
      llvm::support::endian::write32le(Expected.data() + 96, Value);
    else
      llvm::support::endian::write64le(Expected.data() + 96, Value);
    equal(R, Expected);
  }
}
TEST_P(AndroidThreadAttribute, InheritFlagsOverrideHistoricalPolicyFallback) {
  for (auto [Flags, Policy, Inherit] :
       {std::tuple<uint32_t, uint32_t, uint32_t>{0, 0, 1},
        {0, 1, 0},
        {0, UINT32_MAX, 0},
        {8, 0, 0},
        {4, 1, 1},
        {12, 1, 1}}) {
    word(0, 4, Flags);
    word(32, 4, Policy);
    auto R = call(5, Buffer + 96);
    returned(R);
    auto Expected = before();
    llvm::support::endian::write32le(Expected.data() + 96, Inherit);
    equal(R, Expected);
  }
}
TEST_P(AndroidThreadAttribute,
       InvalidValuesAndScopesDoNotDereferenceAttributes) {
  for (auto [Op, V, Extra, Code] :
       {std::tuple<unsigned, uint64_t, uint64_t, unsigned>{2, 2, 0, 22},
        {4, UINT64_MAX, 0, 22},
        {10, 16383, 0, 22},
        {14, 0, 8192, 22},
        {14, 1, 16384, 22},
        {14, 0, 16385, 22},
        {16, 0x100000000, 0, 0},
        {16, 0x100000001, 0, 95},
        {16, 2, 0, 22}}) {
    auto R = call(Op, V, Extra, 1);
    returned(R, Code);
    equal(R, before());
  }
  auto R = call(17, Buffer + 96, 0, UINT64_MAX);
  returned(R);
  auto Expected = before();
  llvm::support::endian::write32le(Expected.data() + 96, 0);
  equal(R, Expected);
}
TEST_P(AndroidThreadAttribute,
       StackStoresOpaqueAddressesAndReadsAliasedOutputsInOrder) {
  for (uint64_t Base : {uint64_t(0), uint64_t(0xffff123456789000)}) {
    auto R = call(14, Base, 0x100000000);
    returned(R);
    auto Expected = before();
    llvm::support::endian::write64le(Expected.data() + 8, Base);
    llvm::support::endian::write64le(Expected.data() + 16, 0x100000000);
    equal(R, Expected);
  }
  word(8, 8, 0xffff123456789000);
  word(16, 8, 16384);
  auto R = call(15, Buffer + 16, Buffer + 96);
  returned(R);
  auto Expected = before();
  llvm::support::endian::write64le(Expected.data() + 16, 0xffff123456789000);
  llvm::support::endian::write64le(Expected.data() + 96, 0xffff123456789000);
  equal(R, Expected);
  R = call(15, Buffer + 96, Buffer + 96);
  returned(R);
  Expected = before();
  llvm::support::endian::write64le(Expected.data() + 96, 16384);
  equal(R, Expected);
}
TEST_P(AndroidThreadAttribute, BadPointersFailWithoutInventedPthreadErrors) {
  for (auto [Op, V, Extra, Attr] :
       {std::tuple<unsigned, uint64_t, uint64_t, uint64_t>{0, 0, 0, Buffer + 4},
        {6, 0, 0, UINT64_MAX - 7},
        {3, Buffer + 97, 0, Buffer},
        {8, 1, 0, Buffer},
        {13, 1, 0, Buffer},
        {17, 1, 0, 1},
        {15, Buffer + 96, 1, Buffer}}) {
    auto R = call(Op, V, Extra, Attr);
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_FALSE(R.NativeCalls.back().Result);
    EXPECT_FALSE(R.ReturnValue);
  }
}
TEST_P(AndroidThreadAttribute, UnusedTailAndConditionalPolicyAreNotAccessed) {
  Options.Android->ReadMemory = {{Buffer + 4096 - 40, 56}};
  auto R =
      run("thread_attribute_readonly", {Buffer + 4096 - 40, 0, Buffer + 4096});
  returned(R);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(std::vector<uint8_t>(R.MemorySnapshots[0].Bytes.begin() + 40,
                                 R.MemorySnapshots[0].Bytes.end()),
            std::vector<uint8_t>(16, 0xa5));
  EXPECT_EQ(
      run("thread_attribute_readonly", {Buffer + 4096 - 40, 1, Buffer + 4096})
          .Stop,
      ProcessStopReason::RuntimeFailure);
  // An explicit flag avoids reading the unmapped historical policy field.
  Options.Android->Memory[0].Size = 4096;
  Options.Android->Memory[0].Bytes.resize(4096);
  Options.Android->ReadMemory = {{Buffer + 96, 8}};
  word(4088, 4, 8);
  R = call(5, Buffer + 96, 0, Buffer + 4088);
  returned(R);
  word(4088, 4, 0);
  EXPECT_EQ(call(5, Buffer + 96, 0, Buffer + 4088).Stop,
            ProcessStopReason::RuntimeFailure);
}
TEST_P(AndroidThreadAttribute, NamedDynamicCallsRetainProviderLifetime) {
  Options.Android->Libraries["libthread-model.so"] = {
      "pthread_attr_init", "pthread_attr_getstacksize"};
  auto R = run("thread_attribute_dynamic", {Buffer, 0});
  returned(R);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  const auto &Bytes = R.MemorySnapshots[0].Bytes;
  EXPECT_EQ(llvm::support::endian::read64le(Bytes.data() + 16), 0xfc000u);
  unsigned Count = 0;
  for (const auto &Call : R.NativeCalls)
    if (llvm::StringRef(Call.Name).starts_with("pthread_attr_")) {
      ++Count;
      EXPECT_EQ(Call.Library, "libthread-model.so");
      EXPECT_EQ(Call.PC,
                llvm::support::endian::read64le(
                    Bytes.data() + (Call.Name == "pthread_attr_init" ? 0 : 8)));
      EXPECT_EQ(Call.Result, 0u);
    }
  EXPECT_EQ(Count, 2u);
  R = run("thread_attribute_dynamic", {Buffer, 1});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "pthread_attr_init");
  EXPECT_FALSE(R.NativeCalls.back().Result);
}
TEST_P(AndroidThreadAttribute,
       ThreadCreationAndThreadQueriesRemainUnsupported) {
  for (unsigned Operation : {18, 19, 20}) {
    auto R = call(Operation, Buffer + 96);
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_FALSE(R.NativeCalls.back().Result);
    equal(R, before());
  }
}
INSTANTIATE_TEST_SUITE_P(OptimizationAndPacking, AndroidThreadAttribute,
                         testing::Values("thread-attributes-O0-none",
                                         "thread-attributes-O0-android",
                                         "thread-attributes-O0-relr",
                                         "thread-attributes-O2-none",
                                         "thread-attributes-O2-android",
                                         "thread-attributes-O2-relr"));
TEST(AndroidThreadAttributeMemory,
     RejectedWritesDoNotPartlyInitializeOrPublishAnOutput) {
  ExecutionConfiguration Configuration;
  Configuration.Backend = ExecutionBackendKind::Unicorn;
  Configuration.Architecture = GuestArchitecture::AArch64;
  Configuration.Contract = ExecutionContract::CheckedUserAArch64;
  auto Probe = probeExecutionBackend(Configuration);
  ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
  if (Probe->Availability != BackendAvailability::Available)
    GTEST_SKIP() << Probe->Reason;
  auto Backend = createExecutionBackend(Configuration, 4 * 1024 * 1024);
  ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
  auto &CPU = *Backend->CPU;
  auto &Space = *CPU.addressSpace();
  ASSERT_EQ(
      llvm::toString(Space.map(Buffer, 8192, Read | Write | UserAccessible)),
      "");
  std::array<uint8_t, 8192> Before;
  Before.fill(0xa5);
  ASSERT_EQ(llvm::toString(CPU.write(Buffer, Before)), "");
  ASSERT_EQ(
      llvm::toString(Space.protect(Buffer + 4096, 4096, Read | UserAccessible)),
      "");
  ProcessOptions Options;
  Options.Android.emplace();
  const linux_model::MemoryLayout Layout{linux_model::UserLimitARM64, 4096};
  linux_model::LinuxMemory Memory(Space, Layout, Buffer + 8192, Options);
  ProcessResult Result{ProcessProfile::AndroidNativeAArch64,
                       GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       {}};
  auto Budget = ExecutionBudget::create(Options.Limits);
  ASSERT_TRUE(bool(Budget)) << llvm::toString(Budget.takeError());
  const android_model::LinkedImage Linked{};
  android_model::Bionic Model(CPU, Memory, Layout, Options, Result, **Budget,
                              Linked);
  for (const char *Name :
       {"pthread_attr_init", "pthread_attr_destroy", "pthread_attr_getstack"}) {
    NativeCallEvent Call{};
    Call.Name = Name;
    Call.Arguments[0] = llvm::StringRef(Name) == "pthread_attr_getstack"
                            ? Buffer + 128
                            : Buffer + 4088;
    Call.Arguments[1] = Buffer + 96;
    Call.Arguments[2] = 0;
    auto Value = Model.invoke(Call);
    ASSERT_FALSE(bool(Value));
    EXPECT_NE(llvm::toString(Value.takeError()).find("invalid guest pointer"),
              std::string::npos);
    std::array<uint8_t, 8192> After{};
    ASSERT_EQ(llvm::toString(Space.snapshotBacking(Buffer, After)), "");
    EXPECT_EQ(After, Before);
  }
}
} // namespace
} // namespace neverd::emulation
