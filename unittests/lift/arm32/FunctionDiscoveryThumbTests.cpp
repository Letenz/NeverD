//===- FunctionDiscoveryThumbTests.cpp - Thumb function discovery tests --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/FuncDetector.h"
#include "neverd/loader/COFF/COFFLoaderUtils.h"
#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/ISAEncoding.h"

#include "llvm/Object/COFF.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBufferRef.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <vector>

namespace {

using namespace neverd;
namespace fs = std::filesystem;

std::vector<uint8_t> readFixture(llvm::StringRef Name) {
  std::ifstream In(fs::path(TEST_OBJ_DIR) / Name.str(), std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(In), {});
}

std::unique_ptr<llvm::object::COFFObjectFile>
createCOFFObject(const std::vector<uint8_t> &Bytes) {
  llvm::StringRef Data(reinterpret_cast<const char *>(Bytes.data()),
                       Bytes.size());
  auto ObjOrErr = llvm::object::COFFObjectFile::create(
      llvm::MemoryBufferRef(Data, "FunctionDiscoveryThumb fixture"));
  if (!ObjOrErr) {
    ADD_FAILURE() << llvm::toString(ObjOrErr.takeError());
    return nullptr;
  }
  return std::move(*ObjOrErr);
}

BinaryImage makeThumbImage() {
  BinaryImage Img;
  Img.Arch = Arch::ARM;
  Img.Mode = InstructionMode::Thumb;
  Img.Bits = Bitness::Bits32;
  Img.Base = 0x1000;
  return Img;
}

Segment makeExecutableSegment(va_t VA, llvm::ArrayRef<uint8_t> Bytes) {
  Segment Seg;
  Seg.VA = VA;
  Seg.Size = Bytes.size();
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data.assign(Bytes.begin(), Bytes.end());
  return Seg;
}

void encodeThumbMovImm16(uint8_t *P, uint16_t Imm) {
  uint16_t Op1 = readLE<uint16_t>(P);
  uint16_t Op2 = readLE<uint16_t>(P + 2);
  Op1 = uint16_t((Op1 & arm::kThumbMovImmOp1Mask) | ((Imm & 0x0800u) >> 1) |
                 ((Imm >> 12) & 0x000fu));
  Op2 = uint16_t((Op2 & arm::kThumbMovIPOp2Mask) | ((Imm & 0x0700u) << 4) |
                 (Imm & 0x00ffu));
  writeLE<uint16_t>(P, Op1);
  writeLE<uint16_t>(P + 2, Op2);
}

std::array<uint8_t, arm::kThumbImportThunkLen>
makeImportThunk(uint32_t IATAddr) {
  std::array<uint8_t, arm::kThumbImportThunkLen> Bytes = {
      0x40, 0xf2, 0x00, 0x0c, // movw ip, low16
      0xc0, 0xf2, 0x00, 0x0c, // movt ip, high16
      0xdc, 0xf8, 0x00, 0xf0, // ldr.w pc, [ip]
  };
  encodeThumbMovImm16(Bytes.data(), uint16_t(IATAddr));
  encodeThumbMovImm16(Bytes.data() + arm::kThumbMovImmInsnSize,
                      uint16_t(IATAddr >> 16));
  return Bytes;
}

TEST(FunctionDiscoveryThumb, NormalizesCodePointersBeforeDiscovery) {
  BinaryImage Img = makeThumbImage();
  constexpr va_t CodeVA = 0x1000;
  const std::array<uint8_t, 4> Prologue = {
      0x10,
      0xb5, // push {r4, lr}
      0x70,
      0x47, // bx lr
  };
  Img.Segments.push_back(makeExecutableSegment(CodeVA, Prologue));

  Segment Rodata;
  Rodata.VA = 0x2000;
  Rodata.Size = sizeof(uint32_t);
  Rodata.Flags = SegmentFlags::Readable;
  Rodata.Data.resize(sizeof(uint32_t));
  writeLE<uint32_t>(Rodata.Data.data(), uint32_t(CodeVA | 1));
  Img.Segments.push_back(std::move(Rodata));

  scanDataFuncPointers(Img);

  auto It = std::find_if(Img.Symbols.begin(), Img.Symbols.end(),
                         [](const Symbol &S) { return S.IsFunc; });
  ASSERT_NE(It, Img.Symbols.end());
  EXPECT_EQ(It->Addr, CodeVA);
  EXPECT_EQ(It->Addr & 1u, 0u);
}

TEST(FunctionDiscoveryThumb, IgnoresCodePointersInsideSizedFunctions) {
  BinaryImage Img = makeThumbImage();
  constexpr va_t CodeVA = 0x1000;
  constexpr va_t CaseVA = CodeVA + 4;
  const std::array<uint8_t, 8> Code = {
      0x70, 0x47, 0x00, 0xbf, // bx lr; nop
      0x10, 0xb5, 0x70, 0x47, // push {r4, lr}; bx lr
  };
  Img.Segments.push_back(makeExecutableSegment(CodeVA, Code));

  Symbol Covering = Symbol::makeFunc(CodeVA, Code.size());
  Covering.Name = "covering";
  Img.Symbols.push_back(std::move(Covering));

  Segment Rodata;
  Rodata.VA = 0x2000;
  Rodata.Size = sizeof(uint32_t);
  Rodata.Flags = SegmentFlags::Readable;
  Rodata.Data.resize(sizeof(uint32_t));
  writeLE<uint32_t>(Rodata.Data.data(), uint32_t(CaseVA | 1));
  Img.Segments.push_back(std::move(Rodata));

  scanDataFuncPointers(Img);

  EXPECT_EQ(std::count_if(Img.Symbols.begin(), Img.Symbols.end(),
                          [](const Symbol &S) { return S.Addr == CaseVA; }),
            0u);
}

TEST(FunctionDiscoveryThumb, IgnoresPaddingCandidatesInsideSizedFunctions) {
  BinaryImage Img = makeThumbImage();
  constexpr va_t CodeVA = 0x1000;
  constexpr va_t CaseVA = CodeVA + 4;
  const std::array<uint8_t, 8> Code = {
      0x70, 0x47, 0x00, 0x00, // bx lr; instruction bytes resembling padding
      0x10, 0xb5, 0x70, 0x47, // switch case resembling a function prologue
  };
  Img.Segments.push_back(makeExecutableSegment(CodeVA, Code));

  Symbol Covering = Symbol::makeFunc(CodeVA, Code.size());
  Covering.Name = "covering";
  Img.Symbols.push_back(std::move(Covering));

  scanPaddingBoundaries(Img);

  EXPECT_EQ(std::count_if(Img.Symbols.begin(), Img.Symbols.end(),
                          [](const Symbol &S) { return S.Addr == CaseVA; }),
            0u);
}

TEST(FunctionDiscoveryARM,
     ConditionalNoReturnCallDoesNotTerminateCandidateVerification) {
  constexpr va_t EntryVA = 0x1000;
  constexpr va_t CandidateVA = 0x1100;
  constexpr va_t AbortVA = 0x1300;

  BinaryImage Img;
  Img.Arch = Arch::ARM;
  Img.Mode = InstructionMode::Default;
  Img.Format = BinaryFormat::ELF;
  Img.Bits = Bitness::Bits32;
  Img.Base = EntryVA;
  Img.Entry = EntryVA;

  std::vector<uint8_t> Code(AbortVA - EntryVA + sizeof(uint32_t));
  for (size_t Off = 0; Off < Code.size(); Off += sizeof(uint32_t))
    writeLE<uint32_t>(Code.data() + Off, 0xE1A00000u); // nop
  writeLE<uint32_t>(Code.data(), 0xEB00003Eu);         // bl CandidateVA
  writeLE<uint32_t>(Code.data() + 4, 0xE12FFF1Eu);     // bx lr
  writeLE<uint32_t>(Code.data() + (CandidateVA - EntryVA),
                    0x0B00007Eu); // bleq AbortVA
  writeLE<uint32_t>(Code.data() + (AbortVA - EntryVA),
                    0xE12FFF1Eu); // bx lr
  Img.Segments.push_back(makeExecutableSegment(EntryVA, Code));

  Symbol Main = Symbol::makeFunc(EntryVA, 8);
  Main.Name = "main";
  Img.Symbols.push_back(std::move(Main));
  Symbol Abort = Symbol::makeFunc(AbortVA, sizeof(uint32_t));
  Abort.Name = "abort";
  Img.Symbols.push_back(std::move(Abort));

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::ARM, InstructionMode::Default));
  FuncDetector Detector;
  auto Functions = Detector.detect(Img, Dec);

  EXPECT_EQ(std::count_if(Functions.begin(), Functions.end(),
                          [](const auto &F) { return F.first == CandidateVA; }),
            0u);
}

