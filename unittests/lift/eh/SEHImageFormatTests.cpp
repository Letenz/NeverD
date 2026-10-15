//===- SEHImageFormatTests.cpp - Source-written image format effects -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks live writable-format bytes and the calls that can invalidate them.

#include "PipelineSEHRuntimeMemory.h"
#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/SEHFrameProof.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <map>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t Entry = 0x140001000, Leaf = Entry + 0x200;
constexpr va_t Unwind = Entry + 0x300, Format = Entry + 0x1000;
constexpr va_t StreamIAT = Entry + 0x2000, PrintIAT = StreamIAT + 8;

void word(std::vector<uint8_t> &Bytes, uint64_t Value, unsigned Size) {
  for (unsigned I = 0; I != Size; ++I)
    Bytes.push_back(Value >> (I * 8));
}

enum class Mutation {
  None,
  WordStore,
  FileOnly,
  MissingByte,
  MissingNUL,
  CountOutput,
  UnknownByte,
  MatchingDiamond,
  ConflictingDiamond,
  FilledByCallee,
  SavedAfterCallee,
  NeighborWrite,
  ChangedByCallee,
  SavedFrameOverwrite,
  UnknownCallee,
  WrongProvider,
  TruncatedStorage,
  ConflictingStorage,
};

struct Fixture {
  BinaryImage Image;
  va_t UnwindCall = InvalidVA;
  size_t FormatByteImmediate = 0;

