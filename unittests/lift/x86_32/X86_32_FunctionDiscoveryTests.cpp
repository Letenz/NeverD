//===- X86_32_FunctionDiscoveryTests.cpp - PE gap function recovery ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/FuncDetector.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFLoaderUtils.h"
#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/loader/PointerRelocation.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>

using namespace neverd;

namespace {

BinaryImage makePeText(std::vector<uint8_t> Bytes, va_t Base = 0x401000) {
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::COFF;
  Img.IsRelocatable = false;
  Img.Base = 0x400000;
  Img.Entry = Base;

  Segment Text;
  Text.Name = ".text";
  Text.VA = Base;
  Text.Size = Bytes.size();
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = std::move(Bytes);
  Img.Segments.push_back(Text);

  Section Sec;
  Sec.Name = ".text";
  Sec.VA = Base;
  Sec.Size = Img.Segments[0].Size;
  Sec.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Sec.Type = 0x60000020;
  Img.Sections.push_back(Sec);
  Img.KnownCodeRanges.emplace_back(Base, Base + Img.Segments[0].Size);
  return Img;
}

} // namespace

TEST(X86_32_FunctionDiscovery, RecoversAlignedUnsymbolizedStackPrologue) {
  // Entry at 0x401000 is a tiny EBP frame.  An unsymbolized callee sits at
  // the next 16-byte boundary with `sub esp, imm32` and is not called.
  std::vector<uint8_t> Bytes(0x30, 0xCC);
  const uint8_t Entry[] = {0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  std::copy(Entry, Entry + sizeof(Entry), Bytes.begin());
  const uint8_t Hidden[] = {0x81, 0xEC, 0x20, 0x00, 0x00,
                            0x00, 0x83, 0xC4, 0x20, 0xC3};
  std::copy(Hidden, Hidden + sizeof(Hidden), Bytes.begin() + 0x20);

  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);

  const bool FoundEntry =
      std::any_of(Functions.begin(), Functions.end(),
                  [](const auto &F) { return F.first == 0x401000; });
  const bool FoundHidden =
      std::any_of(Functions.begin(), Functions.end(),
                  [](const auto &F) { return F.first == 0x401020; });
  EXPECT_TRUE(FoundEntry);
  EXPECT_TRUE(FoundHidden) << "expected 16-byte-aligned sub-esp prologue";
}

TEST(X86_32_FunctionDiscovery, DoesNotPromoteInt3PaddingAsAFunction) {
  std::vector<uint8_t> Bytes(0x30, 0xCC);
  const uint8_t Entry[] = {0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  std::copy(Entry, Entry + sizeof(Entry), Bytes.begin());

  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_EQ(std::count_if(Functions.begin(), Functions.end(),
                          [](const auto &F) { return F.first == 0x401010; }),
            0);
}

static bool hasEntry(const std::vector<std::pair<va_t, std::string>> &Functions,
                     va_t Addr) {
  return std::any_of(Functions.begin(), Functions.end(),
                     [Addr](const auto &F) { return F.first == Addr; });
}

TEST(X86_32_FunctionDiscovery, RecoversUnalignedFramePrologue) {
  std::vector<uint8_t> Bytes(0x20, 0xCC);
  const uint8_t Entry[] = {0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  std::copy(Entry, Entry + sizeof(Entry), Bytes.begin());
  const uint8_t Hidden[] = {0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  std::copy(Hidden, Hidden + sizeof(Hidden), Bytes.begin() + 0x09);

  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401000));
  EXPECT_TRUE(hasEntry(Functions, 0x401009))
      << "expected unaligned push-ebp frame prologue";
}

TEST(X86_32_FunctionDiscovery, RecoversAlignedPushRegAndSehPrologues) {
  std::vector<uint8_t> Bytes(0x50, 0xCC);
  const uint8_t Entry[] = {0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  std::copy(Entry, Entry + sizeof(Entry), Bytes.begin());
  const uint8_t PushReg[] = {0x53, 0x56, 0x57, 0x8B, 0x7C, 0x24,
                             0x10, 0x5F, 0x5E, 0x5B, 0xC3};
  std::copy(PushReg, PushReg + sizeof(PushReg), Bytes.begin() + 0x20);
  const uint8_t Seh[] = {0x6A, 0xFF, 0x58, 0xC3};
  std::copy(Seh, Seh + sizeof(Seh), Bytes.begin() + 0x40);

  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401020))
      << "expected 16-byte-aligned push-ebx/esi/edi prologue";
  EXPECT_TRUE(hasEntry(Functions, 0x401040))
      << "expected 16-byte-aligned SEH push -1 prologue";
}

TEST(X86_32_FunctionDiscovery,
     RecoversLargeDirectCallWhenVerifyBudgetExhausts) {
  const size_t NopCount = static_cast<size_t>(limits::kMaxVerifyInsns) + 8;
  std::vector<uint8_t> Bytes(0x10 + NopCount + 1, 0xCC);
  Bytes[0] = 0xE8;
  writeLE<uint32_t>(Bytes.data() + 1, 3); // call 0x401008
  Bytes[5] = 0xC3;
  std::fill(Bytes.begin() + 8, Bytes.begin() + 8 + NopCount, 0x90);
  Bytes[8 + NopCount] = 0xC3;

  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401000));
  EXPECT_TRUE(hasEntry(Functions, 0x401008))
      << "direct CALL target must survive a verify walk that exhausts "
         "kMaxVerifyInsns";
}

TEST(X86_32_FunctionDiscovery,
     RecoversRelocBackedLeafWhenVerifyBudgetExhausts) {
  const size_t NopCount = static_cast<size_t>(limits::kMaxVerifyInsns) + 8;
  std::vector<uint8_t> Bytes(0x10 + NopCount + 1, 0xCC);
  const uint8_t Entry[] = {0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  std::copy(Entry, Entry + sizeof(Entry), Bytes.begin());
  std::fill(Bytes.begin() + 8, Bytes.begin() + 8 + NopCount, 0x90);
  Bytes[8 + NopCount] = 0xC3;

  BinaryImage Img = makePeText(std::move(Bytes));
  Segment Data;
  Data.Name = ".rdata";
  Data.VA = 0x402000;
  Data.Size = 4;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(4);
  writeLE<uint32_t>(Data.Data.data(), 0x401008);
  Img.Segments.push_back(std::move(Data));
  Img.CodePtrRelocSlots.insert(0x402000);

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401000));
  EXPECT_TRUE(hasEntry(Functions, 0x401008))
      << "PE HIGHLOW code-pointer target must survive verify budget exhaustion";
}

