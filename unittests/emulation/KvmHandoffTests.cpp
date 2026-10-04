//===- KvmHandoffTests.cpp - Bounded handoff polling under changing load --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "backends/kvm/KvmHandoffPolicy.h"
#include "gtest/gtest.h"

namespace neverd::emulation {
namespace {
#define NEVERD_KVM_HANDOFF_TEST_VALUE(Name, Value)                             \
  constexpr unsigned Name = Value;
#include "KvmHandoffCases.def"
#undef NEVERD_KVM_HANDOFF_TEST_VALUE

TEST(KvmHandoff, UnproductivePollingIsBoundedAndSlowPeersCanRecover) {
  KvmHandoffPolicy Slow, Ready;
  unsigned SlowPolls = 0;
  for (unsigned I = 0; I < PolicyEntries; ++I) {
    if (Slow.begin()) {
      ++SlowPolls;
      Slow.finish(false);
    }
    // A slow direction must not suppress its independently productive peer.
    ASSERT_TRUE(Ready.begin());
    Ready.finish(true);
  }
  EXPECT_GT(SlowPolls, 0u);
  EXPECT_LE(SlowPolls, MaximumSlowProbes);

  unsigned Recovered = 0;
  for (unsigned I = 0; I < PolicyEntries; ++I)
    if (Slow.begin()) {
      ++Recovered;
      Slow.finish(true);
    }
  EXPECT_GE(Recovered, MinimumRecoveredPolls);
}

TEST(KvmHandoff, IsolatedDeschedulingDoesNotDisableProductivePolling) {
  KvmHandoffPolicy Policy;
  for (unsigned I = 0; I < PolicyEntries; ++I) {
    ASSERT_TRUE(Policy.begin());
    Policy.finish(I % PreemptionPeriod != 0);
  }
}
} // namespace
} // namespace neverd::emulation
