//===- RegistrationSEHJumpABITests.cpp - Checked jump arguments -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep incidental register and private-stack values out of jump-call ABIs.

#include "gtest/gtest.h"

#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/X86RegistrationSEHJump.h"
#include "neverd/lift/X86Regs.h"

namespace {
using namespace neverd;

MedVar reg(int Id, int Version, uint64_t Offset) {
  MedVar Value;
  Value.Kind = MedVar::Reg;
  Value.Id = Id;
  Value.SSAVer = Version;
  Value.RegOff = Offset;
  Value.Size = 4;
  Value.TheArch = Arch::X86;
  return Value;
}

MedOp operation(NdOp Opcode, MedVar Output,
                std::initializer_list<MedVar> Inputs) {
  MedOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  for (const auto &Input : Inputs)
    Op.addInput(Input);
  return Op;
}

struct JumpCallFixture {
  BinaryImage Image;
  MedFunc Function;
  MedVar Buffer = reg(2, 1, x86reg::RAX);
  MedVar ResumeBuffer = reg(2, 3, x86reg::RAX);

  JumpCallFixture() {
    Image.Arch = Arch::X86;
    Image.Format = BinaryFormat::COFF;
    Function.Entry = 0x1000;
    auto &EH = Function.ExceptionMetadata.emplace();
    EH.ParseStatus = ExceptionParseStatus::Complete;
    EH.Encoding = ExceptionEncoding::X86ScopeTableEH3;
    EH.Personality = ExceptionPersonality::ExceptHandler3;
    EH.CodeRange = {0x1000, 0x1025};
    EH.Registration.emplace();
    auto &States = Function.RegistrationStates.emplace();
    States.Complete = States.CallbackStatesComplete = true;
    States.RegistrationLifetimeComplete = true;
    States.HasEH3JumpCaptures = true;
    States.SEHJumpCaptures.push_back(
        {0x1010, 0x1015, 10, 0x2000, 0x3000, -96, -128, 0});
    RegistrationSEHJumpResume Resume;
    Resume.Address = 0x1020;
    Resume.OpSeq = 20;
    Resume.CaptureCall = 0x1010;
    Resume.TargetVA = 0x1015;
    Resume.EntryVA = 0;
    Resume.BufferOffset = -96;
    Resume.StackOffset = -128;
    Resume.ValueOffset = -132;
    Resume.ResumedLevels = {0};
    States.SEHJumpResumptions.push_back(Resume);
    Function.Blocks.resize(2);
    for (unsigned I = 0; I != 2; ++I) {
      auto &Block = Function.Blocks[I];
      Block.Id = I;
      Block.StartAddr = I ? 0x1015 : 0x1000;
      Block.EndAddr = I ? 0x1025 : 0x1015;
      if (!I)
        Block.Succs = {1};
      States.Blocks.push_back(
          {Block.Id, {Block.StartAddr, Block.EndAddr}, {0}});
      // All ABI register banks contain incidental values. The buffer remains
      // in EAX after its explicit stack store, just like the real LEA/PUSH.
      const auto CurrentBuffer = I ? ResumeBuffer : Buffer;
      Block.Ops.push_back(
          operation(NdOp::INT_SUB, CurrentBuffer,
                    {reg(3, 0, x86reg::RBP), MedVar::makeConst(96, 4)}));
      Block.Ops.push_back(operation(NdOp::COPY, reg(4, I + 1, x86reg::RCX),
                                    {MedVar::makeConst(88, 4)}));
      Block.Ops.push_back(operation(NdOp::COPY, reg(5, I + 1, x86reg::RDX),
                                    {MedVar::makeConst(99, 4)}));
      Block.Ops.push_back(operation(NdOp::COPY, reg(6, I + 1, x86reg::XMM0),
                                    {MedVar::makeConst(0x3f800000, 4)}));
      const auto SP = reg(1, I + 1, x86reg::RSP);
      Block.Ops.push_back(operation(
          NdOp::INT_SUB, SP,
          {reg(1, 0, x86reg::RSP), MedVar::makeConst(I ? 136 : 128, 4)}));
      const std::vector<MedVar> Arguments =
          I ? std::vector<MedVar>{CurrentBuffer, MedVar::makeConst(42, 4)}
            : std::vector<MedVar>{CurrentBuffer, MedVar::makeConst(2, 4),
                                  MedVar::makeConst(0x3000, 4),
                                  MedVar::makeConst(0, 4)};
      for (unsigned J = 0; J <= Arguments.size(); ++J) {
        // One extra private spill must not extend the protocol's argument run.
        MedVar Address = reg(10 + J, I + 1, 0);
        Address.Kind = MedVar::Temp;
        Block.Ops.push_back(operation(NdOp::INT_ADD, Address,
                                      {SP, MedVar::makeConst(J * 4, 4)}));
        Block.Ops.push_back(operation(
            NdOp::STORE, {},
            {Address, J == Arguments.size() ? CurrentBuffer : Arguments[J]}));
      }
      MedOp Call = operation(NdOp::CALL, reg(2, I ? 4 : 2, x86reg::RAX),
                             {MedVar::makeConst(I ? 0x2010 : 0x2000, 4)});
      Call.Addr = I ? 0x1020 : 0x1010;
      Call.OriginSeq = I ? 20 : 10;
      Call.DoesNotReturn = I != 0;
      Block.Ops.push_back(Call);
    }
  }

