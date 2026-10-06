#include "../../../lib/loader/Swift/SwiftFunctionSymbols.h"
#include "../../../lib/sdk/capi/SwiftMangledSourceABI.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <limits>
#include <random>

using namespace neverd;

TEST(SwiftFunctionSymbols, ExactUniquenessIsIndependentOfSymbolOrder) {
  BinaryImage Image;
  const auto Last = std::numeric_limits<va_t>::max();
  Image.Symbols = {
      {"function", 0x1000, 0, true},   {"data_alias", 0x1000, 0, false},
      {"duplicate", 0x2000, 0, true},  {"duplicate", 0x2000, 0, true},
      {"only_data", 0x3000, 0, false}, {"zero", 0, 0, true},
      {"last", Last, 0, true}};
  std::mt19937 Random(5231);
  for (unsigned Order = 0; Order < 64; ++Order) {
    std::shuffle(Image.Symbols.begin(), Image.Symbols.end(), Random);
    const SwiftFunctionSymbolIndex Index(Image);
    for (const auto Entry : {va_t(0), va_t(0x1000), va_t(0x1001), va_t(0x2000),
                             va_t(0x3000), Last}) {
      const auto *Indexed = uniqueSwiftFunctionSymbol(Image, Entry, &Index);
      EXPECT_EQ(Indexed, uniqueSwiftFunctionSymbol(Image, Entry));
      if (Entry == 0 || Entry == 0x1000 || Entry == Last) {
        ASSERT_NE(Indexed, nullptr);
        EXPECT_EQ(Indexed->Addr, Entry);
        EXPECT_TRUE(Indexed->IsFunc);
      } else {
        EXPECT_EQ(Indexed, nullptr);
      }
    }
  }
}

TEST(SwiftFunctionSymbols, ForeignIndexUsesTheRequestedImage) {
  BinaryImage Original;
  Original.Symbols = {{"original", 0x1000, 0, true}};
  const SwiftFunctionSymbolIndex Index(Original);
  BinaryImage Other;
  EXPECT_EQ(uniqueSwiftFunctionSymbol(Other, 0x1000, &Index), nullptr);
  Other.Symbols = {{"other", 0x1000, 0, true}};
  EXPECT_EQ(uniqueSwiftFunctionSymbol(Other, 0x1000, &Index),
            &Other.Symbols.front());
  Other.Symbols.push_back({"alias", 0x1000, 0, true});
  EXPECT_EQ(uniqueSwiftFunctionSymbol(Other, 0x1000, &Index), nullptr);
  EXPECT_EQ(uniqueSwiftFunctionSymbol(Original, 0x1000, &Index),
            &Original.Symbols.front());
}

TEST(SwiftFunctionSymbols, RebuildingObservesChangedStorageAndFunctionFlags) {
  BinaryImage Image;
  Image.Symbols = {{"first", 0x1000, 0, true}};
  {
    const SwiftFunctionSymbolIndex Index(Image);
    EXPECT_EQ(uniqueSwiftFunctionSymbol(Image, 0x1000, &Index),
              &Image.Symbols.front());
  }
  Image.Symbols.push_back({"alias", 0x1000, 0, true});
  {
    const SwiftFunctionSymbolIndex Index(Image);
    EXPECT_EQ(uniqueSwiftFunctionSymbol(Image, 0x1000, &Index), nullptr);
  }
  Image.Symbols.front().IsFunc = false;
  {
    const SwiftFunctionSymbolIndex Index(Image);
    EXPECT_EQ(uniqueSwiftFunctionSymbol(Image, 0x1000, &Index),
              &Image.Symbols.back());
  }
  Image.Symbols.clear();
  const SwiftFunctionSymbolIndex Empty(Image);
  EXPECT_EQ(uniqueSwiftFunctionSymbol(Image, 0x1000, &Empty), nullptr);
}

TEST(SwiftFunctionSymbols, RepeatedDeclarationsKeepEveryRecordField) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    BinaryImage Image;
    Image.Symbols = {{"declared", 0x1000, 32, true},
                     {"declared", 0x1000, 32, true}};
    if (Mutation == 1)
      Image.Symbols.back().Name += "alias";
    if (Mutation == 2)
      ++Image.Symbols.back().Size;
    if (Mutation == 3)
      Image.Symbols.back().IsBoundaryGuess = true;
    if (Mutation == 4)
      Image.Symbols.back().Origin = NameOrigin::Analysis;
    for (bool Reverse : {false, true}) {
      if (Reverse)
        std::reverse(Image.Symbols.begin(), Image.Symbols.end());
      const SwiftFunctionSymbolIndex Index(Image);
      BinaryImage Foreign;
      const SwiftFunctionSymbolIndex ForeignIndex(Foreign);
      EXPECT_FALSE(uniqueSwiftFunctionSymbol(Image, 0x1000, &Index));
      for (const auto *Selected : {&Index, &ForeignIndex})
        EXPECT_EQ(bool(consistentSwiftFunctionDeclarationSymbol(Image, 0x1000,
                                                                Selected)),
                  Mutation == 0);
    }
  }
}

