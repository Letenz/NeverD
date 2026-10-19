//===- AllocatorTests.cpp - Acceleration allocator coexistence
//-------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Checks process allocation and the absence of oneTBB allocator libraries.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include <cstdlib>
#include <string_view>

#if NEVERD_TEST_MIMALLOC
#include "mimalloc.h"
#endif
#if NEVERD_USE_TBB
#include "oneapi/tbb/tbb_allocator.h"
#endif
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <link.h>
#endif

TEST(AccelerationAllocator, RetainsConfiguredProcessAllocator) {
  auto *Memory = std::malloc(193);
  ASSERT_NE(Memory, nullptr);
#if NEVERD_TEST_MIMALLOC
  EXPECT_EQ(mi_version(), MI_MALLOC_VERSION);
  EXPECT_TRUE(mi_is_in_heap_region(Memory));
#endif
  std::free(Memory);
  auto *Object = new unsigned(47);
  EXPECT_EQ(*Object, 47u);
#if NEVERD_TEST_MIMALLOC
  EXPECT_TRUE(mi_is_in_heap_region(Object));
#endif
  delete Object;
}

TEST(AccelerationAllocator, DoesNotLoadTBBAllocators) {
#if NEVERD_USE_TBB
  using Allocator = oneapi::tbb::tbb_allocator<unsigned>;
  EXPECT_EQ(Allocator::allocator_type(), Allocator::standard);
  Allocator A;
  auto *Memory = A.allocate(47);
#if NEVERD_TEST_MIMALLOC
  EXPECT_TRUE(mi_is_in_heap_region(Memory));
#endif
  A.deallocate(Memory, 47);
#endif
#ifdef _WIN32
  EXPECT_EQ(GetModuleHandleA("tbbmalloc.dll"), nullptr);
  EXPECT_EQ(GetModuleHandleA("tbbmalloc_debug.dll"), nullptr);
  EXPECT_EQ(GetModuleHandleA("tbbmalloc_proxy.dll"), nullptr);
  EXPECT_EQ(GetModuleHandleA("tbbmalloc_proxy_debug.dll"), nullptr);
#elif defined(__APPLE__)
  for (uint32_t I = 0; I < _dyld_image_count(); ++I)
    EXPECT_EQ(std::string_view(_dyld_get_image_name(I)).find("tbbmalloc"),
              std::string_view::npos);
#elif defined(__linux__)
  dl_iterate_phdr(
      [](dl_phdr_info *Info, size_t, void *) {
        EXPECT_EQ(std::string_view(Info->dlpi_name).find("tbbmalloc"),
                  std::string_view::npos);
        return 0;
      },
      nullptr);
#endif
}