TEST(FunctionDiscoveryARM, PreservesVerifiedMachODirectCallInsideBroadRange) {
  constexpr va_t ImageVA = 0x1000;
  constexpr va_t HelperVA = ImageVA + 0x10;
  constexpr va_t InvalidVA = ImageVA + 0x18;

  BinaryImage Img;
  Img.Arch = Arch::ARM;
  Img.Mode = InstructionMode::Default;
  Img.Format = BinaryFormat::MachO;
  Img.Bits = Bitness::Bits32;
  Img.Base = ImageVA;
  Img.Entry = ImageVA;

  std::vector<uint8_t> Code(0x1a, 0);
  writeLE<uint32_t>(Code.data(), 0xEB000002u);      // bl HelperVA
  writeLE<uint32_t>(Code.data() + 4, 0xEB000003u);  // bl InvalidVA
  writeLE<uint32_t>(Code.data() + 8, 0xE12FFF1Eu);  // bx lr
  writeLE<uint32_t>(Code.data() + 12, 0xE1A00000u); // nop
  writeLE<uint32_t>(Code.data() + 16, 0xE3A0002Au); // mov r0, #42
  writeLE<uint32_t>(Code.data() + 20, 0xE12FFF1Eu); // bx lr
  Code[24] = 0xff;                                  // truncated ARM instruction
  Code[25] = 0xff;
  Img.Segments.push_back(makeExecutableSegment(ImageVA, Code));

  Symbol Covering = Symbol::makeFunc(ImageVA, Code.size());
  Covering.Name = "_main";
  Img.Symbols.push_back(std::move(Covering));
  Img.Exports.push_back({"_main", 0, ImageVA});
  Img.KnownCodeRanges.emplace_back(ImageVA, ImageVA + Code.size());
  Img.ARMCodeModeEntries.emplace(ImageVA, InstructionMode::ARM);
  if (llvm::Error Err = discoverARMReachableModes(Img))
    FAIL() << llvm::toString(std::move(Err));

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::ARM, InstructionMode::Default));
  FuncDetector Detector;
  auto Functions = Detector.detect(Img, Dec);

  EXPECT_EQ(std::count_if(Functions.begin(), Functions.end(),
                          [](const auto &F) { return F.first == ImageVA; }),
            1u);
  EXPECT_EQ(std::count_if(Functions.begin(), Functions.end(),
                          [](const auto &F) { return F.first == HelperVA; }),
            1u);
  EXPECT_EQ(std::count_if(Functions.begin(), Functions.end(),
                          [](const auto &F) { return F.first == InvalidVA; }),
            0u);
}

