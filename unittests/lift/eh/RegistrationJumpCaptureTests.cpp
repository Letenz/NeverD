//===- RegistrationJumpCaptureTests.cpp - Physical capture arguments ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check the current frame, physical PC and complete EH3 buffer separation.

#include "../../../lib/ir/low/X86/RegistrationJumpCapture.h"
#include "../../../lib/ir/low/X86/RegistrationJumpRuntime.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/COFF/COFFLoader.h"

using namespace neverd;
using namespace neverd::registration_state;

#ifdef NEVERD_BINARY_CORPUS_ROOT
TEST(RegistrationJumpCapture, PhysicalCapturePCPartitionsOrdinaryFlow) {
  auto Image = COFFLoader().load(
      std::string(NEVERD_BINARY_CORPUS_ROOT) +
      "/corpus/windows-eh/msvc/vs2019/x86/native/no-gs/o0/windows-seh-tests/"
      "xcpt4-msvc-x86-native-no-gs-o0.exe");
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  Decoder Dec;
  ASSERT_TRUE(Dec.init(*Image));
  const auto Function =
      CFGBuilder().build(*Image, Dec, 0x401580, "capture-pcs");
  unsigned Captures = 0, Resumes = 0;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops) {
      if (Op.Opcode != NdOp::CALL || Op.NumInputs != 1 ||
          !Op.Inputs[0].isConst() ||
          (Op.Inputs[0].Offset != 0x407c7e && Op.Inputs[0].Offset != 0x407c66))
        continue;
      const bool Capture = Op.Inputs[0].Offset == 0x407c7e;
      if (Capture)
        ++Captures;
      else
        ++Resumes;
      EXPECT_EQ(Block.EndAddr, Op.Addr + 5);
      if (!Capture)
        continue;
      ASSERT_EQ(Block.Succs.size(), 1u);
      const auto Next = llvm::find_if(Function.Blocks, [&](const LowBlock &B) {
        return B.Id == Block.Succs[0];
      });
      ASSERT_NE(Next, Function.Blocks.end());
      EXPECT_EQ(Next->StartAddr, Op.Addr + 5);
      EXPECT_FALSE(Function.ModuleAnalysisRoots.count(Next->StartAddr));
      EXPECT_FALSE(Function.OrdinaryModuleAnalysisRoots.count(Next->StartAddr));
    }
  EXPECT_EQ(Captures, 7u);
  EXPECT_EQ(Resumes, 6u);
}

