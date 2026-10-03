//===- WindowsHeapTests.cpp - Original Win32 heap and atomic failures -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"
#include "os/windows/process/WindowsProcessExceptions.h"
#include "os/windows/process/WindowsProcessModules.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <initializer_list>

namespace neverd::emulation {
namespace {
namespace win = windows_process;
#define NEVERD_HEAP_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_HEAP_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/WindowsHeapCases.def"
#undef NEVERD_HEAP_TEXT
#undef NEVERD_HEAP_VALUE
struct Case {
  const char *Name, *Argument, *Expected;
};
constexpr Case Cases[] = {
#define NEVERD_HEAP_CASE(Name, Argument, Expected) {#Name, Argument, Expected},
#include "fixtures/WindowsHeapCases.def"
#undef NEVERD_HEAP_CASE
};
struct Profile {
  const char *Name;
  ExecutionBackendKind Backend;
  GuestArchitecture ISA;
  const char *Directory;
};
constexpr Profile Profiles[] = {
#define NEVERD_USER_PROFILE(Name, Backend, ISA, Contract)                      \
  {#Name, ExecutionBackendKind::Backend, GuestArchitecture::ISA, ISA##Dir},
#include "UserExecutionCases.def"
#undef NEVERD_USER_PROFILE
};
void PrintTo(const Profile &P, std::ostream *OS) { *OS << P.Name; }
class WindowsHeap : public testing::TestWithParam<Profile> {
protected:
  ExecutionConfiguration Config;
  ProcessOptions Options;
  std::filesystem::path Path;
  void SetUp() override {
#ifndef NEVERD_WINDOWS_HEAP_FIXTURE_DIR
    if (requireHvf(GetParam().Backend, GetParam().ISA))
      FAIL() << MissingTools;
    GTEST_SKIP() << MissingTools;
#else
    const auto &P = GetParam();
    Config.Backend = P.Backend;
    Config.Architecture = P.ISA;
    Config.Contract = P.ISA == GuestArchitecture::X64
                          ? ExecutionContract::CheckedUserX64
                          : ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(Config);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available) {
      if (requireHvf(P.Backend, P.ISA))
        FAIL() << Probe->Reason;
      GTEST_SKIP() << Probe->Reason;
    }
    Path = std::filesystem::path(NEVERD_WINDOWS_HEAP_FIXTURE_DIR) /
           P.Directory / ProgramFile;
    Options.Backend = P.Backend;
    Options.Limits.Instructions = InstructionLimit;
    Options.Limits.TimeoutMicroseconds = TimeoutMicroseconds;
#endif
  }
};

// Drive the same OS service boundary over actual backend memory. Deliberately
// control placement and budgets here; the independent native EXE below must
// not depend on a particular Windows heap allocator's placement decisions.
struct Heap {
  ProcessOptions Options;
  std::shared_ptr<AddressSpace> Space;
  BackendSelection Backend;
  std::unique_ptr<ExecutionBudget> Budget;
  win::Image Image{};
  win::Environment Env{};
  ProcessResult Result{};
  win::Program Program;
  std::unique_ptr<win::VirtualMemory> Virtual;
  std::unique_ptr<win::Services> OS;
  std::unique_ptr<win::VectoredExceptions> Exceptions;

