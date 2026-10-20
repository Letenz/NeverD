//===- RelocatedLiteralTests.cpp - Data targets among executable bytes ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/loader/PointerRelocation.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/IR/Verifier.h"

using namespace neverd;

namespace {

BinaryImage literalImage(BinaryFormat Format, Arch Target) {
  BinaryImage Img;
  Img.Format = Format;
  Img.Arch = Target;
  Img.Bits = Target == Arch::X86 ? Bitness::Bits32 : Bitness::Bits64;
  Img.Entry = 0x1000;
  Segment Code;
  Code.VA = 0x1000;
  Code.Size = 0x100;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Data.assign(Code.Size, 0xcc);
  Code.Data[0] = 0x55;
  Code.Data[1] = 0x5d;
  Code.Data[2] = 0xc3;
  const uint8_t Text[] = {'n', 0,   't', 0,   'f', 0,   's', 0, '.',
                          0,   's', 0,   'y', 0,   's', 0,   0, 0};
  std::copy(std::begin(Text), std::end(Text), Code.Data.begin() + 0x20);
  Img.Segments.push_back(std::move(Code));
  Segment Data;
  Data.VA = 0x2000;
  Data.Size = 0x20;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(Data.Size);
  if (Target == Arch::X86) {
    writeLE<uint32_t>(Data.Data.data(), 0x1020);
    writeLE<uint32_t>(Data.Data.data() + 8, 0x1000);
  } else {
    writeLE<uint64_t>(Data.Data.data(), 0x1020);
    writeLE<uint64_t>(Data.Data.data() + 8, 0x1000);
  }
  Img.Segments.push_back(std::move(Data));
  recordAbsolutePointerRelocations(
      Img, {{0x2000, 0x1020, InvalidVA}, {0x2008, 0x1000, InvalidVA}});
  return Img;
}

TEST(RelocatedLiterals, CorrectsDataTargetWithoutChangingFunctionTarget) {
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
    for (Arch Target : {Arch::X86, Arch::X64}) {
      SCOPED_TRACE(::testing::Message() << static_cast<int>(Format) << ':'
                                        << static_cast<int>(Target));
      auto Img = literalImage(Format, Target);
      ASSERT_TRUE(Img.CodePtrRelocSlots.count(0x2000));
      runPostLoadDiscovery(Img, "relocated literals");
      EXPECT_FALSE(Img.CodePtrRelocSlots.count(0x2000));
      EXPECT_TRUE(Img.DataPtrRelocSlots.count(0x2000));
      EXPECT_TRUE(Img.DataInCode.count(0x1020));
      EXPECT_EQ(Img.DataPtrRelocTargetOwners.at(0x2000), 0x1000u);
      EXPECT_TRUE(Img.CodePtrRelocSlots.count(0x2008));
      EXPECT_FALSE(Img.DataPtrRelocSlots.count(0x2008));
      for (const auto &Sym : Img.Symbols)
        EXPECT_NE(Sym.Addr, 0x1020u);
    }
  }
}

TEST(RelocatedLiterals, LLVMRebuildsDataInsteadOfDeclaringAStringFunction) {
  auto Img = literalImage(BinaryFormat::COFF, Arch::X64);
  runPostLoadDiscovery(Img, "LLVM literal target");
  MedFunc F;
  F.Entry = 0x1000;
  F.Name = "literal_pointer";
  F.ReturnType = NdType::makeInt(8);
  F.Blocks.emplace_back();
  F.Blocks[0].Id = 0;
  F.Blocks[0].StartAddr = 0x1000;
  F.Blocks[0].EndAddr = 0x1004;
  MedOp Copy;
  Copy.Opcode = NdOp::COPY;
  Copy.Output.Kind = MedVar::Reg;
  Copy.Output.TheArch = Arch::X64;
  Copy.Output.RegOff = getTargetRegInfo(Arch::X64).IntReturnReg;
  Copy.Output.Id = 1;
  Copy.Output.SSAVer = 1;
  Copy.Output.Size = 8;
  Copy.addInput(
      MedVar::makeConst(0x2000, 8, ConstantAddressProvenance::DataAddress));
  F.Blocks[0].Ops.push_back(Copy);
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(Copy.Output);
  F.Blocks[0].Ops.push_back(Return);
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit({F}, Context, "literal-data", Arch::X64,
                                      {}, &Img, BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  EXPECT_FALSE(llvm::verifyModule(*Module));
  EXPECT_EQ(Module->getFunction("sub_1020"), nullptr);
}

TEST(RelocatedLiterals, AlsoClassifiesRestrictedLoadsAndMachOFunctionStarts) {
  for (BinaryFormat Format : {BinaryFormat::COFF, BinaryFormat::MachO}) {
    auto Img = literalImage(Format, Arch::X64);
    Img.LoadOnlyFunctionEntries.insert(0x1000);
    Img.KnownCodeRanges.emplace_back(0x1000, 0x1003);
    Img.MachOHasFunctionStarts = true;
    runPostLoadDiscovery(Img, "restricted relocated literals");
    EXPECT_TRUE(Img.DataPtrRelocSlots.count(0x2000));
  }
}

TEST(RelocatedLiterals, KeepsTargetsClaimedByCodeMetadata) {
  for (unsigned Proof = 0; Proof != 5; ++Proof) {
    auto Img = literalImage(BinaryFormat::COFF, Arch::X64);
    switch (Proof) {
    case 0:
      Img.Symbols.push_back(Symbol::makeFunc(0x1020));
      break;
    case 1:
      Img.RuntimeFunctionAddrs.insert(0x1020);
      break;
    case 2:
      Img.KnownCodeRanges.emplace_back(0x1010, 0x1040);
      break;
    case 3:
      Img.VerifiedFunctionEntries.insert(0x1020);
      break;
    case 4:
      Img.KnownCodeRanges.emplace_back(0x1000, 0x1020);
      break;
    }
    runPostLoadDiscovery(Img, "code metadata wins");
    EXPECT_TRUE(Img.CodePtrRelocSlots.count(0x2000));
    EXPECT_FALSE(Img.DataInCode.count(0x1020));
  }
}

TEST(RelocatedLiterals, RetainsUnknownAndReturningCodeTargets) {
  for (const std::vector<uint8_t> Bytes :
       {std::vector<uint8_t>{0x90, 0x0f, 0x0b},
        std::vector<uint8_t>{'H', 'k', 0, 0xcc, 0x90, 0xc3},
        std::vector<uint8_t>{'H', 't', 0x7e, 0}}) {
    auto Img = literalImage(BinaryFormat::COFF, Arch::X64);
    // A conditional jump can escape the literal/padding region into other
    // bytes; that is no proof of data, even if the taken target also traps.
    Img.Segments[0].Data[0x60] = 0x90;
    std::copy(Bytes.begin(), Bytes.end(), Img.Segments[0].Data.begin() + 0x20);
    runPostLoadDiscovery(Img, "not a proved literal");
    EXPECT_TRUE(Img.CodePtrRelocSlots.count(0x2000));
    EXPECT_FALSE(Img.DataPtrRelocSlots.count(0x2000));
  }
}

} // namespace
