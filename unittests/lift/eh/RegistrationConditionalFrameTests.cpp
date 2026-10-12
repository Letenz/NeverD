//===- RegistrationConditionalFrameTests.cpp - Bound SEH inputs -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check invocation-specific SEH frame inputs and bounded effects.

#include "../../../lib/ir/low/X86/RegistrationABIPrivate.h"
#include "../../../lib/ir/low/X86/RegistrationStateSolver.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/COFF/COFFLoader.h"

using namespace neverd;

#ifdef NEVERD_BINARY_CORPUS_ROOT
namespace {
auto loadXCPT4() {
  return COFFLoader().load(
      std::string(NEVERD_BINARY_CORPUS_ROOT) +
      "/corpus/windows-eh/msvc/vs2019/x86/native/no-gs/o0/windows-seh-tests/"
      "xcpt4-msvc-x86-native-no-gs-o0.exe");
}
} // namespace

TEST(RegistrationConditionalFrame, XCPT4HelperChecksItsBoundIncomingCounter) {
  auto Image = loadXCPT4();
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  Decoder Dec;
  ASSERT_TRUE(Dec.init(*Image));
  CFGBuilder Builder;
  const auto Function =
      Builder.build(*Image, Dec, 0x401250, "conditional-counter");
  ASSERT_TRUE(Function.ExceptionMetadata &&
              Function.ExceptionMetadata->Registration);
  RegistrationCallCalleeIndex Index(*Image);
  const auto Stacks = Index.stackContracts(Function);
  const auto Unwinds = Index.localUnwindContracts(Function);
  const auto Controls = Index.sehControlContracts(Function);
  ASSERT_TRUE(Stacks && Unwinds && Controls);
  using namespace registration_state;
  for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Current = Function;
    ConditionalFrame Inputs;
    Inputs.Arguments = {{8, FrameValue::constant(0xc0000005)},
                        {12, FrameValue::frame(256)}};
    Inputs.Objects = {{256, 260}};
    Inputs.ObjectCells = {{256, FrameValue::constant(1)}};
    Inputs.ProveCall = [&](const LowOp &Op, const FrameState &State) {
      return Mutation != 4 && registration_abi::hasConditionalCallFrame(
                                  *Image, Op, State, Inputs.Work);
    };
    if (Mutation == 1)
      Inputs.Objects[0].End = 259;
    if (Mutation == 2)
      Inputs.Arguments[12] = FrameValue::frame(4);
    if (Mutation == 3)
      Inputs.ObjectCells[256] = FrameValue::frame(300);
    if (Mutation == 5)
      Inputs.Arguments.erase(8);
    if (Mutation == 6) {
      // Leave the decoded slice intact, but replace the exception-code store
      // with a different local. The subsequent -32 read is uninitialized.
      bool Changed = false;
      for (auto &Block : Current.Blocks)
        for (auto &Op : Block.Ops)
          if (Op.Addr == 0x4012a0 && Op.Opcode == NdOp::INT_ADD &&
              Op.NumInputs == 2 && Op.Inputs[1].isConst()) {
            Op.Inputs[1].Offset = uint32_t(-36);
            Changed = true;
          }
      ASSERT_TRUE(Changed);
    }
    RegistrationStateSolver Solver(Current, 0, 0, nullptr, nullptr, &*Stacks,
                                   &*Unwinds, &*Controls, &Inputs);
    const auto States = Solver.run();
    if (Mutation == 0) {
      for (const auto &Diagnostic : States.Diagnostics)
        ADD_FAILURE() << Diagnostic;
      EXPECT_TRUE(States.Complete);
      EXPECT_TRUE(States.CallbackStatesComplete);
      EXPECT_TRUE(States.RegistrationLifetimeComplete);
      EXPECT_TRUE(Inputs.Complete);
      EXPECT_EQ(Inputs.Reads,
                (std::set<std::pair<int32_t, int32_t>>{{256, 260}}));
      EXPECT_EQ(Inputs.Writes,
                (std::set<std::pair<int32_t, int32_t>>{{256, 260}}));
    } else {
      EXPECT_FALSE(States.Complete && States.CallbackStatesComplete &&
                   States.RegistrationLifetimeComplete && Inputs.Complete);
    }
  }
}

TEST(RegistrationConditionalFrame,
     CallerCounterExtentAndOriginalCodeAreRequired) {
  using namespace registration_state;
  for (unsigned Mutation = 0; Mutation != 12; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = loadXCPT4();
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    LowOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = 0x40215f;
    Call.Seq = 0;
    Call.NumInputs = 1;
    Call.Inputs[0] = NdVar::cst(0x401250, 4);
    FrameState Parent;
    Parent.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
        FrameValue::frame(-308);
    Parent.Cells[-308] = FrameValue::constant(0xc0000005);
    Parent.Cells[-304] = FrameValue::frame(-28);
    Parent.Cells[-28] = FrameValue::constant(1);
    size_t Work = 0;
    if (Mutation == 1)
      Parent.Cells.erase(-28);
    if (Mutation == 2)
      Parent.Cells[-304] = FrameValue::frame(-20);
    if (Mutation == 3)
      Parent.Cells[-28] = FrameValue::frame(-100);
    if (Mutation == 4)
      Parent.Cells[-304] = FrameValue::frame(-304);
    if (Mutation == 5)
      Parent.Registers[x86reg::RSP / x86reg::GeneralRegStride] = {};
    if (Mutation == 6)
      Call.Inputs[0].Size = 8;
    if (Mutation == 7)
      Work = limits::kMaxRegistrationEHStateWork;
    if (Mutation == 8) {
      Image->Segments[0].Flags =
          Image->Segments[0].Flags | SegmentFlags::Writable;
      for (auto &Section : Image->Sections)
        if (Section.isExecutable())
          Section.Flags = Section.Flags | SegmentFlags::Writable;
    }
    if (Mutation == 9 || Mutation == 10) {
      auto *Bytes = const_cast<uint8_t *>(Image->readVA(0x401312, 1));
      ASSERT_TRUE(Bytes);
      *Bytes = Mutation == 9 ? 0xcb : 0x90; // far RET / no exact near RET
    }
    if (Mutation == 11)
      ++Call.Addr;
    const auto Effect = proveBoundSEHCallee(*Image, Call, Parent, Work);
    if (Mutation == 0) {
      ASSERT_TRUE(Effect);
      EXPECT_EQ(Effect->StackPopBytes, 0u);
      ASSERT_EQ(Effect->Reads.size(), 1u);
      EXPECT_EQ(Effect->Reads[0].Begin, -28);
      EXPECT_EQ(Effect->Reads[0].End, -24);
      ASSERT_EQ(Effect->Writes.size(), 1u);
      EXPECT_EQ(Effect->Writes[0].Begin, -28);
      EXPECT_EQ(Effect->Writes[0].End, -24);
      EXPECT_GT(Work, 0u);
      // Repeat the same authenticated invocation with one work unit less
      // than its complete nested proof needs. No partial effect may escape.
      ASSERT_LT(Work, limits::kMaxRegistrationEHStateWork);
      size_t Limited = limits::kMaxRegistrationEHStateWork - Work + 1;
      EXPECT_FALSE(proveBoundSEHCallee(*Image, Call, Parent, Limited));
      EXPECT_EQ(Limited, limits::kMaxRegistrationEHStateWork);
    } else
      EXPECT_FALSE(Effect);
  }
}
#endif