TEST(X86_32_FunctionDiscovery, RecoversUnalignedStackAdjustAfterStdcallRet) {
  std::vector<uint8_t> Bytes(0x20, 0xCC);
  const uint8_t Entry[] = {0x55, 0x8B, 0xEC, 0xC2, 0x08, 0x00};
  std::copy(Entry, Entry + sizeof(Entry), Bytes.begin());
  const uint8_t Hidden[] = {0x83, 0xEC, 0x08, 0x33, 0xC0, 0x83,
                            0xC4, 0x08, 0xC2, 0x08, 0x00};
  std::copy(Hidden, Hidden + sizeof(Hidden), Bytes.begin() + 0x06);

  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401000));
  EXPECT_TRUE(hasEntry(Functions, 0x401006))
      << "expected packed unaligned sub-esp prologue after ret imm16";
}

TEST(X86_32_FunctionDiscovery, StartsUnalignedHotpatchFrameAtItsNoOp) {
  // MSVC's hotpatchable entry, `mov edi, edi; push ebp; mov ebp, esp`, packed
  // after another function's ret.  The frame scan finds `push ebp` two bytes
  // in, but the function begins at the no-op.
  std::vector<uint8_t> Bytes(0x20, 0xCC);
  const uint8_t Entry[] = {0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  std::copy(Entry, Entry + sizeof(Entry), Bytes.begin());
  const uint8_t Hotpatch[] = {0x8B, 0xFF, 0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  std::copy(Hotpatch, Hotpatch + sizeof(Hotpatch), Bytes.begin() + 0x05);

  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401005));
  EXPECT_FALSE(hasEntry(Functions, 0x401007))
      << "the push after the hotpatch no-op is not an entry of its own";
}

TEST(X86_32_FunctionDiscovery, LoaderRegistersPackedHotpatchEntries) {
  std::vector<uint8_t> Bytes = {
      0x55, 0x8B, 0xEC, 0x5D, 0xC2, 0x08, 0x00, // 00: frame; ret 8
      0x8B, 0xFF, 0x56, 0x5E, 0xC3,             // 07: no-op; push esi
      0x8B, 0xFF, 0x55, 0x8B, 0xEC, 0x5D, 0xC3, // 0C: no-op; EBP frame
      0xB8, 0x8B, 0xFF, 0x56, 0x00, 0xC3,       // 13: mov eax, 0056FF8Bh
      0x8B, 0xFF, 0x55, 0x29, 0x40, 0x00,       // 19: no-op, jump table
  };
  BinaryImage Img = makePeText(std::move(Bytes));
  // x86 PE has no function table to claim ranges from.
  Img.KnownCodeRanges.clear();

  scanX86HotpatchEntries(Img);

  std::vector<va_t> Found;
  for (const Symbol &Sym : Img.Symbols) {
    EXPECT_TRUE(Sym.IsFunc);
    EXPECT_FALSE(Sym.IsBoundaryGuess);
    Found.push_back(Sym.Addr);
  }
  std::sort(Found.begin(), Found.end());
  // A push after the no-op begins a function only after another one ends:
  // inside an immediate, or where MSVC pads a jump table, it does not.
  EXPECT_EQ(Found, (std::vector<va_t>{0x401007, 0x40100C}));
}

TEST(X86_32_FunctionDiscovery, BoundaryGuessesAreThoseNothingElseConfirms) {
  BinaryImage Img;
  const auto Add = [&](va_t Addr, bool Guess, const char *Name = nullptr) {
    Symbol Sym = Symbol::makeFunc(Addr, 4);
    if (Name)
      Sym.Name = Name;
    Sym.IsBoundaryGuess = Guess;
    Img.Symbols.push_back(Sym);
  };
  Add(0x401100, false);
  EXPECT_TRUE(Img.boundaryGuessFunctionStarts().empty());
  // A guess alone, one another symbol confirms, one debug information
  // named, one a known code range starts, and one whose range continues
  // another function's, which confirms nothing.
  Add(0x401000, true);
  Add(0x401010, true);
  Add(0x401010, false);
  Add(0x401020, true, "named_by_debug_info");
  Add(0x401030, true);
  Add(0x401040, true);
  Img.KnownCodeRanges = {{0x401030, 0x401038}, {0x401040, 0x401048}};
  Img.ContinuationCodeStarts = {0x401040};
  EXPECT_EQ(Img.boundaryGuessFunctionStarts(),
            (std::set<va_t>{0x401000, 0x401040}));
}

TEST(X86_32_FunctionDiscovery, HotpatchEntryAfterPaddingIsNotAGuess) {
  std::vector<uint8_t> Bytes = {0x55, 0x8B, 0xEC, 0x5D, 0xC3, 0xCC, 0xCC,
                                0x8B, 0xFF, 0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  BinaryImage Img = makePeText(std::move(Bytes));
  Img.KnownCodeRanges.clear();

  runPostLoadDiscovery(Img, "test");

  const auto It =
      std::find_if(Img.Symbols.begin(), Img.Symbols.end(),
                   [](const Symbol &Sym) { return Sym.Addr == 0x401007; });
  ASSERT_NE(It, Img.Symbols.end());
  EXPECT_FALSE(It->IsBoundaryGuess)
      << "a direct jump to a hotpatch entry is a tail call";
  EXPECT_TRUE(Img.boundaryGuessFunctionStarts().empty());
}

TEST(X86_32_FunctionDiscovery,
     SeedsPrimaryExceptionFunctionInsideCoarseKnownRange) {
  std::vector<uint8_t> Bytes(0x20, 0xCC);
  Bytes[0] = 0xC3;
  Bytes[0x10] = 0xC3;
  BinaryImage Img = makePeText(std::move(Bytes));
  Img.KnownCodeRanges = {{0x401000, 0x401020}};
  ExceptionFunction EH;
  EH.CodeRange = {0x401010, 0x401012};
  EH.Kind = RuntimeFunctionKind::Primary;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  Img.ExceptionMetadata.Functions.push_back(std::move(EH));
  Img.ExceptionMetadata.rebuildIndex();

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401010))
      << "pdata/registration primary starts are functions even inside a coarse "
         "KnownCodeRange";
}

class SizedFuncDebugContext : public DebugContext {
public:
  FunctionSym Function;
  std::optional<FunctionSym> resolveFunction(va_t Addr) const override {
    if (Addr == Function.Addr)
      return Function;
    return std::nullopt;
  }
  std::optional<VariableSym> resolveVariable(va_t, int64_t) const override {
    return std::nullopt;
  }
  std::optional<TypeSym> resolveType(uint64_t) const override {
    return std::nullopt;
  }
  std::optional<SourceLoc> sourceLocation(va_t) const override {
    return std::nullopt;
  }
  std::vector<FunctionSym> allFunctions() const override { return {Function}; }
  bool hasInfo() const override { return true; }
};

