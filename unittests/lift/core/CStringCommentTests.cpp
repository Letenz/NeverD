//===- CStringCommentTests.cpp - Strings read beside their references -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/Support/raw_ostream.h"

#include <string>
#include <vector>

using namespace neverd;

namespace {

constexpr auto NotFound = std::string::npos;
constexpr va_t RodataVA = 0x2000, DataVA = 0x4000, TextVA = 0x1000;
// Offsets of the strings in the readonly segment.
constexpr va_t Gbk = 0x00, Wide = 0x10, Closing = 0x30, Long = 0x40,
               Control = 0xA0, Binary = 0xB0;

void put(std::vector<uint8_t> &Bytes, va_t Offset,
         std::initializer_list<uint8_t> Values) {
  std::copy(Values.begin(), Values.end(), Bytes.begin() + Offset);
}

void put(std::vector<uint8_t> &Bytes, va_t Offset, const std::string &Text) {
  std::copy(Text.begin(), Text.end(), Bytes.begin() + Offset);
}

/// Readonly strings in several encodings, a writable pointer to the wide one
/// and an executable segment.
BinaryImage stringImage() {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::ELF;
  std::vector<uint8_t> Rodata(0x100, 0);
  // "中文字符" in GBK.
  put(Rodata, Gbk, {0xD6, 0xD0, 0xCE, 0xC4, 0xD7, 0xD6, 0xB7, 0xFB});
  for (size_t I = 0; I < 9; ++I)
    Rodata[Wide + 2 * I] = static_cast<uint8_t>("Wide text"[I]);
  put(Rodata, Closing, std::string("a */ b"));
  put(Rodata, Long, std::string(80, 'x'));
  put(Rodata, Control, std::string("tab\there"));
  put(Rodata, Binary, {0x01, 0x02, 0x03, 0x04, 0x05});
  Segment Readonly;
  Readonly.Name = ".rodata";
  Readonly.VA = RodataVA;
  Readonly.Size = Rodata.size();
  Readonly.Flags = SegmentFlags::Readable;
  Readonly.Data = std::move(Rodata);
  Img.Segments.push_back(std::move(Readonly));
  Segment Data;
  Data.Name = ".data";
  Data.VA = DataVA;
  Data.Size = 8;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(8);
  const va_t Pointer = RodataVA + Wide;
  for (unsigned I = 0; I < 8; ++I)
    Data.Data[I] = static_cast<uint8_t>(Pointer >> (8 * I));
  Img.Segments.push_back(std::move(Data));
  Segment Text;
  Text.Name = ".text";
  Text.VA = TextVA;
  Text.Size = 0x10;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(0x10, 'A');
  Text.Data.back() = 0;
  Img.Segments.push_back(std::move(Text));
  return Img;
}

HighStmt putsCall(ExprPtr Argument) {
  HighStmt Call;
  Call.Kind = StmtKind::Call;
  Call.CallExpr = HighExpr::makeCall("puts", 0x1100, {std::move(Argument)});
  return Call;
}

TEST(CStringComments, CallArgumentsShowTheStringsTheyPointTo) {
  const BinaryImage Img = stringImage();
  HighFunc F;
  F.Name = "show";
  F.Entry = TextVA;
  F.ReturnType = NdType::makeVoid();
  // An address, and a pointer the image holds.
  F.Body = {putsCall(HighExpr::makeConst(RodataVA + Gbk, 8)),
            putsCall(HighExpr::makeLoad(HighExpr::makeConst(DataVA, 8),
                                        NdType::makeInt(8, false)))};
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::ELF;
  Options.Image = &Img;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit({F}, OS, Options));
  EXPECT_NE(Source.find("/* GBK \"中文字符\" */)"), NotFound) << Source;
  EXPECT_NE(Source.find("/* UTF-16LE \"Wide text\" */)"), NotFound) << Source;

  // Without comments the code is the same, less them.
  Options.EmitComments = false;
  std::string Plain;
  llvm::raw_string_ostream PlainOS(Plain);
  ASSERT_TRUE(HighCEmitter().emit({F}, PlainOS, Options));
  EXPECT_EQ(Plain.find("GBK"), NotFound) << Plain;
}

TEST(CStringComments, CommentsStayInsideTheirDelimiters) {
  const BinaryImage Img = stringImage();
  EXPECT_EQ(imageStringComment(&Img, RodataVA + Closing),
            std::optional<std::string>("\"a * / b\""));
  EXPECT_EQ(imageStringComment(&Img, RodataVA + Control),
            std::optional<std::string>("\"tab\\there\""));
  const auto Cut = imageStringComment(&Img, RodataVA + Long);
  ASSERT_TRUE(Cut);
  EXPECT_EQ(*Cut, "\"" + std::string(64, 'x') + "\xE2\x80\xA6\"");
  // A pointer into a string reads the rest of it.
  EXPECT_EQ(imageStringComment(&Img, RodataVA + Closing + 2),
            std::optional<std::string>("\"* / b\""));
  // No string starts at bytes that are no text, or in code.
  EXPECT_FALSE(imageStringComment(&Img, RodataVA + Binary));
  EXPECT_FALSE(imageStringComment(&Img, TextVA));
}

} // namespace
