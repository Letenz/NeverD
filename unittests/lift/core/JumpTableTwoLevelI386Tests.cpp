//===- JumpTableTwoLevelI386Tests.cpp - i386 compact switch tables -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent machine-code cases for compact switches whose byte index and
/// relocated target tables share an executable section.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFLoaderUtils.h"
#include "neverd/loader/PointerRelocation.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"

#include <array>

using namespace neverd;

namespace {

constexpr va_t Entry = 0x401000;
constexpr va_t Dispatch = Entry + 16;
constexpr va_t TargetsVA = Entry + 0x80;
constexpr va_t IndexVA = TargetsVA + 12;
constexpr va_t UnrelatedPointerVA = IndexVA + 0x40;
constexpr std::array<uint8_t, 6> Indices = {2, 0, 1, 2, 1, 0};
constexpr std::array<va_t, 3> Targets = {Entry + 29, Entry + 35, Entry + 41};

BinaryImage twoLevelImage(bool IndexInCode = true, bool SelectorInECX = false) {
  BinaryImage Image;
  Image.Format = BinaryFormat::COFF;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Entry = Entry;
  const va_t Index = IndexInCode ? IndexVA : 0x600000;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  // mov eax,[esp+4]; cmp eax,5; ja default;
  // movzx ecx,byte ptr [eax+index]; jmp dword ptr [ecx*4+targets]
  Text.Data = {0x8b, 0x44, 0x24, 0x04, 0x83, 0xf8, 0x05, 0x77, 0x0e, 0x0f,
               0xb6, 0x88, 0,    0,    0,    0,    0xff, 0x24, 0x8d, 0,
               0,    0,    0,    0xb8, 0xff, 0xff, 0xff, 0xff, 0xc3};
  if (SelectorInECX) {
    Text.Data[1] = 0x4c;  // mov ecx,[esp+4]
    Text.Data[5] = 0xf9;  // cmp ecx,5
    Text.Data[11] = 0x81; // movzx eax,byte ptr [ecx+index]
    Text.Data[18] = 0x85; // jmp dword ptr [eax*4+targets]
  }
  writeLE<uint32_t>(Text.Data.data() + 12, Index);
  writeLE<uint32_t>(Text.Data.data() + 19, TargetsVA);
  for (uint8_t Result : {11, 23, 37})
    Text.Data.insert(Text.Data.end(), {0xb8, Result, 0, 0, 0, 0xc3});
  // A whole-code mirror is deliberately too large. Recovery must authenticate
  // the two physical tables, not copy the surrounding instructions or pointers.
  Text.Data.resize(0x110000, 0xcc);
  for (unsigned I = 0; I != Targets.size(); ++I)
    writeLE<uint32_t>(Text.Data.data() + TargetsVA - Entry + I * 4, Targets[I]);
  // A generic suffix copy would also copy this unrelated original code
  // address. Only the authenticated index extent is safe to retain verbatim.
  for (unsigned I = 0; I != 3; ++I)
    writeLE<uint32_t>(Text.Data.data() + UnrelatedPointerVA - Entry + I * 4,
                      Entry);
  if (IndexInCode)
    std::copy(Indices.begin(), Indices.end(),
              Text.Data.begin() + IndexVA - Entry);
  Text.Size = Text.FileSz = Text.Data.size();
  Section Code;
  Code.Name = Text.Name;
  Code.VA = Text.VA;
  Code.Size = Text.Size;
  Code.Flags = Text.Flags;
  Image.Sections.push_back(Code);
  Image.Segments.push_back(std::move(Text));
  if (!IndexInCode) {
    Segment Data;
    Data.Name = ".rdata";
    Data.VA = Index;
    Data.Flags = SegmentFlags::Readable;
    Data.Data.assign(Indices.begin(), Indices.end());
    Data.Size = Data.FileSz = Data.Data.size();
    Section ReadOnly;
    ReadOnly.Name = Data.Name;
    ReadOnly.VA = Data.VA;
    ReadOnly.Size = Data.Size;
    ReadOnly.Flags = Data.Flags;
    Image.Sections.push_back(ReadOnly);
    Image.Segments.push_back(std::move(Data));
  }
  Symbol Function = Symbol::makeFunc(Entry, 47);
  Function.Name = "compact_switch";
  Image.Symbols.push_back(std::move(Function));
  std::vector<AbsolutePointerRelocation> Relocations;
  for (unsigned I = 0; I != Targets.size(); ++I)
    Relocations.push_back({TargetsVA + I * 4, Targets[I], InvalidVA});
  Relocations.push_back({Entry + 12, Index, InvalidVA});
  Relocations.push_back({Entry + 19, TargetsVA, InvalidVA});
  for (unsigned I = 0; I != 3; ++I)
    Relocations.push_back({UnrelatedPointerVA + I * 4, Entry, InvalidVA});
  recordAbsolutePointerRelocations(Image, Relocations);
  coff_loader::recordRelocatedPointerRunsInCode(Image);
  return Image;
}

LowFunc recover(const BinaryImage &Image) {
  Decoder Dec;
  if (!Dec.init(Image)) {
    ADD_FAILURE() << "decoder initialization failed";
    return {};
  }
  CFGBuilder Builder;
  return Builder.build(Image, Dec, Entry, "compact_switch");
}

void expectComposedSwitch(const LowFunc &Low) {
  ASSERT_EQ(Low.JumpTables.size(), 1u);
  const JumpTable &Table = Low.JumpTables.front();
  EXPECT_EQ(Table.InsnAddr, Dispatch);
  std::vector<va_t> Expected;
  for (uint8_t Index : Indices)
    Expected.push_back(Targets[Index]);
  EXPECT_EQ(Table.Targets, Expected);
  EXPECT_EQ(Table.AuthenticatedTableLoads.size(), 2u);
  EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
}

BinaryImage observableIndexImage() {
  BinaryImage Image = twoLevelImage();
  for (va_t Target : Targets) {
    // mov eax,ecx; ret. The compact index is now an observable result, so
    // replacing its LOAD with zero would change every nonzero result.
    auto At = Image.Segments.front().Data.begin() + Target - Entry;
    const uint8_t ReturnIndex[] = {0x89, 0xc8, 0xc3, 0xcc, 0xcc, 0xcc};
    std::copy(std::begin(ReturnIndex), std::end(ReturnIndex), At);
  }
  return Image;
}

TEST(JumpTableTwoLevelI386, ReadOnlyIndexUsesTheCompleteGuestAddress) {
  expectComposedSwitch(recover(twoLevelImage(false)));
}

TEST(JumpTableTwoLevelI386, InCodeIndexUsesTheGuardedOuterSelector) {
  expectComposedSwitch(recover(twoLevelImage()));
}

TEST(JumpTableTwoLevelI386, OuterSelectorMayUseECXAndInnerIndexEAX) {
  expectComposedSwitch(recover(twoLevelImage(true, true)));
}

TEST(JumpTableTwoLevelI386, MissingOuterGuardCannotPublishACompactSwitch) {
  BinaryImage Image = twoLevelImage();
  // Replace JA with two NOPs: the table still exists, but EAX is unbounded.
  Image.Segments.front().Data[7] = 0x90;
  Image.Segments.front().Data[8] = 0x90;
  EXPECT_TRUE(recover(Image).JumpTables.empty());
}

TEST(JumpTableTwoLevelI386, WritableIndexCannotPublishACompactSwitch) {
  BinaryImage Image = twoLevelImage(false);
  Image.Segments.back().Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Image.Sections.back().Flags = Image.Segments.back().Flags;
  EXPECT_TRUE(recover(Image).JumpTables.empty());
}

TEST(JumpTableTwoLevelI386, OutOfRangeIndexCannotPublishACompactSwitch) {
  BinaryImage Image = twoLevelImage();
  Image.Segments.front().Data[IndexVA - Entry + 5] = Targets.size();
  EXPECT_TRUE(recover(Image).JumpTables.empty());
}

TEST(JumpTableTwoLevelI386, WholeLiftRetainsEverySelectorCase) {
  BinaryImage Image = twoLevelImage();
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.LiftMode = true;
  Options.NoOpt = true;
  Options.EmitDumpOutput = false;
  Options.OnlyFunctionEntries = {Entry};
  auto Result = Pipeline().run(Image, Context, Options);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_NE(Result.LlvmModule, nullptr);
  EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule));
  const llvm::Function *Function =
      Result.LlvmModule->getFunction("compact_switch");
  ASSERT_NE(Function, nullptr);
  unsigned Switches = 0;
  for (const llvm::BasicBlock &Block : *Function)
    if (const auto *Switch =
            llvm::dyn_cast<llvm::SwitchInst>(Block.getTerminator())) {
      ++Switches;
      EXPECT_EQ(Switch->getNumCases(), Indices.size());
    }
  EXPECT_EQ(Switches, 1u);
}