// A code pointer's bit 0 is the instruction set `blx` enters.  One the
// runtime calls states either set; a stored one only Thumb, since an even
// address may be data.  A pointer that contradicts other evidence states
// nothing, and the runtime's own pointers say so in a load diagnostic.
TEST(FunctionDiscoveryThumb, CodePointersStateTheModeOfWhatTheyAddress) {
  BinaryImage Img;
  Img.Arch = Arch::ARM;
  Img.Bits = Bitness::Bits32;
  Img.Base = 0x1000;
  const std::array<uint8_t, 12> Code = {
      0x01, 0x20, 0x70, 0x47, // 0x1000: movs r0, #1; bx lr
      0x1e, 0xff, 0x2f, 0xe1, // 0x1004: bx lr
      0x1e, 0xff, 0x2f, 0xe1, // 0x1008: bx lr
  };
  Img.Segments.push_back(makeExecutableSegment(0x1000, Code));
  Section Text;
  Text.Name = ".text";
  Text.VA = 0x1000;
  Text.Size = Code.size();
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Code.begin(), Code.end());
  Img.Sections.push_back(Text);
  const auto ModeAt = [&](va_t Address) -> std::optional<InstructionMode> {
    const auto It = Img.ARMCodeModeEntries.find(Address);
    if (It == Img.ARMCodeModeEntries.end())
      return std::nullopt;
    return It->second;
  };

  recordARMCodePointerMode(Img, 0x1001, ARMCodePointerUse::Stored);
  EXPECT_EQ(ModeAt(0x1000), InstructionMode::Thumb);
  recordARMCodePointerMode(Img, 0x1004, ARMCodePointerUse::Stored);
  EXPECT_EQ(ModeAt(0x1004), std::nullopt);
  recordARMCodePointerMode(Img, 0x1004, ARMCodePointerUse::Called);
  EXPECT_EQ(ModeAt(0x1004), InstructionMode::ARM);
  // Outside the code, a pointer is no code pointer.
  recordARMCodePointerMode(Img, 0x2001, ARMCodePointerUse::Called);
  EXPECT_EQ(ModeAt(0x2000), std::nullopt);
  EXPECT_TRUE(Img.LoadDiagnostics.empty());

  // Contradictions keep the entry as it was.
  recordARMCodePointerMode(Img, 0x1005, ARMCodePointerUse::Stored);
  EXPECT_TRUE(Img.LoadDiagnostics.empty());
  recordARMCodePointerMode(Img, 0x1005, ARMCodePointerUse::Called);
  EXPECT_EQ(ModeAt(0x1004), InstructionMode::ARM);
  ASSERT_EQ(Img.LoadDiagnostics.size(), 1u);
  EXPECT_EQ(Img.LoadDiagnostics.back().Code, "elf.arm_code_pointer_mode");
  // `blx` to an ARM address with bit 1 set is unpredictable.
  recordARMCodePointerMode(Img, 0x1002, ARMCodePointerUse::Called);
  EXPECT_EQ(ModeAt(0x1002), std::nullopt);
  EXPECT_EQ(Img.LoadDiagnostics.size(), 2u);
  // Mapping symbols already decide what they cover.
  Img.ARMCodeRegions.push_back({0x1008, 0x100c, ARMCodeRegionKind::ARM});
  recordARMCodePointerMode(Img, 0x1008, ARMCodePointerUse::Called);
  EXPECT_EQ(ModeAt(0x1008), std::nullopt);
  recordARMCodePointerMode(Img, 0x1009, ARMCodePointerUse::Called);
  EXPECT_EQ(ModeAt(0x1008), std::nullopt);
  EXPECT_EQ(Img.LoadDiagnostics.size(), 3u);

  // Without a section table read-only data can share the executable
  // segment, so a stored odd address there may be a string's.
  Img.Sections.clear();
  Img.ARMCodeModeEntries.clear();
  recordARMCodePointerMode(Img, 0x1001, ARMCodePointerUse::Stored);
  EXPECT_EQ(ModeAt(0x1000), std::nullopt);
  recordARMCodePointerMode(Img, 0x1001, ARMCodePointerUse::Called);
  EXPECT_EQ(ModeAt(0x1000), InstructionMode::Thumb);
}

