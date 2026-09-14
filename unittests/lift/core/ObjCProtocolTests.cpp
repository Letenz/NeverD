#include "gtest/gtest.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCCallHints.h"
#include "neverd/loader/ObjC/ObjCEncoding.h"
#include "neverd/loader/ObjC/ObjCMethods.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <cstring>
#include <functional>

namespace {
using namespace neverd;

void word(BinaryImage &Image, va_t Address, uint64_t Value) {
  llvm::support::endian::write64le(
      Image.Segments[0].Data.data() + Address - 0x1000, Value);
}
void integer(BinaryImage &Image, va_t Address, uint32_t Value) {
  llvm::support::endian::write32le(
      Image.Segments[0].Data.data() + Address - 0x1000, Value);
}
void string(BinaryImage &Image, va_t Address, const char *Value) {
  std::memcpy(Image.Segments[0].Data.data() + Address - 0x1000, Value,
              std::strlen(Value) + 1);
}

BinaryImage protocolImage() {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  Segment Segment;
  Segment.VA = 0x1000;
  Segment.Size = Segment.FileSz = 0x5000;
  Segment.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Segment.Data.resize(Segment.Size);
  Image.Segments.push_back(Segment);
  auto Section = [&](const char *Name, va_t Address, uint64_t Size,
                     bool Executable = false) {
    neverd::Section S;
    S.Name = Name;
    S.VA = Address;
    S.FileOff = Address - 0x1000;
    S.Size = S.FileSz = Size;
    S.Flags = SegmentFlags::Readable;
    if (Executable)
      S.Flags = S.Flags | SegmentFlags::Executable;
    Image.Sections.push_back(S);
  };
  Section("__text", 0x1000, 0x1000, true);
  Section("__objc_protolist", 0x2000, 8);
  Section("__objc_const", 0x2100, 0x2f00);
  Section("__objc_selrefs", 0x5000, 8);
  Section("__got", 0x5008, 8);
  word(Image, 0x2000, 0x2200);
  word(Image, 0x2208, 0x2600);
  word(Image, 0x2218, 0x2400);
  integer(Image, 0x2240, 96);
  string(Image, 0x2600, "ExternalMetrics");
  string(Image, 0x2680, "measure:");
  string(Image, 0x2700, "Q24@0:8@16");
  integer(Image, 0x2400, 24);
  integer(Image, 0x2404, 1);
  word(Image, 0x2408, 0x2680);
  word(Image, 0x2410, 0x2700);
  // Protocol method descriptions have no implementation address.
  word(Image, 0x2418, 0);
  word(Image, 0x5000, 0x2680);
  Image.ImportPtrSlots[0x5008] = "_objc_msgSend";
  // ADRP/LDR x1 select measure:, then ADRP/LDR/BR x16 dispatch normally.
  const uint32_t Stub[] = {0x90000021, 0xf9400021, 0x90000030, 0xf9400610,
                           0xd61f0200};
  for (unsigned I = 0; I != 5; ++I)
    integer(Image, 0x1100 + I * 4, Stub[I]);
  return Image;
}

LowFunc caller() {
  LowFunc Function;
  Function.Entry = 0x1200;
  Function.Blocks.resize(1);
  auto &Block = Function.Blocks[0];
  Block.Id = 0;
  Block.StartAddr = 0x1200;
  Block.EndAddr = 0x1208;
  LowOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = 0x1200;
  Call.Output = NdVar::reg(getTargetRegInfo(Arch::AArch64).IntReturnReg, 8);
  Call.addInput(NdVar::cst(0x1100, 8));
  Block.Ops.push_back(Call);
  return Function;
}

TEST(ObjCProtocols, DeclarationsBindCallsWithoutInventingImplementations) {
  auto Image = protocolImage();
  parseObjCMethods(Image);
  parseObjCStorage(Image);
  EXPECT_TRUE(Image.ObjCMethods.empty());
  EXPECT_TRUE(Image.ObjCClasses.empty());
  EXPECT_TRUE(Image.Symbols.empty());
  const auto Hints = buildObjCSourceCallHints(Image, caller());
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Hint = Hints.begin()->second;
  EXPECT_EQ(Hint.Selector, "measure:");
  EXPECT_EQ(Hint.Signature.ReturnType->Size, 8U);
  ASSERT_EQ(Hint.Signature.Parameters.size(), 3U);
  EXPECT_EQ(Hint.Signature.Parameters[2].Type->Kind, NdTypeKind::Ptr);
}

TEST(ObjCProtocols, AggregatePointersHaveOpaqueScalarCarriers) {
  const std::string Encoding = "^{?=Q^@^Q[5Q]}";
  size_t Offset = 0;
  const auto Type = parseObjCScalarType(Encoding, Offset);
  ASSERT_TRUE(Type);
  EXPECT_EQ(Offset, Encoding.size());
  EXPECT_EQ(Type->Kind, NdTypeKind::Ptr);
  ASSERT_TRUE(Type->Pointee);
  EXPECT_EQ(Type->Pointee->Kind, NdTypeKind::Void);
  EXPECT_EQ(Type->Size, 8U);
}

void compactList(BinaryImage &Image, bool Direct) {
  integer(Image, 0x2400, Direct ? 0xc000000c : 0x8000000c);
  integer(Image, 0x2408, (Direct ? 0x2680 : 0x5000) - 0x2408);
  integer(Image, 0x240c, 0x2700 - 0x240c);
  integer(Image, 0x2410, 0);
}

TEST(ObjCProtocols, X64CallsitesUseProtocolDeclarationsWithObservedSelectors) {
  auto Image = protocolImage();
  Image.Arch = Arch::X64;
  compactList(Image, false);
  parseObjCMethods(Image);
  parseObjCStorage(Image);
  auto Low = caller();
  auto &Ops = Low.Blocks[0].Ops;
  Ops[0].Opcode = NdOp::INDIR_CALL;
  Ops[0].Inputs[0] = NdVar::cst(0x5008, 8);
  Ops[0].Output = NdVar::reg(getTargetRegInfo(Arch::X64).IntReturnReg, 8);
  LowOp Selector;
  Selector.Opcode = NdOp::LOAD;
  Selector.Addr = 0x11f0;
  Selector.Output = NdVar::reg(getTargetRegInfo(Arch::X64).IntParamRegs[1], 8);
  Selector.addInput(NdVar::cst(0x5000, 8));
  Ops.insert(Ops.begin(), Selector);
  EXPECT_EQ(buildObjCSourceCallHints(Image, Low).size(), 1U);
  Ops.erase(Ops.begin());
  EXPECT_TRUE(buildObjCSourceCallHints(Image, Low).empty());
}

TEST(ObjCProtocols, ListLayoutsAndMethodKindsPreserveBothDarwinABIs) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Layout = 0; Layout < 3; ++Layout)
      for (unsigned Kind = 0; Kind < 4; ++Kind) {
        SCOPED_TRACE(::testing::Message()
                     << int(Architecture) << ':' << Layout << ':' << Kind);
        auto Image = protocolImage();
        Image.Arch = Architecture;
        if (Layout)
          compactList(Image, Layout == 2);
        word(Image, 0x2218, 0);
        word(Image, 0x2218 + Kind * 8, 0x2400);
        parseObjCMethods(Image);
        ASSERT_EQ(Image.ObjCProtocols.size(), 1U);
        const auto &Protocol = Image.ObjCProtocols[0];
        EXPECT_EQ(Protocol.Status, "recovered");
        ASSERT_EQ(Protocol.Methods.size(), 1U);
        const auto &Method = Protocol.Methods[0];
        EXPECT_EQ(Method.Selector, "measure:");
        EXPECT_EQ(Method.IsClassMethod, (Kind & 1) != 0);
        EXPECT_EQ(Method.IsOptional, Kind >= 2);
        ASSERT_TRUE(Method.TypeHint);
        EXPECT_EQ(Method.TypeHint->ReturnLocation.RegisterOffset,
                  getTargetRegInfo(Architecture).IntReturnReg);
        EXPECT_EQ(Method.TypeHint->Parameters[2].Location.RegisterOffset,
                  getTargetRegInfo(Architecture).IntParamRegs[2]);
        EXPECT_TRUE(Image.ObjCMethods.empty());
        EXPECT_TRUE(Image.Symbols.empty());
      }
}

