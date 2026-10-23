//===- DyldSharedCacheTests.cpp - Cache address and slide contracts ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent serialized fixtures exercise cache-relative targets, subcache
/// identity and malformed-input boundaries without copying parser constants.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/MachO/DyldSharedCache.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>

using namespace neverd;
using namespace neverd::macho_loader;

namespace {

constexpr va_t Base = 0x180000000;

struct CacheFixture {
  std::vector<uint8_t> Data = std::vector<uint8_t>(0x5000);

  template <typename T> void set(size_t Offset, T Value) {
    writeLE<T>(Data.data() + Offset, Value);
  }

  explicit CacheFixture(uint32_t Version = 5, va_t Address = Base) {
    std::memcpy(Data.data(), "dyld_v1  arm64e", 16);
    set<uint32_t>(16, 0x200);
    set<uint32_t>(20, 2);
    Data[88] = Address == Base ? 1 : 2;
    set<uint64_t>(224, Address);
    set<uint64_t>(232, Address == Base ? 0x20000 : 0);
    set<uint32_t>(312, 0x280);
    set<uint32_t>(316, 2);
    set<uint32_t>(448, 0x300);
    set<uint32_t>(452, 1);
    for (size_t I = 0; I != 2; ++I) {
      const uint64_t VA = Address + (I ? 0x2000 : 0);
      const uint64_t Size = I ? 0x3000 : 0x1000;
      const uint64_t Offset = I ? 0x1000 : 0;
      const uint32_t Protection = I ? 3 : 5;
      for (size_t Row : {0x200 + I * 32, 0x280 + I * 56}) {
        set<uint64_t>(Row, VA);
        set<uint64_t>(Row + 8, Size);
        set<uint64_t>(Row + 16, Offset);
      }
      set<uint32_t>(0x200 + I * 32 + 24, Protection);
      set<uint32_t>(0x200 + I * 32 + 28, Protection);
      set<uint32_t>(0x280 + I * 56 + 48, Protection);
      set<uint32_t>(0x280 + I * 56 + 52, Protection);
    }
    set<uint64_t>(0x280 + 56 + 24, 0x4400);
    set<uint64_t>(0x280 + 56 + 32, 30);
    set<uint64_t>(0x300, Address + 0x600);
    set<uint32_t>(0x300 + 24, 0x340);
    std::memcpy(Data.data() + 0x340, "/usr/lib/libfixture.dylib", 25);
    set<uint32_t>(0x600, 0xfeedfacf);
    set<uint32_t>(0x604, 0x100000c);
    set<uint32_t>(0x608, 2);
    set<uint32_t>(0x4400, Version);
    set<uint32_t>(0x4404, 0x1000);
    set<uint32_t>(0x4408, 3);
    set<uint64_t>(0x4410, Base);
    set<uint16_t>(0x4418, 8);
    set<uint16_t>(0x441a, 0xffff);
    set<uint16_t>(0x441c, 0);
    if (Version == 3) {
      set<uint64_t>(0x1008, (2ull << 51) | (0xabull << 43) | (Base + 0x80));
      set<uint64_t>(0x1018, (1ull << 63) | (3ull << 49) | (1ull << 48) |
                                (0xbeefull << 32) | 0x700);
      set<uint64_t>(0x3000, Base + 0x90);
    } else {
      set<uint64_t>(0x1008, (2ull << 52) | (0xabull << 34) | 0x80);
      set<uint64_t>(0x1018, (1ull << 63) | (1ull << 51) | (1ull << 50) |
                                (0xbeefull << 34) | 0x700);
      set<uint64_t>(0x3000, 0x90);
    }
  }

  std::unique_ptr<llvm::MemoryBuffer> buffer() const {
    return llvm::MemoryBuffer::getMemBufferCopy(llvm::StringRef(
        reinterpret_cast<const char *>(Data.data()), Data.size()));
  }

  llvm::Expected<DyldSharedCache> parse() const {
    std::vector<std::unique_ptr<llvm::MemoryBuffer>> Files;
    Files.push_back(buffer());
    return DyldSharedCache::parse(std::move(Files));
  }