  explicit Heap(const ExecutionConfiguration &Config) {
    Options.MemoryLimit = DirectLimit;
    auto RAM = llvm::cantFail(PhysicalMemory::create(DirectLimit));
    Space = llvm::cantFail(AddressSpace::create(RAM, DirectLimit));
    llvm::cantFail(
        Space->map(win::value::TEB, PageSize, Read | Write | UserAccessible));
    Backend = llvm::cantFail(createExecutionBackend(Config, Space));
    Budget = llvm::cantFail(ExecutionBudget::create(Options.Limits));
    Virtual = std::make_unique<win::VirtualMemory>(*Space, Options);
    Exceptions = std::make_unique<win::VectoredExceptions>(
        *Backend.CPU,
        llvm::cantFail(
            IntegerABI::get(Config.Architecture == GuestArchitecture::X64
                                ? IntegerCallingConvention::Win64
                                : IntegerCallingConvention::AAPCS64)),
        win::value::StackTop - Options.StackSize);
    OS = std::make_unique<win::Services>(*Backend.CPU, *Space, Image, Env,
                                         Options, Result, *Virtual, Program,
                                         *Budget, *Exceptions);
    llvm::cantFail(
        Backend.CPU->writeInteger(win::value::TEB + win::value::TebLastError,
                                  LastErrorSeed, win::value::DWordSize));
  }
  llvm::Expected<win::ServiceOutcome>
  call(win::API API, std::initializer_list<uint64_t> Arguments) {
    NativeCallEvent Event{};
    std::copy(Arguments.begin(), Arguments.end(), Event.Arguments.begin());
    const auto *S = llvm::find_if(win::services(),
                                  [&](const auto &S) { return S.Kind == API; });
    return OS->invoke(*S, Event);
  }
  uint64_t number(win::API API, std::initializer_list<uint64_t> Arguments) {
    auto R = llvm::cantFail(call(API, Arguments));
    EXPECT_TRUE(R.Value.has_value()) << Result.Diagnostic;
    return R.Value.value_or(UINT64_MAX);
  }
  uint64_t alloc(uint64_t Size) {
    return number(win::API::HeapAlloc, {win::value::HeapHandle, 0, Size});
  }
  uint64_t resize(uint64_t Address, uint64_t Size, uint32_t Flags = 0) {
    return number(win::API::HeapReAlloc,
                  {win::value::HeapHandle, Flags, Address, Size});
  }
  uint64_t size(uint64_t Address) {
    return number(win::API::HeapSize, {win::value::HeapHandle, 0, Address});
  }
  void free(uint64_t Address) {
    EXPECT_EQ(number(win::API::HeapFree, {win::value::HeapHandle, 0, Address}),
              1u);
  }
  std::vector<uint8_t> bytes(uint64_t Address, uint64_t Size) {
    std::vector<uint8_t> Data(Size);
    llvm::cantFail(Space->readBacking(Address, Data));
    return Data;
  }
  void fill(uint64_t Address, uint64_t Size) {
    std::vector<uint8_t> Data(Size, Poison);
    llvm::cantFail(Space->write(Address, Data));
  }
  void lastError(uint32_t Expected = LastErrorSeed) {
    EXPECT_EQ(
        llvm::cantFail(Backend.CPU->readInteger(
            win::value::TEB + win::value::TebLastError, win::value::DWordSize)),
        Expected);
  }
};

TEST_P(WindowsHeap, ExecutesOriginalHeapScenarios) {
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Name);
    Options.Arguments = {ProgramFile, C.Argument};
    auto R = emulateProcess(Path, ProcessProfile::WindowsPE64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
    EXPECT_EQ(R->ExitStatus, ExitStatus) << llvm::toHex(R->StandardError);
    EXPECT_TRUE(R->StandardError.empty());
    EXPECT_EQ(llvm::toHex(R->StandardOutput), C.Expected);
  }
}
TEST_P(WindowsHeap, MoveAndInPlaceResizePreserveDataAndOwnership) {
  Heap H(Config);
  const uint64_t Before = H.Space->physicalMemory()->allocatedBytes();
  const uint64_t A = H.alloc(SmallSize);
  const uint64_t Neighbor = H.alloc(PageSize);
  ASSERT_NE(A, 0u);
  ASSERT_EQ(Neighbor, A + PageSize);
  H.fill(A, PageSize);
  H.fill(Neighbor, PageSize);
  EXPECT_EQ(H.resize(A, MediumSize, HeapInPlace | HeapZero), A);
  EXPECT_EQ(H.bytes(A, SmallSize), std::vector<uint8_t>(SmallSize, Poison));
  EXPECT_EQ(H.bytes(A + SmallSize, MediumSize - SmallSize),
            std::vector<uint8_t>(MediumSize - SmallSize));
  const auto Original = H.bytes(A, PageSize);
  const uint64_t Used = H.Space->mappedBytes();
  EXPECT_EQ(H.resize(A, LargeSize, HeapInPlace | HeapZero), 0u);
  EXPECT_EQ(H.bytes(A, PageSize), Original);
  EXPECT_EQ(H.size(A), MediumSize);
  EXPECT_EQ(H.Space->mappedBytes(), Used);
  const uint64_t Moved = H.resize(A, LargeSize, HeapZero);
  ASSERT_NE(Moved, 0u);
  EXPECT_NE(Moved, A);
  EXPECT_FALSE(llvm::cantFail(H.Backend.CPU->canAccess(A, 1, Read)));
  EXPECT_EQ(
      H.bytes(Moved, MediumSize),
      std::vector<uint8_t>(Original.begin(), Original.begin() + MediumSize));
  EXPECT_EQ(H.bytes(Moved + MediumSize, LargeSize - MediumSize),
            std::vector<uint8_t>(LargeSize - MediumSize));
  EXPECT_EQ(H.bytes(Neighbor, PageSize),
            std::vector<uint8_t>(PageSize, Poison));
  H.fill(Moved, LargeSize);
  EXPECT_EQ(H.resize(Moved, TinySize, HeapInPlace), Moved);
  EXPECT_EQ(H.resize(Moved, SmallSize, HeapZero | HeapInPlace), Moved);
  EXPECT_EQ(H.bytes(Moved, TinySize), std::vector<uint8_t>(TinySize, Poison));
  EXPECT_EQ(H.bytes(Moved + TinySize, SmallSize - TinySize),
            std::vector<uint8_t>(SmallSize - TinySize));
  H.free(Moved);
  H.free(Neighbor);
  EXPECT_EQ(H.Space->physicalMemory()->allocatedBytes(), Before);
  H.lastError(OutOfMemoryError);
}
TEST_P(WindowsHeap, AllocationFailurePreservesBlocksAndShrinkReturnsCapacity) {
  Heap H(Config);
  const uint64_t A = H.alloc(3 * PageSize);
  const uint64_t Neighbor = H.alloc(PageSize);
  const uint64_t Filler = H.alloc(DirectLimit - H.Space->mappedBytes());
  ASSERT_NE(A, 0u);
  ASSERT_NE(Neighbor, 0u);
  ASSERT_NE(Filler, 0u);
  ASSERT_EQ(H.Space->physicalMemory()->allocatedBytes(), DirectLimit);
  H.fill(A, 3 * PageSize);
  for (uint64_t Size : {5 * PageSize, UINT64_MAX}) {
    EXPECT_EQ(H.resize(A, Size, HeapZero), 0u);
    EXPECT_EQ(H.size(A), 3 * PageSize);
    EXPECT_EQ(H.bytes(A, 3 * PageSize),
              std::vector<uint8_t>(3 * PageSize, Poison));
    EXPECT_EQ(H.Space->mappedBytes(), DirectLimit);
    EXPECT_EQ(H.Space->physicalMemory()->allocatedBytes(), DirectLimit);
  }
  EXPECT_EQ(H.resize(A, SmallSize, HeapInPlace), A);
  EXPECT_EQ(H.Space->physicalMemory()->allocatedBytes(),
            DirectLimit - 2 * PageSize);
  // No spare RAM remains after this growth: only newly required pages count.
  EXPECT_EQ(H.resize(A, 3 * PageSize, HeapZero | HeapInPlace), A);
  EXPECT_EQ(H.bytes(A, SmallSize), std::vector<uint8_t>(SmallSize, Poison));
  EXPECT_EQ(H.bytes(A + SmallSize, 3 * PageSize - SmallSize),
            std::vector<uint8_t>(3 * PageSize - SmallSize));
  EXPECT_EQ(H.Space->physicalMemory()->allocatedBytes(), DirectLimit);
  EXPECT_EQ(H.resize(A, 0, HeapInPlace), A);
  EXPECT_EQ(H.size(A), 0u);
  const uint64_t Reused = H.alloc(2 * PageSize);
  EXPECT_EQ(Reused, A + PageSize);
  H.free(Reused);
  H.free(Filler);
  H.free(Neighbor);
  H.free(A);
  EXPECT_EQ(H.Space->physicalMemory()->allocatedBytes(), PageSize);
  H.lastError(OutOfMemoryError);
}
TEST_P(WindowsHeap, InvalidOwnershipFlagsAndPermissionsDoNotPublishChanges) {
  Heap H(Config);
  const uint64_t A = H.alloc(LargeSize);
  const uint64_t Neighbor = H.alloc(PageSize);
  ASSERT_NE(A, 0u);
  H.fill(A, 2 * PageSize);
  const auto Original = H.bytes(A, 2 * PageSize);
  const uint64_t Used = H.Space->mappedBytes();
  auto Check = [&] {
    EXPECT_EQ(H.bytes(A, 2 * PageSize), Original);
    EXPECT_EQ(H.size(A), LargeSize);
    EXPECT_EQ(H.Space->mappedBytes(), Used);
    EXPECT_EQ(H.Space->physicalMemory()->allocatedBytes(), Used);
    H.lastError();
  };
  for (auto Args : {std::array<uint64_t, 4>{win::value::HeapHandle,
                                            HeapException, A, SmallSize},
                    {win::value::HeapHandle, HeapUnknown, A, SmallSize},
                    {win::value::HeapHandle, 0, 0, SmallSize},
                    {win::value::HeapHandle, 0, A + 1, SmallSize},
                    {win::value::HeapHandle + PageSize, 0, A, SmallSize}}) {
    auto R =
        H.call(win::API::HeapReAlloc, {Args[0], Args[1], Args[2], Args[3]});
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_FALSE(R->Value);
    EXPECT_EQ(H.Result.Stop, ProcessStopReason::UnsupportedService);
    Check();
  }
  // A denied byte anywhere in retained padding prevents in-place zeroing.
  llvm::cantFail(
      H.Space->protect(A + PageSize, PageSize, Read | UserAccessible));
  for (uint64_t Size : {LargeSize + SmallSize, 3 * PageSize}) {
    if (Size == 3 * PageSize)
      H.free(Neighbor);
    const uint64_t Current = H.Space->mappedBytes();
    auto R = H.call(win::API::HeapReAlloc,
                    {win::value::HeapHandle, HeapZero, A, Size});
    ASSERT_FALSE(bool(R));
    EXPECT_EQ(llvm::toString(R.takeError()),
              std::string(win::text::Prefix) + win::text::UserException);
    EXPECT_EQ(H.bytes(A, 2 * PageSize), Original);
    EXPECT_EQ(H.size(A), LargeSize);
    EXPECT_EQ(H.Space->mappedBytes(), Current);
    EXPECT_EQ(H.Space->physicalMemory()->allocatedBytes(), Current);
  }
  const uint64_t Blocker = H.alloc(PageSize);
  llvm::cantFail(H.Space->protect(A + PageSize, PageSize, UserAccessible));
  auto R = H.call(win::API::HeapReAlloc,
                  {win::value::HeapHandle, 0, A, 3 * PageSize});
  ASSERT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  Check();
  H.free(Blocker);
  H.free(A);
}
TEST_P(WindowsHeap,
       RepeatedResizeReclaimsBudgetAndRejectsEnvironmentOwnership) {
  Options.MemoryLimit = LimitedMemory;
  Options.StackSize = LimitedStack;
  Options.OutputLimit = PageSize;
  Options.Arguments = {ProgramFile, ReclaimArgument};
  auto R = emulateProcess(Path, ProcessProfile::WindowsPE64, Options);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
  EXPECT_EQ(R->ExitStatus, ExitStatus) << llvm::toHex(R->StandardError);
  Options.Arguments = {ProgramFile, SnapshotArgument};
  R = emulateProcess(Path, ProcessProfile::WindowsPE64, Options);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService) << R->Diagnostic;
  EXPECT_FALSE(R->ExitStatus);
  EXPECT_TRUE(R->StandardOutput.empty());
  ASSERT_FALSE(R->NativeCalls.empty());
  EXPECT_FALSE(R->NativeCalls.back().Result);
}
INSTANTIATE_TEST_SUITE_P(Backends, WindowsHeap, testing::ValuesIn(Profiles),
                         [](const auto &P) { return P.param.Name; });