void inherit(BinaryImage &Image) {
  // The root has no methods and adopts the only declaration-bearing protocol.
  word(Image, 0x2218, 0);
  word(Image, 0x2210, 0x2800);
  word(Image, 0x2800, 2);
  word(Image, 0x2808, 0x2900);
  word(Image, 0x2810, 0x2900);
  word(Image, 0x2908, 0x2600);
  word(Image, 0x2918, 0x2400);
  integer(Image, 0x2940, 72);
}

TEST(ObjCProtocols,
     InheritanceDeduplicatesAndReparseDiscardsStaleDeclarations) {
  auto Image = protocolImage();
  inherit(Image);
  auto Reference = Image.Sections[1];
  Reference.Name = "__objc_protorefs";
  Reference.VA = 0x2010;
  Reference.FileOff = 0x1010;
  Image.Sections.push_back(Reference);
  word(Image, 0x2010, 0x2900);
  for (unsigned Order = 0; Order < 2; ++Order) {
    parseObjCMethods(Image);
    parseObjCStorage(Image);
    ASSERT_EQ(Image.ObjCProtocols.size(), 2U);
    ASSERT_EQ(Image.ObjCProtocols[0].AdoptedProtocols.size(), 1U);
    EXPECT_EQ(Image.ObjCProtocols[0].AdoptedProtocols[0], 0x2900U);
    EXPECT_EQ(buildObjCSourceCallHints(Image, caller()).size(), 1U);
    std::reverse(Image.Sections.begin(), Image.Sections.end());
  }
  word(Image, 0x2000, 0);
  word(Image, 0x2010, 0);
  parseObjCMethods(Image);
  EXPECT_TRUE(Image.ObjCProtocols.empty());
  EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
}