  void imageLayout(va_t DataAddress = Base + 0x2000) {
    set<uint32_t>(0x610, 2);
    set<uint32_t>(0x614, 224);
    set<uint32_t>(0x620, 0x19);
    set<uint32_t>(0x624, 152);
    std::memcpy(Data.data() + 0x628, "__TEXT", 7);
    set<uint64_t>(0x638, Base + 0x600);
    set<uint64_t>(0x640, 0x400);
    set<uint64_t>(0x648, 0x600);
    set<uint64_t>(0x650, 0x400);
    set<uint32_t>(0x658, 5);
    set<uint32_t>(0x65c, 5);
    set<uint32_t>(0x660, 1);
    std::memcpy(Data.data() + 0x668, "__text", 7);
    std::memcpy(Data.data() + 0x678, "__TEXT", 7);
    set<uint64_t>(0x688, Base + 0x900);
    set<uint64_t>(0x690, 0x10);
    set<uint32_t>(0x698, 0x900);
    set<uint32_t>(0x69c, 2);
    set<uint32_t>(0x6a8, 0x80000400);
    set<uint32_t>(0x6b8, 0x19);
    set<uint32_t>(0x6bc, 72);
    std::memcpy(Data.data() + 0x6c0, "__DATA", 7);
    set<uint64_t>(0x6d0, DataAddress);
    set<uint64_t>(0x6d8, 0x3000);
    set<uint64_t>(0x6e0, 0x1000);
    set<uint64_t>(0x6e8, 0x3000);
    set<uint32_t>(0x6f0, 3);
    set<uint32_t>(0x6f4, 3);
    Data[0x900] = 0x42;
  }

  void v2Slides() {
    set<uint64_t>(0x2d8, 64);
    set<uint32_t>(0x4400, 2);
    set<uint32_t>(0x4408, 40);
    set<uint32_t>(0x440c, 3);
    set<uint32_t>(0x4410, 46);
    set<uint32_t>(0x4414, 2);
    set<uint64_t>(0x4418, 0x00ffff0000000000ull);
    set<uint64_t>(0x4420, Base);
    set<uint16_t>(0x4428, 0x8000);
    set<uint16_t>(0x442a, 0x4000);
    set<uint16_t>(0x442c, 1);
    set<uint16_t>(0x442e, 2);
    set<uint16_t>(0x4430, 0x800a);
    set<uint64_t>(0x1008, (4ull << 40) | 0x80);
    set<uint64_t>(0x1018, 0);
    set<uint64_t>(0x1028, 0xab00000000000090ull);
    set<uint64_t>(0x3004, 0xa0);
  }
};

} // namespace

TEST(DyldSharedCache, MapsFilesAndRetainsBothSlideLayoutsWithoutPatching) {
  for (uint32_t Version : {3u, 5u}) {
    CacheFixture Fixture(Version);
    auto Cache = Fixture.parse();
    ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
    EXPECT_EQ(Cache->baseAddress(), Base);
    ASSERT_EQ(Cache->mappings().size(), 2u);
    ASSERT_EQ(Cache->images().size(), 1u);
    EXPECT_EQ(Cache->images()[0].Path, "/usr/lib/libfixture.dylib");
    EXPECT_EQ(Cache->images()[0].Address, Base + 0x600);
    EXPECT_FALSE(Cache->readBytes(Base + 0x1000, 1));
    EXPECT_FALSE(Cache->readBytes(Base + 0xfff, 2));
    EXPECT_FALSE(Cache->readBytes(Base + 0x4fff, 2));
    EXPECT_FALSE(Cache->readBytes(InvalidVA, 8));
    auto Fixups = Cache->readSlideFixups(1);
    ASSERT_TRUE(bool(Fixups)) << llvm::toString(Fixups.takeError());
    ASSERT_EQ(Fixups->size(), 3u);
    EXPECT_EQ((*Fixups)[0].Slot, Base + 0x2008);
    EXPECT_EQ((*Fixups)[0].Target, 0xab00000000000000ull | (Base + 0x80));
    EXPECT_FALSE((*Fixups)[0].Authentication);
    EXPECT_EQ((*Fixups)[1].Slot, Base + 0x2018);
    ASSERT_TRUE((*Fixups)[1].Authentication);
    const auto &Auth = *(*Fixups)[1].Authentication;
    EXPECT_EQ(Auth.Target, Base + 0x700);
    EXPECT_EQ(Auth.Key, Version == 3 ? 3u : 2u);
    EXPECT_EQ(Auth.Diversity, 0xbeef);
    EXPECT_TRUE(Auth.AddressDiversity);
    EXPECT_EQ(Auth.EncodedValue, (*Fixups)[1].EncodedValue);
    EXPECT_EQ((*Fixups)[2].Slot, Base + 0x4000);
    EXPECT_EQ((*Fixups)[2].Target, Base + 0x90);
    const auto Bytes = Cache->readBytes(Base + 0x2018, 8);
    ASSERT_TRUE(Bytes);
    EXPECT_EQ(readLE<uint64_t>(Bytes->data()), Auth.EncodedValue);
    auto Empty = Cache->readSlideFixups(0);
    ASSERT_TRUE(bool(Empty));
    EXPECT_TRUE(Empty->empty());
  }
}

