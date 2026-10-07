//===- StringScanTests.cpp - text strings in binary data ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/support/StringScan.h"

#include <string>
#include <vector>

using namespace neverd::strings;

namespace {

/// The bytes of a literal, without the NUL the compiler adds.
template <size_t N> std::vector<uint8_t> bytes(const char (&Text)[N]) {
  return std::vector<uint8_t>(Text, Text + N - 1);
}

/// \p Text as UTF-16 or UTF-32 code units with a terminator.
std::vector<uint8_t> wide(std::u32string_view Text, unsigned Unit,
                          bool BigEndian) {
  std::vector<uint8_t> Out;
  const auto put = [&](uint32_t Value) {
    for (unsigned I = 0; I < Unit; ++I) {
      const unsigned Shift = 8 * (BigEndian ? Unit - 1 - I : I);
      Out.push_back(static_cast<uint8_t>(Value >> Shift));
    }
  };
  for (char32_t C : Text) {
    if (Unit == 2 && C >= 0x10000) {
      put(0xd800 + ((C - 0x10000) >> 10));
      put(0xdc00 + ((C - 0x10000) & 0x3ff));
    } else {
      put(C);
    }
  }
  put(0);
  return Out;
}

std::vector<FoundString> scanAll(const std::vector<uint8_t> &Data,
                                 ScanOptions Options = {}) {
  std::vector<FoundString> Found;
  scan(Data, Options,
       [&](FoundString &&String) { Found.push_back(std::move(String)); });
  return Found;
}

constexpr EncodingSet Everything =
    encodingBit(Encoding::ASCII) | encodingBit(Encoding::UTF8) |
    encodingBit(Encoding::UTF16LE) | encodingBit(Encoding::UTF16BE) |
    encodingBit(Encoding::UTF32LE) | encodingBit(Encoding::UTF32BE);

TEST(StringScan, FindsASCIIAndUTF8CStrings) {
  const auto Found =
      scanAll(bytes("\x01hello\0\xff\xfe"
                    "\xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97\xe7\xac\xa6\0"
                    "\xe9\x94\x99\xe8\xaf\xaf: file\0"));
  ASSERT_EQ(Found.size(), 3u);
  EXPECT_EQ(Found[0].Offset, 1u);
  EXPECT_EQ(Found[0].Kind, Encoding::ASCII);
  EXPECT_EQ(Found[0].Text, "hello");
  EXPECT_EQ(Found[1].Kind, Encoding::UTF8);
  EXPECT_EQ(Found[1].Chars, 4u);
  EXPECT_EQ(Found[1].Bytes, 12u);
  EXPECT_EQ(Found[1].Text, "\xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97\xe7\xac\xa6");
  EXPECT_EQ(Found[2].Kind, Encoding::UTF8);
  EXPECT_EQ(Found[2].Chars, 8u);
}

TEST(StringScan, RejectsMalformedControlAndUnterminatedText) {
  // A truncated sequence, a C1 control (U+0085), a short string and text
  // that runs to the end of the data.
  const auto Found = scanAll(bytes("abcd\xe4\xb8\0wxyz\xc2\x85\0abc\0tail"));
  EXPECT_TRUE(Found.empty());
  ScanOptions OnlyASCII;
  OnlyASCII.Encodings = encodingBit(Encoding::ASCII);
  EXPECT_TRUE(scanAll(bytes("\xe4\xb8\xad\xe6\x96\x87"
                            "ab\0"),
                      OnlyASCII)
                  .empty());
}

TEST(StringScan, FindsWideStringsAtTheirAlignment) {
  ScanOptions Options;
  Options.Encodings = Everything;
  for (const auto &[Unit, BigEndian, Kind] :
       {std::tuple{2u, false, Encoding::UTF16LE},
        std::tuple{2u, true, Encoding::UTF16BE},
        std::tuple{4u, false, Encoding::UTF32LE},
        std::tuple{4u, true, Encoding::UTF32BE}}) {
    SCOPED_TRACE(encodingName(Kind).str());
    std::vector<uint8_t> Data(Unit, 0);
    const auto Text = wide(U"#document 中\U0001F600", Unit, BigEndian);
    Data.insert(Data.end(), Text.begin(), Text.end());
    const auto Found = scanAll(Data, Options);
    ASSERT_EQ(Found.size(), 1u);
    EXPECT_EQ(Found[0].Offset, Unit);
    EXPECT_EQ(Found[0].Kind, Kind);
    EXPECT_EQ(Found[0].Text, "#document \xe4\xb8\xad\xf0\x9f\x98\x80");
  }
  // One byte off the unit alignment, no wide string is read.
  std::vector<uint8_t> Shifted(1, 0);
  const auto Text = wide(U"#document", 2, false);
  Shifted.insert(Shifted.end(), Text.begin(), Text.end());
  for (const auto &String : scanAll(Shifted, Options))
    EXPECT_NE(String.Kind, Encoding::UTF16LE);
}

TEST(StringScan, KeepsWideTextMostlyASCII) {
  ScanOptions Options;
  Options.Encodings = encodingBit(Encoding::UTF16LE);
  // Ideographs alone are indistinguishable from 16-bit numbers.
  EXPECT_TRUE(scanAll(wide(U"中文字符", 2, false), Options).empty());
  EXPECT_EQ(scanAll(wide(U"File 文件", 2, false), Options).size(), 1u);
  // An ASCII C string read as UTF-16 decodes to ideographs, not ASCII.
  EXPECT_TRUE(scanAll(bytes("ABCDEFGH\0\0"), Options).empty());
}

TEST(StringScan, OverlappingStringsKeepTheFirst) {
  ScanOptions Options;
  Options.Encodings = Everything;
  // "abcd" as a C string, then a UTF-16LE string right after its NUL.
  auto Data = bytes("abcd\0\0");
  const auto Text = wide(U"wide text", 2, false);
  Data.insert(Data.end(), Text.begin(), Text.end());
  const auto Found = scanAll(Data, Options);
  ASSERT_EQ(Found.size(), 2u);
  EXPECT_EQ(Found[0].Kind, Encoding::ASCII);
  EXPECT_EQ(Found[1].Kind, Encoding::UTF16LE);
  EXPECT_EQ(Found[1].Offset, 6u);
}

TEST(StringScan, NamesEncodings) {
  EXPECT_EQ(encodingNamed("UTF-16le"), Encoding::UTF16LE);
  EXPECT_EQ(encodingSpelling(Encoding::UTF32BE), "UTF-32BE");
  EXPECT_TRUE(encodingSpelling(Encoding::ASCII).empty());
  EXPECT_FALSE(encodingNamed("ebcdic"));
}

} // namespace
