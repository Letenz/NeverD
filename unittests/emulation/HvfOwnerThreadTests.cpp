//===- HvfOwnerThreadTests.cpp - Borrowed executor lifetime boundaries
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "backends/hvf/HvfOwnerThread.h"
#include "gtest/gtest.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <new>
#include <vector>

namespace neverd::emulation::hvf {
namespace {
using namespace std::chrono_literals;

TEST(HvfOwnerThread, SequentialLifetimesKeepOneThreadAndRetireCaptures) {
  OwnerThread Owner;
  std::thread::id First, LastDestruction;
  unsigned Executed = 0, Destroyed = 0;
  for (unsigned I = 0; I < 500; ++I) {
    auto Capture = std::shared_ptr<unsigned>(new unsigned(I), [&](auto *P) {
      LastDestruction = std::this_thread::get_id();
      ++Destroyed;
      delete P;
    });
    auto Lifetime = Owner.start([&, Capture = std::move(Capture)] {
      const auto Thread = std::this_thread::get_id();
      if (Executed == 0)
        First = Thread;
      EXPECT_EQ(Thread, First);
      EXPECT_EQ(*Capture, Executed++);
    });
    Owner.wait(Lifetime);
    EXPECT_EQ(Lifetime, I + 1);
    EXPECT_EQ(Destroyed, I + 1);
    EXPECT_EQ(LastDestruction, First);
  }
  EXPECT_NE(First, std::this_thread::get_id());
}

TEST(HvfOwnerThread, OldWaitDoesNotWaitForALaterLiveExecutor) {
  OwnerThread Owner;
  const auto First = Owner.start([] {});
  Owner.wait(First);
  std::promise<void> Entered, Release;
  auto Ready = Entered.get_future();
  auto Finish = Release.get_future();
  const auto Second = Owner.start([&] {
    Entered.set_value();
    Finish.wait();
  });
  EXPECT_EQ(Ready.wait_for(2s), std::future_status::ready);
  auto OldWait = std::async(std::launch::async, [&] { Owner.wait(First); });
  const auto OldStatus = OldWait.wait_for(2s);
  Release.set_value();
  Owner.wait(Second);
  OldWait.get();
  EXPECT_EQ(OldStatus, std::future_status::ready);
}

TEST(HvfOwnerThread, ConcurrentCallersCannotOverlapLifetimes) {
  OwnerThread Owner;
  std::atomic<unsigned> Running{0}, Completed{0};
  std::vector<std::thread> Callers;
  for (unsigned N = 0; N < 8; ++N)
    Callers.emplace_back([&] {
      for (unsigned I = 0; I < 50; ++I) {
        const auto Lifetime = Owner.start([&] {
          EXPECT_EQ(Running.fetch_add(1), 0u);
          ++Completed;
          EXPECT_EQ(Running.fetch_sub(1), 1u);
        });
        Owner.wait(Lifetime);
      }
    });
  for (auto &Caller : Callers)
    Caller.join();
  EXPECT_EQ(Completed.load(), 400u);
}

TEST(HvfOwnerThread, LastClientOutlivesFactoryAndOwnsLeaseUntilJoin) {
  auto Factory = std::make_shared<OwnerThread>();
  auto Client = Factory;
  std::weak_ptr<OwnerThread> Weak = Client;
  auto *Borrowed = Client.get();
  Factory.reset();
  EXPECT_FALSE(Weak.expired());
  std::atomic<bool> Retired{false};
  std::promise<void> Entered, Release;
  auto Ready = Entered.get_future();
  auto Finish = Release.get_future();
  Client->start([&] {
    std::unique_lock Lease(Borrowed->vmMutex());
    Entered.set_value();
    Finish.wait();
    Lease.unlock();
    Retired.store(true);
  });
  EXPECT_EQ(Ready.wait_for(2s), std::future_status::ready);
  Release.set_value();
  // No explicit wait: the last client's destructor must join, and must keep
  // the lease mutex alive until the still-borrowing action has returned.
  Client.reset();
  EXPECT_TRUE(Retired.load());
  EXPECT_TRUE(Weak.expired());
}

TEST(HvfOwnerThread, FailedLifetimeAndRejectedPublicationAllowNextCaller) {
  OwnerThread Owner;
  EXPECT_THROW(Owner.start({}), std::invalid_argument);
  struct CopyFailure {
    CopyFailure() = default;
    CopyFailure(CopyFailure &&) = default;
    CopyFailure(const CopyFailure &) { throw std::bad_alloc(); }
    void operator()() const { ADD_FAILURE() << "failed copy was published"; }
  };
  OwnerThread::Job Uncopyable{CopyFailure{}};
  EXPECT_THROW(Owner.start(Uncopyable), std::bad_alloc);
  std::promise<void> Cleanup;
  auto Finish = Cleanup.get_future();
  std::atomic<bool> Cleaned{false};
  // A failed initialization still ends its work loop before releasing the
  // owner slot; a following caller can then create a fresh native session.
  const auto Failed = Owner.start([&] {
    Finish.wait();
    Cleaned.store(true);
  });
  EXPECT_EQ(Failed, 1u);
  auto Next = std::async(std::launch::async, [&] {
    const auto Lifetime = Owner.start([&] { EXPECT_TRUE(Cleaned.load()); });
    Owner.wait(Lifetime);
    return Lifetime;
  });
  Cleanup.set_value();
  Owner.wait(Failed);
  EXPECT_EQ(Next.get(), Failed + 1);
}
} // namespace
} // namespace neverd::emulation::hvf