TEST(DyldSharedCache, V2ExtraChainsRetainNullAndTaggedTargets) {
  CacheFixture Fixture;
  Fixture.v2Slides();
  auto Cache = Fixture.parse();
  ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
  auto Fixups = Cache->readSlideFixups(1);
  ASSERT_TRUE(bool(Fixups)) << llvm::toString(Fixups.takeError());
  ASSERT_EQ(Fixups->size(), 4u);
  const va_t Slots[] = {Base + 0x2008, Base + 0x2018, Base + 0x2028,
                        Base + 0x4004};
  const va_t Targets[] = {Base + 0x80, 0, 0xab00000000000000ull | (Base + 0x90),
                          Base + 0xa0};
  for (size_t I = 0; I != 4; ++I) {
    EXPECT_EQ((*Fixups)[I].Slot, Slots[I]);
    EXPECT_EQ((*Fixups)[I].Target, Targets[I]);
    EXPECT_EQ((*Fixups)[I].SlideVersion, 2u);
    EXPECT_FALSE((*Fixups)[I].Authentication);
    const auto Bytes = Cache->readBytes(Slots[I], 8);
    ASSERT_TRUE(Bytes);
    EXPECT_EQ(readLE<uint64_t>(Bytes->data()), (*Fixups)[I].EncodedValue);
  }
}

TEST(DyldSharedCache, V2MalformedExtraChainsFailTransactionally) {
  using Mutation = std::function<void(CacheFixture &)>;
  const std::vector<Mutation> Cases = {
      [](auto &F) { F.template set<uint64_t>(0x2d8, 39); },
      [](auto &F) { F.template set<uint32_t>(0x4408, 39); },
      [](auto &F) { F.template set<uint32_t>(0x440c, 4); },
      [](auto &F) { F.template set<uint32_t>(0x4410, 63); },
      [](auto &F) { F.template set<uint64_t>(0x4418, 0); },
      [](auto &F) { F.template set<uint64_t>(0x4418, 3); },
      [](auto &F) { F.template set<uint64_t>(0x4418, 0x500); },
      [](auto &F) { F.template set<uint16_t>(0x4428, 0x8002); },
      [](auto &F) { F.template set<uint16_t>(0x4430, 10); },
      [](auto &F) { F.template set<uint16_t>(0x4430, 0x8002); },
      [](auto &F) { F.template set<uint16_t>(0x4430, 0xc00a); },
      [](auto &F) { F.template set<uint16_t>(0x442c, 1023); },
      [](auto &F) { F.template set<uint64_t>(0x1008, (1ull << 40) | 0x80); },
      [](auto &F) { F.template set<uint64_t>(0x1008, (1024ull << 40) | 0x80); },
      [](auto &F) { F.template set<uint64_t>(0x4420, InvalidVA); },
  };
  for (size_t I = 0; I < Cases.size(); ++I) {
    SCOPED_TRACE(I);
    CacheFixture Fixture;
    Fixture.v2Slides();
    Cases[I](Fixture);
    auto Cache = Fixture.parse();
    ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
    auto Fixups = Cache->readSlideFixups(1);
    ASSERT_FALSE(bool(Fixups));
    llvm::consumeError(Fixups.takeError());
  }
}