TEST(JumpTableTwoLevelI386, ObservableIndexKeepsExactlyItsBoundedBytes) {
  BinaryImage Image = observableIndexImage();
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.LiftMode = true;
  Options.NoOpt = true;
  Options.EmitDumpOutput = false;
  Options.OnlyFunctionEntries = {Entry};
  auto Result = Pipeline().run(Image, Context, Options);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_NE(Result.LlvmModule, nullptr);
  EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule));
  const llvm::Function *Function =
      Result.LlvmModule->getFunction("compact_switch");
  ASSERT_NE(Function, nullptr);
  unsigned ByteLoads = 0;
  for (const llvm::BasicBlock &Block : *Function)
    for (const llvm::Instruction &Instruction : Block)
      if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction))
        ByteLoads += Load->getType()->isIntegerTy(8);
  EXPECT_GT(ByteLoads, 0u);
  unsigned BoundedMirrors = 0;
  for (const llvm::GlobalVariable &Global : Result.LlvmModule->globals()) {
    if (!Global.hasInitializer())
      continue;
    const auto *Initializer =
        llvm::dyn_cast<llvm::ConstantStruct>(Global.getInitializer());
    if (!Initializer || Initializer->getNumOperands() != Targets.size() + 1)
      continue;
    const auto *Bytes = llvm::dyn_cast<llvm::ConstantDataArray>(
        Initializer->getOperand(Targets.size()));
    if (!Bytes || Bytes->getNumElements() != Indices.size())
      continue;
    ++BoundedMirrors;
    EXPECT_EQ(Result.LlvmModule->getDataLayout().getTypeAllocSize(
                  Global.getValueType()),
              Targets.size() * 4 + Indices.size());
    for (unsigned I = 0; I != Indices.size(); ++I)
      EXPECT_EQ(Bytes->getElementAsInteger(I), Indices[I]);
  }
  EXPECT_EQ(BoundedMirrors, 1u);
}