TEST(ObjCProtocols,
     ConflictingAndUnsupportedDeclarationsCannotSelectAPrototype) {
  for (const auto *Encoding :
       {"Q24@0:8@16", "d24@0:8@16", "{Pair=QQ}24@0:8@16"})
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(Encoding);
      auto Image = protocolImage();
      inherit(Image);
      word(Image, 0x2218, 0x2a00);
      integer(Image, 0x2a00, 24);
      integer(Image, 0x2a04, 1);
      word(Image, 0x2a08, 0x2680);
      word(Image, 0x2a10, 0x2b00);
      string(Image, 0x2b00, Encoding);
      parseObjCMethods(Image);
      parseObjCStorage(Image);
      if (Reverse)
        std::reverse(Image.ObjCProtocols.begin(), Image.ObjCProtocols.end());
      EXPECT_EQ(buildObjCSourceCallHints(Image, caller()).size(),
                Encoding[0] == 'Q' ? 1U : 0U);
      ObjCMethod Local;
      Local.Selector = "measure:";
      Local.TypeHint = parseObjCMethodEncoding(Local.Selector, "d24@0:8@16");
      ASSERT_TRUE(Local.TypeHint);
      Image.ObjCMethods.push_back(Local);
      EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
    }
}

TEST(ObjCProtocols,
     FullWidthIntegerResultsPreserveBitsAcrossSignedDeclarations) {
  for (const auto *Encoding :
       {"q24@0:8@16", "i24@0:8@16", "@24@0:8@16", "Q24@0:8Q16"})
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(Encoding);
      auto Image = protocolImage();
      parseObjCMethods(Image);
      parseObjCStorage(Image);
      auto Other = Image.ObjCProtocols.front();
      Other.Methods[0].TypeHint = parseObjCMethodEncoding("measure:", Encoding);
      Image.ObjCProtocols.push_back(Other);
      if (Reverse)
        std::reverse(Image.ObjCProtocols.begin(), Image.ObjCProtocols.end());
      const auto Hints = buildObjCSourceCallHints(Image, caller());
      if (Encoding[0] != 'q') {
        EXPECT_TRUE(Hints.empty());
        continue;
      }
      ASSERT_EQ(Hints.size(), 1U);
      const auto &Result = Hints.begin()->second.Signature;
      EXPECT_EQ(Result.ReturnType->Size, 8U);
      EXPECT_FALSE(Result.ReturnType->IsSigned);
      EXPECT_EQ(Result.ReturnLocation.RegisterOffset,
                getTargetRegInfo(Arch::AArch64).IntReturnReg);
      EXPECT_EQ(Image.ObjCProtocols[Reverse ? 0 : 1]
                    .Methods[0]
                    .TypeHint->ReturnType->IsSigned,
                true);
    }
}

