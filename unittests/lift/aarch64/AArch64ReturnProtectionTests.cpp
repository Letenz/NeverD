//===- AArch64ReturnProtectionTests.cpp - Source return protection ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedPrologueSaves.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/Support/raw_ostream.h"

#include <initializer_list>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace neverd;

MedFunc decodedFunction(std::initializer_list<uint32_t> Words,
                        BinaryFormat Format) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Format = Format;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = Words.size() * 4;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  size_t At = 0;
  for (uint32_t Word : Words) {
    writeLE<uint32_t>(Text.Data.data() + At, Word);
    At += 4;
  }
  Image.Segments.push_back(std::move(Text));
  Segment Callee;
  Callee.VA = 0x1100;
  Callee.Size = Callee.FileSz = 4;
  Callee.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Callee.Data.resize(4);
  writeLE<uint32_t>(Callee.Data.data(), 0xd65f03c0);
  Image.Segments.push_back(std::move(Callee));
  Decoder Dec;
  EXPECT_TRUE(Dec.init(Image.Arch));
  Dec.setStrict(true);
  CFGBuilder Builder;
  const std::set<va_t> Entries{0x1000, 0x1100};
  Builder.setKnownFuncEntries(&Entries);
  auto Low = Builder.build(Image, Dec, 0x1000, "protected_return");
  LowToMedConverter Converter;
  Converter.setBinaryImage(&Image);
  auto Med = Converter.convert(Low, Image.Arch, Format);
  markSourceStackAccesses(Med, Image.Arch, Format);
  return Med;
}

std::string source(const MedFunc &Med, BinaryFormat Format,
                   Arch OutputArchitecture = Arch::AArch64) {
  auto High = MedToHighConverter().convert(Med, Arch::AArch64);
  CEmitterOptions Options;
  Options.TheArch = OutputArchitecture;
  Options.Format = Format;
  std::string TextC;
  llvm::raw_string_ostream Out(TextC);
  EXPECT_TRUE(HighCEmitter().emit({High}, Out, Options));
  return TextC;
}

std::string source(std::initializer_list<uint32_t> Words, BinaryFormat Format,
                   Arch OutputArchitecture = Arch::AArch64) {
  return source(decodedFunction(Words, Format), Format, OutputArchitecture);
}

TEST(AArch64ReturnProtection, UnobservedLinkPhisDoNotKeepMachineProtection) {
  for (BinaryFormat Format :
       {BinaryFormat::ELF, BinaryFormat::MachO, BinaryFormat::COFF}) {
    // A call on only one arm creates an LR PHI, overwritten by the reload.
    // paciasp; stp fp,lr,[sp,#-16]!; cbz x0,join; bl 0x1100;
    // join: mov x0,#7; ldp fp,lr,[sp],#16; autiasp; ret.
    const auto Text = source({0xd503233f, 0xa9bf7bfd, 0xb4000040, 0x9400003d,
                              0xd28000e0, 0xa8c17bfd, 0xd50323bf, 0xd65f03c0},
                             Format);
    EXPECT_NE(Text.find("branch-protection=pac-ret"), std::string::npos)
        << Text;
    EXPECT_EQ(Text.find("__pointer"), std::string::npos) << Text;
    EXPECT_NE(Text.find("return 7;"), std::string::npos) << Text;
  }
}

TEST(AArch64ReturnProtection, ConditionalPrologueAuthenticatesOnlyItsOwnPath) {
  for (BinaryFormat Format :
       {BinaryFormat::ELF, BinaryFormat::MachO, BinaryFormat::COFF}) {
    // cbz x0,ret; paciasp; stp fp,lr,[sp,#-16]!; bl 0x1100;
    // ldp fp,lr,[sp],#16; autiasp; ret.
    const auto Text = source({0xb40000c0, 0xd503233f, 0xa9bf7bfd, 0x9400003d,
                              0xa8c17bfd, 0xd50323bf, 0xd65f03c0},
                             Format);
    EXPECT_NE(Text.find("branch-protection=pac-ret"), std::string::npos)
        << Text;
    EXPECT_EQ(Text.find("__pointer"), std::string::npos) << Text;
  }
}

