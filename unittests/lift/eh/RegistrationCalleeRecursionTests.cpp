//===- RegistrationCalleeRecursionTests.cpp - Mixed proof cycles ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep registration and x87 call proofs in one bounded invocation closure.

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>

using namespace neverd;

namespace {
BinaryImage makeRecursiveSEHImage(bool Cycle) {
  constexpr va_t Entry = 0x401000, Table = 0x403000;
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x400000;
  Image.Entry = Entry;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(0x110, 0xcc);
  const uint8_t Parent[] = {
      0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68, 0,    0x30, 0x40, 0,
      0x68, 0x70, 0x10, 0x40, 0,    0x64, 0xa1, 0,    0,    0,
      0,    0x50, 0x64, 0x89, 0x25, 0,    0,    0,    0,    0xc7,
      0x45, 0xfc, 0,    0,    0,    0, // Activate the finally scope.
      0xe8, 0xd7, 0,    0,    0,       // Call the ordinary wrapper.
      0xc7, 0x45, 0xfc, 0xff, 0xff, 0xff, 0xff, 0xe8, 0x2b, 0,
      0,    0, // Call the finally on ordinary exit.
      0x8b, 0x4d, 0xf0, 0x64, 0x89, 0x0d, 0,    0,    0,    0,
      0x8b, 0xe5, 0x5d, 0xc3};
  std::copy(std::begin(Parent), std::end(Parent), Text.Data.begin());
  Text.Data[0x60] = 0xc3; // Separate finally invocation.
  Text.Data[0x70] = 0xc3; // Personality reference, not callable authority.
  if (Cycle) {
    // This wrapper cannot borrow its own pending ESP answer: the argument
    // remains below entry ESP when the parent returns with zero cleanup.
    const uint8_t Wrapper[] = {0x6a, 0, 0xe8, 0xf9, 0xfe, 0xff, 0xff, 0xc3};
    std::copy(std::begin(Wrapper), std::end(Wrapper),
              Text.Data.begin() + 0x100);
  } else
    Text.Data[0x100] = 0xc3;
  Text.Size = Text.FileSz = Text.Data.size();
  Section Code;
  Code.Name = Text.Name;
  Code.VA = Entry;
  Code.Size = Code.FileSz = Text.Size;
  Code.Flags = Text.Flags;
  Image.Sections.push_back(Code);
  Image.Segments.push_back(std::move(Text));
  Segment ScopeTable;
  ScopeTable.Name = ".rdata";
  ScopeTable.VA = Table;
  ScopeTable.Flags = SegmentFlags::Readable;
  ScopeTable.Data.resize(12);
  writeLE<uint32_t>(ScopeTable.Data.data(), UINT32_MAX);
  writeLE<uint32_t>(ScopeTable.Data.data() + 8, Entry + 0x60);
  ScopeTable.Size = ScopeTable.FileSz = ScopeTable.Data.size();
  Image.Segments.push_back(std::move(ScopeTable));
  Image.Symbols.push_back(Symbol::makeFunc(Entry, 0x80));
  Image.Symbols.push_back(Symbol::makeFunc(Entry + 0x100, 0x10));
  ExceptionFunction EH;
  EH.CodeRange = {Entry, Entry + 0x80};
  EH.Personality = ExceptionPersonality::ExceptHandler3;
  EH.Encoding = ExceptionEncoding::X86ScopeTableEH3;
  auto &Chain = EH.Registration.emplace();
  Chain.HandlerVA = Entry + 0x70;
  Chain.ScopeTableVA = Table;
  Chain.SeededTryLevel = -1;
  Chain.TryLevelOffset = -4;
  Chain.RegistrationOffset = -16;
  Chain.ChainInstallVA = Entry + 0x16;
  Chain.ChainRemoveVA = Entry + 0x38;
  Chain.TryLevelStores = {{Entry + 0x1d, Entry + 0x24, 0},
                          {Entry + 0x29, Entry + 0x30, -1}};
  Chain.Scopes = {{-1, 0, Entry + 0x60, true}};
  Image.ExceptionMetadata.Functions.push_back(EH);
  Image.ExceptionMetadata.rebuildIndex();
  return Image;
}
} // namespace

TEST(RegistrationCallABI, FloatingAndRegistrationProofsSharePendingCallees) {
  for (bool Cycle : {false, true}) {
    SCOPED_TRACE(Cycle);
    auto Image = makeRecursiveSEHImage(Cycle);
    size_t Work = 0;
    // The parent's explicit EBP restoration proves only its own RET. The
    // recursive wrapper still has no balanced returning-stack contract.
    EXPECT_EQ(getCheckedX86CalleeStackPop(Image, Image.Entry, &Work), 0u);
    EXPECT_LE(Work, limits::kMaxRegistrationEHStateWork);
    Work = 0;
    EXPECT_EQ(getCheckedX86CalleeStackPop(Image, Image.Entry + 0x100, &Work),
              Cycle ? std::nullopt : std::optional<uint32_t>{0});
    EXPECT_LE(Work, limits::kMaxRegistrationEHStateWork);
  }
}
