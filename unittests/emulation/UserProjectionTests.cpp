//===- UserProjectionTests.cpp - Privilege checks without CPU preflight --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "arch/aarch64/AArch64Machine.h"
#include "arch/x86_64/X64Machine.h"
#include "backends/MachineFactories.h"
#include "gtest/gtest.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation {
namespace {
#define NEVERD_USER_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_USER_X64(Name, ...) constexpr uint8_t Name[] = {__VA_ARGS__};
#define NEVERD_USER_ARM(Name, ...) constexpr uint32_t Name[] = {__VA_ARGS__};
#include "UserExecutionCases.def"
#undef NEVERD_USER_ARM
#undef NEVERD_USER_X64
#undef NEVERD_USER_VALUE
struct Profile {
  const char *Name;
  ExecutionBackendKind Backend;
  GuestArchitecture ISA;
};
void PrintTo(const Profile &P, std::ostream *OS) { *OS << P.Name; }
constexpr Profile Profiles[] = {
#define NEVERD_USER_PROFILE(Name, Backend, ISA, Contract)                      \
  {#Name, ExecutionBackendKind::Backend, GuestArchitecture::ISA},
#include "UserExecutionCases.def"
#undef NEVERD_USER_PROFILE
};
/// Intentionally bypasses CheckedBackend, decoder admission and access(). A
/// fault here must come from the architectural transport/page-table boundary.
class UserProjection : public testing::TestWithParam<Profile> {
protected:
  std::unique_ptr<MemoryProjection> Memory;
  std::unique_ptr<X64Machine> X64;
  std::unique_ptr<AArch64Machine> ARM;
  X64MachineState XS;
  AArch64MachineState AS;
  uint64_t Root = 0;
  llvm::Error initialize() {
    if (GetParam().ISA == GuestArchitecture::X64) {
      auto M = GetParam().Backend == ExecutionBackendKind::KVM
                   ? createKvmMachine(*Memory)
               : GetParam().Backend == ExecutionBackendKind::WHP
                   ? createWhpMachine(*Memory)
               : GetParam().Backend == ExecutionBackendKind::HVF
                   ? createHvfX64Machine(*Memory)
                   : createUnicornX64Machine(*Memory, true);
      if (!M)
        return M.takeError();
      X64 = std::move(*M);
    } else {
      auto M = GetParam().Backend == ExecutionBackendKind::KVM
                   ? createKvmAArch64Machine(*Memory)
               : GetParam().Backend == ExecutionBackendKind::WHP
                   ? createWhpAArch64Machine(*Memory)
               : GetParam().Backend == ExecutionBackendKind::HVF
                   ? createHvfAArch64Machine(*Memory)
                   : createUnicornAArch64Machine(*Memory, true);
      if (!M)
        return M.takeError();
      ARM = std::move(*M);
    }
    return llvm::Error::success();
  }
  void SetUp() override {
    Memory = llvm::cantFail(MemoryProjection::create(Limit));
    auto E = initialize();
    if (E) {
      const bool Unavailable = E.isA<BackendUnavailableError>();
      auto Reason = llvm::toString(std::move(E));
      if (Unavailable && !requireHvf(GetParam().Backend, GetParam().ISA))
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    llvm::cantFail(
        Memory->map(Code, PageSize, Read | Write | Execute | UserAccessible));
    llvm::cantFail(Memory->map(Data, PageSize, Read | Write));
    llvm::cantFail(
        Memory->addressSpace()->writeInteger(Data, Value, sizeof(uint64_t)));
    code(LoadX64, LoadARM);
    XS.UserMode = AS.UserMode = true;
    XS.reg(X64Register::CX) = AS.reg(AArch64Register::X1) = Data;
    XS.reg(X64Register::FLAGS) = x64::InitialFlags;
  }
  void code(llvm::ArrayRef<uint8_t> X64, llvm::ArrayRef<uint32_t> ARM) {
    if (GetParam().ISA == GuestArchitecture::X64) {
      llvm::cantFail(Memory->write(Code, X64));
      return;
    }
    std::vector<uint8_t> Bytes(ARM.size() * sizeof(uint32_t));
    for (size_t I = 0; I < ARM.size(); ++I)
      llvm::support::endian::write32le(Bytes.data() + I * sizeof(uint32_t),
                                       ARM[I]);
    llvm::cantFail(Memory->write(Code, Bytes));
  }
  llvm::Error step(uint64_t PC = Code) {
    const auto Deadline =
        std::chrono::steady_clock::now() + std::chrono::microseconds(Timeout);
    if (X64) {
      auto R =
          buildX64PageTables(*Memory, true, X64->requiresExceptionMonitor());
      if (!R)
        return R.takeError();
      Root = *R;
      XS.reg(X64Register::PC) = PC;
      return X64->step(XS, Root, {Deadline});
    }
    if (auto E = buildAArch64PageTables(*Memory, true))
      return E;
    AS.reg(AArch64Register::PC) = PC;
    return ARM->step(AS, {Deadline});
  }
  void expectDenied(uint64_t PC = Code) {
    auto E = step(PC);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
  uint64_t result() const {
    return X64 ? XS.reg(X64Register::AX) : AS.reg(AArch64Register::X0);
  }
};
TEST_P(UserProjection, ExecutesUserLoadWithoutPreflight) {
  llvm::cantFail(Memory->protect(Data, PageSize, Read | UserAccessible));
  ASSERT_EQ(llvm::toString(step()), "");
  EXPECT_EQ(result(), Value);
}
TEST_P(UserProjection, DeniesSupervisorReadWithoutPreflight) {
  expectDenied();
  EXPECT_EQ(result(), 0u);
}
TEST_P(UserProjection, DeniesSupervisorWriteWithoutPreflight) {
  code(StoreX64, StoreARM);
  XS.reg(X64Register::AX) = AS.reg(AArch64Register::X0) = Updated;
  expectDenied();
  EXPECT_EQ(llvm::cantFail(
                Memory->addressSpace()->readInteger(Data, sizeof(uint64_t))),
            Value);
}
TEST_P(UserProjection, DeniesSupervisorFetchWithoutPreflight) {
  llvm::cantFail(Memory->protect(Code, PageSize, Read | Execute));
  expectDenied();
}
TEST_P(UserProjection, DeniesReadOnlyUserWriteWithoutPreflight) {
  code(StoreX64, StoreARM);
  llvm::cantFail(Memory->protect(Data, PageSize, Read | UserAccessible));
  XS.reg(X64Register::AX) = AS.reg(AArch64Register::X0) = Updated;
  expectDenied();
  EXPECT_EQ(llvm::cantFail(
                Memory->addressSpace()->readInteger(Data, sizeof(uint64_t))),
            Value);
}
TEST_P(UserProjection, DeniesUserDataFetchWithoutPreflight) {
  llvm::cantFail(
      Memory->protect(Code, PageSize, Read | Write | UserAccessible));
  expectDenied();
}
TEST_P(UserProjection, UserMarkerAloneIsNotAPresentPage) {
  llvm::cantFail(Memory->protect(Data, PageSize, UserAccessible));
  expectDenied();
}
TEST_P(UserProjection, PrivilegedInstructionProvesCPL3OrEL0) {
  code(PrivilegedX64, PrivilegedARM);
  expectDenied();
}
TEST_P(UserProjection, RevokesPreviouslyCachedUserTranslation) {
  llvm::cantFail(Memory->protect(Data, PageSize, Read | UserAccessible));
  ASSERT_EQ(llvm::toString(step()), "");
  EXPECT_EQ(result(), Value);
  llvm::cantFail(Memory->protect(Data, PageSize, Read));
  XS.reg(X64Register::AX) = AS.reg(AArch64Register::X0) = Updated;
  expectDenied();
  EXPECT_EQ(result(), Updated);
}
TEST_P(UserProjection, PrivateMonitorCodeCannotExecuteInUserMode) {
  expectDenied(X64 ? x64::BootstrapPC : aarch64::EntryGPA);
}
INSTANTIATE_TEST_SUITE_P(Transports, UserProjection,
                         testing::ValuesIn(Profiles),
                         [](const testing::TestParamInfo<Profile> &P) {
                           return P.param.Name;
                         });
} // namespace
} // namespace neverd::emulation