  explicit Fixture(Mutation M) {
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::COFF;
    Image.Base = Entry - 0x1000;
    Image.Entry = Entry;
    std::vector<uint8_t> Code{0x48, 0x83, 0xec, 56};
    const auto SaveFrame = [&] {
      Code.insert(Code.end(), {0x48, 0x89, 0x64, 0x24, 40});
    };
    if (M != Mutation::SavedAfterCallee)
      SaveFrame();
    const auto DirectCall = [&](va_t Target) {
      Code.push_back(0xe8);
      word(Code, Target - (Entry + Code.size() + 4), 4);
    };
    const auto ImportCall = [&](va_t Slot) {
      Code.insert(Code.end(), {0xff, 0x15});
      word(Code, Slot - (Entry + Code.size() + 4), 4);
    };
    Code.insert(Code.end(), {0x48, 0xb8}); // writable format address
    word(Code, Format, 8);
    if (M == Mutation::WordStore) {
      Code.insert(Code.end(), {0xc7, 0x00});
      word(Code, 0x6425, 4); // DWORD "%d\0\0"
    } else if (M != Mutation::FileOnly) {
      Code.insert(Code.end(), {0xc6, 0x00, '%'});
      if (M != Mutation::MissingByte && M != Mutation::FilledByCallee) {
        FormatByteImmediate = Code.size() + 3;
        Code.insert(
            Code.end(),
            {0xc6, 0x40, 1, uint8_t(M == Mutation::CountOutput ? 'n' : 'd')});
      }
      if (M != Mutation::MissingNUL)
        Code.insert(Code.end(), {0xc6, 0x40, 2, 0});
    }
    if (M == Mutation::UnknownByte)
      Code.insert(Code.end(), {0x88, 0x50, 1}); // unknown dl replaces one byte
    if (M == Mutation::MatchingDiamond || M == Mutation::ConflictingDiamond)
      Code.insert(Code.end(),
                  {0x85, 0xff, 0x74, 6, 0xc6, 0x40, 1,
                   uint8_t(M == Mutation::MatchingDiamond ? 'd' : 'n'), 0xeb, 4,
                   0xc6, 0x40, 1, 'd'});
    if (M == Mutation::FilledByCallee || M == Mutation::SavedAfterCallee ||
        M == Mutation::NeighborWrite || M == Mutation::ChangedByCallee ||
        M == Mutation::SavedFrameOverwrite || M == Mutation::UnknownCallee) {
      Code.insert(Code.end(), {0x48, 0x8d, 0x4c, 0x24, 40});
      DirectCall(Leaf);
    }
    if (M == Mutation::SavedAfterCallee)
      SaveFrame();
    Code.insert(Code.end(), {0xb9, 1, 0, 0, 0});
    ImportCall(StreamIAT);
    Code.insert(Code.end(), {0x48, 0x89, 0xc2, 0x49, 0xb8});
    word(Code, Format, 8);
    Code.insert(Code.end(), {0x45, 0x31, 0xc9, 0x48, 0x8d, 0x44, 0x24, 48, 0x48,
                             0x89, 0x44, 0x24, 32});
    ImportCall(PrintIAT);
    Code.insert(Code.end(), {0x48, 0x8b, 0x4c, 0x24, 40});
    UnwindCall = Entry + Code.size();
    DirectCall(Unwind);
    Code.insert(Code.end(), {0x48, 0x83, 0xc4, 56, 0xc3});
    Segment Text;
    Text.Name = ".text";
    Text.VA = Entry;
    Text.Size = Text.FileSz = 0x301;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.assign(Text.Size, 0xcc);
    std::copy(Code.begin(), Code.end(), Text.Data.begin());
    std::vector<uint8_t> Body{0x48, 0xb8};
    word(Body, Format, 8);
    if (M == Mutation::SavedFrameOverwrite)
      Body.insert(Body.end(), {0x48, 0xc7, 0x01, 0, 0, 0, 0});
    else
      Body.insert(Body.end(),
                  {0xc6, 0x40, uint8_t(M == Mutation::NeighborWrite ? 4 : 1),
                   uint8_t(M == Mutation::ChangedByCallee ? 'n' : 'd')});
    Body.push_back(0xc3);
    std::copy(Body.begin(), Body.end(), Text.Data.begin() + Leaf - Entry);
    Text.Data[Unwind - Entry] = 0xc3;
    Image.Segments.push_back(std::move(Text));
    Image.Symbols.push_back(Symbol::makeFunc(Entry, Code.size()));
    Image.Symbols.push_back(Symbol::makeFunc(Leaf, Body.size()));
    Image.Symbols.push_back(Symbol::makeFunc(Unwind, 1));
    Segment Data;
    Data.Name = ".data";
    Data.VA = Format;
    Data.Size = Data.FileSz = 8;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    // A harmless file-only format cannot grant runtime authority. Other cases
    // start with %n, so a success has to come from the decoded writes.
    Data.Data = {
        '%', uint8_t(M == Mutation::FileOnly ? 'd' : 'n'), 0, 0, 0, 0, 0, 0};
    if (M == Mutation::TruncatedStorage)
      Data.FileSz = 2;
    Image.Segments.push_back(std::move(Data));
    Segment IAT;
    IAT.Name = ".idata";
    IAT.VA = StreamIAT;
    IAT.Size = IAT.FileSz = 16;
    IAT.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    IAT.Data.assign(16, 0);
    Image.Segments.push_back(std::move(IAT));
    for (const auto &S : Image.Segments) {
      Section Section;
      Section.Name = S.Name;
      Section.VA = S.VA;
      Section.Size = S.Size;
      Section.FileSz = S.FileSz;
      Section.FileOff = S.FileOff;
      Section.Flags = S.Flags;
      Image.Sections.push_back(std::move(Section));
    }
    if (M == Mutation::ConflictingStorage)
      Image.Segments.push_back(Image.Segments[1]);
    Image.Imports.push_back(
        {"api-ms-win-crt-stdio-l1-1-0.dll", "__acrt_iob_func", 0, StreamIAT});
    Image.Imports.push_back(
        {M == Mutation::WrongProvider ? "application.dll" : "ucrtbase.dll",
         "__stdio_common_vfprintf", 0, PrintIAT});
    for (const auto &Import : Image.Imports)
      EXPECT_TRUE(Image.recordImportStorageSlot(
          Import.IATAddr, Import.Name, 0,
          ImportStorageEvidence::ImportDirectory));
  }
};

