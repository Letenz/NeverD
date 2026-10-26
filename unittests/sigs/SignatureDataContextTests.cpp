//===- SignatureDataContextTests.cpp - External-data context tests --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent data anchors, storage bounds and frozen candidate settlement.
///
//===----------------------------------------------------------------------===//

#include "../../lib/sigs/X86/X86DataReference.h"
#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/sigs/SignatureDB.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::sigs;

namespace {

const std::string Body = "90909090909090909090909090909090488B05........C3";

std::string root(std::string Name = "anchor", bool Refine = false,
                 std::string Ref = " &0013 global", std::string Bytes = Body) {
  return Bytes + " 00 0000 0018 " + (Refine ? "!caller " : "") + ":0000 " +
         Name + (Refine ? Ref : "") + "\n";
}

std::string getter(std::string Name = "getter",
                   std::string Ref = "&0003 global") {
  return "488D05........C3 00 0000 0008 !refs :0000 " + Name + " " + Ref + "\n";
}

void displacement(BinaryImage &Img, uint64_t Field, uint64_t Next,
                  uint64_t Target) {
  const uint32_t Value = static_cast<uint32_t>(Target - Next);
  for (unsigned I = 0; I < 4; ++I)
    Img.Segments.front().Data[Field - 0x1000 + I] = Value >> (I * 8);
}

BinaryImage image() {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Format = BinaryFormat::COFF;
  Segment Code;
  Code.VA = 0x1000;
  Code.Size = Code.FileSz = 0x300;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Data.resize(Code.Size, 0xcc);
  std::fill_n(Code.Data.begin(), 16, 0x90);
  const uint8_t Load[] = {0x48, 0x8b, 0x05, 0, 0, 0, 0, 0xc3};
  std::copy(std::begin(Load), std::end(Load), Code.Data.begin() + 16);
  std::copy(std::begin(Load), std::end(Load), Code.Data.begin() + 0x100);
  Code.Data[0x101] = 0x8d;
  Img.Segments.push_back(std::move(Code));
  Segment Data;
  Data.VA = 0x2000;
  Data.Size = 0x100;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Img.Segments.push_back(std::move(Data)); // Deliberately zero-fill storage.
  displacement(Img, 0x1013, 0x1017, 0x2020);
  displacement(Img, 0x1103, 0x1107, 0x2020);
  return Img;
}

void add(SignatureDB &DB, const std::string &Patterns) {
  auto Error = DB.loadPatternText(Patterns, "data-context");
  ASSERT_FALSE(Error) << llvm::toString(std::move(Error));
}

} // namespace

TEST(SignatureDataContext, CompleteIndependentAnchorSupportsCandidateOnly) {
  SignatureDB DB;
  add(DB, root() + root("anchor", true) + getter() +
              getter("rival", "&0003 other"));
  DB.apply(image(), {0x1000, 0x1100});
  EXPECT_EQ(DB.buildCandidateNameMap().at(0x1100), "getter");
  ASSERT_NE(DB.findMatch(0x1100), nullptr);
  EXPECT_TRUE(DB.findMatch(0x1100)->Confirmed);
  EXPECT_TRUE(DB.buildNameMap().empty());
  DB.apply(image(), {0x1100});
  EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
}

TEST(SignatureDataContext, WeakPartialAmbiguousAndRequiredRootsDoNotBind) {
  for (const auto &Anchor :
       {"90 00 0000 0018 :0000 anchor\n",
        "................................488B05........C3 00 0000 0018 :0000 "
        "anchor\n",
        "90909090909090909090909090909090488B05........C3 00 0000 0018 !refs "
        ":0000 anchor &0013 global\n"}) {
    SignatureDB DB;
    add(DB, std::string(Anchor) + root("anchor", true) + getter());
    DB.apply(image(), {0x1000, 0x1100});
    EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
  }
  SignatureDB DB;
  add(DB, root() + root("another") + root("anchor", true) + getter());
  DB.apply(image(), {0x1000, 0x1100});
  EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
}

TEST(SignatureDataContext,
     EveryMatchingSourceMustAgreeIncludingAbsenceAndAlternatives) {
  for (const auto &Other : {"", " &0013 other", " &0013 global &0013 other"}) {
    for (bool Reverse : {false, true}) {
      SignatureDB DB;
      const auto Good = root("anchor", true);
      const auto Bad = root("anchor", true, Other);
      add(DB, root() + (Reverse ? Bad + Good : Good + Bad) + getter());
      DB.apply(image(), {0x1000, 0x1100});
      EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
    }
  }
}

