//===- ProjectionCacheTests.cpp - ISA and root cache ownership -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "arch/aarch64/AArch64Machine.h"
#include "arch/x86_64/X64Machine.h"
#include "core/ExecutionDiagnostics.h"
#include "core/MemoryProjection.h"
#include "gtest/gtest.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation {
namespace {
#define NEVERD_MEMORY_TEST_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "MemoryLifecycleCases.def"
#undef NEVERD_MEMORY_TEST_VALUE

TEST(ProjectionCache, X64RootSurvivesAChangeOfCaller) {
  auto Memory = llvm::cantFail(MemoryProjection::create(Limit));
  const auto First = llvm::cantFail(buildX64PageTables(*Memory, false, true));
  ASSERT_NE(First, 0u);
  // Initialization and checked execution share one private projection, but
  // they do not share a caller-local cached root.
  const auto Second = llvm::cantFail(buildX64PageTables(*Memory, false, true));
  EXPECT_EQ(Second, First);
}
TEST(ProjectionCache, ArmLeavesRemainUnguardedAcrossPermissionsAndAliases) {
  auto Memory = llvm::cantFail(MemoryProjection::create(Limit));
  llvm::cantFail(Memory->map(Code, PageSize, Read | Write | Execute));
  constexpr uint64_t Alias = 0xffff000000400000;
  llvm::cantFail(Memory->aliases(
      {}, {{Alias, Code, PageSize, Read | Execute | UserAccessible}}));
  // Independently walk four-level 4 KiB tables. GP is bit 50 in a leaf.
  auto Leaf = [&](uint64_t VA) {
    uint64_t Table = VA >> 48 ? aarch64::HighRoot : aarch64::LowRoot;
    uint64_t Entry = 0;
    for (unsigned Shift : {39u, 30u, 21u, 12u}) {
      Entry = llvm::support::endian::read64le(Memory->data() + Table +
                                              ((VA >> Shift) & 511) * 8);
      EXPECT_EQ(Entry & 3, 3u);
      Table = Entry & 0x0000fffffffff000ULL;
    }
    return Entry;
  };
  for (bool User : {false, true})
    for (unsigned Permissions :
         {unsigned(Read), unsigned(Read | Write), unsigned(Read | Execute),
          unsigned(Read | Write | Execute)}) {
      llvm::cantFail(
          Memory->protect(Code, PageSize, Permissions | UserAccessible));
      llvm::cantFail(buildAArch64PageTables(*Memory, User));
      for (uint64_t VA : {Code, Alias, aarch64::VectorGPA, aarch64::EntryGPA})
        EXPECT_EQ(Leaf(VA) & (1ULL << 50), 0u);
    }
}
TEST(ProjectionCache, ArmThenX64CannotReuseAnotherISATables) {
  auto Memory = llvm::cantFail(MemoryProjection::create(Limit));
  llvm::cantFail(buildAArch64PageTables(*Memory));
  const auto Root = llvm::cantFail(buildX64PageTables(*Memory));
  EXPECT_NE(Root, 0u);
}
TEST(ProjectionCache, X64ThenArmCannotReuseAnotherISATables) {
  auto Memory = llvm::cantFail(MemoryProjection::create(Limit));
  llvm::cantFail(buildX64PageTables(*Memory));
  llvm::cantFail(buildAArch64PageTables(*Memory));
  EXPECT_EQ(
      llvm::support::endian::read32le(Memory->data() + aarch64::VectorGPA),
      aarch64::GatewayHypercall);
}
TEST(ProjectionCache, X64RootHistorySurvivesAnInterveningISAProjection) {
  auto Memory = llvm::cantFail(MemoryProjection::create(Limit));
  const auto First = llvm::cantFail(buildX64PageTables(*Memory));
  llvm::cantFail(buildAArch64PageTables(*Memory));
  const auto Next = llvm::cantFail(buildX64PageTables(*Memory));
  EXPECT_NE(Next, First);
  EXPECT_EQ(llvm::cantFail(buildX64PageTables(*Memory)), Next);
}
TEST(ProjectionCache, PrivilegeAndMonitorChangesRequireNewRoots) {
  auto Memory = llvm::cantFail(MemoryProjection::create(Limit));
  auto Previous = llvm::cantFail(buildX64PageTables(*Memory));
  for (auto [UserMode, Monitor] :
       {std::pair{true, false}, {true, true}, {false, true}, {false, false}}) {
    const auto Next =
        llvm::cantFail(buildX64PageTables(*Memory, UserMode, Monitor));
    EXPECT_NE(Next, Previous);
    EXPECT_EQ(llvm::cantFail(buildX64PageTables(*Memory, UserMode, Monitor)),
              Next);
    Previous = Next;
  }
}
TEST(ProjectionCache, MappingAndProtectionChangesInvalidateTheCurrentRoot) {
  auto Memory = llvm::cantFail(MemoryProjection::create(Limit));
  const auto Empty = llvm::cantFail(buildX64PageTables(*Memory));
  llvm::cantFail(Memory->map(Code, PageSize, Read | Write | Execute));
  const auto Mapped = llvm::cantFail(buildX64PageTables(*Memory));
  EXPECT_NE(Mapped, Empty);
  llvm::cantFail(Memory->protect(Code, PageSize, Read));
  const auto Protected = llvm::cantFail(buildX64PageTables(*Memory));
  EXPECT_NE(Protected, Mapped);
  EXPECT_EQ(llvm::cantFail(buildX64PageTables(*Memory)), Protected);
}
TEST(ProjectionCache, AddressSpaceIdentityMattersWithMatchingGenerations) {
  auto RAM = llvm::cantFail(PhysicalMemory::create(Limit));
  auto First = llvm::cantFail(AddressSpace::create(RAM, Limit));
  auto Second = llvm::cantFail(AddressSpace::create(RAM, Limit));
  llvm::cantFail(First->map(Code, PageSize, Read | Execute));
  llvm::cantFail(Second->map(Code, PageSize, Read | Execute));
  ASSERT_EQ(First->mappingGeneration(), Second->mappingGeneration());
  auto Memory = llvm::cantFail(MemoryProjection::create(First));
  const auto FirstRoot = llvm::cantFail(buildX64PageTables(*Memory));
  llvm::cantFail(Memory->bind(Second, x64::canonicalRange));
  const auto SecondRoot = llvm::cantFail(buildX64PageTables(*Memory));
  EXPECT_NE(SecondRoot, FirstRoot);
  EXPECT_EQ(llvm::cantFail(buildX64PageTables(*Memory)), SecondRoot);
  llvm::cantFail(Memory->bind(First, x64::canonicalRange));
  EXPECT_NE(llvm::cantFail(buildX64PageTables(*Memory)), SecondRoot);
}
TEST(ProjectionCache, FailedReplacementCannotRevalidatePartiallyWrittenTables) {
  for (auto FirstISA : {GuestArchitecture::X64, GuestArchitecture::AArch64}) {
    SCOPED_TRACE(unsigned(FirstISA));
    auto RAM = llvm::cantFail(PhysicalMemory::create(Limit));
    auto Healthy = llvm::cantFail(AddressSpace::create(RAM, Limit));
    auto Sparse =
        llvm::cantFail(AddressSpace::create(RAM, (SparsePages + 1) * PageSize));
    auto Page = llvm::cantFail(RAM->allocate(PageSize));
    llvm::cantFail(Healthy->mapRegion(Code, Page, 0, PageSize, Read | Execute));
    for (uint64_t N = 1; N <= SparsePages; ++N)
      llvm::cantFail(
          Sparse->mapRegion(N * SparseStride, Page, 0, PageSize, Read | Write));
    auto Memory = llvm::cantFail(MemoryProjection::create(Healthy));
    const auto Build = [&](GuestArchitecture ISA) -> llvm::Error {
      if (ISA == GuestArchitecture::AArch64)
        return buildAArch64PageTables(*Memory);
      auto Root = buildX64PageTables(*Memory);
      return Root ? llvm::Error::success() : Root.takeError();
    };
    llvm::cantFail(Build(FirstISA));
    llvm::cantFail(Memory->bind(Sparse, x64::canonicalRange));
    const auto NextISA = FirstISA == GuestArchitecture::X64
                             ? GuestArchitecture::AArch64
                             : GuestArchitecture::X64;
    EXPECT_EQ(llvm::toString(Build(NextISA)), diagnostic::PageTables);
    llvm::cantFail(Memory->bind(Healthy, x64::canonicalRange));
    EXPECT_TRUE(Memory->needsProjection(FirstISA));
    llvm::cantFail(Build(FirstISA));
    EXPECT_FALSE(Memory->needsProjection(FirstISA));
    if (FirstISA == GuestArchitecture::AArch64)
      EXPECT_EQ(
          llvm::support::endian::read32le(Memory->data() + aarch64::VectorGPA),
          aarch64::GatewayHypercall);
  }
}
} // namespace
} // namespace neverd::emulation