TEST(FunctionDiscoveryThumb, UndefinedAliasesStopReachableModeDiscovery) {
  for (uint8_t Code : {0x00, 0xf9, 0xfe, 0xff}) {
    SCOPED_TRACE(unsigned(Code));
    BinaryImage Img = makeThumbImage();
    Img.Mode = InstructionMode::Default;
    Img.Format = BinaryFormat::ELF;
    const std::array<uint8_t, 6> Bytes = {
        Code, 0xde, // udf #imm8, including __brkdiv0 and trap
        42,   0x20, 0x70, 0x47};
    Img.Segments.push_back(makeExecutableSegment(0x1000, Bytes));
    Img.ARMCodeModeEntries.emplace(0x1000, InstructionMode::Thumb);
    ARMModeDiscovery Discovery(Img);
    if (llvm::Error Err = Discovery.discover())
      FAIL() << llvm::toString(std::move(Err));
    EXPECT_EQ(Discovery.disassembledInstructions(), 1u);
    EXPECT_EQ(Img.instructionModeAt(0x1000), InstructionMode::Thumb);
    EXPECT_EQ(Img.instructionModeAt(0x1002), std::nullopt);
    ASSERT_EQ(Img.ARMReachableCodeRegions.size(), 1u);
    EXPECT_EQ(Img.ARMReachableCodeRegions.front().End, 0x1002u);
  }
}