TEST(X86_32_FunctionDiscovery, PipelineDropsInteriorsInsideDebugExtents) {
  std::vector<uint8_t> Bytes(0x40, 0xCC);
  const uint8_t Leaf[] = {0x68, 0x00, 0x00, 0x00, 0x00, 0x55,
                          0x8B, 0xEC, 0x5D, 0xC3, 0x90};
  std::copy(Leaf, Leaf + sizeof(Leaf), Bytes.begin());
  const uint8_t Caller[] = {0x55, 0x8B, 0xEC, 0xE8, 0xDD,
                            0xFF, 0xFF, 0xFF, 0x5D, 0xC3};
  std::copy(Caller, Caller + sizeof(Caller), Bytes.begin() + 0x20);

  BinaryImage Img = makePeText(std::move(Bytes));
  SizedFuncDebugContext Dbg;
  Dbg.Function.Name = "covering";
  Dbg.Function.Addr = 0x401000;
  Dbg.Function.Size = sizeof(Leaf);

  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  PipelineResult Result = Pipeline().run(Img, Ctx, Opts, &Dbg);
  EXPECT_TRUE(
      std::any_of(Result.HighFuncs.begin(), Result.HighFuncs.end(),
                  [](const HighFunc &F) { return F.Entry == 0x401000; }));
  EXPECT_FALSE(
      std::any_of(Result.HighFuncs.begin(), Result.HighFuncs.end(),
                  [](const HighFunc &F) { return F.Entry == 0x401005; }))
      << "debug-sized function must keep interior CALL targets as blocks";
  const auto Covering =
      std::find_if(Result.HighFuncs.begin(), Result.HighFuncs.end(),
                   [](const HighFunc &F) { return F.Entry == 0x401000; });
  ASSERT_NE(Covering, Result.HighFuncs.end());
  EXPECT_FALSE(Covering->Body.empty());
}

TEST(X86_32_FunctionDiscovery, DropsCallTargetsInsideSizedFunctionSymbol) {
  std::vector<uint8_t> Bytes(0x40, 0xCC);
  // 11-byte leaf: push imm32 then a nested-looking EBP frame.  A later caller
  // targets the interior frame; the sized symbol still owns the bytes.
  const uint8_t Leaf[] = {0x68, 0x00, 0x00, 0x00, 0x00, 0x55,
                          0x8B, 0xEC, 0x5D, 0xC3, 0x90};
  std::copy(Leaf, Leaf + sizeof(Leaf), Bytes.begin());
  // push ebp; mov ebp, esp; call 0x401005; pop ebp; ret
  const uint8_t Caller[] = {0x55, 0x8B, 0xEC, 0xE8, 0xDD,
                            0xFF, 0xFF, 0xFF, 0x5D, 0xC3};
  std::copy(Caller, Caller + sizeof(Caller), Bytes.begin() + 0x20);

  BinaryImage Img = makePeText(std::move(Bytes));
  Symbol Covering = Symbol::makeFunc(0x401000, sizeof(Leaf));
  Covering.Name = "covering";
  Img.Symbols.push_back(std::move(Covering));

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401000));
  EXPECT_TRUE(hasEntry(Functions, 0x401020));
  EXPECT_FALSE(hasEntry(Functions, 0x401005))
      << "direct-call target inside a sized function symbol is an interior "
         "block";

  std::set<va_t> Entries;
  for (const auto &F : Functions)
    Entries.insert(F.first);
  CFGBuilder CFG;
  CFG.setKnownFuncEntries(&Entries);
  auto LeafIR = CFG.build(Img, Dec, 0x401000, "covering");
  EXPECT_GE(LeafIR.DecodedInstructionCount, 2u)
      << "CFG must not stop at a dropped interior start 5 bytes in";
}

TEST(X86_32_FunctionDiscovery, RecoversAlignedThiscallVtableStoreThunk) {
  std::vector<uint8_t> Bytes(0x30, 0x90);
  const uint8_t Entry[] = {0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  std::copy(Entry, Entry + sizeof(Entry), Bytes.begin());
  const uint8_t Thunk[] = {0xC7, 0x01, 0x00, 0x20, 0x40, 0x00, 0xC3};
  std::copy(Thunk, Thunk + sizeof(Thunk), Bytes.begin() + 0x20);

  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401020))
      << "expected 16-byte-aligned mov [ecx], imm32 destructor thunk";
}

TEST(X86_32_FunctionDiscovery, KeepsAnExportThatEndsAtANoReturnImport) {
  // The entry returns.  Finish, which only its export names, ends at the
  // last bytes of .text calling ExitProcess through its import slot:
  //   mov eax, [esp+4]; cmp eax, 4; jge exit; inc eax; ret
  //   exit: push eax; call dword ptr [0x402000]
  std::vector<uint8_t> Bytes(0x10, 0xCC);
  Bytes[0] = 0xC3;
  const uint8_t Finish[] = {0x8B, 0x44, 0x24, 0x04, 0x83, 0xF8,
                            0x04, 0x7D, 0x02, 0x40, 0xC3, 0x50,
                            0xFF, 0x15, 0x00, 0x20, 0x40, 0x00};
  Bytes.insert(Bytes.end(), std::begin(Finish), std::end(Finish));
  BinaryImage Img = makePeText(std::move(Bytes));
  Import Exit;
  Exit.Name = "ExitProcess";
  Exit.IATAddr = 0x402000;
  Img.Imports.push_back(Exit);
  Export Exp;
  Exp.Name = "Finish";
  Exp.Addr = 0x401010;
  Img.Exports.push_back(Exp);

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  EXPECT_TRUE(hasEntry(Detector.detect(Img, Dec), 0x401010))
      << "the call through ExitProcess's slot does not return to run past "
         ".text";
}

TEST(X86_32_FunctionDiscovery, DecompileIncludesImportStubs) {
  std::vector<uint8_t> Bytes(0x20, 0xCC);
  // jmp dword ptr [0x402000]
  const uint8_t Thunk[] = {0xFF, 0x25, 0x00, 0x20, 0x40, 0x00};
  std::copy(Thunk, Thunk + sizeof(Thunk), Bytes.begin());
  BinaryImage Img = makePeText(std::move(Bytes));
  Import Imp;
  Imp.Name = "imported";
  Imp.IATAddr = 0x402000;
  Img.Imports.push_back(Imp);
  ASSERT_TRUE(Img.recordImportStub(0x401000, 0));
  ASSERT_TRUE(Img.isImportStubAt(0x401000));

  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Pipeline Pipe;
  PipelineResult Result = Pipe.run(Img, Ctx, Opts);
  const bool Found =
      std::any_of(Result.HighFuncs.begin(), Result.HighFuncs.end(),
                  [](const HighFunc &F) { return F.Entry == 0x401000; });
  EXPECT_TRUE(Found) << "decompile must emit import thunks, not skip them";
}