TEST(ObjCProtocols, MalformedMetadataCannotSupplyHintsOrFunctionSeeds) {
  const std::vector<std::function<void(BinaryImage &)>> Mutations{
      [](auto &I) { integer(I, 0x2240, 71); },
      [](auto &I) { integer(I, 0x2240, 4097); },
      [](auto &I) { integer(I, 0x2404, 65537); },
      [](auto &I) { integer(I, 0x2400, 16); },
      [](auto &I) { integer(I, 0x2400, 0x40000018); },
      [](auto &I) { word(I, 0x2418, 0x1100); },
      [](auto &I) { I.ImportPtrSlots[0x2418] = "_foreign"; },
      [](auto &I) { word(I, 0x2208, 0); },
      [](auto &I) { I.ImportPtrSlots[0x2208] = "_foreign"; },
      [](auto &I) { word(I, 0x2408, 0); },
      [](auto &I) { word(I, 0x2410, 0); },
      [](auto &I) { I.MachOHasChainedFixups = true; },
      [](auto &I) { I.MachOChainedFixupsAmbiguous = true; },
      [](auto &I) { I.Sections[2].FileSz = 0x100; },
      [](auto &I) { I.Sections[2].FileOff += 8; },
      [](auto &I) { I.Sections[2].Type = llvm::MachO::S_ZEROFILL; },
      [](auto &I) { I.Sections[1].Size = 9; },
      [](auto &I) { I.IsRelocatable = true; },
      [](auto &I) { I.Arch = Arch::ARM; },
      [](auto &I) { I.Format = BinaryFormat::ELF; },
      [](auto &I) {
        compactList(I, false);
        integer(I, 0x2408, 0x80000000);
      },
      [](auto &I) {
        compactList(I, true);
        integer(I, 0x2410, 4);
      },
      [](auto &I) {
        inherit(I);
        word(I, 0x2800, ~uint64_t(0));
      },
      [](auto &I) {
        inherit(I);
        word(I, 0x2800, 1);
        word(I, 0x2808, 0x6000);
      },
      [](auto &I) {
        inherit(I);
        word(I, 0x2910, 0x2800);
      },
  };
  for (size_t Case = 0; Case < Mutations.size(); ++Case) {
    SCOPED_TRACE(Case);
    auto Image = protocolImage();
    Mutations[Case](Image);
    parseObjCMethods(Image);
    parseObjCStorage(Image);
    EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
    EXPECT_TRUE(Image.Symbols.empty());
    EXPECT_TRUE(Image.ObjCMethods.empty());
    for (const auto &Protocol : Image.ObjCProtocols)
      for (const auto &Method : Protocol.Methods)
        EXPECT_FALSE(Method.TypeHint);
  }
}

TEST(ObjCProtocols,
     ResolvedFixupsPreserveLocalDeclarationsAndExternalReferences) {
  auto Image = protocolImage();
  Image.MachOHasChainedFixups = true;
  for (va_t Slot : {0x2000, 0x2208, 0x2218, 0x2408, 0x2410, 0x5000})
    Image.MachOResolvedChainedPointerSlots.insert(Slot);
  parseObjCMethods(Image);
  ASSERT_EQ(Image.ObjCProtocols.size(), 1U);
  ASSERT_EQ(Image.ObjCProtocols[0].Methods.size(), 1U);
  ASSERT_TRUE(Image.ObjCProtocols[0].Methods[0].TypeHint);
  Image.Sections[1].Name = "__objc_protorefs";
  Image.Segments[0].Flags = SegmentFlags::Readable;
  Image.ImportPtrSlots[0x2000] = "_OBJC_PROTOCOL_$_External";
  parseObjCMethods(Image);
  EXPECT_TRUE(Image.ObjCProtocols.empty());
  EXPECT_TRUE(Image.ObjCMetadataDiagnostics.empty());
}

TEST(ObjCProtocols, OpaquePointeesRequireCompleteBoundedTypeSyntax) {
  for (const auto *Encoding :
       {"^{Opaque}", "^(Choice=Qd)", "^[5Q]",
        "^{?=\"field\"Q\"nested\"{S=@\"NSObject\"[2^v]}}"}) {
    SCOPED_TRACE(Encoding);
    size_t Offset = 0;
    EXPECT_TRUE(parseObjCScalarType(Encoding, Offset));
    EXPECT_EQ(Offset, std::string(Encoding).size());
  }
  for (const auto &Encoding : std::vector<std::string>{
           "^{S=Q]", "^{S=\"fieldQ}", "^{S=Z}", "^{=Q}", "^[Q]",
           "^[99999999999Q]", "^{S=^{Inner=Q}", "^{S=\"x\"}",
           std::string(18, '^') + "{Opaque}", "{S=Q}", "[5Q]", "(Choice=Qd)"}) {
    SCOPED_TRACE(Encoding);
    size_t Offset = 0;
    EXPECT_FALSE(parseObjCScalarType(Encoding, Offset));
  }
  EXPECT_TRUE(
      parseObjCMethodEncoding("countByEnumeratingWithState:objects:count:",
                              "Q40@0:8^{?=Q^@^Q[5Q]}16^@24Q32"));
  for (const auto *Encoding :
       {"Q24@0:8", "Q24@0:8@16Q24", "Q24@?0:8@16", "Q24@0@8@16",
        "Q24@0:8{S=Q}16", "Q24@0:8^{S=Q16"})
    EXPECT_FALSE(parseObjCMethodEncoding("measure:", Encoding)) << Encoding;
}
} // namespace