TEST(AArch64ReturnProtection, AuthenticatedDirectAndIndirectTailTransfers) {
  for (BinaryFormat Format :
       {BinaryFormat::ELF, BinaryFormat::MachO, BinaryFormat::COFF}) {
    // paciasp; stp fp,lr,[sp,#-16]!; ldp fp,lr,[sp],#16;
    // autiasp; b 0x1100 / br x16.
    for (uint32_t Branch : {0x1400003cu, 0xd61f0200u}) {
      for (bool BKey : {false, true}) {
        const auto Med = decodedFunction(
            {BKey ? 0xd503237fu : 0xd503233fu, 0xa9bf7bfd, 0xa8c17bfd,
             BKey ? 0xd50323ffu : 0xd50323bfu, Branch},
            Format);
        unsigned Tails = 0;
        for (const auto &Block : Med.Blocks)
          for (const auto &Op : Block.Ops)
            Tails += Op.IsTailCall;
        EXPECT_EQ(Tails, 2u); // The classified call and its synthetic return.
        const auto Text = source(Med, Format);
        EXPECT_NE(Text.find(BKey ? "branch-protection=pac-ret+leaf+b-key"
                                 : "branch-protection=pac-ret+leaf\""),
                  std::string::npos)
            << Text;
        EXPECT_EQ(Text.find("__pointer"), std::string::npos) << Text;
      }
    }
  }
}

TEST(AArch64ReturnProtection, ProvenNoReturnRetainsTheSigningPolicy) {
  auto Med =
      decodedFunction({0xd503233f, 0xa9bf7bfd, 0x9400003e}, BinaryFormat::ELF);
  ASSERT_EQ(Med.Blocks.back().Ops.back().Opcode, NdOp::CALL);
  // The decoder/runtime declaration owns this marker; a missing RET alone
  // must never be taken as evidence that the call cannot return.
  EXPECT_FALSE(findAArch64ReturnProtection(Med, BinaryFormat::ELF));
  Med.Blocks.back().Ops.back().DoesNotReturn = true;
  const auto Text = source(Med, BinaryFormat::ELF);
  EXPECT_NE(Text.find("branch-protection=pac-ret"), std::string::npos) << Text;
  EXPECT_EQ(Text.find("__pointer"), std::string::npos) << Text;
}

TEST(AArch64ReturnProtection, UsedLinkPhiAndRecoveredArgumentStayExplicit) {
  const auto Original =
      decodedFunction({0xd503233f, 0xa9bf7bfd, 0xb4000040, 0x9400003d,
                       0xd28000e0, 0xa8c17bfd, 0xd50323bf, 0xd65f03c0},
                      BinaryFormat::ELF);
  ASSERT_TRUE(findAArch64ReturnProtection(Original, BinaryFormat::ELF));
  auto Observable = Original;
  bool Replaced = false;
  for (auto &Block : Observable.Blocks)
    for (const auto &Phi : Block.Phis)
      if (Phi.Output.Kind == MedVar::Reg && Phi.Output.RegOff == a64reg::X30)
        for (auto &Op : Block.Ops)
          if (Op.Output.Kind == MedVar::Reg && Op.Output.RegOff == a64reg::X0 &&
              Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
              Op.Inputs[0].isConst() && Op.Inputs[0].ConstVal == 7) {
            Op.Inputs[0] = Phi.Output;
            Replaced = true;
          }
  ASSERT_TRUE(Replaced);
  EXPECT_FALSE(findAArch64ReturnProtection(Observable, BinaryFormat::ELF));

  auto Passed = Original;
  for (const auto &Block : Passed.Blocks)
    for (const auto &Phi : Block.Phis)
      if (Phi.Output.Kind == MedVar::Reg && Phi.Output.RegOff == a64reg::X30) {
        MedCallInfo Call;
        Call.Args.push_back(Phi.Output);
        Passed.CallInfos.push_back(Call);
      }
  ASSERT_FALSE(Passed.CallInfos.empty());
  EXPECT_FALSE(findAArch64ReturnProtection(Passed, BinaryFormat::ELF));
}

