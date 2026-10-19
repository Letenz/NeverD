//===- ByteScanTests.cpp - Bounded text acceleration contracts
//-------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Checks byte predicates, dispatch targets and inaccessible buffer tails.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/support/ByteScan.h"

#include <array>
#include <string>
#include <vector>

#if NEVERD_TEST_HIGHWAY
#include "hwy/targets.h"
#endif
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

using neverd::strings::detail::textASCIIPrefix;

namespace {
size_t referencePrefix(const uint8_t *Data, size_t Size) {
  size_t I = 0;
  for (; I < Size; ++I)
    if (!(Data[I] >= 32 && Data[I] <= 126) && Data[I] != 9 && Data[I] != 10 &&
        Data[I] != 13)
      break;
  return I;
}

void checkPrefixTargets() {
  std::array<uint8_t, 384> Data;
  for (unsigned Byte = 0; Byte != 256; ++Byte) {
    for (size_t Pos = 0; Pos != 192; ++Pos) {
      Data.fill('a');
      Data[Pos] = Byte;
      EXPECT_EQ(textASCIIPrefix(Data.data(), Data.size()),
                referencePrefix(Data.data(), Data.size()))
          << Byte << ' ' << Pos;
    }
  }
  for (size_t Offset = 0; Offset != 64; ++Offset)
    for (size_t Size = 0; Size != 256; ++Size) {
      for (size_t I = 0; I != Data.size(); ++I)
        Data[I] = static_cast<uint8_t>((I * 37 + Size * 13) & 255);
      EXPECT_EQ(textASCIIPrefix(Data.data() + Offset, Size),
                referencePrefix(Data.data() + Offset, Size));
    }
}
} // namespace

TEST(ByteScan, EveryByteLaneAndAlignment) {
#if NEVERD_TEST_HIGHWAY
  const auto Targets = hwy::SupportedAndGeneratedTargets();
  struct Reset {
    ~Reset() { hwy::SetSupportedTargetsForTest(0); }
  } Restore;
  ASSERT_FALSE(Targets.empty());
  for (const auto Target : Targets) {
    SCOPED_TRACE(hwy::TargetName(Target));
    hwy::SetSupportedTargetsForTest(Target);
    checkPrefixTargets();
  }
#else
  checkPrefixTargets();
#endif
}

TEST(ByteScan, EmptyAndGuardedTails) {
  EXPECT_EQ(textASCIIPrefix(nullptr, 0), 0u);
#ifdef _WIN32
  SYSTEM_INFO Info{};
  GetSystemInfo(&Info);
  const size_t Page = Info.dwPageSize;
  auto *Memory = static_cast<uint8_t *>(VirtualAlloc(
      nullptr, Page * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  ASSERT_NE(Memory, nullptr);
  DWORD Old = 0;
  ASSERT_TRUE(VirtualProtect(Memory + Page, Page, PAGE_NOACCESS, &Old));
#else
  const size_t Page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  auto *Memory =
      static_cast<uint8_t *>(mmap(nullptr, Page * 2, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANON, -1, 0));
  ASSERT_NE(Memory, MAP_FAILED);
  ASSERT_EQ(mprotect(Memory + Page, Page, PROT_NONE), 0);
#endif
  std::fill(Memory, Memory + Page, 'x');
#if NEVERD_TEST_HIGHWAY
  struct Reset {
    ~Reset() { hwy::SetSupportedTargetsForTest(0); }
  } Restore;
  for (const auto Target : hwy::SupportedAndGeneratedTargets()) {
    SCOPED_TRACE(hwy::TargetName(Target));
    hwy::SetSupportedTargetsForTest(Target);
#endif
    for (size_t Size = 0; Size <= 256; ++Size) {
      const auto *At = Memory + Page - Size;
      EXPECT_EQ(textASCIIPrefix(At, Size), Size);
      EXPECT_EQ(
          neverd::knownValidUTF8({reinterpret_cast<const char *>(At), Size}),
          static_cast<bool>(NEVERD_TEST_SIMDUTF));
    }
#if NEVERD_TEST_HIGHWAY
  }
#endif
#ifdef _WIN32
  EXPECT_TRUE(VirtualFree(Memory, 0, MEM_RELEASE));
#else
  EXPECT_EQ(munmap(Memory, Page * 2), 0);
#endif
}

TEST(ByteScan, UnicodeValidationDoesNotRepairOrNormalize) {
  const std::vector<std::string> Valid = {"",
                                          "ASCII",
                                          std::string("a\0b", 3),
                                          "\xc2\xa0",
                                          "\xe4\xb8\xad",
                                          "\xef\xbb\xbf",
                                          "\xef\xbf\xbf",
                                          "\xf4\x8f\xbf\xbf"};
  for (const auto &Value : Valid)
    for (size_t Prefix = 0; Prefix < 65; ++Prefix) {
      const auto Text = std::string(Prefix, 'x') + Value;
      EXPECT_EQ(neverd::knownValidUTF8(Text),
                static_cast<bool>(NEVERD_TEST_SIMDUTF));
    }
  const std::vector<std::string> Invalid = {"\x80",
                                            "\xc0\x80",
                                            "\xc1\xbf",
                                            "\xc2",
                                            "\xe0\x80\x80",
                                            "\xed\xa0\x80",
                                            "\xe1\x80",
                                            "\xf0\x80\x80\x80",
                                            "\xf4\x90\x80\x80",
                                            "\xf5\x80\x80\x80",
                                            "\xf1\x80\x80",
                                            "\xff"};
  for (const auto &Value : Invalid)
    for (size_t Prefix = 0; Prefix < 65; ++Prefix)
      EXPECT_FALSE(neverd::knownValidUTF8(std::string(Prefix, 'x') + Value));
  for (unsigned Byte = 0; Byte < 256; ++Byte) {
    const std::string Value(1, static_cast<char>(Byte));
    EXPECT_EQ(neverd::knownValidUTF8(Value), NEVERD_TEST_SIMDUTF && Byte < 128);
  }
}
