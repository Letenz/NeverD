//===- WhpMemoryTests.cpp - WHP memory registration boundaries -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#if defined(_WIN32) && defined(NEVERD_EMULATION_WHP)
#include "backends/whp/WhpPartition.h"
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
    WhpPartition Partition;
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

#if defined(_M_X64) || defined(__x86_64__)
/// Isolate the host mapping API from startup XSAVE, page tables and instruction
/// execution. A mapping failure must remain visible even if CPU startup fails.
class MappingPartition final : public WhpPartition {
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
TEST_P(WHPMapping, TwoLivePartitionsRegisterAndRetireBacking) {
  const auto &Case = GetParam();
  llvm::sys::MemoryBlock FirstRAM, SecondRAM;
  auto Release = llvm::scope_exit([&] {
    (void)llvm::sys::Memory::releaseMappedMemory(FirstRAM);
    (void)llvm::sys::Memory::releaseMappedMemory(SecondRAM);
  });
  // Retire the partitions before releasing their registered host bytes.
  auto First = std::make_unique<MappingPartition>();
  MappingPartition Second;
  if (auto E = First->initialize()) {
    const bool Unavailable = E.isA<BackendUnavailableError>();
    const auto Text = llvm::toString(std::move(E));
    if (Unavailable && !std::getenv(RequireNative))
      GTEST_SKIP() << Text;
    FAIL() << Text;
  }
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
  ASSERT_EQ(llvm::toString(First->map(FirstRAM.base(), Case.Size)), "");
  if (Case.CPU)
    ASSERT_EQ(llvm::toString(First->createCPU()), "");
  ASSERT_EQ(llvm::toString(Second.initialize()), "");
  ASSERT_EQ(llvm::toString(Second.map(Other, Case.Size)), "");
  if (Case.CPU)
    ASSERT_EQ(llvm::toString(Second.createCPU()), "");
  First.reset();
  // The surviving partition and host RAM remain usable after peer retirement.
  ASSERT_EQ(llvm::toString(Second.map(Other, Case.Size)), "");
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