TEST(AArch64ReturnProtection, UnsafeExitsAndModifiedSaveSlotsStayExplicit) {
  for (auto Words :
       {// Conditional prologue, but its signed arm never authenticates.
        std::initializer_list<uint32_t>{0xb40000c0, 0xd503233f, 0xa9bf7bfd,
                                        0x9400003d, 0xa8c17bfd, 0xd503201f,
                                        0xd65f03c0},
        // A four-byte store overlaps the saved LR.
        {0xd503233f, 0xa9bf7bfd, 0xb9000be0, 0xa8c17bfd, 0xd50323bf,
         0xd65f03c0},
        // One predecessor reloads an uninitialized LR slot.
        {0xd503233f, 0xd10043ff, 0xb4000040, 0xf90007fe, 0xf94007fe, 0x910043ff,
         0xd50323bf, 0xd65f03c0},
        // Tail transfer with a signed LR, without AUT.
        {0xd503233f, 0xa9bf7bfd, 0xa8c17bfd, 0xd61f0200},
        // Authentication succeeds, but the outgoing SP is not restored.
        {0xd503233f, 0xa9bf7bfd, 0xa8c17bfd, 0xd50323bf, 0xd10043ff,
         0xd61f0200},
        // RET likewise needs the native entry stack position.
        {0xd503233f, 0xd50323bf, 0xd10043ff, 0xd65f03c0},
        // The authenticated address is a tail-call argument.
        {0xd503233f, 0xa9bf7bfd, 0xa8c17bfd, 0xd50323bf, 0xaa1e03e0,
         0xd61f0200},
        // An ordinary BL after AUT clobbers LR; CALL+RET is not tail proof.
        {0xd503233f, 0xd50323bf, 0x9400003e, 0xd65f03c0}}) {
    const auto Med = decodedFunction(Words, BinaryFormat::ELF);
    EXPECT_FALSE(findAArch64ReturnProtection(Med, BinaryFormat::ELF));
  }
}

TEST(AArch64ReturnProtection, VersionedEntryCopiesDoNotHideThePrologue) {
  auto Med =
      decodedFunction({0xd503233f, 0xd50323bf, 0xd65f03c0}, BinaryFormat::ELF);
  auto Copy = Med.Blocks.front().Ops.front();
  ASSERT_EQ(Copy.Opcode, NdOp::COPY);
  Copy.Output.SSAVer = 900;
  Med.Blocks.front().Ops.insert(Med.Blocks.front().Ops.begin(), Copy);
  EXPECT_TRUE(findAArch64ReturnProtection(Med, BinaryFormat::ELF));
}

TEST(AArch64ReturnProtection, LandingLabelsRetainTheEntryFrameSave) {
  auto Med = decodedFunction({0xd503233f, 0xa9bf7bfd, 0xb4000040, 0x9400003d,
                              0xd28000e0, 0xa8c17bfd, 0xd50323bf, 0xd65f03c0},
                             BinaryFormat::ELF);
  ASSERT_GT(Med.Blocks.size(), 1u);
  // Address-taken labels/landing pads are analysis roots inside this native
  // invocation, not fresh calls that bypass its entry frame initialization.
  Med.ModuleAnalysisRoots.insert(Med.Blocks.back().StartAddr);
  EXPECT_TRUE(findAArch64ReturnProtection(Med, BinaryFormat::ELF));
}

TEST(AArch64ReturnProtection, UnresolvedLocalDispatchKeepsAuthentication) {
  // A BR with an active frame may be a local switch. Missing successors do
  // not prove a tail call or a no-return function.
  const auto Text =
      source({0xd503233f, 0xa9bf7bfd, 0xd61f0200}, BinaryFormat::ELF);
  EXPECT_EQ(Text.find("branch-protection="), std::string::npos) << Text;
  EXPECT_NE(Text.find("pacia "), std::string::npos) << Text;
}

