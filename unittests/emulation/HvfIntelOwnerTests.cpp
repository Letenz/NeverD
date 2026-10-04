//===- HvfIntelOwnerTests.cpp - Fresh VMs on a retained Intel owner
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#if defined(__APPLE__) && defined(__x86_64__) && defined(NEVERD_EMULATION_HVF)
#include "backends/hvf/HvfExecutor.h"
#include "backends/hvf/HvfOwnerThread.h"
#include "core/ExecutionDiagnostics.h"

#include <cstdlib>

namespace neverd::emulation::hvf {
struct ExecutorProbe {
  static std::shared_ptr<OwnerThread> owner(Executor &Host) {
    return Host.Owner;
  }
  static llvm::Error identity(Executor &Host, std::thread::id &ID) {
    return Host.submit([&](Executor &H) -> llvm::Error {
      if (!H.VMCreated || !H.CPUCreated || !H.VMLease.owns_lock())
        return diagnostic::error("fresh Intel executor lacks native resources");
      ID = std::this_thread::get_id();
      return llvm::Error::success();
    });
  }
};
namespace {
TEST(HvfIntelOwner, FreshExecutorsRetireVMsOnOneOwnerThread) {
  std::thread::id Original;
  std::shared_ptr<OwnerThread> Owner;
  for (unsigned I = 0; I < 50; ++I) {
    auto Created = Executor::acquire();
    if (!Created) {
      auto E = Created.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      auto Text = llvm::toString(std::move(E));
      if (!I && Unavailable && !std::getenv("NEVERD_REQUIRE_HVF"))
        GTEST_SKIP() << Text;
      FAIL() << Text;
    }
    auto Host = std::move(*Created);
    std::thread::id Current;
    ASSERT_EQ(llvm::toString(ExecutorProbe::identity(*Host, Current)), "");
    if (!I) {
      Original = Current;
      Owner = ExecutorProbe::owner(*Host);
    }
    EXPECT_EQ(Current, Original);
    EXPECT_EQ(ExecutorProbe::owner(*Host), Owner);
    std::weak_ptr<Executor> Retired = Host;
    Host.reset();
    ASSERT_TRUE(Retired.expired());

    // The process VM must really be absent in every gap. This native probe
    // cannot succeed if a strong cache merely retained the previous VM.
    hv_return_t Create = HV_ERROR, Destroy = HV_ERROR;
    const auto Probe = Owner->start([&] {
      std::unique_lock Lease(Owner->vmMutex());
      EXPECT_EQ(std::this_thread::get_id(), Original);
      Create = hv_vm_create(HV_VM_DEFAULT);
      if (Create == HV_SUCCESS)
        Destroy = hv_vm_destroy();
    });
    Owner->wait(Probe);
    ASSERT_EQ(Create, HV_SUCCESS);
    ASSERT_EQ(Destroy, HV_SUCCESS);
  }
}
} // namespace
} // namespace neverd::emulation::hvf
#endif