void check(Mutation M, bool Expected) {
  SCOPED_TRACE(unsigned(M));
  Fixture F(M);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  CFGBuilder Builder;
  std::vector<LowFunc> Bodies;
  Bodies.push_back(Builder.build(F.Image, Dec, Entry, "caller"));
  Bodies.push_back(Builder.build(F.Image, Dec, Leaf, "leaf"));
  ASSERT_TRUE(Bodies[0].hasCompleteLiftCoverage());
  ASSERT_TRUE(Bodies[1].hasCompleteLiftCoverage());
  std::map<va_t, const LowFunc *> Callees;
  if (M != Mutation::UnknownCallee)
    Callees.emplace(Leaf, &Bodies[1]);
  const auto Memory = collectLowSEHRuntimeMemory(F.Image, Bodies);
  EXPECT_TRUE(Memory.scalarFormats().empty());
  size_t Work = 100000;
  const auto Proof = proveLowSEHFrames(Bodies[0], getTargetRegInfo(Arch::X64),
                                       Work, &Callees, &Memory);
  if (!Expected) {
    EXPECT_TRUE(Proof.Calls.empty());
    return;
  }
  ASSERT_EQ(Proof.Calls.size(), 1u);
  EXPECT_EQ(Proof.Calls.begin()->first.first, F.UnwindCall);
  EXPECT_EQ(Proof.Calls.begin()->second, -56);
  EXPECT_TRUE(Proof.UsesRuntimeMemory);
  EXPECT_EQ(Proof.DependencyDigest,
            lowSEHFrameDependencyDigest(Bodies[0], Proof.CalleeDependencies,
                                        &Callees, &Memory));
}

} // namespace

TEST(SEHImageFormat, DecodedStoresOwnWritableFormatBytes) {
  check(Mutation::None, true);
  check(Mutation::WordStore, true);
  check(Mutation::MatchingDiamond, true);
  check(Mutation::FilledByCallee, true);
  check(Mutation::SavedAfterCallee, true);
  check(Mutation::NeighborWrite, true);
}

TEST(SEHImageFormat, InitialOrConflictingBytesDoNotSupplyAuthority) {
  for (auto M :
       {Mutation::FileOnly, Mutation::MissingByte, Mutation::MissingNUL,
        Mutation::CountOutput, Mutation::UnknownByte,
        Mutation::ConflictingDiamond, Mutation::ChangedByCallee})
    check(M, false);
}

TEST(SEHImageFormat, UnknownCallsAndSavedFrameOverwritesRemainUnproved) {
  check(Mutation::UnknownCallee, false);
  check(Mutation::SavedFrameOverwrite, false);
  check(Mutation::WrongProvider, false);
}

TEST(SEHImageFormat, EveryWrittenByteRequiresAnUnambiguousImageExtent) {
  check(Mutation::TruncatedStorage, false);
  check(Mutation::ConflictingStorage, false);
}

TEST(SEHImageFormat, ReceiptsBindDecodedStoresAndIgnoreInitialWritableValues) {
  Fixture F(Mutation::None);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  CFGBuilder Builder;
  auto Caller = Builder.build(F.Image, Dec, Entry, "caller");
  ASSERT_TRUE(Caller.hasCompleteLiftCoverage());
  const auto Memory = collectLowSEHRuntimeMemory(F.Image, {Caller});
  size_t Work = 100000;
  const auto Original = proveLowSEHFrames(Caller, getTargetRegInfo(Arch::X64),
                                          Work, nullptr, &Memory);
  ASSERT_EQ(Original.Calls.size(), 1u);
  F.Image.Segments[1].Data[1] = 'd';
  const auto OtherFileBytes = collectLowSEHRuntimeMemory(F.Image, {Caller});
  EXPECT_EQ(lowSEHRuntimeMemoryDigest(Memory),
            lowSEHRuntimeMemoryDigest(OtherFileBytes));
  Work = 100000;
  EXPECT_EQ(Original.DependencyDigest,
            proveLowSEHFrames(Caller, getTargetRegInfo(Arch::X64), Work,
                              nullptr, &OtherFileBytes)
                .DependencyDigest);
  ASSERT_NE(F.FormatByteImmediate, 0u);
  F.Image.Segments[0].Data[F.FormatByteImmediate] = 'n';
  auto Changed = Builder.build(F.Image, Dec, Entry, "caller");
  ASSERT_TRUE(Changed.hasCompleteLiftCoverage());
  EXPECT_NE(lowSEHFrameDependencyDigest(Caller),
            lowSEHFrameDependencyDigest(Changed));
  Work = 100000;
  EXPECT_TRUE(proveLowSEHFrames(Changed, getTargetRegInfo(Arch::X64), Work,
                                nullptr, &OtherFileBytes)
                  .Calls.empty());
}