TEST(AArch64ReturnProtection, LeafKeepsKeyAndReturnValue) {
  for (BinaryFormat Format :
       {BinaryFormat::ELF, BinaryFormat::MachO, BinaryFormat::COFF}) {
    // pacibsp; mov x0, #7; retab.
    const auto Text = source({0xd503237f, 0xd28000e0, 0xd65f0fff}, Format);
    EXPECT_NE(Text.find("branch-protection=pac-ret+leaf+b-key"),
              std::string::npos)
        << Text;
    EXPECT_NE(Text.find("return 7;"), std::string::npos) << Text;
    EXPECT_EQ(Text.find("__pointer"), std::string::npos) << Text;
    EXPECT_EQ(Text.find("__builtin_ptrauth_auth"), std::string::npos) << Text;
  }
}

TEST(AArch64ReturnProtection, SavedReturnAddressBecomesFunctionAttribute) {
  for (BinaryFormat Format :
       {BinaryFormat::ELF, BinaryFormat::MachO, BinaryFormat::COFF}) {
    // paciasp; stp fp,lr,[sp,#-32]!; mov fp,sp; bl 0x1100;
    // mov x0,#7; ldp fp,lr,[sp],#32; autiasp; ret.
    const auto Text = source({0xd503233f, 0xa9be7bfd, 0x910003fd, 0x9400003d,
                              0xd28000e0, 0xa8c27bfd, 0xd50323bf, 0xd65f03c0},
                             Format);
    EXPECT_NE(Text.find("target(\"branch-protection=pac-ret\")"),
              std::string::npos)
        << Text;
    EXPECT_NE(Text.find("return 7;"), std::string::npos) << Text;
    EXPECT_EQ(Text.find("__pointer"), std::string::npos) << Text;
    EXPECT_EQ(Text.find("stack_storage"), std::string::npos) << Text;
  }
}

TEST(AArch64ReturnProtection, IncompleteOrObservableRecipesStayExplicit) {
  for (auto Words :
       {// PAC and AUT keys differ.
        std::initializer_list<uint32_t>{0xd503233f, 0xd50323ff, 0xd65f03c0},
        // Signed LR is the observable result: mov x0,lr.
        {0xd503233f, 0xaa1e03e0, 0xd50323bf, 0xd65f03c0},
        // No authentication.
        {0xd503233f, 0xd28000e0, 0xd65f03c0},
        // Authentication uses a different stack position.
        {0xd503233f, 0xd10043ff, 0xd50323bf, 0xd65f03c0}}) {
    const auto Text = source(Words, BinaryFormat::ELF);
    EXPECT_EQ(Text.find("branch-protection="), std::string::npos) << Text;
    EXPECT_NE(Text.find("pacia "), std::string::npos) << Text;
  }
}

TEST(AArch64ReturnProtection, OtherTargetsCannotDiscardThePolicy) {
  EXPECT_THROW(source({0xd503237f, 0xd28000e0, 0xd65f0fff}, BinaryFormat::ELF,
                      Arch::X64),
               std::invalid_argument);
}

TEST(AArch64ReturnProtection, MalformedRecipesCannotDisappearBeforeValidation) {
  // Use the owning lowering path for register identities and entry seeds.
  // paciasp; autiasp; ret.
  const auto Function =
      decodedFunction({0xd503233f, 0xd50323bf, 0xd65f03c0}, BinaryFormat::ELF);
  ASSERT_TRUE(findAArch64ReturnProtection(Function, BinaryFormat::ELF));
  unsigned Checked = 0;
  for (size_t Index = 0; Index < Function.Blocks.front().Ops.size(); ++Index) {
    if (Function.Blocks.front().Ops[Index].Opcode != NdOp::INTRINSIC)
      continue;
    auto Broken = Function;
    Broken.Blocks.front().Ops[Index].Inputs[2].Size = 4;
    EXPECT_FALSE(findAArch64ReturnProtection(Broken, BinaryFormat::ELF));
    ++Checked;
  }
  EXPECT_EQ(Checked, 2u);
}
} // namespace