// The passes that decode an image's reachable code repeat when they learn a
// literal or a routine that never returns, and the landing pads of the
// exception tables extend what they decoded.  Neither decodes an instruction
// twice: what an instruction does depends only on its bytes and its mode.
TEST(FunctionDiscoveryThumb, LandingPadsExtendTheDecodeWithoutRepeatingIt) {
  BinaryImage Img;
  Img.Arch = Arch::ARM;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::ELF;
  Img.Base = 0x1000;
  const std::array<uint8_t, 40> Code = {
      0x80, 0xb5,             // 0x1000: push {r7, lr}
      0x00, 0xf0, 0x0d, 0xf8, // 0x1002: bl 0x1020, guarded
      0x00, 0x20,             // 0x1006: movs r0, #0
      0x80, 0xbd,             // 0x1008: pop {r7, pc}
      0x00, 0xbf,             // 0x100a: nop, unreached
      0x01, 0x20,             // 0x100c: movs r0, #1, the landing pad
      0x80, 0xbd,             // 0x100e: pop {r7, pc}
      0x01, 0x48,             // 0x1010: ldr r0, [pc, #4]
      0x00, 0xf0, 0x07, 0xf8, // 0x1012: bl 0x1024
      0x00, 0xbf,             // 0x1016: nop, after a call that never returns
      0x78, 0x56, 0x34, 0x12, // 0x1018: the literal
      0x00, 0xbf, 0x00, 0xbf, // 0x101c: padding
      0x70, 0x47,             // 0x1020: bx lr
      0x00, 0xbf,             // 0x1022: padding
      0x00, 0xde,             // 0x1024: udf #0
      0x00, 0xbf,             // 0x1026: padding
  };
  Img.Segments.push_back(makeExecutableSegment(0x1000, Code));
  Section Text;
  Text.Name = ".text";
  Text.VA = 0x1000;
  Text.Size = Code.size();
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Code.begin(), Code.end());
  Img.Sections.push_back(Text);
  Img.ARMCodeModeEntries.emplace(0x1000, InstructionMode::Thumb);
  Img.ARMCodeModeEntries.emplace(0x1010, InstructionMode::Thumb);

  ARMModeDiscovery Discovery(Img);
  if (llvm::Error Err = Discovery.discover())
    FAIL() << llvm::toString(std::move(Err));
  // The first pass followed the call to 0x1024 and stopped at the literal.
  // Once it proved 0x1024 never returns, the next pass stopped at the call.
  EXPECT_EQ(Img.instructionModeAt(0x1012), InstructionMode::Thumb);
  EXPECT_EQ(Img.instructionModeAt(0x1016), std::nullopt);
  EXPECT_EQ(Img.instructionModeAt(0x100c), std::nullopt);
  EXPECT_EQ(Discovery.disassembledInstructions(), 9u);

  ExceptionFunction Function;
  Function.Itanium.emplace();
  ItaniumCallSite Site;
  Site.GuardedRange = {0x1002, 0x1006};
  Site.LandingPadVA = 0x100c;
  Function.Itanium->CallSites.push_back(Site);
  Img.ExceptionMetadata.Functions.push_back(std::move(Function));
  if (llvm::Error Err = Discovery.followLandingPads())
    FAIL() << llvm::toString(std::move(Err));
  EXPECT_EQ(Img.instructionModeAt(0x100c), InstructionMode::Thumb);
  EXPECT_EQ(Img.instructionModeAt(0x100e), InstructionMode::Thumb);
  EXPECT_EQ(Img.instructionModeAt(0x100a), std::nullopt);
  EXPECT_EQ(Img.instructionModeAt(0x1016), std::nullopt);
  EXPECT_EQ(Discovery.disassembledInstructions(), 11u);
  const std::vector<ARMCodeRegion> Expected = {
      {0x1000, 0x100a, ARMCodeRegionKind::Thumb},
      {0x100c, 0x1016, ARMCodeRegionKind::Thumb},
      {0x1020, 0x1022, ARMCodeRegionKind::Thumb},
      {0x1024, 0x1026, ARMCodeRegionKind::Thumb},
  };
  ASSERT_EQ(Img.ARMReachableCodeRegions.size(), Expected.size());
  for (size_t Index = 0; Index < Expected.size(); ++Index) {
    SCOPED_TRACE(Index);
    EXPECT_EQ(Img.ARMReachableCodeRegions[Index].Start, Expected[Index].Start);
    EXPECT_EQ(Img.ARMReachableCodeRegions[Index].End, Expected[Index].End);
    EXPECT_EQ(Img.ARMReachableCodeRegions[Index].Kind, Expected[Index].Kind);
  }
}