TEST(SwiftFunctionSymbols, IndexedDeclarationsRetainTypeAndCarrierProofs) {
  constexpr const char *Name =
      "_$s4main9localized_12languageCode6bundle5value7commentS2S_SSSgSo8"
      "NSBundleCSgS2StF";
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    BinaryImage Image;
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Architecture;
    Image.Bits = Bitness::Bits64;
    Segment Text;
    Text.VA = 0x1000;
    Text.Size = Text.FileSz = 0x100;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(0x100);
    Image.Segments.push_back(Text);
    Image.Symbols = {{Name, 0x1000, 0, true}};
    const SwiftFunctionSymbolIndex Index(Image);
    const auto Live = swiftMangledStringBundleSourceABI(Image, 0x1000, true);
    const auto Indexed =
        swiftMangledStringBundleSourceABI(Image, 0x1000, true, &Index);
    ASSERT_TRUE(Live);
    ASSERT_TRUE(Indexed);
    EXPECT_TRUE(equalSourceABIs(*Live, *Indexed));
    EXPECT_FALSE(
        swiftMangledStringBundleSourceABI(Image, 0x1000, false, &Index));
    for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
      auto Wrong = Image;
      switch (Mutation) {
      case 0:
        Wrong.Symbols.push_back({Name, 0x1000, 0, true});
        break;
      case 1:
        Wrong.Symbols.front().IsFunc = false;
        break;
      case 2:
        Wrong.Symbols.front().Addr += 4;
        break;
      case 3:
        Wrong.Symbols.front().Name += "junk";
        break;
      case 4:
        Wrong.IsRelocatable = true;
        break;
      case 5:
        Wrong.Format = BinaryFormat::ELF;
        break;
      case 6:
        Wrong.Bits = Bitness::Bits32;
        break;
      case 7:
        Wrong.Segments.clear();
        break;
      }
      const SwiftFunctionSymbolIndex Changed(Wrong);
      EXPECT_FALSE(
          swiftMangledStringBundleSourceABI(Wrong, 0x1000, true, &Changed));
      EXPECT_FALSE(
          swiftMangledStringBundleSourceABI(Wrong, 0x1000, true, &Index));
      EXPECT_FALSE(swiftMangledStringBundleSourceABI(Wrong, 0x1000, true));
    }
  }
}

TEST(SwiftFunctionSymbols, RegularExpressionInitializerRetainsContextAndError) {
  constexpr const char *Name =
      "_$sSo19NSRegularExpressionC7pattern7optionsABSS_So0aB7OptionsVtKcfcTO";
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    BinaryImage Image;
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Architecture;
    Image.Bits = Bitness::Bits64;
    Segment Text;
    Text.VA = 0x1000;
    Text.Size = Text.FileSz = 16;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(16);
    Image.Segments.push_back(Text);
    Image.Symbols = {{Name, 0x1000, 16, true}};
    const auto Hint =
        sdk::swiftMangledRegularExpressionInitializerSourceABI(Image, 0x1000);
    ASSERT_TRUE(Hint);
    ASSERT_EQ(Hint->Parameters.size(), 5U);
    EXPECT_EQ(Hint->Parameters[3].TheRole,
              SourceParameterTypeHint::Role::SwiftContext);
    EXPECT_TRUE(sourceABIErrorResult(*Hint));
    auto Repeated = Image;
    Repeated.Symbols.push_back(Repeated.Symbols.front());
    const SwiftFunctionSymbolIndex RepeatedIndex(Repeated);
    const auto RepeatedHint =
        sdk::swiftMangledRegularExpressionInitializerSourceABI(Repeated, 0x1000,
                                                               &RepeatedIndex);
    ASSERT_TRUE(RepeatedHint);
    EXPECT_TRUE(equalSourceABIs(*RepeatedHint, *Hint));
    for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
      auto Wrong = Image;
      if (Mutation == 0)
        Wrong.Symbols.push_back(
            {std::string(Name) + "alias", 0x1000, 16, true});
      else if (Mutation == 1)
        Wrong.Symbols[0].IsFunc = false;
      else if (Mutation == 2)
        Wrong.Symbols[0].Name += "x";
      else if (Mutation == 3)
        Wrong.Symbols[0].Name =
            "_$sSo19NSRegularExpressionC7pattern7optionsABSS_"
            "So0aB7OptionsVtKcfc";
      else if (Mutation == 4)
        Wrong.IsRelocatable = true;
      else if (Mutation == 5)
        Wrong.Format = BinaryFormat::ELF;
      else if (Mutation == 6)
        Wrong.Bits = Bitness::Bits32;
      else
        Wrong.Segments[0].Flags = SegmentFlags::Readable;
      const SwiftFunctionSymbolIndex Changed(Wrong);
      EXPECT_FALSE(sdk::swiftMangledRegularExpressionInitializerSourceABI(
          Wrong, 0x1000, &Changed))
          << Mutation;
    }
  }
}