TEST(WindowsHeapNative, RunsOriginalHeapExecutable) {
#if !defined(_WIN32) || !defined(_M_X64)
  GTEST_SKIP() << NativeOnly;
#elif !defined(NEVERD_WINDOWS_HEAP_FIXTURE_DIR)
  FAIL() << MissingTools;
#else
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(TempPrefix, Temporary));
  const std::filesystem::path Root(Temporary.c_str());
  auto Cleanup = llvm::scope_exit(
      [&] { llvm::sys::fs::remove_directories(Root.string()); });
  const auto Program = (std::filesystem::path(NEVERD_WINDOWS_HEAP_FIXTURE_DIR) /
                        X64Dir / ProgramFile)
                           .string();
  const auto Output = (Root / StdoutFile).string();
  const auto Error = (Root / StderrFile).string();
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                      Error};
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Name);
    std::string Diagnostic;
    bool Failed = false;
    const int Status = llvm::sys::ExecuteAndWait(
        Program, {Program, C.Argument}, std::nullopt, Redirects,
        NativeTimeoutSeconds, 0, &Diagnostic, &Failed);
    ASSERT_FALSE(Failed) << Diagnostic;
    auto Out = llvm::MemoryBuffer::getFile(Output);
    auto Err = llvm::MemoryBuffer::getFile(Error);
    ASSERT_TRUE(bool(Out));
    ASSERT_TRUE(bool(Err));
    llvm::outs() << ObservationLabel << C.Argument << ' ' << Status << ' '
                 << llvm::toHex((*Out)->getBuffer()) << '\n';
    EXPECT_EQ(Status, ExitStatus) << llvm::toHex((*Err)->getBuffer());
    EXPECT_TRUE((*Err)->getBuffer().empty());
    EXPECT_EQ(llvm::toHex((*Out)->getBuffer()), C.Expected);
  }
#endif
}
} // namespace
} // namespace neverd::emulation