TEST(FunctionDiscoveryThumb, NormalizesOnlyFunctionSymbols) {
  std::vector<uint8_t> Bytes = readFixture("test_patch_coff_arm.obj");
  if (Bytes.empty())
    GTEST_SKIP() << "ARM32 COFF fixture not built";
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);

  std::optional<va_t> ExpectedFunctionAddr;
  std::optional<va_t> ExpectedDataAddr;
  const char *FileBegin = Obj->getData().data();
  for (const llvm::object::SymbolRef &SymRef : Obj->symbols()) {
    llvm::object::COFFSymbolRef Sym = Obj->getCOFFSymbol(SymRef);
    auto NameOrErr = Obj->getSymbolName(Sym);
    ASSERT_TRUE(static_cast<bool>(NameOrErr))
        << llvm::toString(NameOrErr.takeError());
    if (*NameOrErr != "pe_stacky" && *NameOrErr != "g_result")
      continue;

    const char *Raw = static_cast<const char *>(Sym.getRawPtr());
    ASSERT_GE(Raw, FileBegin);
    size_t SymOff = static_cast<size_t>(Raw - FileBegin);
    size_t ValueOff =
        SymOff + offsetof(llvm::object::coff_symbol_generic, Value);
    ASSERT_TRUE(rangeInBounds(ValueOff, sizeof(uint32_t), Bytes.size()));
    uint32_t OddValue = Sym.getValue() | 1u;
    writeLE<uint32_t>(Bytes.data() + ValueOff, OddValue);

    va_t Addr = OddValue;
    if (Sym.getSectionNumber() > 0) {
      auto SecOrErr = Obj->getSection(Sym.getSectionNumber());
      ASSERT_TRUE(static_cast<bool>(SecOrErr))
          << llvm::toString(SecOrErr.takeError());
      Addr += (*SecOrErr)->VirtualAddress;
    }
    if (*NameOrErr == "pe_stacky")
      ExpectedFunctionAddr = clearThumbBit(Addr);
    else
      ExpectedDataAddr = Addr;
  }
  ASSERT_TRUE(ExpectedFunctionAddr.has_value());
  ASSERT_TRUE(ExpectedDataAddr.has_value());
  ASSERT_EQ(*ExpectedFunctionAddr & 1u, 0u);
  ASSERT_EQ(*ExpectedDataAddr & 1u, 1u);

  Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  BinaryImage Img = makeThumbImage();
  coff_loader::parseSymbolTable(*Obj, Img, 0);

  auto Find = [&](llvm::StringRef Name) {
    return std::find_if(Img.Symbols.begin(), Img.Symbols.end(),
                        [&](const Symbol &S) { return S.Name == Name; });
  };
  auto Function = Find("pe_stacky");
  ASSERT_NE(Function, Img.Symbols.end());
  EXPECT_TRUE(Function->IsFunc);
  EXPECT_EQ(Function->Addr, *ExpectedFunctionAddr);

  auto Data = Find("g_result");
  ASSERT_NE(Data, Img.Symbols.end());
  EXPECT_FALSE(Data->IsFunc);
  EXPECT_EQ(Data->Addr, *ExpectedDataAddr);
}

