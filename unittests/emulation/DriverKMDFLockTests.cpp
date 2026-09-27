//===- DriverKMDFLockTests.cpp - Genuine KMDF lock execution --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
#ifdef NEVERD_KMDF_LOCK_FIXTURE
TEST(DriverKMDFLocks, ContentionPreservesThreadAPCsAndWaitingIRQL) {
  std::vector<const char *> Images{NEVERD_KMDF_LOCK_FIXTURE};
#ifdef NEVERD_KMDF_LOCK_CFG_FIXTURE
  Images.push_back(NEVERD_KMDF_LOCK_CFG_FIXTURE);
#endif
  for (const auto *Image : Images)
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Address);
      DriverOptions Options;
      Options.LoadAddress = Address;
      Options.Unload = true;
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      SCOPED_TRACE(::testing::PrintToString(Result->Messages));
      ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      EXPECT_EQ(Result->NTStatus, 0u);
      EXPECT_TRUE(Result->UnloadCompleted);
      std::vector<std::string> Messages;
      for (const auto &Message : Result->Messages)
        if (llvm::StringRef(Message).starts_with("KMDF locks:"))
          Messages.push_back(Message);
      EXPECT_EQ(Messages,
                (std::vector<std::string>{"KMDF locks: worker completed\n",
                                          "KMDF locks: APC waiter completed\n",
                                          "KMDF locks: APC owner completed\n",
                                          "KMDF locks: complete\n",
                                          "KMDF locks: unload\n"}));
      for (llvm::StringRef Name :
           {"WdfSpinLockCreate", "WdfSpinLockAcquire", "WdfSpinLockRelease",
            "WdfWaitLockCreate", "WdfWaitLockAcquire", "WdfWaitLockRelease",
            "WdfObjectAcquireLock", "WdfObjectReleaseLock", "KfRaiseIrql",
            "KeLowerIrql"})
        EXPECT_TRUE(
            std::any_of(Result->Calls.begin(), Result->Calls.end(),
                        [&](const auto &Call) { return Call.Name == Name; }))
            << Name.str();
    }
}
#endif
} // namespace
} // namespace neverd::emulation