TEST(JumpTableTwoLevelI386,
     IndexMirrorRequiresContiguousRelocationFreeStorage) {
  BinaryImage Image = observableIndexImage();
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.LiftMode = true;
  Options.NoOpt = true;
  Options.EmitDumpOutput = false;
  Options.OnlyFunctionEntries = {Entry};
  auto Result = Pipeline().run(Image, Context, Options);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_EQ(Result.MedFuncs.size(), 1u);
  ASSERT_EQ(Result.MedFuncs.front().JumpTables.size(), 1u);
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Functions = Result.MedFuncs;
    BinaryImage Changed = Image;
    auto &Ranges = Functions.front().JumpTables.front().StorageRanges;
    auto Range = std::find_if(Ranges.begin(), Ranges.end(),
                              [](const auto &R) { return R.EntrySize == 1; });
    ASSERT_NE(Range, Ranges.end());
    switch (Mutation) {
    case 0:
      // The module inventory order must not change the object's extent.
      std::reverse(Ranges.begin(), Ranges.end());
      break;
    case 1:
      Ranges.erase(Range);
      break;
    case 2:
      Range->EntryStride = 2;
      break;
    case 3:
      Range->PhysicalSlotCount = InvalidVA;
      break;
    case 4:
      Range->BaseAddr -= 1; // Partial overlap with the preceding pointer.
      break;
    case 5:
      Changed.CodePtrRelocSlots.insert(IndexVA + 1);
      break;
    case 6:
      Changed.DataPtrRelocSlots.insert(IndexVA + 1);
      break;
    case 7:
      Ranges.push_back(
          {IndexVA + 2, 1, 1, 4}); // A shared suffix is one object.
      std::reverse(Ranges.begin(), Ranges.end());
      break;
    }
    llvm::LLVMContext EmissionContext;
    testing::internal::CaptureStderr();
    auto Module =
        MedLLVMEmitter().emit(Functions, EmissionContext, "compact-index",
                              Arch::X86, {}, &Changed, BinaryFormat::COFF);
    const std::string Diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Module != nullptr, Mutation == 0 || Mutation == 7) << Diagnostic;
    if (Module)
      EXPECT_FALSE(llvm::verifyModule(*Module));
  }
}

} // namespace