TEST(SignatureDataContext, WeakSourceRefinementsCannotAddDataToAStrongRoot) {
  const std::string Weak = "................................488B05........C3";
  for (bool AddStrongRefinement : {false, true}) {
    SignatureDB DB;
    add(DB, root() + root("anchor", true, " &0013 global", Weak) +
                (AddStrongRefinement ? root("anchor", true) : "") + getter());
    DB.apply(image(), {0x1000, 0x1100});
    EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
  }
}

TEST(SignatureDataContext, ConflictingNamesOrLocationsCannotBind) {
  for (bool SameAddress : {false, true}) {
    auto Img = image();
    std::copy_n(Img.Segments.front().Data.begin(), 24,
                Img.Segments.front().Data.begin() + 0x40);
    Img.Segments.front().Data[0x40] = 0x91;
    displacement(Img, 0x1053, 0x1057, SameAddress ? 0x2020 : 0x2040);
    const auto OtherBody = "91" + Body.substr(2);
    SignatureDB DB;
    add(DB,
        root() + root("anchor", true) + root("second", false, "", OtherBody) +
            root("second", true, SameAddress ? " &0013 other" : " &0013 global",
                 OtherBody) +
            getter());
    DB.apply(Img, {0x1000, 0x1040, 0x1100});
    EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
  }
}

TEST(SignatureDataContext, FunctionReferencesNeverAcquireDataSemantics) {
  SignatureDB DB;
  add(DB, root() + root("anchor", true) + getter("getter", "^0003 global"));
  DB.apply(image(), {0x1000, 0x1100});
  EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
}

TEST(SignatureDataContext,
     DerivedCandidatesCannotEstablishFurtherDataBindings) {
  auto Img = image();
  // A second strong body uses the root's global and takes another address.
  auto &Code = Img.Segments.front().Data;
  std::fill_n(Code.begin() + 0x80, 16, 0x91);
  const uint8_t Operands[] = {0x48, 0x8b, 0x05, 0, 0, 0, 0,   0x48,
                              0x8d, 0x0d, 0,    0, 0, 0, 0xc3};
  std::copy(std::begin(Operands), std::end(Operands), Code.begin() + 0x90);
  displacement(Img, 0x1093, 0x1097, 0x2020);
  displacement(Img, 0x109a, 0x109e, 0x2040);
  displacement(Img, 0x1103, 0x1107, 0x2040);
  const std::string Derived =
      "91919191919191919191919191919191488B05........488D0D........C3 "
      "00 0000 001F ";
  SignatureDB DB;
  add(DB, root() + root("anchor", true) + Derived +
              "!refs :0000 derived &0013 global\n" + Derived +
              "!caller :0000 derived &0013 global &001A second\n" +
              getter("getter", "&0003 second"));
  DB.apply(Img, {0x1000, 0x1080, 0x1100});
  EXPECT_EQ(DB.buildCandidateNameMap().at(0x1080), "derived");
  EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
  EXPECT_TRUE(DB.buildNameMap().empty());
}

TEST(SignatureDataContext,
     OneContradictedSourceRefinementInvalidatesTheAnchor) {
  auto Img = image();
  auto &Code = Img.Segments.front().Data;
  Code[0x17] = 0xe8;
  Code[0x1c] = 0xc3;
  displacement(Img, 0x1018, 0x101c, 0x1200);
  Code[0x200] = 0xaa;
  const std::string Called =
      Body.substr(0, Body.size() - 2) + "E8........C3 00 0000 001D ";
  SignatureDB DB;
  add(DB, Called + ":0000 anchor\n" + Called +
              "!caller :0000 anchor &0013 global ^0018 actual\n" + Called +
              "!caller :0000 anchor &0013 global ^0018 contradicted\n" +
              "AA 00 0000 0001 :0000 actual\n" + getter());
  DB.apply(Img, {0x1000, 0x1100, 0x1200});
  EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
}

TEST(SignatureDataContext,
     RequiresReadableNonexecutingUnambiguousMappedExtent) {
  for (unsigned Case = 0; Case < 11; ++Case) {
    auto Img = image();
    switch (Case) {
    case 0:
      Img.Segments.back().Flags = SegmentFlags::None;
      break;
    case 1:
      Img.Segments.back().Flags =
          SegmentFlags::Readable | SegmentFlags::Executable;
      break;
    case 2:
      Img.Segments.back().Size = 0x27;
      break; // 8-byte root crosses end.
    case 3:
      Img.Segments.push_back(Img.Segments.back());
      break;
    case 4:
      Img.IsRelocatable = true;
      break;
    case 5:
      Img.Arch = Arch::X86;
      break;
    case 6:
      Img.Format = BinaryFormat::ELF;
      break;
    case 7:
      displacement(Img, 0x1103, 0x1107, 0x2040);
      break;
    case 8: {
      Section Sec;
      Sec.VA = 0x2000;
      Sec.Size = 0x100;
      Sec.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
      Img.Sections.push_back(Sec);
      break;
    }
    case 9:
      Img.Segments.back().VA = InvalidVA - 0x10;
      break;
    case 10:
      displacement(Img, 0x1013, 0x1017, 0x1000);
      displacement(Img, 0x1103, 0x1107, 0x1000);
      break;
    }
    SignatureDB DB;
    add(DB, root() + root("anchor", true) + getter());
    DB.apply(Img, {0x1000, 0x1100});
    EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100)) << Case;
  }
}