TEST(FunctionDiscoveryThumb, NormalizesTLSCallbackCodePointers) {
  std::vector<uint8_t> Bytes = readFixture("test_patch_coff_arm.exe");
  if (Bytes.empty())
    GTEST_SKIP() << "ARM32 PE fixture not built";
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);

  const llvm::object::data_directory *TLSDir =
      Obj->getDataDirectory(llvm::COFF::TLS_TABLE);
  ASSERT_NE(TLSDir, nullptr);
  const char *FileBegin = Obj->getData().data();
  const char *TLSDirPtr = reinterpret_cast<const char *>(TLSDir);
  ASSERT_GE(TLSDirPtr, FileBegin);
  size_t TLSDirOff = static_cast<size_t>(TLSDirPtr - FileBegin);
  ASSERT_TRUE(rangeInBounds(TLSDirOff, sizeof(*TLSDir), Bytes.size()));

  std::optional<size_t> RawTLSOff;
  uint32_t TLSRVA = 0;
  for (const llvm::object::SectionRef &SecRef : Obj->sections()) {
    auto NameOrErr = SecRef.getName();
    ASSERT_TRUE(static_cast<bool>(NameOrErr))
        << llvm::toString(NameOrErr.takeError());
    if (*NameOrErr != section_names::coff::Rdata)
      continue;
    const llvm::object::coff_section *Sec = Obj->getCOFFSection(SecRef);
    ASSERT_NE(Sec, nullptr);
    ASSERT_GE(uint32_t(Sec->VirtualSize),
              sizeof(llvm::object::coff_tls_directory32));
    uint32_t Offset =
        uint32_t(Sec->VirtualSize) - sizeof(llvm::object::coff_tls_directory32);
    ASSERT_LE(uint64_t(Offset) + sizeof(llvm::object::coff_tls_directory32),
              uint64_t(Sec->SizeOfRawData));
    TLSRVA = uint32_t(Sec->VirtualAddress) + Offset;
    RawTLSOff = uint64_t(Sec->PointerToRawData) + Offset;
    break;
  }
  ASSERT_TRUE(RawTLSOff.has_value());
  ASSERT_TRUE(rangeInBounds(
      *RawTLSOff, sizeof(llvm::object::coff_tls_directory32), Bytes.size()));

  constexpr va_t CallbackTableVA = 0x2800;
  std::fill_n(Bytes.data() + *RawTLSOff,
              sizeof(llvm::object::coff_tls_directory32), uint8_t(0));
  writeLE<uint32_t>(
      Bytes.data() + *RawTLSOff +
          offsetof(llvm::object::coff_tls_directory32, AddressOfCallBacks),
      uint32_t(CallbackTableVA));
  writeLE<uint32_t>(
      Bytes.data() + TLSDirOff +
          offsetof(llvm::object::data_directory, RelativeVirtualAddress),
      TLSRVA);
  writeLE<uint32_t>(Bytes.data() + TLSDirOff +
                        offsetof(llvm::object::data_directory, Size),
                    sizeof(llvm::object::coff_tls_directory32));

  Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  BinaryImage Img = makeThumbImage();
  const std::array<uint8_t, 4> CallbackCode = {0x10, 0xb5, 0x70, 0x47};
  Img.Segments.push_back(makeExecutableSegment(0x1000, CallbackCode));
  Segment CallbackTable;
  CallbackTable.VA = CallbackTableVA;
  CallbackTable.Size = 2 * sizeof(uint32_t);
  CallbackTable.Flags = SegmentFlags::Readable;
  CallbackTable.Data.resize(2 * sizeof(uint32_t));
  writeLE<uint32_t>(CallbackTable.Data.data(), 0x1001u);
  writeLE<uint32_t>(CallbackTable.Data.data() + sizeof(uint32_t), 0u);
  Img.Segments.push_back(std::move(CallbackTable));

  coff_loader::parseTLSDirectory(*Obj, Img, 0);

  ASSERT_EQ(Img.Symbols.size(), 1u);
  EXPECT_TRUE(Img.Symbols.front().IsFunc);
  EXPECT_EQ(Img.Symbols.front().Addr, 0x1000u);
  EXPECT_TRUE(Img.isRuntimeFunctionAt(0x1000));
}