  void recover() {
    // Contradictory inferred prototypes cannot override the source protocol.
    std::map<va_t, int> RegisterArity{{0x2000, 1}, {0x2010, 1}};
    std::map<va_t, int> TotalArity = RegisterArity;
    std::map<va_t, int> FPArity = RegisterArity;
    recoverCallAbi(Function, Arch::X86,
                   {{0x2000, "unnamed_capture"}, {0x2010, "unnamed_resume"}},
                   &Image, &RegisterArity, &TotalArity, &FPArity);
  }
};

TEST(RegistrationSEHJumpABI, SourceProtocolOwnsStackOrderAndExactArity) {
  JumpCallFixture F;
  ASSERT_TRUE(getX86RegistrationSEHJumpCalls(F.Function));
  for (unsigned Pass = 0; Pass != 2; ++Pass) {
    SCOPED_TRACE(Pass);
    F.recover();
    ASSERT_EQ(F.Function.CallInfos.size(), 2u);
    const auto &Capture = F.Function.CallInfos[0];
    const auto &Resume = F.Function.CallInfos[1];
    ASSERT_EQ(Capture.Args.size(), 4u);
    ASSERT_EQ(Resume.Args.size(), 2u);
    EXPECT_EQ(Capture.Args[0], F.Buffer);
    EXPECT_EQ(Capture.Args[1].ConstVal, 2u);
    EXPECT_EQ(Capture.Args[2].ConstVal, 0x3000u);
    EXPECT_EQ(Capture.Args[3].ConstVal, 0u);
    EXPECT_EQ(Resume.Args[0], F.ResumeBuffer);
    EXPECT_EQ(Resume.Args[1].ConstVal, 42u);
    for (const auto &Call : F.Function.CallInfos) {
      EXPECT_EQ(Call.IntegerRegisterArgCount, 0);
      EXPECT_EQ(Call.FloatingRegisterArgCount, 0);
      EXPECT_EQ(Call.StackArgumentSlotBytes, 4u);
    }
    EXPECT_EQ(Capture.StackArgumentOffsets,
              (std::vector<int64_t>{0, 4, 8, 12}));
    EXPECT_EQ(Resume.StackArgumentOffsets, (std::vector<int64_t>{0, 4}));
  }
}

TEST(RegistrationSEHJumpABI, RuntimeArityDoesNotInventMissingStackArguments) {
  JumpCallFixture F;
  for (auto &Block : F.Function.Blocks)
    std::erase_if(Block.Ops, [](const auto &Op) {
      return Op.Opcode == NdOp::STORE && Op.NumInputs == 2 &&
             Op.Inputs[0].Kind == MedVar::Temp && Op.Inputs[0].Id == 11;
    });
  // The retained source receipt proves the ABI, not newly edited SSA values.
  // A gap in current stores must truncate recovery rather than synthesize zero.
  F.recover();
  ASSERT_EQ(F.Function.CallInfos.size(), 2u);
  for (const auto &Call : F.Function.CallInfos)
    EXPECT_EQ(Call.Args.size(), 1u);
}

TEST(RegistrationSEHJumpABI,
     IncompleteOrChangedReceiptsCannotSelectRuntimeABI) {
  for (unsigned Mutation = 0; Mutation != 15; ++Mutation) {
    SCOPED_TRACE(Mutation);
    JumpCallFixture F;
    auto &States = *F.Function.RegistrationStates;
    auto &Capture = States.SEHJumpCaptures.front();
    auto &Resume = States.SEHJumpResumptions.front();
    auto &CaptureOp = F.Function.Blocks[0].Ops.back();
    switch (Mutation) {
    case 0:
      States.Complete = false;
      break;
    case 1:
      States.CallbackStatesComplete = false;
      break;
    case 2:
      States.RegistrationLifetimeComplete = false;
      break;
    case 3:
      States.SEHJumpCapturesComplete = false;
      break;
    case 4:
      States.SEHJumpResumptionsComplete = false;
      break;
    case 5:
      Capture.OpSeq += 1;
      break;
    case 6:
      Capture.TargetVA += 1;
      break;
    case 7:
      Capture.EndAddress += 1;
      break;
    case 8:
      Resume.CaptureCall += 1;
      break;
    case 9:
      Resume.ValueOffset.reset();
      break;
    case 10:
      CaptureOp.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 11:
      CaptureOp.ExactArguments = true;
      break;
    case 12:
      CaptureOp.Dead = true;
      break;
    case 13:
      F.Function.Blocks[1].Ops.back().DoesNotReturn = false;
      break;
    case 14:
      States.Blocks[0].Unknown = true;
      break;
    }
    EXPECT_FALSE(getX86RegistrationSEHJumpCalls(F.Function));
  }
  JumpCallFixture F;
  F.Function.RegistrationStates->Complete = false;
  F.recover();
  ASSERT_EQ(F.Function.CallInfos.size(), 2u);
  // Merely retaining the runtime name/target must not select stack-only ABI.
  EXPECT_NE(F.Function.CallInfos[0].IntegerRegisterArgCount, 0);
}

TEST(RegistrationSEHJumpABI, MultipleStatesRequireACurrentSourceFrameCell) {
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    JumpCallFixture F;
    auto &Chain = *F.Function.ExceptionMetadata->Registration;
    Chain.SeededTryLevel = -1;
    Chain.Scopes = {{-1, 0x4000, 0x4100, false}};
    auto &Resume = F.Function.RegistrationStates->SEHJumpResumptions.front();
    Resume.ResumedLevels = {-1, 0};
    Resume.StateInFrame = true;
    switch (Mutation) {
    case 1:
      Resume.StateInFrame = false;
      break;
    case 2:
      Resume.ResumedLevels = {-2, 0};
      break;
    case 3:
      Resume.ResumedLevels = {-1, 1};
      break;
    case 4:
      Resume.ResumedLevels = {0, 0};
      break;
    case 5:
      Resume.ResumedLevels = {0, -1};
      break;
    case 6:
      Resume.ResumedLevels.clear();
      break;
    case 7:
      Chain.SeededTryLevel.reset();
      break;
    }
    const auto Calls = getX86RegistrationSEHJumpCalls(F.Function);
    if (Mutation) {
      EXPECT_FALSE(Calls);
      continue;
    }
    ASSERT_TRUE(Calls);
    ASSERT_EQ(Calls->size(), 2u);
    for (const auto &[Address, Call] : *Calls)
      EXPECT_FALSE(Call.ResumeLevel);
    F.recover();
    ASSERT_EQ(F.Function.CallInfos.size(), 2u);
    EXPECT_EQ(F.Function.CallInfos[0].Args.size(), 4u);
    EXPECT_EQ(F.Function.CallInfos[1].Args.size(), 2u);
  }
}

} // namespace