TEST(DyldSharedCache, SubcacheTargetsUseTheMainCacheBase) {
  CacheFixture Main;
  CacheFixture Sub(5, Base + 0x10000);
  Main.set<uint32_t>(392, 0x380);
  Main.set<uint32_t>(396, 1);
  Main.Data[0x380] = 2;
  Main.set<uint64_t>(0x390, 0x10000);
  std::memcpy(Main.Data.data() + 0x398, ".01", 4);
  // An image header can reside in a different file from its catalogue.
  Main.set<uint64_t>(0x300, Base + 0x10600);
  auto Parse = [&]() {
    std::vector<std::unique_ptr<llvm::MemoryBuffer>> Files;
    Files.push_back(Main.buffer());
    Files.push_back(Sub.buffer());
    return DyldSharedCache::parse(std::move(Files));
  };
  auto Cache = Parse();
  ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
  ASSERT_EQ(Cache->mappings().size(), 4u);
  EXPECT_EQ(Cache->images()[0].Address, Base + 0x10600);
  auto Fixups = Cache->readSlideFixups(3);
  ASSERT_TRUE(bool(Fixups)) << llvm::toString(Fixups.takeError());
  EXPECT_EQ((*Fixups)[1].Slot, Base + 0x12018);
  EXPECT_EQ((*Fixups)[1].Target, Base + 0x700);
  Sub.Data[88] ^= 1;
  auto WrongUUID = Parse();
  ASSERT_FALSE(bool(WrongUUID));
  llvm::consumeError(WrongUUID.takeError());
  Sub.Data[88] ^= 1;
  Main.set<uint64_t>(0x390, 0x20000);
  auto WrongPlacement = Parse();
  ASSERT_FALSE(bool(WrongPlacement));
  llvm::consumeError(WrongPlacement.takeError());
  auto Missing = Main.parse();
  ASSERT_FALSE(bool(Missing));
  llvm::consumeError(Missing.takeError());
}

TEST(DyldSharedCache, LegacyHeadersUseTheOriginalTables) {
  CacheFixture Main(3);
  std::memcpy(Main.Data.data() + 0x80, Main.Data.data() + 0x200, 64);
  Main.set<uint32_t>(16, 0x80);
  Main.set<uint32_t>(24, 0x300);
  Main.set<uint32_t>(28, 1);
  Main.set<uint64_t>(56, 0x4400);
  Main.set<uint64_t>(64, 30);
  auto Cache = Main.parse();
  ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
  ASSERT_EQ(Cache->images().size(), 1u);
  auto Fixups = Cache->readSlideFixups(1);
  ASSERT_TRUE(bool(Fixups)) << llvm::toString(Fixups.takeError());
  ASSERT_EQ(Fixups->size(), 3u);
  EXPECT_EQ((*Fixups)[1].Authentication->Target, Base + 0x700);
}

TEST(DyldSharedCache, LegacySubcacheEntriesDoNotReadSuffixBytes) {
  CacheFixture Main;
  CacheFixture Sub(5, Base + 0x10000);
  std::memcpy(Main.Data.data() + 400, Main.Data.data() + 0x200, 64);
  Main.set<uint32_t>(16, 400);
  Main.set<uint32_t>(24, 0x300);
  Main.set<uint32_t>(28, 1);
  Main.set<uint32_t>(392, 0x380);
  Main.set<uint32_t>(396, 1);
  Main.Data[0x380] = 2;
  Main.set<uint64_t>(0x390, 0x10000);
  std::memset(Main.Data.data() + 0x398, 0xff, 32);
  std::vector<std::unique_ptr<llvm::MemoryBuffer>> Files;
  Files.push_back(Main.buffer());
  Files.push_back(Sub.buffer());
  auto Cache = DyldSharedCache::parse(std::move(Files));
  ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
  ASSERT_EQ(Cache->mappings().size(), 4u);
  auto Fixups = Cache->readSlideFixups(3);
  ASSERT_TRUE(bool(Fixups)) << llvm::toString(Fixups.takeError());
  EXPECT_EQ((*Fixups)[1].Authentication->Target, Base + 0x700);
}