TEST(X86_32_FunctionDiscovery, SkipsSsaWhenInstructionCountExceedsCap) {
  LowFunc Low;
  Low.Name = "wide_ssa";
  Low.Entry = 0x401000;
  Low.DecodedInstructionCount =
      static_cast<uint64_t>(limits::kMaxSSAFunctionOps) + 1;
  LowBlock B0;
  B0.Id = 0;
  B0.StartAddr = 0x401000;
  B0.Succs = {1};
  LowOp Nop;
  Nop.Opcode = NdOp::NOP;
  Nop.Addr = 0x401000;
  B0.Ops.push_back(Nop);
  LowBlock B1;
  B1.Id = 1;
  B1.StartAddr = 0x401010;
  B1.Preds = {0};
  LowOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Ret.Addr = 0x401010;
  B1.Ops.push_back(Ret);
  Low.Blocks = {B0, B1};

  MedFunc Med = LowToMedConverter().convert(Low, Arch::X86, BinaryFormat::COFF);
  EXPECT_TRUE(Med.SkippedSSA);
  ASSERT_EQ(Med.Blocks.size(), 2u);
  EXPECT_TRUE(Med.Blocks[0].Phis.empty());
  EXPECT_TRUE(Med.Blocks[1].Phis.empty());
  EXPECT_EQ(Med.Entry, 0x401000u);
}

static MedFunc makeWideChainMed() {
  MedFunc Med;
  Med.Name = "wide_cfg";
  Med.Entry = 0x401000;
  for (int I = 0; I < 1025; ++I) {
    MedBlock Block;
    Block.Id = I;
    Block.StartAddr = 0x401000 + static_cast<va_t>(I);
    if (I + 1 < 1025)
      Block.Succs = {I + 1};
    Med.Blocks.push_back(Block);
  }
  return Med;
}

TEST(X86_32_FunctionDiscovery, LargeSkippedSSAMedIREmitsGotoSkeleton) {
  MedFunc Med = makeWideChainMed();
  Med.SkippedSSA = true;
  MedToHighConverter Conv;
  HighFunc HF = Conv.convert(Med, Arch::X86);
  ASSERT_FALSE(HF.Body.empty());
  const bool HasGoto =
      std::any_of(HF.Body.begin(), HF.Body.end(),
                  [](const HighStmt &S) { return S.Kind == StmtKind::Goto; });
  EXPECT_TRUE(HasGoto);
}

TEST(X86_32_FunctionDiscovery, LargeSSAMedIRIsLoweredBlockByBlock) {
  // Past the structuring limits an SSA function still gets its statements;
  // a straight chain needs no goto at all.
  MedFunc Med = makeWideChainMed();
  MedToHighConverter Conv;
  HighFunc HF = Conv.convert(Med, Arch::X86);
  ASSERT_FALSE(HF.Body.empty());
  const bool HasGoto =
      std::any_of(HF.Body.begin(), HF.Body.end(),
                  [](const HighStmt &S) { return S.Kind == StmtKind::Goto; });
  EXPECT_FALSE(HasGoto);
  EXPECT_EQ(HF.Body.back().Kind, StmtKind::Return);
}

TEST(X86_32_FunctionDiscovery, FillUnstructuredSkeletonFromTwoBlocks) {
  MedFunc Med;
  Med.Name = "two_block";
  Med.Entry = 0x401000;
  MedBlock First;
  First.Id = 0;
  First.StartAddr = 0x401000;
  First.Succs = {1};
  MedBlock Second;
  Second.Id = 1;
  Second.StartAddr = 0x401010;
  Med.Blocks = {First, Second};

  HighFunc HF;
  HF.Name = Med.Name;
  HF.Entry = Med.Entry;
  fillUnstructuredGotoSkeleton(HF, Med);
  ASSERT_EQ(HF.Body.size(), 4u);
  EXPECT_EQ(HF.Body[0].Kind, StmtKind::Nop);
  EXPECT_EQ(HF.Body[1].Kind, StmtKind::Goto);
  EXPECT_EQ(HF.Body[1].GotoTarget, 0x401010u);
  EXPECT_EQ(HF.Body[3].Kind, StmtKind::Return);
}

TEST(X86_32_FunctionDiscovery, CfgDoesNotFollowPastNextFunctionStart) {
  std::vector<uint8_t> Bytes(0x20, 0x90);
  Bytes[0x1F] = 0xC3;
  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  std::set<va_t> Entries{0x401000, 0x401010};
  CFGBuilder CFG;
  CFG.setKnownFuncEntries(&Entries);
  auto First = CFG.build(Img, Dec, 0x401000, "first");
  EXPECT_LE(First.DecodedInstructionCount, 16u)
      << "jump/fallthrough must not decode past the next function start";
}

TEST(X86_32_FunctionDiscovery, CfgStopsAtNextFunctionEntry) {
  std::vector<uint8_t> Bytes(0x20, 0xCC);
  // Two packed functions: NOPs then a RET that belongs to the second start.
  Bytes[0] = 0x90;
  Bytes[1] = 0x90;
  Bytes[2] = 0xC3;
  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  std::set<va_t> Entries{0x401000, 0x401002};
  CFGBuilder CFG;
  CFG.setKnownFuncEntries(&Entries);
  auto First = CFG.build(Img, Dec, 0x401000, "first");
  EXPECT_LT(First.DecodedInstructionCount, 3u)
      << "must not decode the next function's RET as part of the first body";
}

TEST(X86_32_FunctionDiscovery, LiftsMmxPunpckldq) {
  std::vector<uint8_t> Bytes(0x20, 0xCC);
  const uint8_t Body[] = {0x0F, 0x62, 0xC1, 0x0F, 0x77, 0xC3};
  std::copy(Body, Body + sizeof(Body), Bytes.begin());
  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  CFGBuilder CFG;
  auto LF = CFG.build(Img, Dec, 0x401000, "mmx_punpck");
  EXPECT_TRUE(LF.hasCompleteInstructionLift())
      << "unsupported=" << LF.UnsupportedInstructionAddresses.size()
      << " decode_fail=" << LF.DecodeFailureAddresses.size();
}

