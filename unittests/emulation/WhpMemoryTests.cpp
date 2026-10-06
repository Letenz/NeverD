//===- WhpMemoryTests.cpp - WHP memory registration boundaries -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#if defined(_WIN32) && defined(NEVERD_EMULATION_WHP)
#include "backends/whp/WhpResourceCache.h"
#include "backends/whp/WhpVirtualProcessor.h"
#include "core/MemoryLayout.h"
#include "gtest/gtest.h"

#include "llvm/ADT/ScopeExit.h"

#include <cstdlib>
#include <cstring>

namespace neverd::emulation {
namespace {
#define NEVERD_WHP_MEMORY_TEXT(Name, Text) constexpr char Name[] = Text;
#define NEVERD_WHP_MEMORY_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "WhpMemoryCases.def"
#undef NEVERD_WHP_MEMORY_VALUE
#undef NEVERD_WHP_MEMORY_TEXT

struct FailedRegistration {
  unsigned FailAt = 0, Calls = 0;
  static HRESULT WINAPI map(WHV_PARTITION_HANDLE Handle, VOID *,
                            WHV_GUEST_PHYSICAL_ADDRESS, UINT64,
                            WHV_MAP_GPA_RANGE_FLAGS) {
    auto &Self = *static_cast<FailedRegistration *>(Handle);
    return Self.Calls++ == Self.FailAt ? E_FAIL : S_OK;
  }
  static HRESULT WINAPI destroy(WHV_PARTITION_HANDLE) { return S_OK; }
};
TEST(WhpMemoryProtocol, FailureRetainsStatusAndExactRegistration) {
  auto Memory = llvm::cantFail(MemoryProjection::create(RAMBytes));
  const auto Registrations = Memory->registrations();
  for (unsigned FailAt = 0; FailAt < Registrations.size(); ++FailAt) {
    FailedRegistration Target{FailAt};
    WhpVirtualProcessor Partition;
    Partition.Partition = &Target;
    Partition.API.WHvMapGpaRange = FailedRegistration::map;
    Partition.API.WHvDeletePartition = FailedRegistration::destroy;
    const auto &Expected = Registrations[FailAt];
    EXPECT_EQ(llvm::toString(Partition.mapMemory(*Memory)),
              llvm::formatv(MapFailure, diagnostic::WhpMap, uint32_t(E_FAIL),
                            Expected.Physical, Expected.Size)
                  .str());
    EXPECT_EQ(Target.Calls, FailAt + 1);
  }
}
TEST(WhpMemoryProtocol,
     SharedHostRetiresBeforeReplacementAndRetriesFailedSetup) {
  struct Lifetime {
    unsigned Live = 0, Created = 0, Destroyed = 0;
    static HRESULT WINAPI destroy(WHV_PARTITION_HANDLE Handle) {
      auto &Self = *static_cast<Lifetime *>(Handle);
      --Self.Live;
      ++Self.Destroyed;
      return S_OK;
    }
  } Stats;
  bool Fail = false;
  auto Configure = [&](WhpAPI &API,
                       WHV_PARTITION_HANDLE &Handle) -> llvm::Error {
    EXPECT_EQ(Stats.Live, 0u);
    ++Stats.Live;
    ++Stats.Created;
    API.WHvDeletePartition = Lifetime::destroy;
    Handle = &Stats;
    return Fail ? diagnostic::error(diagnostic::WhpCreate)
                : llvm::Error::success();
  };
  std::lock_guard Lock(WhpPartitionHost::mutex());
  auto First = llvm::cantFail(WhpPartitionHost::acquire(Configure));
  First->Processors.set(0);
  auto Second = llvm::cantFail(WhpPartitionHost::acquire(Configure));
  Second->Processors.set(1);
  EXPECT_EQ(First, Second);
  EXPECT_EQ(Stats.Created, 1u);
  First->Processors.reset(0);
  WhpPartitionHost::retireIdle(First);
  EXPECT_EQ(Stats.Live, 1u);
  Second->Processors.reset(1);
  WhpPartitionHost::retireIdle(Second);
  EXPECT_EQ(Stats.Live, 0u);
  Fail = true;
  auto Rejected = WhpPartitionHost::acquire(Configure);
  ASSERT_FALSE(bool(Rejected));
  EXPECT_EQ(llvm::toString(Rejected.takeError()), diagnostic::WhpCreate);
  EXPECT_EQ(Stats.Live, 0u);
  Fail = false;
  auto Replacement = llvm::cantFail(WhpPartitionHost::acquire(Configure));
  WhpPartitionHost::retireIdle(Replacement);
  EXPECT_EQ(Stats.Created, 3u);
  EXPECT_EQ(Stats.Created, Stats.Destroyed);
}

#if defined(_M_X64) || defined(__x86_64__)
/// Isolate the host mapping API from startup XSAVE, page tables and instruction
/// execution. A mapping failure must remain visible even if CPU startup fails.
class MappingPartition final : public WhpVirtualProcessor {
public:
  llvm::Error initialize() {
    if (auto E = API.load())
      return E;
    WHV_CAPABILITY Capability{};
    const auto Status =
        API.WHvGetCapability(WHvCapabilityCodeHypervisorPresent, &Capability,
                             sizeof(Capability), nullptr);
    if (FAILED(Status))
      return whpError(diagnostic::WhpCapability, Status);
    if (!Capability.HypervisorPresent)
      return diagnostic::unavailable(diagnostic::WhpCapability,
                                     BackendAvailability::MissingCapability);
    if (const auto Status = API.WHvCreatePartition(&Partition); FAILED(Status))
      return whpError(diagnostic::WhpCreate, Status);
    WHV_PARTITION_PROPERTY Property{};
    Property.ProcessorCount = 1;
    if (const auto Status = API.WHvSetPartitionProperty(
            Partition, WHvPartitionPropertyCodeProcessorCount, &Property,
            sizeof(Property));
        FAILED(Status))
      return whpError(diagnostic::WhpCreate, Status);
    const auto Setup = API.WHvSetupPartition(Partition);
    return FAILED(Setup) ? whpError(diagnostic::WhpCreate, Setup)
                         : llvm::Error::success();
  }
  llvm::Error map(void *Backing, uint64_t Size) {
    const auto Status =
        API.WHvMapGpaRange(Partition, Backing, 0, Size,
                           WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite |
                               WHvMapGpaRangeFlagExecute);
    return FAILED(Status) ? whpError(diagnostic::WhpMap, Status)
                          : llvm::Error::success();
  }
  llvm::Error createCPU() {
    const auto Status = API.WHvCreateVirtualProcessor(Partition, 0, 0);
    return FAILED(Status) ? whpError(diagnostic::WhpCreate, Status)
                          : llvm::Error::success();
  }
};
struct NativeMemoryCase {
  const char *Name;
  uint64_t Size;
  bool Shared, Prefault, CPU;
};
void PrintTo(const NativeMemoryCase &Case, std::ostream *OS) {
  *OS << Case.Name;
}
const NativeMemoryCase Cases[] = {
#define NEVERD_WHP_MEMORY_CASE(Name, Size, Shared, Prefault, CPU)              \
  {#Name, Size, Shared, Prefault, CPU},
#include "WhpMemoryCases.def"
#undef NEVERD_WHP_MEMORY_CASE
};
class WHPMapping : public testing::TestWithParam<NativeMemoryCase> {};
TEST_P(WHPMapping, TwoLogicalOwnersRegisterSwitchAndRetireBacking) {
  const auto &Case = GetParam();
  llvm::sys::MemoryBlock FirstRAM, SecondRAM;
  auto Release = llvm::scope_exit([&] {
    (void)llvm::sys::Memory::releaseMappedMemory(FirstRAM);
    (void)llvm::sys::Memory::releaseMappedMemory(SecondRAM);
  });
  std::error_code EC;
  FirstRAM = llvm::sys::Memory::allocateMappedMemory(
      Case.Size, nullptr,
      llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE, EC);
  ASSERT_FALSE(EC) << EC.message();
  if (!Case.Shared) {
    SecondRAM = llvm::sys::Memory::allocateMappedMemory(
        Case.Size, nullptr,
        llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE, EC);
    ASSERT_FALSE(EC) << EC.message();
  }
  auto *Other = Case.Shared ? FirstRAM.base() : SecondRAM.base();
  if (Case.Prefault) {
    std::memset(FirstRAM.base(), Fill, Case.Size);
    std::memset(Other, Fill, Case.Size);
  }
  using Binding = WhpResourceBinding<WhpVirtualProcessor>;
  auto Bind = [&](void *Backing, unsigned &Creations) {
    return std::make_unique<Binding>(
        [&, Backing]() -> llvm::Expected<std::unique_ptr<WhpVirtualProcessor>> {
          auto P = std::make_unique<MappingPartition>();
          if (auto E = P->initialize())
            return E;
          if (auto E = P->map(Backing, Case.Size))
            return E;
          if (Case.CPU)
            if (auto E = P->createCPU())
              return E;
          ++Creations;
          return std::unique_ptr<WhpVirtualProcessor>(std::move(P));
        });
  };
  // Bindings retire native mappings before their borrowed host bytes. Both
  // logical owners stay live, but the cache never overlaps their partitions.
  unsigned FirstCreations = 0, SecondCreations = 0;
  auto First = Bind(FirstRAM.base(), FirstCreations);
  auto Second = Bind(Other, SecondCreations);
  const MachineRunControl Control{std::chrono::steady_clock::time_point::max()};
  {
    auto Active = First->acquire(Control);
    if (!Active) {
      auto E = Active.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Text = llvm::toString(std::move(E));
      if (Unavailable && !std::getenv(RequireNative))
        GTEST_SKIP() << Text;
      FAIL() << Text;
    }
  }
  for (auto *Owner : {Second.get(), First.get(), Second.get()}) {
    auto Active = Owner->acquire(Control);
    ASSERT_TRUE(bool(Active)) << llvm::toString(Active.takeError());
  }
  EXPECT_EQ(FirstCreations, 2u);
  EXPECT_EQ(SecondCreations, 2u);
  First.reset();
  // Retiring an inactive peer must keep the current partition hot and usable.
  auto Active = Second->acquire(Control);
  ASSERT_TRUE(bool(Active)) << llvm::toString(Active.takeError());
  EXPECT_EQ(SecondCreations, 2u);
  ASSERT_EQ(llvm::toString(static_cast<MappingPartition &>(**Active).map(
                Other, Case.Size)),
            "");
  const auto *Bytes = static_cast<const uint8_t *>(Other);
  EXPECT_EQ(Bytes[0], Case.Prefault ? Fill : 0);
  EXPECT_EQ(Bytes[Case.Size - 1], Case.Prefault ? Fill : 0);
}
INSTANTIATE_TEST_SUITE_P(
    NativeMemory, WHPMapping, testing::ValuesIn(Cases),
    [](const testing::TestParamInfo<NativeMemoryCase> &Info) {
      return Info.param.Name;
    });
#endif
} // namespace
} // namespace neverd::emulation
#endif