TEST(FunctionDiscoveryThumb, RecognizesLldImportThunk) {
  BinaryImage Img = makeThumbImage();
  constexpr va_t ThunkVA = 0x1800;
  constexpr va_t IATAddr = 0x12345678;
  const auto Bytes = makeImportThunk(uint32_t(IATAddr));
  Img.Segments.push_back(makeExecutableSegment(ThunkVA, Bytes));

  Import Imp;
  Imp.Name = "imported";
  Imp.IATAddr = IATAddr;
  Img.Imports.push_back(std::move(Imp));

  scanImportThunks(Img);
  scanImportThunks(Img);

  ASSERT_EQ(Img.Symbols.size(), 1u);
  EXPECT_TRUE(Img.Symbols.front().IsFunc);
  EXPECT_EQ(Img.Symbols.front().Addr, ThunkVA);
  EXPECT_EQ(Img.Symbols.front().Addr & 1u, 0u);
  EXPECT_EQ(Img.Symbols.front().Size, arm::kThumbImportThunkLen);
  ASSERT_EQ(Img.ImportStubIndices.size(), 1u);
  EXPECT_EQ(Img.ImportStubIndices.at(ThunkVA), 0u);
  EXPECT_EQ(Img.findImportAt(ThunkVA), &Img.Imports[0]);
  EXPECT_EQ(Img.Imports[0].IATAddr, IATAddr);
}

TEST(FunctionDiscoveryThumb, RejectsThunkWithDifferentTerminalOpcode) {
  BinaryImage Img = makeThumbImage();
  constexpr va_t IATAddr = 0x12345678;
  auto Bytes = makeImportThunk(uint32_t(IATAddr));
  Bytes[8] ^= 1;
  Img.Segments.push_back(makeExecutableSegment(0x1800, Bytes));

  Import Imp;
  Imp.Name = "imported";
  Imp.IATAddr = IATAddr;
  Img.Imports.push_back(std::move(Imp));

  scanImportThunks(Img);

  EXPECT_TRUE(Img.Symbols.empty());
}

} // namespace