TEST(X86_32_FunctionDiscovery, LiftsMmxPunpckldqFromMemory) {
  std::vector<uint8_t> Bytes(0x30, 0xCC);
  // punpckldq mm0, qword ptr [0x401020]; emms; ret
  const uint8_t Body[] = {0x0F, 0x62, 0x05, 0x20, 0x10,
                          0x40, 0x00, 0x0F, 0x77, 0xC3};
  std::copy(Body, Body + sizeof(Body), Bytes.begin());
  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  CFGBuilder CFG;
  auto LF = CFG.build(Img, Dec, 0x401000, "mmx_punpck_mem");
  EXPECT_TRUE(LF.hasCompleteInstructionLift())
      << "unsupported=" << LF.UnsupportedInstructionAddresses.size()
      << (LF.UnsupportedInstructionAddresses.empty()
              ? ""
              : " at " + std::to_string(LF.UnsupportedInstructionAddresses[0]));
}

TEST(X86_32_FunctionDiscovery,
     DecompileKeepsFirstPassWhenJumpTableBudgetExhausts) {
  std::vector<uint8_t> Bytes(0x20, 0xCC);
  const uint8_t Entry[] = {0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  std::copy(Entry, Entry + sizeof(Entry), Bytes.begin());
  BinaryImage Img = makePeText(std::move(Bytes));

  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.JumpTableEvidenceBudgetForTesting = 0;
  Opts.EmitDumpOutput = false;
  Pipeline Pipe;
  PipelineResult Result = Pipe.run(Img, Ctx, Opts);
  ASSERT_FALSE(Result.HighFuncs.empty())
      << "decompile must keep first-pass CFGs when table evidence is exhausted";
  EXPECT_FALSE(Result.HighFuncs.front().Name.empty());
}

namespace {

void addPeSection(BinaryImage &Img, const char *Name, va_t VA,
                  std::vector<uint8_t> Bytes, SegmentFlags Flags) {
  Segment Seg;
  Seg.Name = Name;
  Seg.VA = VA;
  Seg.Size = Bytes.size();
  Seg.FileSz = Bytes.size();
  Seg.Flags = Flags;
  Seg.Data = std::move(Bytes);
  Section Sec;
  Sec.Name = Name;
  Sec.VA = VA;
  Sec.Size = Seg.Size;
  Sec.FileSz = Seg.Size;
  Sec.Flags = Flags;
  Img.Segments.push_back(std::move(Seg));
  Img.Sections.push_back(std::move(Sec));
}

/// A linked i386 PE whose switch dispatches through the table MSVC places in
/// .text after the function, two bytes past a four-byte boundary.  The next
/// function stores a pointer with `mov dword ptr [disp32], imm32`, whose two
/// relocated fields sit back to back, and reads a relocated .rdata pointer
/// an instruction names: an anchor past the table, four-byte aligned.  The
/// base relocations are recorded as the COFF loader records them.
BinaryImage makeSwitchTableAmongCode() {
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::COFF;
  Img.IsRelocatable = false;
  Img.Base = 0x400000;
  Img.Entry = 0x401000;
  std::vector<uint8_t> Text;
  auto Emit = [&](std::initializer_list<uint8_t> Bytes) {
    Text.insert(Text.end(), Bytes);
  };
  // 0x401000: switch_on_arg.
  Emit({0x55, 0x8B, 0xEC});                         // push ebp; mov ebp, esp
  Emit({0x8B, 0x45, 0x08});                         // mov eax, [ebp+8]
  Emit({0x83, 0xF8, 0x02, 0x77, 0x1D});             // cmp eax, 2; ja 0x401028
  Emit({0xFF, 0x24, 0x85, 0x2E, 0x10, 0x40, 0x00}); // jmp [eax*4+0x40102E]
  Emit({0xB8, 0x0A, 0x00, 0x00, 0x00, 0x5D, 0xC3}); // 0x401012: case 0
  Emit({0xB8, 0x14, 0x00, 0x00, 0x00, 0x5D, 0xC3}); // 0x401019: case 1
  Emit({0xB8, 0x1E, 0x00, 0x00, 0x00, 0x5D, 0xC3}); // 0x401020: case 2
  Emit({0x90, 0x33, 0xC0, 0x5D, 0xC3});             // nop; 0x401028: default
  Emit({0x8B, 0xC0});                               // mov eax, eax
  Emit({0x12, 0x10, 0x40, 0x00, 0x19, 0x10});       // 0x40102E: the table
  Emit({0x40, 0x00, 0x20, 0x10, 0x40, 0x00});       //
  Emit({0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC});       //
  // 0x401040: mov dword ptr [0x403000], 0x401040.
  Emit({0xC7, 0x05, 0x00, 0x30, 0x40, 0x00, 0x40, 0x10, 0x40, 0x00});
  Emit({0xB9, 0x00, 0x20, 0x40, 0x00}); // mov ecx, 0x402000
  Emit({0x8B, 0x01, 0xC3});             // mov eax, [ecx]; ret
  addPeSection(Img, ".text", 0x401000, std::move(Text),
               SegmentFlags::Readable | SegmentFlags::Executable);
  addPeSection(Img, ".rdata", 0x402000, {0x40, 0x10, 0x40, 0x00},
               SegmentFlags::Readable);
  addPeSection(Img, ".data", 0x403000, std::vector<uint8_t>(4, 0),
               SegmentFlags::Readable | SegmentFlags::Writable);
  recordAbsolutePointerRelocations(Img, {{0x40100E, 0x40102E, InvalidVA},
                                         {0x40102E, 0x401012, InvalidVA},
                                         {0x401032, 0x401019, InvalidVA},
                                         {0x401036, 0x401020, InvalidVA},
                                         {0x401042, 0x403000, InvalidVA},
                                         {0x401046, 0x401040, InvalidVA},
                                         {0x40104B, 0x402000, InvalidVA},
                                         {0x402000, 0x401040, InvalidVA}});
  coff_loader::recordRelocatedPointerRunsInCode(Img);
  return Img;
}

} // namespace

TEST(X86_32_FunctionDiscovery, CallbackStoredByARelocatedImmediateIsAFunction) {
  // A linked i386 PE stores a callback into an object with
  // `mov dword ptr [ecx+0x10], offset cb`, whose imm32 is a base relocation.
  // The callback is a function the code only takes the address of; a target
  // the storing function names inside itself stays its own label.
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::COFF;
  Img.IsRelocatable = false;
  Img.Base = 0x400000;
  Img.Entry = 0x401000;
  std::vector<uint8_t> Text;
  auto Emit = [&](std::initializer_list<uint8_t> Bytes) {
    Text.insert(Text.end(), Bytes);
  };
  // 0x401000: register_callback.
  Emit({0x55, 0x8B, 0xEC});                         // push ebp; mov ebp, esp
  Emit({0x8B, 0x4D, 0x08});                         // mov ecx, [ebp+8]
  Emit({0xC7, 0x41, 0x10, 0x40, 0x10, 0x40, 0x00}); // mov [ecx+0x10], 0x401040
  Emit({0x5D, 0xC3});                               // pop ebp; ret
  Text.resize(0x40, 0xCC);
  // 0x401040: the callback.
  Emit({0x55, 0x8B, 0xEC, 0x33, 0xC0, 0x5D, 0xC3}); // frame; xor eax,eax; ret
  addPeSection(Img, ".text", 0x401000, std::move(Text),
               SegmentFlags::Readable | SegmentFlags::Executable);
  // The imm32 of the mov at 0x401009 is at 0x40100C.
  Img.CodeAddressRelocOperands.emplace(
      0x40100C, RelocatedAddressField{.EncodedValue = 0x401040,
                                      .TargetVA = 0x401040,
                                      .Width = 4,
                                      .TargetOwnerVA = 0x401040});

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401000));
  EXPECT_TRUE(hasEntry(Functions, 0x401040))
      << "a callback a relocated immediate names is a function of its own";
}

