//===- StringLiteralSymbolsTests.cpp - Symbols that spell literals --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/StringLiteralSymbols.h"

#include <string>

using namespace neverd;

namespace {

constexpr va_t CodeVA = 0x140413000;

std::string leading(const StringLiteralName &Name) {
  return std::string(Name.Leading.begin(), Name.Leading.end());
}

/// An image whose one code segment holds \p Bytes, as ntoskrnl keeps
/// literals among its code.
BinaryImage imageHolding(const std::string &Bytes) {
  BinaryImage Img;
  Segment Code;
  Code.Name = "PAGE";
  Code.VA = CodeVA;
  Code.Size = Bytes.size();
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Data.assign(Bytes.begin(), Bytes.end());
  Img.Segments.push_back(std::move(Code));
  return Img;
}

TEST(StringLiteralSymbols, NamesSpellTheTypeTheSizeAndTheLeadingBytes) {
  // L"SA": six bytes, a character high byte first.
  const auto Wide = parseStringLiteralName("??_C@_15OHIPBLFN@?$AAS?$AAA@");
  ASSERT_TRUE(Wide);
  EXPECT_EQ(Wide->Bytes, 6u);
  EXPECT_EQ(Wide->UnitBytes, 2u);
  EXPECT_EQ(leading(*Wide), std::string("S\0A\0", 4));
  // Eighty bytes, named for their first 31 and the module that holds them.
  const auto Long = parseStringLiteralName(
      "??_C@_0FA@OGCCJEAN@CC?3?5SharedCacheMap?9?$DOOpenCount?5?$DN@FNODOBFM@");
  ASSERT_TRUE(Long);
  EXPECT_EQ(Long->Bytes, 80u);
  EXPECT_EQ(Long->UnitBytes, 0u);
  EXPECT_EQ(leading(*Long), "CC: SharedCacheMap->OpenCount =");
  // Letters after `?` spell bytes past ASCII, digits common punctuation.
  const auto Escaped = parseStringLiteralName("??_C@_05GKMNHHOB@?a?Z?6?0@");
  ASSERT_TRUE(Escaped);
  EXPECT_EQ(leading(*Escaped), "\xE1\xDA\n,");
  // "" and L"".
  ASSERT_TRUE(parseStringLiteralName("??_C@_00CNPNBAHC@@"));
  EXPECT_EQ(parseStringLiteralName("??_C@_00CNPNBAHC@@")->Bytes, 1u);
  ASSERT_TRUE(parseStringLiteralName("??_C@_11LOCGONAA@@NNGAKEGL@"));
  EXPECT_EQ(parseStringLiteralName("??_C@_11LOCGONAA@@NNGAKEGL@")->Bytes, 2u);
}

TEST(StringLiteralSymbols, OtherAndMalformedNamesSpellNoLiteral) {
  for (const char *Name : {
           "??_7type_info@@6B@",
           "?nodeType@QDomNode@@QEBAHXZ",
           "aSa",
           "??_C@_2",
           "??_C@_0",
           "??_C@_03ABC",
           // More bytes than precede the terminator.
           "??_C@_03ABC@abcd@",
           // Half a wide character, and an odd size for one.
           "??_C@_15OHIPBLFN@?$AAS?$AA@",
           "??_C@_14ABC@@",
           // Digits run from A to P.
           "??_C@_03ABC@a?$QQ@",
           "??_C@_03ABZ@ab@",
           "??_C@_03ABC@ab@trailing",
           "??_C@_03ABC@ab@ABC@extra",
       })
    EXPECT_FALSE(parseStringLiteralName(Name)) << Name;
  EXPECT_TRUE(isStringLiteralName("??_C@_15OHIPBLFN@?$AAS?$AAA@"));
  EXPECT_FALSE(isStringLiteralName("??_7type_info@@6B@"));
}

TEST(StringLiteralSymbols, TheImageHoldsTheLiteralItsSymbolNames) {
  std::string Bytes;
  const auto Place = [&](llvm::StringRef Literal) {
    const va_t At = CodeVA + Bytes.size();
    Bytes += Literal.str();
    return At;
  };
  const va_t Wide = Place(llvm::StringRef("S\0A\0\0\0", 6));
  // A second terminator, as L"%04d\0" ends.
  const va_t Padded = Place(llvm::StringRef("abc\0\0", 5));
  // Binary data pooled as a literal: zeros among other bytes.
  const va_t Binary = Place(llvm::StringRef("\x01\x06\0\x05\0", 5));
  // char16_t, which shares char's prefix: the bytes decide.
  const va_t Char16 = Place(llvm::StringRef("a\0b\0\0\0", 6));
  const va_t Empty = Place(llvm::StringRef("\0", 1));
  // Cut off where the file ends.
  const va_t Cut = Place("xyz");
  const BinaryImage Img = imageHolding(Bytes);

  const auto WideLiteral =
      symbolStringLiteralAt(Img, Wide, "??_C@_15OHIPBLFN@?$AAS?$AAA@");
  ASSERT_TRUE(WideLiteral);
  EXPECT_EQ(WideLiteral->Bytes, 4u);
  EXPECT_EQ(WideLiteral->UnitBytes, 2u);
  EXPECT_EQ(WideLiteral->Size, 6u);
  const auto PaddedLiteral =
      symbolStringLiteralAt(Img, Padded, "??_C@_04NOJCDBJJ@abc@");
  ASSERT_TRUE(PaddedLiteral);
  EXPECT_EQ(PaddedLiteral->Bytes, 3u);
  EXPECT_EQ(PaddedLiteral->UnitBytes, 1u);
  EXPECT_EQ(PaddedLiteral->Size, 5u);
  EXPECT_FALSE(
      symbolStringLiteralAt(Img, Binary, "??_C@_04ABCDEFGH@?$AB?$AG@"));
  const auto Char16Literal =
      symbolStringLiteralAt(Img, Char16, "??_C@_05ABCDEFGH@a?$AAb?$AA?$AA@");
  ASSERT_TRUE(Char16Literal);
  EXPECT_EQ(Char16Literal->Bytes, 4u);
  EXPECT_EQ(Char16Literal->UnitBytes, 2u);
  const auto EmptyLiteral =
      symbolStringLiteralAt(Img, Empty, "??_C@_00CNPNBAHC@@");
  ASSERT_TRUE(EmptyLiteral);
  EXPECT_EQ(EmptyLiteral->Bytes, 0u);
  EXPECT_FALSE(symbolStringLiteralAt(Img, Cut, "??_C@_03ABCDEFGH@xy@"));
  // A stale symbol names other bytes, and other names name no literal.
  EXPECT_FALSE(
      symbolStringLiteralAt(Img, Padded, "??_C@_15OHIPBLFN@?$AAS?$AAA@"));
  EXPECT_FALSE(symbolStringLiteralAt(Img, Wide, "WideName"));
  EXPECT_FALSE(symbolStringLiteralAt(Img, CodeVA - 0x10, "??_C@_00CNPNBAHC@@"));
}

TEST(StringLiteralSymbols, ALiteralLiesInTheFileBytesOfItsSection) {
  // Past its bytes in the file a section holds what the loader fills in,
  // as a PE section's virtual size goes on past its raw data.
  BinaryImage Img = imageHolding(std::string("ab\0cd\0", 6));
  Section Sec;
  Sec.Name = "PAGE";
  Sec.VA = CodeVA;
  Sec.Size = 6;
  Sec.FileSz = 3;
  Sec.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(Sec);
  EXPECT_TRUE(symbolStringLiteralAt(Img, CodeVA, "??_C@_02DKCKIIND@ab@"));
  EXPECT_FALSE(symbolStringLiteralAt(Img, CodeVA + 3, "??_C@_02DKCKIIND@cd@"));
}

TEST(StringLiteralSymbols, OnlyACOFFImageNamesItsOwnLiterals) {
  // A data symbol that spells "ab" after a label at the same address, and a
  // function: only the literal's name answers, and only MSVC names one.
  BinaryImage Img = imageHolding(std::string("ab\0", 3));
  Img.Format = BinaryFormat::COFF;
  Symbol Label;
  Label.Name = "label";
  Label.Addr = CodeVA;
  Symbol Literal;
  Literal.Name = "??_C@_02DKCKIIND@ab@";
  Literal.Addr = CodeVA;
  Symbol Function;
  Function.Name = "??_C@_02DKCKIIND@ab@";
  Function.Addr = CodeVA + 1;
  Function.IsFunc = true;
  Img.Symbols = {Label, Literal, Function};
  EXPECT_EQ(stringLiteralSymbolAt(Img, CodeVA), "??_C@_02DKCKIIND@ab@");
  EXPECT_EQ(stringLiteralSymbolAt(Img, CodeVA + 1), "");
  Img.Format = BinaryFormat::ELF;
  EXPECT_EQ(stringLiteralSymbolAt(Img, CodeVA), "");
}

} // namespace
