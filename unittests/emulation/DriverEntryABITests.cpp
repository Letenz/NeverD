//===- DriverEntryABITests.cpp - Loader caller preservation controls -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../lib/emulation/os/windows/driver/DriverEntryABI.h"
#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include <cctype>
#include <memory>

namespace neverd::emulation {
namespace {
class DriverEntryCaller : public testing::TestWithParam<ExecutionBackendKind> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  void SetUp() override {
    auto B = createExecutionBackend(GetParam(), ExecutionContract::CheckedX64,
                                    profile::DefaultMemoryLimit);
    if (!B) {
      auto E = B.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Reason = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(B->CPU);
  }
  void write(CPURegister R, RegisterValue Value) {
    auto E = CPU->writeRegister(R, Value);
    ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  }
  void preserved(const DriverEntryABI &Caller) {
    auto E = Caller.validateReturn(*CPU);
    EXPECT_FALSE(bool(E)) << llvm::toString(std::move(E));
  }
  void changed(const DriverEntryABI &Caller, const char *Name) {
    auto E = Caller.validateReturn(*CPU);
    EXPECT_TRUE(bool(E));
    EXPECT_NE(llvm::toString(std::move(E)).find(Name), std::string::npos);
  }
};

TEST_P(DriverEntryCaller, NonzeroFreshCallersCannotBeReplacedByZeroSeedValues) {
  constexpr CPURegister Registers[] = {
      CPURegister::X64BX,  CPURegister::X64BP,  CPURegister::X64SI,
      CPURegister::X64DI,  CPURegister::X64R12, CPURegister::X64R13,
      CPURegister::X64R14, CPURegister::X64R15};
  for (uint64_t Seed :
       {uint64_t(1), uint64_t(0xfedcba9876543210), UINT64_MAX}) {
    for (const auto R : Registers)
      write(R, {Seed, 0});
    auto Caller = DriverEntryABI::capture(*CPU);
    ASSERT_TRUE(bool(Caller)) << llvm::toString(Caller.takeError());
    preserved(*Caller);
    for (const auto R : Registers) {
      SCOPED_TRACE(unsigned(R));
      write(R, {0, 0});
      changed(*Caller, "nonvolatile");
      write(R, {Seed, 0});
      preserved(*Caller);
    }
  }
}

TEST_P(DriverEntryCaller, BothHalvesOfEveryNonvolatileXmmRemainLive) {
  for (unsigned Index = 6; Index < 16; ++Index) {
    const auto R = vectorRegister(GuestArchitecture::X64, Index);
    const RegisterValue Seed{0x1122334455667788ULL + Index,
                             0x8877665544332211ULL + Index};
    write(R, Seed);
    auto Caller = DriverEntryABI::capture(*CPU);
    ASSERT_TRUE(bool(Caller)) << llvm::toString(Caller.takeError());
    for (size_t Lane = 0; Lane < 2; ++Lane) {
      SCOPED_TRACE(Index);
      SCOPED_TRACE(Lane);
      auto Value = Seed;
      Value[Lane] ^= uint64_t(1) << 63;
      write(R, Value);
      changed(*Caller, "xmm");
      write(R, Seed);
      preserved(*Caller);
    }
  }
}

TEST_P(DriverEntryCaller,
       VolatileRegistersAndMxcsrStatusDoNotRejectAValidReturn) {
  write(CPURegister::X64MXCSR, {0x1f80, 0});
  auto Caller = DriverEntryABI::capture(*CPU);
  ASSERT_TRUE(bool(Caller)) << llvm::toString(Caller.takeError());
  for (auto R : {CPURegister::X64AX, CPURegister::X64CX, CPURegister::X64DX,
                 CPURegister::X64R8, CPURegister::X64R9, CPURegister::X64R10,
                 CPURegister::X64R11})
    write(R, {0x123456789abcdef0, 0});
  for (unsigned Index = 0; Index < 6; ++Index)
    write(vectorRegister(GuestArchitecture::X64, Index), {UINT64_MAX, 42});
  write(CPURegister::X64MXCSR, {0x1fbf, 0});
  preserved(*Caller);
  write(CPURegister::X64MXCSR, {0x3fbf, 0});
  changed(*Caller, "mxcsr");
}

TEST_P(DriverEntryCaller, FloatingControlAndDirectionFlagMustBeRestored) {
  write(CPURegister::X64FPCW, {0x37f, 0});
  write(CPURegister::X64FLAGS, {0x202, 0});
  auto Caller = DriverEntryABI::capture(*CPU);
  ASSERT_TRUE(bool(Caller)) << llvm::toString(Caller.takeError());
  write(CPURegister::X64FPCW, {0x77f, 0});
  changed(*Caller, "fpcw");
  write(CPURegister::X64FPCW, {0x37f, 0});
  write(CPURegister::X64FLAGS, {0x246, 0}); // Arithmetic flags are volatile.
  preserved(*Caller);
  write(CPURegister::X64FLAGS, {0x646, 0});
  changed(*Caller, "direction flag");
}

INSTANTIATE_TEST_SUITE_P(Backends, DriverEntryCaller,
                         testing::Values(ExecutionBackendKind::Unicorn,
                                         ExecutionBackendKind::KVM,
                                         ExecutionBackendKind::WHP),
                         [](const auto &P) {
                           std::string Name = executionBackendName(P.param);
                           Name[0] = std::toupper(
                               static_cast<unsigned char>(Name[0]));
                           return Name;
                         });
} // namespace
} // namespace neverd::emulation