TEST(DyldSharedCache, AllKeysAndDiversityBitsRetainTheirLayout) {
  for (uint32_t Version : {3u, 5u})
    for (unsigned Key = 0; Key < (Version == 3 ? 4u : 2u); ++Key)
      for (unsigned AddressDiversity = 0; AddressDiversity < 2;
           ++AddressDiversity)
        for (uint64_t Diversity : {0u, 1u, 0xffffu}) {
          CacheFixture F(Version);
          const uint64_t TargetOffset =
              Version == 3 ? 0xffffffffull : 0x3ffffffffull;
          const uint64_t Word =
              (1ull << 63) | TargetOffset |
              (uint64_t(Key) << (Version == 3 ? 49 : 51)) |
              (uint64_t(AddressDiversity) << (Version == 3 ? 48 : 50)) |
              (Diversity << (Version == 3 ? 32 : 34));
          F.set<uint64_t>(0x1018, Word);
          auto Cache = F.parse();
          ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
          auto Fixups = Cache->readSlideFixups(1);
          ASSERT_TRUE(bool(Fixups)) << llvm::toString(Fixups.takeError());
          const auto &Auth = (*Fixups)[1].Authentication;
          ASSERT_TRUE(Auth);
          EXPECT_EQ(Auth->Key, Version == 3 ? Key : Key * 2);
          EXPECT_EQ(Auth->Diversity, Diversity);
          EXPECT_EQ(Auth->AddressDiversity, bool(AddressDiversity));
          EXPECT_EQ(Auth->Target, Base + TargetOffset);
        }
}

TEST(DyldSharedCache, CancellationStopsBeforePublishingResults) {
  CacheFixture F;
  auto Cache = F.parse();
  ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
  unsigned Checks = 0;
  LoadProgress Progress;
  Progress.StopUser = &Checks;
  Progress.Stop = [](void *User) -> int {
    return ++*static_cast<unsigned *>(User) >= 3;
  };
  EXPECT_THROW(
      {
        auto Result = Cache->readSlideFixups(1, Progress);
        if (!Result)
          llvm::consumeError(Result.takeError());
      },
      LoadCancelled);
}

TEST(DyldSharedCache, FileLoadingFollowsValidatedSuffixAndUUID) {
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-dyld-cache", Directory));
  const auto Cleanup = llvm::scope_exit(
      [&] { (void)llvm::sys::fs::remove_directories(Directory); });
  const std::filesystem::path Path =
      std::filesystem::path(std::u8string(Directory.begin(), Directory.end())) /
      "cache";
  auto Subpath = Path;
  Subpath += ".01";
  const auto Write = [](const std::filesystem::path &Name,
                        const CacheFixture &F) {
    std::ofstream File(Name, std::ios::binary);
    File.write(reinterpret_cast<const char *>(F.Data.data()), F.Data.size());
    File.close();
    ASSERT_TRUE(File.good());
  };
  CacheFixture Main;
  CacheFixture Sub(5, Base + 0x10000);
  Main.set<uint32_t>(392, 0x380);
  Main.set<uint32_t>(396, 1);
  Main.Data[0x380] = 2;
  Main.set<uint64_t>(0x390, 0x10000);
  std::memcpy(Main.Data.data() + 0x398, ".01", 4);
  Write(Path, Main);
  auto Missing = DyldSharedCache::open(Path);
  ASSERT_FALSE(bool(Missing));
  llvm::consumeError(Missing.takeError());
  Write(Subpath, Sub);
  auto Cache = DyldSharedCache::open(Path);
  ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
  EXPECT_EQ(Cache->mappings().size(), 4u);
  Sub.Data[88] ^= 1;
  Write(Subpath, Sub);
  auto Stale = DyldSharedCache::open(Path);
  ASSERT_FALSE(bool(Stale));
  llvm::consumeError(Stale.takeError());
  std::memcpy(Main.Data.data() + 0x398, ".01/escape", 11);
  Write(Path, Main);
  auto Invalid = DyldSharedCache::open(Path);
  ASSERT_FALSE(bool(Invalid));
  llvm::consumeError(Invalid.takeError());
}