TEST(X86_32_FunctionDiscovery, SwitchTableAmongCodeBelongsToItsFunction) {
  BinaryImage Img = makeSwitchTableAmongCode();
  // The table is data the code reads, though .text holds it; the two fields
  // of one instruction stay that instruction's operands.
  EXPECT_TRUE(Img.DataInCode.count(0x40102E));
  for (va_t Slot : {0x40102Eu, 0x401032u, 0x401036u}) {
    EXPECT_TRUE(Img.CodePtrRelocSlots.count(Slot)) << std::hex << Slot;
    EXPECT_FALSE(Img.CodeAddressRelocOperands.count(Slot)) << std::hex << Slot;
  }
  EXPECT_TRUE(Img.DataAddressRelocOperands.count(0x40100E));
  EXPECT_TRUE(Img.DataAddressRelocOperands.count(0x401042));
  EXPECT_TRUE(Img.CodeAddressRelocOperands.count(0x401046));

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401000));
  EXPECT_TRUE(hasEntry(Functions, 0x401040));
  for (va_t Case : {0x401012u, 0x401019u, 0x401020u})
    EXPECT_FALSE(hasEntry(Functions, Case))
        << "0x" << std::hex << Case << " is a case of the switch whose table "
        << "follows its code";

  std::set<va_t> Entries;
  for (const auto &F : Functions)
    Entries.insert(F.first);
  CFGBuilder CFG;
  CFG.setKnownFuncEntries(&Entries);
  const LowFunc Switch = CFG.build(Img, Dec, 0x401000, "switch_on_arg");
  ASSERT_EQ(Switch.JumpTables.size(), 1u)
      << "an aligned anchor past the misaligned table does not end it";
  EXPECT_EQ(Switch.JumpTables.front().BaseAddr, 0x40102Eu);
  EXPECT_EQ(std::set<va_t>(Switch.JumpTables.front().Targets.begin(),
                           Switch.JumpTables.front().Targets.end()),
            (std::set<va_t>{0x401012, 0x401019, 0x401020}));
}

TEST(X86_32_FunctionDiscovery, SwitchTableAmongCodeLiftsPastTheMirrorLimit) {
  // The table follows its dispatcher in a .text larger than the LLVM emitter
  // mirrors as one global.  The recovered switch dispatches on its selector
  // and omits the table LOAD, so the address that LOAD would read, and the
  // table's base in it, are never formed and need no global.
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::COFF;
  Img.IsRelocatable = false;
  Img.Base = 0x400000;
  Img.Entry = 0x401000;
  std::vector<uint8_t> Text;
  auto Emit = [&](std::initializer_list<uint8_t> Bytes) {
    Text.insert(Text.end(), Bytes);
  };
  Emit({0x55, 0x8B, 0xEC});                         // push ebp; mov ebp, esp
  Emit({0x8B, 0x45, 0x08});                         // mov eax, [ebp+8]
  Emit({0x83, 0xF8, 0x02, 0x77, 0x1D});             // cmp eax, 2; ja 0x401028
  Emit({0xFF, 0x24, 0x85, 0x2E, 0x10, 0x40, 0x00}); // jmp [eax*4+0x40102E]
  Emit({0xB8, 0x0A, 0x00, 0x00, 0x00, 0x5D, 0xC3}); // 0x401012: case 0
  Emit({0xB8, 0x14, 0x00, 0x00, 0x00, 0x5D, 0xC3}); // 0x401019: case 1
  Emit({0xB8, 0x1E, 0x00, 0x00, 0x00, 0x5D, 0xC3}); // 0x401020: case 2
  Emit({0x90, 0x33, 0xC0, 0x5D, 0xC3});             // nop; 0x401028: default
  Emit({0x8B, 0xC0});                               // mov eax, eax
  Emit({0x12, 0x10, 0x40, 0x00, 0x19, 0x10});       // 0x40102E: the table
  Emit({0x40, 0x00, 0x20, 0x10, 0x40, 0x00});       //
  Text.resize(limits::kMaxSingleGlobalEmbedLen + 0x1000, 0xCC);
  addPeSection(Img, ".text", 0x401000, std::move(Text),
               SegmentFlags::Readable | SegmentFlags::Executable);
  recordAbsolutePointerRelocations(Img, {{0x40100E, 0x40102E, InvalidVA},
                                         {0x40102E, 0x401012, InvalidVA},
                                         {0x401032, 0x401019, InvalidVA},
                                         {0x401036, 0x401020, InvalidVA}});
  coff_loader::recordRelocatedPointerRunsInCode(Img);
  ASSERT_TRUE(Img.DataInCode.count(0x40102E));

  // A large module's arbitration runs out of budget and leaves no slot
  // suppressible; a budget of zero does the same here.
  for (std::optional<size_t> Budget :
       {std::optional<size_t>(), std::optional<size_t>(0)})
    for (bool NoOpt : {true, false}) {
      SCOPED_TRACE(NoOpt);
      SCOPED_TRACE(Budget.has_value());
      llvm::LLVMContext Ctx;
      PipelineOptions Opts;
      Opts.EmitDumpOutput = false;
      Opts.LiftMode = true;
      Opts.NoOpt = NoOpt;
      Opts.JumpTableEvidenceBudgetForTesting = Budget;
      Opts.OnlyFunctionEntries = {0x401000};
      const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
      ASSERT_TRUE(Result.Success) << Result.Error;
      ASSERT_NE(Result.LlvmModule, nullptr);
      EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
      if (!NoOpt)
        continue; // the optimizer folds the cases' constants into arithmetic
      bool Dispatches = false;
      for (const llvm::Function &Function : *Result.LlvmModule)
        for (const llvm::BasicBlock &Block : Function)
          if (const auto *Switch = llvm::dyn_cast_or_null<llvm::SwitchInst>(
                  Block.getTerminator()))
            Dispatches |= Switch->getNumCases() == 3;
      EXPECT_TRUE(Dispatches) << "the three cases dispatch through a switch";
    }
}

