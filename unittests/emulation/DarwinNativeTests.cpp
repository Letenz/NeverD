//===- DarwinNativeTests.cpp - Original workloads on the host kernel -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "os/darwin/kernel/DarwinFiles.h"
#include "os/darwin/kernel/DarwinMemory.h"
#include "os/darwin/kernel/DarwinSystem.h"
#include "os/darwin/kernel/DarwinTime.h"

#include "neverd/emulation/AddressSpace.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#if defined(__APPLE__)
#include <dirent.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/syslimits.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
// The raw LP64 entry point exported by libsystem_kernel; the public legacy
// getdirentries declaration is unavailable with the 64-bit-inode SDK ABI.
extern "C" ssize_t __getdirentries64(int, void *, size_t, off_t *);
#endif

namespace neverd::emulation {
namespace {
TEST(DarwinNative, CredentialsMatchOneSDKCaptureAndExactGroupOrder) {
#if !defined(__APPLE__)
  GTEST_SKIP() << "native Darwin credential capture requires macOS";
#else
  ASSERT_EQ(sizeof(uid_t), 4u);
  ASSERT_EQ(sizeof(gid_t), 4u);
  ASSERT_EQ(NGROUPS_MAX, 16);
  std::array<gid_t, NGROUPS_MAX> Groups;
  const uid_t RealUID = ::getuid(), EffectiveUID = ::geteuid();
  const gid_t RealGID = ::getgid(), EffectiveGID = ::getegid();
  const int Count = ::getgroups(Groups.size(), Groups.data());
  ASSERT_GE(Count, 1);
  ASSERT_LE(Count, NGROUPS_MAX);
  EXPECT_EQ(Groups[0], EffectiveGID);
  std::optional<DarwinSystemOptions> Options = DarwinSystemOptions{};
  Options->Credentials = DarwinCredentials{
      RealUID, EffectiveUID, RealGID, EffectiveGID,
      std::vector<uint32_t>(Groups.begin(), Groups.begin() + Count)};
  ASSERT_FALSE(bool(darwin_model::validateSystemOptions(*Options)));
  for (auto Page : {uint64_t(4096), uint64_t(16384)}) {
    auto Physical = PhysicalMemory::create(Page * 4);
    ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
    auto Created = AddressSpace::create(*Physical, Page * 4);
    ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
    constexpr uint64_t Base = 0x100000;
    auto Space = *Created;
    ASSERT_FALSE(
        bool(Space->map(Base, Page * 2, Read | Write | UserAccessible)));
    ProcessResult Result{
        ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
        ExecutionBackendKind::Unicorn, "one SDK credential capture"};
    for (const auto &[Kind, ID] :
         {std::pair{darwin_model::ServiceKind::GetUID, RealUID},
          std::pair{darwin_model::ServiceKind::GetEUID, EffectiveUID},
          std::pair{darwin_model::ServiceKind::GetGID, RealGID},
          std::pair{darwin_model::ServiceKind::GetEGID, EffectiveGID}}) {
      auto Out = darwin_model::systemService(*Space, Page, Kind,
                                             {0, 0, {UINT64_MAX}, std::nullopt},
                                             Options, Result);
      ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
      ASSERT_TRUE(*Out) << Result.Diagnostic;
      EXPECT_FALSE((**Out).Error);
      EXPECT_EQ((**Out).Value, ID);
    }
    for (auto Capacity :
         {uint64_t(Count), uint64_t(0x1000), uint64_t(0x1234567800001000ULL)}) {
      const auto Offset = Page - 3;
      std::vector<uint8_t> Expected(Page * 2, 0xa5), Actual(Page * 2);
      ASSERT_FALSE(bool(Space->write(Base, Expected)));
      std::memcpy(Expected.data() + Offset, Groups.data(),
                  Count * sizeof(gid_t));
      auto Out = darwin_model::systemService(
          *Space, Page, darwin_model::ServiceKind::GetGroups,
          {0, 79, {Capacity, Base + Offset}, std::nullopt}, Options, Result);
      ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
      ASSERT_TRUE(*Out) << Result.Diagnostic;
      EXPECT_FALSE((**Out).Error);
      EXPECT_EQ((**Out).Value, uint64_t(Count));
      ASSERT_FALSE(bool(Space->read(Base, Actual)));
      EXPECT_EQ(Actual, Expected);
    }
  }
#endif
}

TEST(DarwinNative, UsageMatchesOneSDKCaptureWithEverySignedFieldAndPadding) {
#if !defined(__APPLE__)
  GTEST_SKIP() << "native Darwin SDK capture requires macOS";
#else
  ASSERT_EQ(sizeof(struct rusage), 144u);
  ASSERT_EQ(sizeof(long), 8u);
  ASSERT_EQ(offsetof(struct rusage, ru_utime), 0u);
  ASSERT_EQ(offsetof(struct rusage, ru_stime), 16u);
  ASSERT_EQ(offsetof(struct rusage, ru_maxrss), 32u);
  ASSERT_EQ(offsetof(struct rusage, ru_nivcsw), 136u);
  struct rusage Self, Children;
  std::memset(&Self, 0xa5, sizeof Self);
  std::memset(&Children, 0xa5, sizeof Children);
  ASSERT_EQ(getrusage(RUSAGE_SELF, &Self), 0);
  ASSERT_EQ(getrusage(RUSAGE_CHILDREN, &Children), 0);
  const auto Observation = [](const struct rusage &R) {
    return DarwinResourceUsage{
        R.ru_utime.tv_sec,
        uint32_t(R.ru_utime.tv_usec),
        R.ru_stime.tv_sec,
        uint32_t(R.ru_stime.tv_usec),
        {R.ru_maxrss, R.ru_ixrss, R.ru_idrss, R.ru_isrss, R.ru_minflt,
         R.ru_majflt, R.ru_nswap, R.ru_inblock, R.ru_oublock, R.ru_msgsnd,
         R.ru_msgrcv, R.ru_nsignals, R.ru_nvcsw, R.ru_nivcsw}};
  };
  std::optional<DarwinSystemOptions> Options = DarwinSystemOptions{};
  Options->ResourceUsageSelf = Observation(Self);
  Options->ResourceUsageChildren = Observation(Children);
  ASSERT_FALSE(bool(darwin_model::validateSystemOptions(*Options)));
  for (auto Page : {uint64_t(4096), uint64_t(16384)}) {
    auto Physical = PhysicalMemory::create(Page * 4);
    ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
    auto Created = AddressSpace::create(*Physical, Page * 4);
    ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
    auto Space = *Created;
    constexpr uint64_t Base = 0x100000;
    ASSERT_FALSE(
        bool(Space->map(Base, Page * 2, Read | Write | UserAccessible)));
    for (auto Who : {uint64_t(0), uint64_t(UINT32_MAX)}) {
      ASSERT_FALSE(
          bool(Space->write(Base, std::vector<uint8_t>(Page * 2, 0xa5))));
      ProcessResult Result{ProcessProfile::MacOSMachO64,
                           GuestArchitecture::AArch64,
                           ExecutionBackendKind::Unicorn, "SDK capture"};
      auto Out = darwin_model::systemService(
          *Space, Page, darwin_model::ServiceKind::GetRusage,
          {0, 117, {Who, Base + Page - 71}, std::nullopt}, Options, Result);
      ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
      ASSERT_TRUE(*Out) << Result.Diagnostic;
      EXPECT_FALSE((**Out).Error);
      EXPECT_EQ((**Out).Value, 0u);
      std::vector<uint8_t> Expected(Page * 2, 0xa5), Actual(Page * 2);
      const auto &Captured = Who == 0 ? Self : Children;
      std::memcpy(Expected.data() + Page - 71, &Captured, 144);
      ASSERT_FALSE(bool(Space->read(Base, Actual)));
      EXPECT_EQ(Actual, Expected);
    }
  }
#endif
}

TEST(DarwinNative, ResourcePairsMatchSDKCaptureLayoutAndBothFlagSpellings) {
#if !defined(__APPLE__)
  GTEST_SKIP() << "Native resource observation capture requires macOS";
#else
  ASSERT_EQ(sizeof(rlim_t), 8u);
  ASSERT_EQ(sizeof(struct rlimit), 16u);
  ASSERT_EQ(offsetof(struct rlimit, rlim_cur), 0u);
  ASSERT_EQ(offsetof(struct rlimit, rlim_max), 8u);
  ASSERT_EQ(RLIM_INFINITY, uint64_t(INT64_MAX));
  std::array<struct rlimit, 9> Captured;
  DarwinSystemOptions O;
  for (uint32_t Resource = 0; Resource != Captured.size(); ++Resource) {
    ASSERT_EQ(::getrlimit(Resource, &Captured[Resource]), 0);
    O.ResourceLimits[Resource] = {Captured[Resource].rlim_cur,
                                  Captured[Resource].rlim_max};
  }
  ASSERT_FALSE(bool(darwin_model::validateSystemOptions(O)));
  for (uint64_t Page : {uint64_t(4096), uint64_t(16384)}) {
    auto Physical = PhysicalMemory::create(Page * 2);
    ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
    auto Space = AddressSpace::create(*Physical, Page * 2);
    ASSERT_TRUE(bool(Space)) << llvm::toString(Space.takeError());
    constexpr uint64_t Base = 0x100000;
    ASSERT_FALSE(
        bool((*Space)->map(Base, Page * 2, Read | Write | UserAccessible)));
    ProcessResult Result{
        ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
        ExecutionBackendKind::Unicorn, "native getrlimit capture"};
    for (uint32_t Resource = 0; Resource != Captured.size(); ++Resource) {
      for (uint32_t Flag : {0u, 0x1000u}) {
        SCOPED_TRACE(Resource | Flag);
        std::vector<uint8_t> Expected(Page * 2, 0xa5), Actual(Page * 2);
        ASSERT_FALSE(bool((*Space)->write(Base, Expected)));
        std::memcpy(Expected.data() + Page - 8, &Captured[Resource], 16);
        auto Out = darwin_model::systemService(
            **Space, Page, darwin_model::ServiceKind::GetRlimit,
            {0, 194, {Resource | Flag, Base + Page - 8}, std::nullopt}, O,
            Result);
        ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
        ASSERT_TRUE(*Out) << Result.Diagnostic;
        EXPECT_EQ((**Out).Value, 0u);
        EXPECT_FALSE((**Out).Error);
        ASSERT_FALSE(bool((*Space)->read(Base, Actual)));
        EXPECT_EQ(Actual, Expected);
      }
    }
  }
#endif
}

TEST(DarwinNative, SystemValuesMatchSDKCapturesAndStableMIBWidths) {
#if !defined(__APPLE__)
  GTEST_SKIP() << "Native system observation capture requires macOS";
#else
  EXPECT_EQ(sizeof(size_t), 8u);
  struct Query {
    const char *Name;
    int MIB[2];
    std::vector<uint8_t> Bytes;
  };
  Query Queries[] = {{"kern.ostype", {CTL_KERN, KERN_OSTYPE}, {}},
                     {"kern.osrelease", {CTL_KERN, KERN_OSRELEASE}, {}},
                     {"kern.osrevision", {CTL_KERN, KERN_OSREV}, {}},
                     {"kern.version", {CTL_KERN, KERN_VERSION}, {}},
                     {"kern.osversion", {CTL_KERN, KERN_OSVERSION}, {}},
                     {"hw.machine", {CTL_HW, HW_MACHINE}, {}},
                     {"hw.model", {CTL_HW, HW_MODEL}, {}},
                     {"hw.ncpu", {CTL_HW, HW_NCPU}, {}},
                     {"hw.memsize", {CTL_HW, HW_MEMSIZE}, {}}};
  for (auto &Q : Queries) {
    SCOPED_TRACE(Q.Name);
    size_t Size = 0;
    ASSERT_EQ(::sysctlbyname(Q.Name, nullptr, &Size, nullptr, 0), 0);
    ASSERT_GT(Size, 0u);
    ASSERT_LE(Size, 1024u);
    Q.Bytes.resize(Size);
    ASSERT_EQ(::sysctlbyname(Q.Name, Q.Bytes.data(), &Size, nullptr, 0), 0);
    ASSERT_EQ(Size, Q.Bytes.size());
    std::vector<uint8_t> Numeric(Size);
    ASSERT_EQ(::sysctl(Q.MIB, 2, Numeric.data(), &Size, nullptr, 0), 0);
    EXPECT_EQ(Size, Q.Bytes.size());
    EXPECT_EQ(Numeric, Q.Bytes);
  }
  auto String = [&](unsigned I) {
    EXPECT_EQ(Queries[I].Bytes.back(), 0u);
    return std::string(Queries[I].Bytes.begin(), Queries[I].Bytes.end() - 1);
  };
  ASSERT_EQ(Queries[2].Bytes.size(), 4u);
  ASSERT_EQ(Queries[7].Bytes.size(), 4u);
  ASSERT_EQ(Queries[8].Bytes.size(), 8u);
  using namespace llvm::support::endian;
  DarwinSystemOptions O;
  O.OSType = String(0);
  O.OSRelease = String(1);
  O.OSRevision = int32_t(read32le(Queries[2].Bytes.data()));
  O.KernelVersion = String(3);
  O.OSVersion = String(4);
  O.Machine = String(5);
  O.Model = String(6);
  O.CPUCount = read32le(Queries[7].Bytes.data());
  O.MemorySize = read64le(Queries[8].Bytes.data());
  ASSERT_FALSE(bool(darwin_model::validateSystemOptions(O)));
  auto Physical = PhysicalMemory::create(4096);
  ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
  auto Space = AddressSpace::create(*Physical, 4096);
  ASSERT_TRUE(bool(Space)) << llvm::toString(Space.takeError());
  constexpr uint64_t Base = 0x100000, Length = Base + 64, Output = Base + 128;
  ASSERT_FALSE(bool((*Space)->map(Base, 4096, Read | Write | UserAccessible)));
  ProcessResult Result{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn, "native sysctl capture"};
  for (const auto &Q : Queries) {
    SCOPED_TRACE(Q.Name);
    for (bool Named : {true, false}) {
      std::vector<uint8_t> Expected(Q.Bytes.size() + 2, 0xa5);
      ASSERT_FALSE(bool((*Space)->write(Output, Expected)));
      std::copy(Q.Bytes.begin(), Q.Bytes.end(), Expected.begin() + 1);
      ASSERT_FALSE(bool((*Space)->writeInteger(Length, Q.Bytes.size(), 8)));
      const auto Name = llvm::StringRef(Q.Name);
      if (Named) {
        ASSERT_FALSE(
            bool((*Space)->write(Base, llvm::arrayRefFromStringRef(Name))));
      } else {
        ASSERT_FALSE(bool((*Space)->writeInteger(Base, Q.MIB[0], 4)));
        ASSERT_FALSE(bool((*Space)->writeInteger(Base + 4, Q.MIB[1], 4)));
      }
      auto Stored = darwin_model::systemService(
          **Space, ::getpagesize(),
          Named ? darwin_model::ServiceKind::SysctlByName
                : darwin_model::ServiceKind::Sysctl,
          {0,
           0,
           {Base, Named ? Name.size() : 2, Output + 1, Length},
           std::nullopt},
          O, Result);
      ASSERT_TRUE(bool(Stored)) << llvm::toString(Stored.takeError());
      ASSERT_TRUE(*Stored) << Result.Diagnostic;
      EXPECT_EQ((**Stored).Value, 0u);
      EXPECT_FALSE((**Stored).Error);
      std::vector<uint8_t> Actual(Expected.size());
      ASSERT_FALSE(bool((*Space)->read(Output, Actual)));
      EXPECT_EQ(Actual, Expected);
      EXPECT_EQ(llvm::cantFail((*Space)->readInteger(Length, 8)),
                Q.Bytes.size());
    }
  }
#endif
}

TEST(DarwinNative, MachTimebaseMatchesSDKLayoutAndCapturedRatio) {
#if !defined(__APPLE__)
  GTEST_SKIP() << "Native Darwin timebase capture requires macOS";
#else
  EXPECT_EQ(sizeof(mach_timebase_info_data_t), 8u);
  EXPECT_EQ(offsetof(mach_timebase_info_data_t, numer), 0u);
  EXPECT_EQ(offsetof(mach_timebase_info_data_t, denom), 4u);
  EXPECT_EQ(sizeof(mach_timebase_info_data_t{}.numer), 4u);
  EXPECT_EQ(sizeof(mach_timebase_info_data_t{}.denom), 4u);
  mach_timebase_info_data_t Native;
  ASSERT_EQ(::mach_timebase_info(&Native), 0);
  ASSERT_NE(Native.numer, 0u);
  ASSERT_NE(Native.denom, 0u);
  DarwinTimeOptions Options;
  Options.Timebase = {Native.numer, Native.denom};
  auto Physical = PhysicalMemory::create(4096);
  ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
  auto Space = AddressSpace::create(*Physical, 4096);
  ASSERT_TRUE(bool(Space)) << llvm::toString(Space.takeError());
  constexpr uint64_t Base = 0x100000;
  ASSERT_FALSE(bool((*Space)->map(Base, 4096, Read | Write | UserAccessible)));
  std::array<uint8_t, 10> Expected, Encoded;
  Expected.fill(0xa5);
  ASSERT_FALSE(bool((*Space)->write(Base, Expected)));
  std::memcpy(Expected.data() + 1, &Native, sizeof(Native));
  ProcessResult Result{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn, "native timebase oracle"};
  auto Stored = darwin_model::machTimeService(
      **Space, darwin_model::ServiceKind::TimebaseInfo,
      {0, 0, {Base + 1}, std::nullopt}, Options, Result);
  ASSERT_TRUE(bool(Stored)) << llvm::toString(Stored.takeError());
  ASSERT_TRUE(*Stored) << Result.Diagnostic;
  EXPECT_EQ((**Stored).Value, 0u);
  ASSERT_FALSE(bool((*Space)->read(Base, Encoded)));
  EXPECT_EQ(Encoded, Expected);
#endif
}
TEST(DarwinNative, TimeOutputsMatchSDKLayoutAndOneCapturedRawSample) {
#if !defined(__APPLE__) || (!defined(__aarch64__) && !defined(__x86_64__))
  GTEST_SKIP() << "Native Darwin time capture requires macOS ARM64 or x86_64";
#else
  EXPECT_EQ(sizeof(timeval), 16u);
  EXPECT_EQ(offsetof(timeval, tv_sec), 0u);
  EXPECT_EQ(offsetof(timeval, tv_usec), 8u);
  EXPECT_EQ(sizeof(timeval{}.tv_sec), 8u);
  EXPECT_EQ(sizeof(timeval{}.tv_usec), 4u);
  EXPECT_EQ(sizeof(struct timezone), 8u);
  EXPECT_EQ(offsetof(struct timezone, tz_minuteswest), 0u);
  EXPECT_EQ(offsetof(struct timezone, tz_dsttime), 4u);
  std::array<uint8_t, 34> Captured;
  Captured.fill(0xa5);
  // Capture the three outputs in one raw invocation. libSystem wrappers do
  // not provide this three-output contract to callers of gettimeofday.
#if defined(__aarch64__)
  register uint64_t X0 __asm__("x0") = uint64_t(Captured.data() + 1);
  register uint64_t X1 __asm__("x1") = uint64_t(Captured.data() + 17);
  register uint64_t X2 __asm__("x2") = uint64_t(Captured.data() + 25);
  register uint64_t X16 __asm__("x16") = 116;
  unsigned Carry;
  __asm__ volatile("svc #0x80\n\tcset %w2, cs"
                   : "+r"(X0), "+r"(X1), "=r"(Carry)
                   : "r"(X2), "r"(X16)
                   : "cc", "memory");
  ASSERT_EQ(Carry, 0u);
  ASSERT_EQ(X0, 0u);
  ASSERT_EQ(X1, 0u);
#else
  uint64_t RAX = 0x2000074;
  uint64_t RDX = uint64_t(Captured.data() + 25);
  unsigned char Carry;
  __asm__ volatile("syscall\n\tsetc %2"
                   : "+a"(RAX), "+d"(RDX), "=qm"(Carry)
                   : "D"(Captured.data() + 1), "S"(Captured.data() + 17)
                   : "rcx", "r11", "cc", "memory");
  ASSERT_EQ(Carry, 0u);
  ASSERT_EQ(RAX, 0u);
  ASSERT_EQ(RDX, 0u);
#endif
  using namespace llvm::support::endian;
  const uint64_t Seconds = read64le(Captured.data() + 1);
  ASSERT_LE(Seconds, UINT32_MAX);
  const uint32_t Microseconds = read32le(Captured.data() + 9);
  ASSERT_LT(Microseconds, 1000000u);
  EXPECT_EQ(read32le(Captured.data() + 13), 0u);
  const uint64_t Ticks = read64le(Captured.data() + 25);
  ASSERT_NE(Ticks, 0u);
  EXPECT_EQ(Captured.front(), 0xa5);
  EXPECT_EQ(Captured.back(), 0xa5);
  const DarwinTimeOptions Options{
      DarwinTimeOfDay{uint32_t(Seconds), Microseconds},
      DarwinTimezone{int32_t(read32le(Captured.data() + 17)),
                     int32_t(read32le(Captured.data() + 21))},
      Ticks};
  auto Physical = PhysicalMemory::create(4096);
  ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
  auto Space = AddressSpace::create(*Physical, 4096);
  ASSERT_TRUE(bool(Space)) << llvm::toString(Space.takeError());
  constexpr uint64_t Base = 0x100000;
  ASSERT_FALSE(bool((*Space)->map(Base, 4096, Read | Write | UserAccessible)));
  std::array<uint8_t, 34> Encoded;
  Encoded.fill(0xa5);
  ASSERT_FALSE(bool((*Space)->write(Base, Encoded)));
  ProcessResult Result{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn, "native time oracle"};
  auto Stored = darwin_model::timeService(
      **Space, {0, 116, {Base + 1, Base + 17, Base + 25}, std::nullopt},
      Options, Result);
  ASSERT_TRUE(bool(Stored)) << llvm::toString(Stored.takeError());
  ASSERT_TRUE(*Stored) << Result.Diagnostic;
  ASSERT_FALSE((**Stored).Error);
  ASSERT_FALSE(bool((*Space)->read(Base, Encoded)));
  EXPECT_EQ(Encoded, Captured);
#endif
}

TEST(DarwinNative, PrivateFileOffsetAndTailMatchHostMapping) {
#if !defined(__APPLE__)
  GTEST_SKIP() << "native Darwin file mappings require macOS";
#else
  using namespace darwin_model;
  const uint64_t Page = ::getpagesize();
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-darwin-mapping", Temporary));
  const std::filesystem::path Root(Temporary.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Input = (Root / "data").string();
  std::vector<uint8_t> Bytes(Page + 19);
  for (size_t I = 0; I < Bytes.size(); ++I)
    Bytes[I] = uint8_t(I * 17 + 5);
  {
    std::ofstream File(Input, std::ios::binary);
    File.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    ASSERT_TRUE(File.good());
  }
  const int NativeFD = ::open(Input.c_str(), O_RDONLY);
  ASSERT_GE(NativeFD, 0);
  // The host permits the extra EOF page at mmap time, then raises SIGBUS on
  // access. The bounded model admits only the first, partially filled page.
  void *Native = ::mmap(nullptr, Page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE,
                        NativeFD, Page);
  ASSERT_EQ(::close(NativeFD), 0);
  ASSERT_NE(Native, MAP_FAILED);
  auto Unmap = llvm::scope_exit([&] { ::munmap(Native, Page * 2); });
  auto Physical = PhysicalMemory::create(Page * 2);
  ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
  auto Space = AddressSpace::create(*Physical, Page * 2);
  ASSERT_TRUE(bool(Space)) << llvm::toString(Space.takeError());
  const uint64_t Base = 0x100000;
  ASSERT_FALSE(bool((*Space)->map(Base, Page, Read | Write | UserAccessible)));
  const uint8_t Path[] = {'/', 'd', 'a', 't', 'a', 0};
  ASSERT_FALSE(bool((*Space)->write(Base, Path)));
  ProcessOptions Options;
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = Bytes;
  Options.MemoryLimit = Page * 2;
  Options.StackSize = Page;
  DarwinFiles Files(**Space, Options.DarwinFiles);
  DarwinMemory Memory(**Space, {Page, Base, {}}, Options);
  ProcessResult Result{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       "native mapping comparison"};
  auto Opened =
      Files.handle(ServiceKind::Open, {0, 5, {Base, 0}, std::nullopt}, Result);
  ASSERT_TRUE(bool(Opened)) << llvm::toString(Opened.takeError());
  ASSERT_TRUE(Opened->has_value());
  ASSERT_FALSE((**Opened).Error);
  const auto FD = (**Opened).Value;
  auto Mapped = Memory.handle(
      ServiceKind::Mmap, {0, 197, {0, 19, 3, 0x40002, FD, Page}, std::nullopt},
      Files, Result);
  ASSERT_TRUE(bool(Mapped)) << llvm::toString(Mapped.takeError());
  ASSERT_TRUE(Mapped->has_value()) << Result.Diagnostic;
  ASSERT_FALSE((**Mapped).Error);
  auto Closed =
      Files.handle(ServiceKind::Close, {0, 6, {FD}, std::nullopt}, Result);
  ASSERT_TRUE(bool(Closed)) << llvm::toString(Closed.takeError());
  ASSERT_TRUE(Closed->has_value());
  ASSERT_FALSE((**Closed).Error);
  std::vector<uint8_t> Observed(Page);
  ASSERT_FALSE(bool((*Space)->read((**Mapped).Value, Observed)));
  EXPECT_EQ(
      llvm::ArrayRef<uint8_t>(Observed),
      llvm::ArrayRef<uint8_t>(static_cast<const uint8_t *>(Native), Page));
  const auto Child = ::fork();
  ASSERT_GE(Child, 0);
  if (!Child) {
    const struct rlimit NoCore{0, 0};
    ::setrlimit(RLIMIT_CORE, &NoCore);
    const volatile auto Beyond = static_cast<volatile uint8_t *>(Native)[Page];
    (void)Beyond;
    ::_exit(0);
  }
  int Status = 0;
  ASSERT_EQ(::waitpid(Child, &Status, 0), Child);
  ASSERT_TRUE(WIFSIGNALED(Status));
  EXPECT_EQ(WTERMSIG(Status), SIGBUS);
#endif
}
TEST(DarwinNative, Stat64WireRecordMatchesHostSDKAndFilesystemObservation) {
#if !defined(__APPLE__)
  GTEST_SKIP() << "Darwin SDK and native filesystem observation require macOS";
#else
  using namespace darwin_model;
  struct stat Native{};
  ASSERT_EQ(sizeof(Native), 144u);
#define NEVERD_DARWIN_FILE_STATUS(Member, Host, Offset, Width)                 \
  EXPECT_EQ(offsetof(struct stat, Host), Offset##u);                           \
  EXPECT_EQ(sizeof(Native.Host), Width##u);
#include "os/darwin/kernel/DarwinFileStatus.def"
#undef NEVERD_DARWIN_FILE_STATUS
  EXPECT_EQ(offsetof(struct stat, st_rdev), 24u);
  EXPECT_EQ(offsetof(struct stat, st_lspare), 124u);
  EXPECT_EQ(offsetof(struct stat, st_qspare), 128u);
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-darwin-stat", Temporary));
  const std::filesystem::path Root(Temporary.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Input = (Root / "data").string();
  {
    std::ofstream File(Input, std::ios::binary);
    File << "0123456789";
    ASSERT_TRUE(File.good());
  }
  for (bool Directory : {false, true}) {
    SCOPED_TRACE(Directory);
    Native = {};
    ASSERT_EQ(::stat(Directory ? Root.c_str() : Input.c_str(), &Native), 0);
    std::optional<DarwinFileOptions> Options(std::in_place);
    if (Directory)
      Options->Directories.insert("/data");
    else
      Options->Files["/data"] = {'0', '1', '2', '3', '4',
                                 '5', '6', '7', '8', '9'};
    auto &M = Options->Metadata["/data"];
#define NEVERD_DARWIN_FILE_STATUS(Member, Host, Offset, Width)                 \
  M.Member = Native.Host;
#include "os/darwin/kernel/DarwinFileStatus.def"
#undef NEVERD_DARWIN_FILE_STATUS
    ASSERT_FALSE(bool(validateFileOptions(*Options)));
    auto Physical = PhysicalMemory::create(16384);
    ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
    auto Space = AddressSpace::create(*Physical, 16384);
    ASSERT_TRUE(bool(Space)) << llvm::toString(Space.takeError());
    const uint64_t Base = 0x100000;
    ASSERT_FALSE(
        bool((*Space)->map(Base, 4096, Read | Write | UserAccessible)));
    const uint8_t Path[] = {'/', 'd', 'a', 't', 'a', 0};
    ASSERT_FALSE(bool((*Space)->write(Base, Path)));
    DarwinFiles Files(**Space, Options);
    ProcessResult Result{
        ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
        ExecutionBackendKind::Unicorn, "native ABI comparison"};
    auto Returned =
        Files.handle(ServiceKind::Stat64,
                     {0, 338, {Base, Base + 256}, std::nullopt}, Result);
    ASSERT_TRUE(bool(Returned)) << llvm::toString(Returned.takeError());
    ASSERT_TRUE(Returned->has_value()) << Result.Diagnostic;
    ASSERT_FALSE((**Returned).Error);
    std::array<uint8_t, 144> Bytes;
    ASSERT_FALSE(bool((*Space)->read(Base + 256, Bytes)));
    EXPECT_EQ(llvm::ArrayRef<uint8_t>(Bytes),
              llvm::ArrayRef<uint8_t>(
                  reinterpret_cast<const uint8_t *>(&Native), sizeof(Native)));
  }
#endif
}
TEST(DarwinNative, DirectoryRecordsMatchHostSDKAndCapturedFilesystemBytes) {
#if !defined(__APPLE__)
  GTEST_SKIP() << "Darwin directory records require the macOS SDK and kernel";
#else
  using namespace darwin_model;
  EXPECT_EQ(sizeof(struct dirent), 1048u);
  EXPECT_EQ(offsetof(struct dirent, d_ino), 0u);
  EXPECT_EQ(offsetof(struct dirent, d_seekoff), 8u);
  EXPECT_EQ(offsetof(struct dirent, d_reclen), 16u);
  EXPECT_EQ(offsetof(struct dirent, d_namlen), 18u);
  EXPECT_EQ(offsetof(struct dirent, d_type), 20u);
  EXPECT_EQ(offsetof(struct dirent, d_name), 21u);
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-darwin-dirents", Temporary));
  const std::filesystem::path Root(Temporary.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  ASSERT_TRUE(std::filesystem::create_directory(Root / "empty"));
  std::optional<DarwinFileOptions> Options(std::in_place);
  Options->Directories.insert("/captured/empty");
  for (const auto &Name :
       {std::string("data"), std::string("abcdefgh"), std::string(255, 'q')}) {
    std::ofstream File(Root / Name, std::ios::binary);
    ASSERT_TRUE(File.good());
    Options->Files["/captured/" + Name] = {};
  }
  const int NativeFD = ::open(Root.c_str(), O_RDONLY | O_DIRECTORY);
  ASSERT_GE(NativeFD, 0);
  auto Close = llvm::scope_exit([&] { ::close(NativeFD); });
  std::vector<uint8_t> NativeBytes(4096, 0xa5);
  off_t Initial = -1;
  const auto Count = __getdirentries64(NativeFD, NativeBytes.data(),
                                       NativeBytes.size(), &Initial);
  ASSERT_GT(Count, 0);
  ASSERT_LT(size_t(Count), NativeBytes.size() - 4);
  ASSERT_EQ(Initial, 0);
  const auto Terminal = ::lseek(NativeFD, 0, SEEK_CUR);
  ASSERT_GT(Terminal, 0);
  auto &Contents = Options->DirectoryContents["/captured"];
  Contents.MinimumBufferSize = 1;
  for (size_t Offset = 0; Offset < size_t(Count);) {
    ASSERT_GE(size_t(Count) - Offset, 21u);
    struct dirent Record{};
    std::memcpy(&Record, NativeBytes.data() + Offset, 21);
    ASSERT_GT(Record.d_reclen, 21u + Record.d_namlen);
    ASSERT_LE(Record.d_reclen, sizeof(Record));
    ASSERT_LE(Record.d_reclen, size_t(Count) - Offset);
    std::memcpy(&Record, NativeBytes.data() + Offset, Record.d_reclen);
    // This test observes the full batch endpoint. Intermediate cookies are
    // explicit test inputs; it does not claim they equal APFS's private values.
    Contents.Entries.push_back(
        {std::string(Record.d_name, Record.d_namlen), Record.d_ino,
         Record.d_type, uint64_t(Terminal) + Contents.Entries.size() + 1,
         Record.d_seekoff});
    Offset += Record.d_reclen;
  }
  ASSERT_EQ(Contents.Entries.size(), 6u);
  Contents.Entries.front().MinimumBufferSize = 64;
  Contents.Entries.back().NextOffset = Terminal;
  ASSERT_FALSE(bool(validateFileOptions(*Options)));
  auto Physical = PhysicalMemory::create(16384);
  ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
  auto Space = AddressSpace::create(*Physical, 16384);
  ASSERT_TRUE(bool(Space)) << llvm::toString(Space.takeError());
  const uint64_t Base = 0x100000, Buffer = Base + 256, Position = Base + 8192;
  ASSERT_FALSE(bool((*Space)->map(Base, 16384, Read | Write | UserAccessible)));
  const uint8_t Path[] = {'/', 'c', 'a', 'p', 't', 'u', 'r', 'e', 'd', 0};
  ASSERT_FALSE(bool((*Space)->write(Base, Path)));
  std::vector<uint8_t> Observed(NativeBytes.size(), 0xa5);
  ASSERT_FALSE(bool((*Space)->write(Buffer, Observed)));
  DarwinFiles Files(**Space, Options);
  ProcessResult Result{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       "native directory ABI comparison"};
  auto Opened = Files.handle(ServiceKind::Open,
                             {0, 5, {Base, O_DIRECTORY}, std::nullopt}, Result);
  ASSERT_TRUE(bool(Opened)) << llvm::toString(Opened.takeError());
  ASSERT_TRUE(Opened->has_value()) << Result.Diagnostic;
  ASSERT_FALSE((**Opened).Error);
  const auto FD = (**Opened).Value;
  auto Read = Files.handle(
      ServiceKind::GetDirEntries64,
      {0, 344, {FD, Buffer, NativeBytes.size(), Position}, std::nullopt},
      Result);
  ASSERT_TRUE(bool(Read)) << llvm::toString(Read.takeError());
  ASSERT_TRUE(Read->has_value()) << Result.Diagnostic;
  ASSERT_FALSE((**Read).Error);
  EXPECT_EQ((**Read).Value, uint64_t(Count));
  ASSERT_FALSE(bool((*Space)->read(Buffer, Observed)));
  EXPECT_EQ(Observed, NativeBytes);
  std::array<uint8_t, 8> PositionBytes;
  ASSERT_FALSE(bool((*Space)->read(Position, PositionBytes)));
  EXPECT_EQ(llvm::support::endian::read64le(PositionBytes.data()),
            uint64_t(Initial));
  auto Seek = Files.handle(ServiceKind::Lseek,
                           {0, 199, {FD, 0, SEEK_CUR}, std::nullopt}, Result);
  ASSERT_TRUE(bool(Seek)) << llvm::toString(Seek.takeError());
  ASSERT_TRUE(Seek->has_value());
  ASSERT_FALSE((**Seek).Error);
  EXPECT_EQ((**Seek).Value, uint64_t(Terminal));
#endif
}
TEST(DarwinNative, OriginalMemoryAndWriteContractsMatchHostKernel) {
#if defined(__APPLE__)
  static_assert(sizeof(struct iovec) == 16);
  static_assert(offsetof(struct iovec, iov_base) == 0);
  static_assert(offsetof(struct iovec, iov_len) == 8);
#endif
#ifndef NEVERD_DARWIN_NATIVE_ORACLE
#if defined(__APPLE__)
  if (std::getenv("NEVERD_REQUIRE_HVF"))
    FAIL() << "native Darwin reference executable is required";
#endif
  GTEST_SKIP() << "native Darwin reference requires a macOS build host";
#else
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-darwin-native", Temporary));
  const std::filesystem::path Root(Temporary.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  ASSERT_TRUE(
      std::filesystem::create_directories(Root / "catalogue" / "empty"));
  const std::string Program = NEVERD_DARWIN_NATIVE_ORACLE;
  const auto Input = (Root / "catalogue" / "data").string();
  struct Case {
    const char *Mode;
    int Status;
    const char *Output;
  };
  constexpr Case Cases[] = {
#define NEVERD_DARWIN_NATIVE_CASE(Mode, Status, Output) {Mode, Status, Output},
#include "fixtures/DarwinNativeCases.def"
#undef NEVERD_DARWIN_NATIVE_CASE
  };
  static_assert(std::size(Cases) != 0);
  for (const auto &Test : Cases) {
    SCOPED_TRACE(Test.Mode);
    {
      std::ofstream File(Input, std::ios::binary | std::ios::trunc);
      File << "0123456789";
      ASSERT_TRUE(File.good());
    }
    // ExecuteAndWait does not truncate an existing redirection target on
    // every host. Keep each observation separate, including shorter outputs.
    const auto Output = (Root / (std::string(Test.Mode) + ".stdout")).string();
    const auto Error = (Root / (std::string(Test.Mode) + ".stderr")).string();
    const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                        Error};
    std::string LaunchError;
    bool ExecutionFailed = false;
    const auto Status = llvm::sys::ExecuteAndWait(
        Program, {Program, Test.Mode, Input}, std::nullopt, Redirects, 5, 0,
        &LaunchError, &ExecutionFailed);
    ASSERT_FALSE(ExecutionFailed) << LaunchError;
    auto Out = llvm::MemoryBuffer::getFile(Output);
    ASSERT_TRUE(bool(Out));
    auto Err = llvm::MemoryBuffer::getFile(Error);
    ASSERT_TRUE(bool(Err));
    EXPECT_EQ(Status, Test.Status) << LaunchError << (*Err)->getBuffer().str();
    EXPECT_TRUE((*Err)->getBuffer().empty());
    EXPECT_EQ((*Out)->getBuffer(), Test.Output);
  }
#endif
}
} // namespace
} // namespace neverd::emulation