TEST(DyldSharedCache, CachedImageFileOffsetsResolveThroughTheirSegments) {
  CacheFixture Main;
  CacheFixture Sub(5, Base + 0x10000);
  Main.set<uint32_t>(392, 0x380);
  Main.set<uint32_t>(396, 1);
  Main.Data[0x380] = 2;
  Main.set<uint64_t>(0x390, 0x10000);
  std::memcpy(Main.Data.data() + 0x398, ".01", 4);
  Main.imageLayout(Base + 0x12000);
  Sub.Data[0x1100] = 0x87;
  const auto Parse = [&] {
    std::vector<std::unique_ptr<llvm::MemoryBuffer>> Files;
    Files.push_back(Main.buffer());
    Files.push_back(Sub.buffer());
    return DyldSharedCache::parse(std::move(Files));
  };
  auto Cache = Parse();
  ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
  auto Layout = Cache->readImageLayout(0);
  ASSERT_TRUE(bool(Layout)) << llvm::toString(Layout.takeError());
  ASSERT_EQ(Layout->Segments.size(), 2u);
  ASSERT_EQ(Layout->Sections.size(), 1u);
  EXPECT_EQ(Layout->Sections[0].Name, "__text");
  EXPECT_EQ(Layout->Sections[0].VA, Base + 0x900);
  EXPECT_EQ(Layout->Sections[0].Alignment, 4u);
  EXPECT_TRUE(Layout->Sections[0].isExecutable());
  auto Text = Cache->readImageFileBytes(0, 0x900, 1);
  ASSERT_TRUE(bool(Text)) << llvm::toString(Text.takeError());
  EXPECT_EQ((*Text)[0], 0x42);
  auto Data = Cache->readImageFileBytes(0, 0x1100, 1);
  ASSERT_TRUE(bool(Data)) << llvm::toString(Data.takeError());
  EXPECT_EQ((*Data)[0], 0x87);
  auto Missing = Cache->readImageFileBytes(0, 0xa00, 1);
  ASSERT_FALSE(bool(Missing));
  llvm::consumeError(Missing.takeError());
  Main.set<uint64_t>(0x6e0, 0x600);
  Cache = Parse();
  ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
  auto Ambiguous = Cache->readImageFileBytes(0, 0x900, 1);
  ASSERT_FALSE(bool(Ambiguous));
  llvm::consumeError(Ambiguous.takeError());
}

TEST(DyldSharedCache, CachedImageLayoutRejectsMalformedLoadCommands) {
  using Mutation = std::function<void(CacheFixture &)>;
  const std::vector<Mutation> Cases = {
      [](auto &F) { F.template set<uint32_t>(0x610, 65537); },
      [](auto &F) { F.template set<uint32_t>(0x614, 0xffffffff); },
      [](auto &F) { F.template set<uint32_t>(0x624, 0); },
      [](auto &F) { F.template set<uint32_t>(0x624, 151); },
      [](auto &F) { F.template set<uint32_t>(0x660, 2); },
      [](auto &F) { F.template set<uint64_t>(0x650, 0x5000); },
      [](auto &F) { F.template set<uint64_t>(0x688, Base + 0xa00); },
      [](auto &F) { F.template set<uint32_t>(0x69c, 32); },
      [](auto &F) { F.template set<uint64_t>(0x6d8, InvalidVA); },
      [](auto &F) { F.template set<uint64_t>(0x6d0, Base + 0x800); },
  };
  for (size_t I = 0; I < Cases.size(); ++I) {
    SCOPED_TRACE(I);
    CacheFixture Main;
    Main.imageLayout();
    Cases[I](Main);
    auto Cache = Main.parse();
    ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
    auto Layout = Cache->readImageLayout(0);
    ASSERT_FALSE(bool(Layout));
    llvm::consumeError(Layout.takeError());
  }
}

TEST(DyldSharedCache, SectionFileRangesUseTheCachedSegmentLayout) {
  CacheFixture Fixture;
  Fixture.imageLayout();
  Fixture.set<uint32_t>(0x698, 0x300);
  auto Cache = Fixture.parse();
  ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
  auto Layout = Cache->readImageLayout(0);
  ASSERT_TRUE(bool(Layout)) << llvm::toString(Layout.takeError());
  EXPECT_EQ(Layout->Sections[0].FileOff, 0x900u);
  EXPECT_EQ(readLE<uint32_t>(Layout->HeaderBytes.data() + 0x98), 0x300u);
}