TEST(X86_32_FunctionDiscovery, InCodePointerTableMirrorsPastTheLimit) {
  // A function-pointer table MSVC lays among its code, read as data, in a
  // .text larger than the emitter mirrors as one global.  The whole segment
  // cannot be a single global, but the table's own run (three code-pointer
  // slots) can: the read resolves through that run's mirror instead of
  // refusing because the segment is too large.
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::COFF;
  Img.IsRelocatable = false;
  Img.Base = 0x400000;
  Img.Entry = 0x401000;
  std::vector<uint8_t> Text;
  auto Emit = [&](std::initializer_list<uint8_t> Bytes) {
    Text.insert(Text.end(), Bytes);
  };
  // 0x401000: load the second table slot as data and return it.
  Emit({0xA1, 0x54, 0x10, 0x40, 0x00, 0xC3}); // mov eax, [0x401054]; ret
  Text.resize(0x10, 0xCC);
  Emit({0x33, 0xC0, 0xC3}); // 0x401010 case0: xor eax, eax; ret
  Text.resize(0x18, 0xCC);
  Emit({0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3}); // 0x401018 case1: mov eax,1; ret
  Text.resize(0x20, 0xCC);
  Emit({0xB8, 0x02, 0x00, 0x00, 0x00, 0xC3}); // 0x401020 case2: mov eax,2; ret
  Text.resize(0x50, 0xCC);
  // 0x401050: three consecutive relocated code-pointer slots (a run).
  for (uint32_t T : {0x401010u, 0x401018u, 0x401020u})
    for (unsigned B = 0; B < 4; ++B)
      Text.push_back(static_cast<uint8_t>(T >> (8 * B)));
  Text.resize(limits::kMaxSingleGlobalEmbedLen + 0x1000, 0xCC);
  addPeSection(Img, ".text", 0x401000, std::move(Text),
               SegmentFlags::Readable | SegmentFlags::Executable);
  recordAbsolutePointerRelocations(Img, {{0x401001, 0x401054, InvalidVA},
                                         {0x401050, 0x401010, InvalidVA},
                                         {0x401054, 0x401018, InvalidVA},
                                         {0x401058, 0x401020, InvalidVA}});
  coff_loader::recordRelocatedPointerRunsInCode(Img);
  ASSERT_TRUE(Img.DataInCode.count(0x401050));

  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.LiftMode = true;
  Opts.OnlyFunctionEntries = {0x401000, 0x401010, 0x401018, 0x401020};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_NE(Result.LlvmModule, nullptr);
  EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
  // The read resolves through a mirror global, not a stale load of 0x401054.
  std::string Text2;
  llvm::raw_string_ostream OS(Text2);
  Result.LlvmModule->print(OS, nullptr);
  EXPECT_EQ(Text2.find("inttoptr (i32 4198484"), std::string::npos) << Text2;
  EXPECT_NE(Text2.find("getelementptr"), std::string::npos) << Text2;
}

TEST(X86_32_FunctionDiscovery, ARuntimeIndexedReadOnlyTableResolves) {
  // `table[obj->field]`: a live-in object pointer in EBX, its field loaded at
  // pointer width, scaled by the int stride and added to a read-only .rdata
  // table base, as GOF2's sub_4028D4 does.  The index is an unbounded runtime
  // value, so the offset is unprovable, but the base is a genuine read-only
  // table: the read resolves through the .rdata run mirror by the base's own
  // offset plus the runtime index, rather than refusing as an ambiguous
  // read-only table base.
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::COFF;
  Img.IsRelocatable = false;
  Img.Base = 0x400000;
  Img.Entry = 0x401000;
  std::vector<uint8_t> Text;
  auto Emit = [&](std::initializer_list<uint8_t> Bytes) {
    Text.insert(Text.end(), Bytes);
  };
  // EBX is a live-in with no def, so prove() admits the field load as an
  // opaque runtime word, exactly GOF2's shape.
  Emit({0x8B, 0x43, 0x58});                         // mov eax, [ebx+0x58]
  Emit({0x8B, 0x04, 0x85, 0x00, 0x20, 0x40, 0x00}); // mov eax, [eax*4+0x402000]
  Emit({0xC3});                                     // ret
  addPeSection(Img, ".text", 0x401000, std::move(Text),
               SegmentFlags::Readable | SegmentFlags::Executable);
  std::vector<uint8_t> Rdata;
  for (uint32_t Word : {3u, 1u, 4u, 1u, 5u, 9u, 2u, 6u})
    for (unsigned Byte = 0; Byte < 4; ++Byte)
      Rdata.push_back(static_cast<uint8_t>(Word >> (8 * Byte)));
  addPeSection(Img, ".rdata", 0x402000, std::move(Rdata),
               SegmentFlags::Readable);
  // The base relocation of the displacement operand at 0x401006.
  recordAbsolutePointerRelocations(Img, {{0x401006, 0x402000, InvalidVA}});

  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.LiftMode = true;
  Opts.OnlyFunctionEntries = {0x401000};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_NE(Result.LlvmModule, nullptr);
  EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
  // The read lands in a real read-only global addressed by a GEP, not a
  // reconstructed pointer from the stale absolute table VA (0x402000).
  std::string Text2;
  llvm::raw_string_ostream OS(Text2);
  Result.LlvmModule->print(OS, nullptr);
  EXPECT_EQ(Text2.find("inttoptr (i32 4202496"), std::string::npos) << Text2;
  EXPECT_EQ(Text2.find("4202496"), std::string::npos)
      << "the table base VA must not survive as a constant\n"
      << Text2;
  // A .rdata run global exists and the load GEPs into it.
  const bool HasRodataGlobal = Text2.find("@") != std::string::npos &&
                               Text2.find(".rodata") != std::string::npos;
  EXPECT_TRUE(HasRodataGlobal) << Text2;
  EXPECT_NE(Text2.find("getelementptr i8"), std::string::npos) << Text2;
}