TEST(RegistrationJumpCapture, BindsCurrentSourceAndBoundedCallerStorage) {
  for (unsigned Mutation = 0; Mutation != 23; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = COFFLoader().load(
        std::string(NEVERD_BINARY_CORPUS_ROOT) +
        "/corpus/windows-eh/msvc/vs2019/x86/native/no-gs/o0/windows-seh-tests/"
        "xcpt4-msvc-x86-native-no-gs-o0.exe");
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    const auto *EH = Image->ExceptionMetadata.findFunction(0x401580);
    ASSERT_TRUE(EH && EH->Registration);
    LowFunc Function;
    Function.Entry = 0x401580;
    Function.ExceptionMetadata = *EH;
    LowBlock Block;
    Block.Id = 0;
    Block.StartAddr = 0x40223c;
    Block.EndAddr = 0x402241;
    LowOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = Block.StartAddr;
    Call.Seq = 0;
    Call.NumInputs = 1;
    Call.Inputs[0] = NdVar::cst(0x407c7e, 4);
    Call.Output = NdVar::reg(x86reg::RAX, 4);
    Block.Ops.push_back(Call);
    LowInstructionBoundary Source;
    Source.Address = Call.Addr;
    Source.Size = 5;
    Source.OpCount = 1;
    Source.Control = LowInstructionControl::Call;
    Source.ControlFlags = LowInstructionControlFlag::Call;
    Source.Immediate = Call.Inputs[0].Offset;
    Block.InstructionBoundaries.push_back(Source);
    Function.Blocks.push_back(Block);
    FrameState Frame;
    Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
        FrameValue::frame(-316);
    Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride] =
        FrameValue::frame(0);
    Frame.Registers[x86reg::RBX / x86reg::GeneralRegStride].MayBeFrame = true;
    Frame.Cells[-316] = FrameValue::frame(-288);
    Frame.Cells[-312] = FrameValue::constant(2);
    Frame.Cells[-308] = FrameValue::constant(0x407c78);
    Frame.Cells[-304] = FrameValue::constant(UINT32_MAX);
    Frame.Cells[-16] = FrameValue::previousChain();
    Frame.Cells[-12] = FrameValue::constant(uint32_t(EH->PersonalityVA));
    Frame.Cells[-8] =
        FrameValue::constant(uint32_t(EH->Registration->ScopeTableVA));
    Frame.Cells[-4] = FrameValue::constant(UINT32_MAX);
    auto &Op = Function.Blocks[0].Ops[0];
    auto &Insn = Function.Blocks[0].InstructionBoundaries[0];
    bool Installed = true;
    size_t Work = 0;
    if (Mutation == 1)
      Installed = false;
    if (Mutation == 2)
      Frame.Cells[-316] = FrameValue::frame(-316);
    if (Mutation == 3)
      Frame.Cells[-316] = FrameValue::frame(-84); // end crosses SavedESP.
    if (Mutation == 4)
      Frame.Cells[-312] = FrameValue::constant(1);
    if (Mutation == 5)
      Frame.Cells[-308] = FrameValue::constant(0x407c66);
    if (Mutation == 6)
      Frame.Cells[-304] = FrameValue::constant(0);
    if (Mutation == 7)
      Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride] = {};
    if (Mutation == 8)
      Frame.enterCallback(0x401683);
    if (Mutation == 9)
      Frame.Cells[-16] = FrameValue::constant(0);
    if (Mutation == 10)
      Frame.Cells[-8] = FrameValue::constant(0);
    if (Mutation == 11)
      Insn.Immediate = 0x407c66;
    if (Mutation == 12)
      Insn.OpCount = 0;
    if (Mutation == 13)
      Insn.ControlFlags |= LowInstructionControlFlag::NoReturn;
    if (Mutation == 14)
      Insn.ControlFlags |= LowInstructionControlFlag::Conditional;
    if (Mutation == 15)
      Op.Inputs[0].Size = 8;
    if (Mutation == 16)
      Work = limits::kMaxRegistrationEHStateWork;
    if (Mutation == 17)
      Function.ExceptionMetadata->Registration->RegistrationOffset = -20;
    if (Mutation == 18) {
      auto *Byte = const_cast<uint8_t *>(Image->readVA(Op.Addr, 1));
      ASSERT_TRUE(Byte);
      *Byte = 0x90;
    }
    if (Mutation == 19)
      Frame.Cells[-316].ReturnPC = true;
    if (Mutation == 20)
      Op.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
    if (Mutation == 21)
      Function.Entry = 0x401090;
    if (Mutation == 22)
      Function.ExceptionMetadata->CodeRange.End = Op.Addr + 4;
    const auto Capture =
        prepareEH3JumpCapture(*Image, Function, Op, Frame, Installed, Work);
    EXPECT_EQ(Capture.has_value(), Mutation == 0);
    if (Capture) {
      size_t SourceWork = 0;
      const auto Binding =
          checkedJumpRuntimeCall(*Image, Function, Op, SourceWork);
      ASSERT_TRUE(Binding);
      EXPECT_EQ(Binding->Routine, JumpRuntimeBinding::Kind::Capture);
      EXPECT_TRUE(isEH3JumpRuntimeInstruction(*Image, Op.Addr, 5,
                                              Function.Blocks[0].Ops));
      EXPECT_EQ(Capture->Call, 0x40223cu);
      EXPECT_EQ(Capture->ReturnPC, 0x402241u);
      EXPECT_EQ(Capture->StackOffset, -316);
      EXPECT_EQ(Capture->BufferOffset, -288);
      EXPECT_EQ(Capture->UnwindTarget, 0x407c78u);
      EXPECT_EQ(Capture->Level, -1);
      EXPECT_EQ(Capture->Nonvolatile[0].Offset, 0);
      EXPECT_TRUE(Capture->Nonvolatile[1].MayBeFrame);
      EXPECT_GT(Work, 0u);
      const auto Detached = Op;
      EXPECT_FALSE(prepareEH3JumpCapture(*Image, Function, Detached, Frame,
                                         Installed, Work));
      EXPECT_FALSE(
          checkedJumpRuntimeCall(*Image, Function, Detached, SourceWork));
    }
  }
}
#endif