TEST(DyldSharedCache, RejectsMalformedAddressSpacesAndCatalogues) {
  using Mutation = std::function<void(CacheFixture &)>;
  const std::vector<Mutation> Cases = {
      [](auto &F) { F.Data.resize(103); },
      [](auto &F) { F.Data[0] = 'x'; },
      [](auto &F) { F.template set<uint32_t>(16, 0xfffffff0); },
      [](auto &F) { F.template set<uint32_t>(20, 0xffffffff); },
      [](auto &F) { F.template set<uint64_t>(0x208, InvalidVA); },
      [](auto &F) { F.template set<uint64_t>(0x210, 0x4800); },
      [](auto &F) { F.template set<uint64_t>(0x280, Base + 8); },
      [](auto &F) {
        F.template set<uint64_t>(0x220, Base + 0x800);
        F.template set<uint64_t>(0x2b8, Base + 0x800);
      },
      [](auto &F) {
        F.template set<uint64_t>(0x230, 0x800);
        F.template set<uint64_t>(0x2c8, 0x800);
      },
      [](auto &F) { F.template set<uint32_t>(452, 65537); },
      [](auto &F) { F.template set<uint64_t>(0x300, Base + 0x1000); },
      [](auto &F) { F.template set<uint32_t>(0x318, 0x5000); },
      [](auto &F) { F.Data[0x340] = 0; },
      [](auto &F) {
        F.template set<uint32_t>(0x318, 0x4ffe);
        F.Data[0x4ffe] = 'a';
        F.Data[0x4fff] = 'b';
      },
      [](auto &F) { F.template set<uint64_t>(0x2d0, 0x4fff); },
  };
  for (size_t I = 0; I < Cases.size(); ++I) {
    SCOPED_TRACE(I);
    CacheFixture F;
    Cases[I](F);
    auto Cache = F.parse();
    ASSERT_FALSE(bool(Cache));
    EXPECT_FALSE(llvm::toString(Cache.takeError()).empty());
  }
}

TEST(DyldSharedCache, RejectsMalformedChainsWithoutPublishingTheirPrefix) {
  using Mutation = std::function<void(CacheFixture &)>;
  for (uint32_t Version : {3u, 5u}) {
    const std::vector<Mutation> Cases = {
        [](auto &F) { F.template set<uint32_t>(0x4400, 99); },
        [](auto &F) { F.template set<uint32_t>(0x4404, 1024); },
        [](auto &F) { F.template set<uint32_t>(0x4408, 4); },
        [](auto &F) { F.template set<uint16_t>(0x4418, 7); },
        [](auto &F) { F.template set<uint16_t>(0x4418, 0x1000); },
        [](auto &F) { F.template set<uint64_t>(0x4410, InvalidVA); },
        [=](auto &F) {
          F.template set<uint64_t>(0x1018, uint64_t(0x7ff)
                                               << (Version == 3 ? 51 : 52));
        },
        [=](auto &F) {
          F.template set<uint64_t>(0x1018, uint64_t(1)
                                               << (Version == 3 ? 62 : 42));
        },
    };
    for (size_t I = 0; I < Cases.size(); ++I) {
      SCOPED_TRACE(Version);
      SCOPED_TRACE(I);
      CacheFixture F(Version);
      Cases[I](F);
      auto Cache = F.parse();
      ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
      auto Fixups = Cache->readSlideFixups(1);
      ASSERT_FALSE(bool(Fixups));
      EXPECT_FALSE(llvm::toString(Fixups.takeError()).empty());
    }
  }
}

TEST(DyldSharedCache, OptionalRealCachePreservesAllSlideRecipes) {
  const char *Path = std::getenv("NEVERD_DYLD_CACHE");
  if (!Path)
    GTEST_SKIP()
        << "set NEVERD_DYLD_CACHE to an arm64e cache with its subcaches";
  auto Cache = DyldSharedCache::open(Path);
  ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
  ASSERT_FALSE(Cache->images().empty());
  for (size_t I = 0; I < Cache->images().size(); ++I) {
    SCOPED_TRACE(Cache->images()[I].Path);
    auto Layout = Cache->readImageLayout(I);
    ASSERT_TRUE(bool(Layout)) << llvm::toString(Layout.takeError());
    EXPECT_FALSE(Layout->Segments.empty());
  }
  size_t Authenticated = 0;
  size_t Plain = 0;
  for (size_t I = 0; I < Cache->mappings().size(); ++I) {
    auto Fixups = Cache->readSlideFixups(I);
    ASSERT_TRUE(bool(Fixups)) << llvm::toString(Fixups.takeError());
    for (const auto &Fixup : *Fixups) {
      const auto Bytes = Cache->readBytes(Fixup.Slot, 8);
      ASSERT_TRUE(Bytes);
      ASSERT_EQ(readLE<uint64_t>(Bytes->data()), Fixup.EncodedValue);
      if (Fixup.Authentication)
        ++Authenticated;
      else
        ++Plain;
    }
  }
  EXPECT_GT(Authenticated, 0u);
  EXPECT_GT(Plain, 0u);
  RecordProperty("authenticated_recipes", Authenticated);
  RecordProperty("plain_fixups", Plain);
  RecordProperty("images", Cache->images().size());
  RecordProperty("mappings", Cache->mappings().size());
}