TEST(X86_32_FunctionDiscovery, APointerSpilledAcrossALoopsCallsKeepsItsTable) {
  // A loop walks an array of integers in .rdata with a pointer it keeps in a
  // frame slot across the call in its body: `add dword ptr [ebp-4], 4`.  The
  // slot holds the array's address or itself advanced, so every read through
  // it reads the array, which the LLVM emitter mirrors with the code pointer
  // beside it.
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::COFF;
  Img.IsRelocatable = false;
  Img.Base = 0x400000;
  Img.Entry = 0x401000;
  std::vector<uint8_t> Text;
  auto Emit = [&](std::initializer_list<uint8_t> Bytes) {
    Text.insert(Text.end(), Bytes);
  };
  Emit({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08}); // push ebp; mov ebp, esp; sub
  Emit({0xC7, 0x45, 0xFC, 0x00, 0x20, 0x40, 0x00}); // mov [ebp-4], 0x402000
  Emit({0xC7, 0x45, 0xF8, 0x00, 0x00, 0x00, 0x00}); // mov [ebp-8], 0
  Emit({0x8B, 0x45, 0xFC});                   // 0x401014: mov eax, [ebp-4]
  Emit({0x8B, 0x00, 0x50});                   // mov eax, [eax]; push eax
  Emit({0xE8, 0x21, 0x00, 0x00, 0x00});       // call 0x401040
  Emit({0x83, 0xC4, 0x04});                   // add esp, 4
  Emit({0x83, 0x45, 0xFC, 0x04});             // add [ebp-4], 4
  Emit({0xFF, 0x45, 0xF8});                   // inc [ebp-8]
  Emit({0x83, 0x7D, 0xF8, 0x03, 0x7C, 0xE5}); // cmp [ebp-8], 3; jl
  Emit({0x8B, 0xE5, 0x5D, 0xC3});             // mov esp, ebp; pop; ret
  Text.resize(0x40, 0xCC);
  Emit({0x8B, 0x44, 0x24, 0x04, 0xC3}); // 0x401040: mov eax, [esp+4]; ret
  addPeSection(Img, ".text", 0x401000, std::move(Text),
               SegmentFlags::Readable | SegmentFlags::Executable);
  addPeSection(Img, ".rdata", 0x402000,
               {0x05, 0, 0, 0, 0x0F, 0, 0, 0, 0x1E, 0, 0, 0, // the integers
                0x40, 0x10, 0x40, 0x00},                     // &0x401040
               SegmentFlags::Readable);
  recordAbsolutePointerRelocations(
      Img, {{0x401009, 0x402000, InvalidVA}, {0x40200C, 0x401040, InvalidVA}});
  ASSERT_TRUE(Img.CodePtrRelocSlots.count(0x40200C));

  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.LiftMode = true;
  Opts.OnlyFunctionEntries = {0x401000, 0x401040};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_NE(Result.LlvmModule, nullptr);
  EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
}

TEST(X86_32_FunctionDiscovery, ANestedLoopsSpilledIndexStaysAnOffset) {
  // An outer loop keeps its index in a frame slot and an inner loop its
  // record pointer, `&table[i]` advanced by 4, across a call that takes a
  // pushed argument.  Each loop header merges ESP, and the inner loop's merge
  // lies on the outer one's back edge; every value it merges is the frame's
  // own ESP, so the pushes stay frame slots apart from the two spills.
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::COFF;
  Img.IsRelocatable = false;
  Img.Base = 0x400000;
  Img.Entry = 0x401000;
  std::vector<uint8_t> Text;
  auto Emit = [&](std::initializer_list<uint8_t> Bytes) {
    Text.insert(Text.end(), Bytes);
  };
  Emit({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C}); // push ebp; mov ebp, esp; sub
  Emit({0xC7, 0x45, 0xFC, 0x00, 0x00, 0x00, 0x00}); // mov [ebp-4], 0
  Emit({0x8B, 0x45, 0xFC});             // 0x40100D: mov eax, [ebp-4]
  Emit({0x6B, 0xC0, 0x0C});             // imul eax, eax, 12
  Emit({0x05, 0x00, 0x20, 0x40, 0x00}); // add eax, 0x402000
  Emit({0x89, 0x45, 0xF8});             // mov [ebp-8], eax
  Emit({0xC7, 0x45, 0xF4, 0x00, 0x00, 0x00, 0x00}); // mov [ebp-0Ch], 0
  Emit({0x8B, 0x45, 0xF8});                   // 0x401022: mov eax, [ebp-8]
  Emit({0x8B, 0x00, 0x50});                   // mov eax, [eax]; push eax
  Emit({0xE8, 0x33, 0x00, 0x00, 0x00});       // call 0x401060
  Emit({0x83, 0xC4, 0x04});                   // add esp, 4
  Emit({0x83, 0x45, 0xF8, 0x04});             // add [ebp-8], 4
  Emit({0xFF, 0x45, 0xF4});                   // inc [ebp-0Ch]
  Emit({0x83, 0x7D, 0xF4, 0x03, 0x7C, 0xE5}); // cmp [ebp-0Ch], 3; jl
  Emit({0xFF, 0x45, 0xFC});                   // inc [ebp-4]
  Emit({0x83, 0x7D, 0xFC, 0x02, 0x7C, 0xC7}); // cmp [ebp-4], 2; jl
  Emit({0x8B, 0xE5, 0x5D, 0xC3});             // mov esp, ebp; pop; ret
  Text.resize(0x60, 0xCC);
  Emit({0x8B, 0x44, 0x24, 0x04, 0xC3}); // 0x401060: mov eax, [esp+4]; ret
  addPeSection(Img, ".text", 0x401000, std::move(Text),
               SegmentFlags::Readable | SegmentFlags::Executable);
  // Two records of three integers, then the helper's address.
  std::vector<uint8_t> Rdata;
  for (uint32_t Word : {5u, 15u, 30u, 7u, 8u, 9u, 0x401060u})
    for (unsigned Byte = 0; Byte < 4; ++Byte)
      Rdata.push_back(static_cast<uint8_t>(Word >> (8 * Byte)));
  addPeSection(Img, ".rdata", 0x402000, std::move(Rdata),
               SegmentFlags::Readable);
  recordAbsolutePointerRelocations(
      Img, {{0x401014, 0x402000, InvalidVA}, {0x402018, 0x401060, InvalidVA}});
  ASSERT_TRUE(Img.CodePtrRelocSlots.count(0x402018));

  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.LiftMode = true;
  Opts.OnlyFunctionEntries = {0x401000, 0x401060};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_NE(Result.LlvmModule, nullptr);
  EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
}