TEST(SignatureDataContext, AnImmediateCannotBorrowBytesPastTheSourceBody) {
  auto Img = image();
  Img.Segments.front().Data[0x100] = 0xc7;
  Img.Segments.front().Data[0x101] = 0x05;
  std::fill_n(Img.Segments.front().Data.begin() + 0x106, 4, 0);
  displacement(Img, 0x1102, 0x110a, 0x2020);
  SignatureDB DB;
  add(DB, root() + root("anchor", true) +
              "C705........ 00 0000 0006 !refs :0000 truncated &0002 global\n");
  DB.apply(Img, {0x1000, 0x1100});
  EXPECT_FALSE(DB.buildCandidateNameMap().contains(0x1100));
}

TEST(SignatureDataContext, NegativeDisplacementsAndFinalMappedBytesAreValid) {
  auto Img = image();
  Img.Segments.back().VA = 0x800;
  Img.Segments.back().Size = 8;
  displacement(Img, 0x1013, 0x1017, 0x800);
  displacement(Img, 0x1103, 0x1107, 0x800);
  Img.Segments.front().Data.resize(0x108);
  Img.Segments.front().Size = Img.Segments.front().FileSz = 0x108;
  SignatureDB DB;
  add(DB, root() + root("anchor", true) + getter());
  DB.apply(Img, {0x1000, 0x1100});
  EXPECT_EQ(DB.buildCandidateNameMap().at(0x1100), "getter");
}

TEST(SignatureDataOperand, ExactImmediateBiasWidthsAndPrefixRefusals) {
  struct Form {
    std::vector<uint8_t> Bytes;
    size_t Field;
    unsigned Width;
    unsigned Bias;
  };
  const Form Forms[] = {{{0x48, 0x8b, 0x05, 0, 0, 0, 0}, 3, 8, 0},
                        {{0x4c, 0x89, 0x3d, 0, 0, 0, 0}, 3, 8, 0},
                        {{0x48, 0x8d, 0x0d, 0, 0, 0, 0}, 3, 1, 0},
                        {{0x8b, 0x05, 0, 0, 0, 0}, 2, 4, 0},
                        {{0x83, 0x3d, 0, 0, 0, 0, 0}, 2, 4, 1},
                        {{0xc7, 0x05, 0, 0, 0, 0, 0, 0, 0, 0}, 2, 4, 4},
                        {{0x48, 0x81, 0x3d, 0, 0, 0, 0, 0, 0, 0, 0}, 3, 8, 4}};
  for (const auto &Form : Forms) {
    auto Field = x64DataReferenceField(Form.Bytes, Form.Field);
    ASSERT_TRUE(Field);
    EXPECT_EQ(Field->Width, Form.Width);
    EXPECT_EQ(Field->Bias, Form.Bias);
    EXPECT_EQ(Field->End, Form.Bytes.size());
    auto Truncated = Form.Bytes;
    Truncated.pop_back();
    EXPECT_FALSE(x64DataReferenceField(Truncated, Form.Field));
    for (uint8_t Prefix : {0x66, 0x67, 0x64, 0xf0, 0xf2, 0xf3, 0x41, 0x48}) {
      auto Prefixed = Form.Bytes;
      Prefixed.insert(Prefixed.begin(), Prefix);
      // A single REX.W is supported on a previously unprefixed form.
      if (Prefix == 0x48 && Form.Field == 2)
        continue;
      EXPECT_FALSE(x64DataReferenceField(Prefixed, Form.Field + 1));
    }
  }
  EXPECT_FALSE(x64DataReferenceField({0xff, 0x15, 0, 0, 0, 0}, 2));
  EXPECT_FALSE(x64DataReferenceField({0x8d, 0x05, 0, 0, 0, 0}, 2));
  EXPECT_FALSE(x64DataReferenceField({0x48, 0x8b, 0x45, 0, 0, 0, 0}, 3));
}
