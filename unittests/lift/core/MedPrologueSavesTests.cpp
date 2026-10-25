//===- MedPrologueSavesTests.cpp - Callee-save prologue storage ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Common.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedPrologueSaves.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <initializer_list>
#include <limits>
#include <utility>

namespace {

using namespace neverd;

enum : int { kSP = 1, kRBX = 2, kRCX = 3 };

MedVar reg(int Id, uint64_t RegOff, int Version) {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.TheArch = Arch::X64;
  V.Id = Id;
  V.SSAVer = Version;
  V.Size = 8;
  V.RegOff = RegOff;
  return V;
}
MedVar sp(int Version) { return reg(kSP, reg::RSP, Version); }
MedVar rbx(int Version) { return reg(kRBX, reg::RBX, Version); }
MedVar rcx(int Version) {
  return reg(kRCX, getTargetRegInfo(Arch::X64).Win64ParamRegs[0], Version);
}

MedVar temp(int Id) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.TheArch = Arch::X64;
  V.Id = Id;
  V.SSAVer = 1;
  V.Size = 8;
  return V;
}

MedVar none() {
  MedVar V;
  V.Id = -1;
  return V;
}

MedOp op(NdOp Opcode, MedVar Output, std::initializer_list<MedVar> Inputs) {
  MedOp Result;
  Result.Opcode = Opcode;
  Result.Output = Output;
  for (const MedVar &Input : Inputs)
    Result.addInput(Input);
  return Result;
}

/// `push rbx; mov rbx, rcx; …; pop rbx; ret` in MedIR: the prologue saves the
/// caller's rbx below the entry stack pointer, the epilogue restores it.
MedFunc pushedRbx(CallingConv Convention = CallingConv::SysV_AMD64) {
  MedFunc Func;
  Func.Entry = 0x1000;
  Func.CC = Convention;
  Func.FrameSize = 8;
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Func.Entry;
  Block.Ops = {op(NdOp::COPY, sp(0), {sp(0)}),
               op(NdOp::COPY, rbx(0), {rbx(0)}),
               op(NdOp::COPY, rcx(0), {rcx(0)}),
               op(NdOp::INT_SUB, sp(1), {sp(0), MedVar::makeConst(8, 8)}),
               op(NdOp::STORE, none(), {sp(1), rbx(0)}),
               op(NdOp::COPY, rbx(1), {rcx(0)}),
               op(NdOp::LOAD, temp(10), {sp(1)}),
               op(NdOp::COPY, rbx(2), {temp(10)}),
               op(NdOp::INT_ADD, sp(2), {sp(1), MedVar::makeConst(8, 8)}),
               op(NdOp::RETURN, none(), {rbx(1)})};
  Func.Blocks.push_back(std::move(Block));
  return Func;
}

class MedPrologueSaves : public ::testing::TestWithParam<bool> {
protected:
  void markStackAccesses(MedFunc &Func, Arch Architecture, BinaryFormat Format,
                         StackEntryKind Entry = StackEntryKind::Call) {
    if (GetParam()) {
      markSourceStackAccesses(Func, Architecture, Format, Entry);
    } else {
      markCalleeSavePrologueSaves(Func, Architecture, Format, Entry);
      markReturnAddressReads(Func, Architecture, Format, Entry);
    }
  }
};

INSTANTIATE_TEST_SUITE_P(SourceStackAccesses, MedPrologueSaves,
                         ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool> &Info) {
                           return Info.param ? "Combined" : "Separate";
                         });

const MedOp &save(const MedFunc &Func) { return Func.Blocks[0].Ops[4]; }

TEST_P(MedPrologueSaves, APushedEntryValueIsAPrologueSave) {
  MedFunc Func = pushedRbx();
  markStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
  EXPECT_TRUE(save(Func).PrologueSave);
  // Marking keeps every operation where call records find it.
  EXPECT_EQ(Func.Blocks[0].Ops.size(), 10u);
}

TEST(MedSourceStackAccesses, ChangedStoresLoseTheirOldPrologueProof) {
  MedFunc Func = pushedRbx();
  markSourceStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
  ASSERT_TRUE(save(Func).PrologueSave);
  Func.Blocks[0].Ops[4].Inputs[1] =
      MedVar::makeConst(0, Func.Blocks[0].Ops[4].Inputs[1].Size);
  markSourceStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
  EXPECT_FALSE(save(Func).PrologueSave);
}

TEST_P(MedPrologueSaves, SaveAndReturnAddressAnnotationsShareTheFrame) {
  for (StackEntryKind Entry :
       {StackEntryKind::Call, StackEntryKind::ProcessEntry}) {
    SCOPED_TRACE(static_cast<int>(Entry));
    MedFunc Func = pushedRbx();
    auto &Ops = Func.Blocks[0].Ops;
    Ops.insert(Ops.end() - 1, op(NdOp::LOAD, temp(20), {sp(2)}));
    markStackAccesses(Func, Arch::X64, BinaryFormat::ELF, Entry);
    EXPECT_TRUE(save(Func).PrologueSave);
    EXPECT_EQ(Ops[9].ReturnAddressRead, Entry == StackEntryKind::Call);
    EXPECT_FALSE(Ops[6].ReturnAddressRead);
    EXPECT_EQ(Ops.size(), 11u);
  }
}

TEST_P(MedPrologueSaves, ARestoreTheBodyReadsKeepsItsSave) {
  MedFunc Func = pushedRbx();
  Func.Blocks[0].Ops.back() = op(NdOp::RETURN, none(), {rbx(2)});
  markStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
  EXPECT_FALSE(save(Func).PrologueSave);
}

TEST_P(MedPrologueSaves, OnlyObservedRestoreJoinsKeepTheirSave) {
  for (bool Observed : {false, true}) {
    SCOPED_TRACE(Observed);
    MedFunc Func = pushedRbx();
    auto &Entry = Func.Blocks.front();
    Entry.Ops.pop_back();
    Entry.Succs = {1};
    MedBlock Join;
    Join.Id = 1;
    Join.Preds = {0};
    PhiNode Phi;
    Phi.Output = rbx(3);
    Phi.Args.push_back({0, rbx(2)});
    Join.Phis.push_back(Phi);
    Join.Ops = {op(NdOp::COPY, temp(20), {rbx(3)}),
                op(NdOp::RETURN, none(), {Observed ? temp(20) : rcx(0)})};
    Func.Blocks.push_back(std::move(Join));
    markStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
    EXPECT_EQ(Func.Blocks.front().Ops[4].PrologueSave, !Observed);
  }
}

TEST_P(MedPrologueSaves, AComputedValueIsNoSave) {
  MedFunc Func = pushedRbx();
  Func.Blocks[0].Ops[4] = op(NdOp::STORE, none(), {sp(1), rbx(1)});
  markStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
  EXPECT_FALSE(save(Func).PrologueSave);
}

TEST_P(MedPrologueSaves, AnOverwritePreventsOnlyTheCalleeSaveProof) {
  MedFunc Func = pushedRbx();
  auto &Ops = Func.Blocks[0].Ops;
  Ops.insert(Ops.begin() + 6, op(NdOp::STORE, none(), {sp(1), rcx(0)}));
  markStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
  // It no longer restores rbx's entry value. The combined source pass can
  // nevertheless discard that value: its bytes are overwritten before use.
  EXPECT_EQ(save(Func).PrologueSave, GetParam());
  EXPECT_EQ(Ops.size(), 11u);
}

TEST_P(MedPrologueSaves, AReadBeforeTheOverwriteKeepsTheSave) {
  MedFunc Func = pushedRbx();
  auto &Ops = Func.Blocks[0].Ops;
  Ops.insert(Ops.begin() + 6, op(NdOp::LOAD, temp(20), {sp(1)}));
  Ops.insert(Ops.begin() + 7, op(NdOp::STORE, none(), {sp(1), rcx(0)}));
  Ops.back() = op(NdOp::RETURN, none(), {temp(20)});
  markStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
  EXPECT_FALSE(save(Func).PrologueSave);
  EXPECT_EQ(Ops.size(), 12u);
}

TEST_P(MedPrologueSaves, OverwritesKillOnlyTheirOwnEntrySpillBytes) {
  for (uint16_t Bytes : {4, 8}) {
    SCOPED_TRACE(Bytes);
    MedFunc Func = pushedRbx();
    auto &Ops = Func.Blocks[0].Ops;
    MedVar Value = rcx(0);
    Value.Size = Bytes;
    Ops.insert(Ops.begin() + 6, op(NdOp::STORE, none(), {sp(1), Value}));
    markStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
    // Reusing a save slot defeats the register-save recognizer. Combined
    // source preparation can still prove the initial bytes unobservable
    // when a complete overwrite precedes every load; a partial one cannot.
    EXPECT_EQ(save(Func).PrologueSave, GetParam() && Bytes == 8);
    EXPECT_FALSE(Ops[6].PrologueSave);
  }
}

TEST(SourceStackSpills, OverwrittenValuesNeedNotComeFromTheEntry) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
      for (MedVar::VarKind Kind :
           {MedVar::Reg, MedVar::Temp, MedVar::Unspecified}) {
        SCOPED_TRACE(testing::Message() << unsigned(Architecture) << ':'
                                        << unsigned(Format) << ':' << Kind);
        const auto &TRI = getTargetRegInfo(Architecture);
        const auto Width = TRI.PointerSize;
        auto SP = reg(1, TRI.StackPointer, 0);
        SP.TheArch = Architecture;
        SP.Size = Width;
        auto Address = temp(10);
        Address.TheArch = Architecture;
        Address.Size = Width;
        auto Value = MedVar::makeUnspecified(Width, Architecture);
        if (Kind != MedVar::Unspecified) {
          Value = reg(2, TRI.IntReturnReg, 3);
          Value.Kind = Kind;
          Value.TheArch = Architecture;
          Value.Size = Width;
        }
        MedFunc Func;
        Func.Entry = 0x1000;
        Func.FrameSize = Width;
        MedBlock Block;
        Block.Id = 0;
        Block.StartAddr = Func.Entry;
        Block.Ops = {
            op(NdOp::COPY, SP, {SP}),
            op(NdOp::INT_SUB, Address, {SP, MedVar::makeConst(Width, Width)}),
            op(NdOp::STORE, none(), {Address, Value}),
            op(NdOp::STORE, none(), {Address, MedVar::makeConst(19, Width)}),
            op(NdOp::RETURN, none(), {MedVar::makeConst(19, Width)})};
        Func.Blocks.push_back(std::move(Block));
        markSourceStackAccesses(Func, Architecture, Format);
        EXPECT_TRUE(Func.Blocks[0].Ops[2].PrologueSave);
        EXPECT_EQ(Func.Blocks[0].Ops.size(), 5u);
      }
}

TEST(SourceStackSpills, FlagsTransfersDoNotObserveScratchButKeepTheirEffects) {
  for (Arch Architecture : {Arch::X86, Arch::X64})
    for (unsigned Mode = 0; Mode < 9; ++Mode) {
      SCOPED_TRACE(testing::Message() << unsigned(Architecture) << ':' << Mode);
      MedFunc Func = pushedRbx(CallingConv::Win64);
      auto &Ops = Func.Blocks[0].Ops;
      Ops.resize(5);
      for (auto &Op : Ops) {
        Op.Output.TheArch = Architecture;
        Op.Output.Size =
            Op.Output.Size ? getTargetRegInfo(Architecture).PointerSize : 0;
        for (unsigned I = 0; I < Op.NumInputs; ++I) {
          Op.Inputs[I].TheArch = Architecture;
          Op.Inputs[I].Size = getTargetRegInfo(Architecture).PointerSize;
        }
      }
      const auto Address = Ops[4].Inputs[0];
      const auto Width = getTargetRegInfo(Architecture).PointerSize;
      Ops[4].Inputs[1] = MedVar::makeUnspecified(Width, Architecture);
      auto Flags = temp(20);
      Flags.Size = Width;
      Flags.TheArch = Architecture;
      Ops.push_back(op(NdOp::INTRINSIC, Flags,
                       {MedVar::makeConst(unsigned(Intrinsic::Pushf), 2)}));
      Ops.push_back(
          op(NdOp::INTRINSIC, none(),
             {MedVar::makeConst(unsigned(Intrinsic::Popf), 2), Flags}));
      switch (Mode) {
      case 1: // An unrelated or unknown intrinsic remains a barrier.
        Ops[5].Inputs[0].ConstVal = 0xFFFF;
        break;
      case 2:
        Ops[5].Output.Size = 3;
        break;
      case 3:
        Ops[6].Inputs[1].Size = 3;
        break;
      case 4:
        Ops[6].NumInputs = 1;
        break;
      case 5:
        Ops[5].Inputs[0].Size = 8;
        break;
      case 6:
        Ops[5].IntrinsicOutputs.push_back(temp(30));
        break;
      case 7:
        Ops[6].MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
        break;
      case 8:
        Func.Blocks[0].ExceptionalSuccs.emplace_back(1, 0x2000);
        break;
      }
      Ops.push_back(
          op(NdOp::STORE, none(), {Address, MedVar::makeConst(19, Width)}));
      Ops.push_back(op(NdOp::RETURN, none(), {MedVar::makeConst(19, Width)}));
      markSourceStackAccesses(Func, Architecture, BinaryFormat::COFF);
      EXPECT_EQ(Ops[4].PrologueSave, Mode == 0);
      EXPECT_EQ(Ops[5].Opcode, NdOp::INTRINSIC);
      EXPECT_EQ(Ops[6].Opcode, NdOp::INTRINSIC);
      EXPECT_FALSE(Ops[5].Dead);
      EXPECT_FALSE(Ops[6].Dead);
      EXPECT_EQ(Ops.size(), 9u);
    }
}

TEST(SourceStackSpills, OnlyUnreadScratchRestoresCanBeIgnored) {
  for (unsigned Mode = 0; Mode < 16; ++Mode) {
    SCOPED_TRACE(Mode);
    MedFunc Func = pushedRbx(CallingConv::Win64);
    auto &Ops = Func.Blocks[0].Ops;
    Ops[4].Inputs[1] = MedVar::makeUnspecified(8, Arch::X64);
    const auto Loaded = temp(10);
    auto Restored = reg(30, x86reg::R10, 1);
    // R10 is volatile and cannot be an ordinary ABI result.
    if (Mode == 1)
      Restored = rbx(2);
    if (Mode == 2)
      Restored.RegOff = getTargetRegInfo(Arch::X64).IntReturnReg;
    if (Mode == 3)
      Restored = sp(2);
    Ops[7] = op(NdOp::COPY, Restored, {Loaded});
    Ops[8] = op(NdOp::STORE, none(), {sp(1), MedVar::makeConst(19, 8)});
    Ops[9] = op(NdOp::RETURN, none(), {MedVar::makeConst(19, 8)});
    if (Mode == 4)
      Ops[9].Inputs[0] = Restored;
    if (Mode == 5) {
      PhiNode Phi;
      Phi.Output = temp(40);
      Phi.Args = {{0, Restored}};
      Func.Blocks[0].Phis.push_back(std::move(Phi));
    }
    if (Mode == 6) {
      MedCallClobber Clobber;
      Clobber.Value = temp(40);
      Clobber.PreservedInput = Restored;
      Clobber.PreservedPrefixSize = 4;
      Func.CallClobbers.push_back(Clobber);
    }
    if (Mode == 7)
      Ops.insert(Ops.begin() + 8,
                 op(NdOp::CALL, none(), {MedVar::makeConst(0x2000, 8), sp(1)}));
    if (Mode >= 8 && Mode <= 10) {
      // The slot is dead, but ABI recovery could still observe the restored
      // register at a later call. Only a complete register overwrite kills it.
      Ops.insert(Ops.end() - 1,
                 op(NdOp::CALL, none(), {MedVar::makeConst(0x2000, 8)}));
      if (Mode >= 9) {
        auto Overwritten = Restored;
        ++Overwritten.SSAVer;
        if (Mode == 10)
          Overwritten.Size = 4;
        Ops.insert(Ops.end() - 2, op(NdOp::COPY, Overwritten,
                                     {MedVar::makeConst(0, Overwritten.Size)}));
      }
    }
    if (Mode == 11) {
      Ops[7].Output.RegOff = x86reg::DF;
      Ops[7].Output.Size = 1;
    }
    if (Mode == 12)
      Func.MultiReturn.push_back({Restored.RegOff, Restored.Size, false});
    if (Mode == 13)
      Ops[9] = op(NdOp::INDIR_BR, none(), {MedVar::makeConst(0x2000, 8)});
    if (Mode == 14) {
      // A straight continuation kills the scratch register before its call.
      Ops[9] = op(NdOp::BRANCH, none(), {MedVar::makeConst(0x2000, 8)});
      Func.Blocks[0].Succs = {1};
      auto Overwritten = Restored;
      ++Overwritten.SSAVer;
      MedBlock Next;
      Next.Id = 1;
      Next.StartAddr = 0x2000;
      Next.Preds = {0};
      Next.Ops = {op(NdOp::COPY, Overwritten, {MedVar::makeConst(0, 8)}),
                  op(NdOp::CALL, none(), {MedVar::makeConst(0x3000, 8)}),
                  op(NdOp::RETURN, none(), {MedVar::makeConst(19, 8)})};
      Func.Blocks.push_back(std::move(Next));
    }
    if (Mode == 15)
      Ops[7].Output.RegOff = x86reg::RSI;
    markSourceStackAccesses(
        Func, Arch::X64, Mode == 15 ? BinaryFormat::ELF : BinaryFormat::COFF);
    EXPECT_EQ(Func.Blocks[0].Ops[4].PrologueSave,
              Mode == 0 || Mode == 9 || Mode == 14);
  }
}

TEST_P(MedPrologueSaves, AnArgumentSpillIsNoSave) {
  MedFunc Func = pushedRbx();
  Func.Blocks[0].Ops[4] = op(NdOp::STORE, none(), {sp(1), rcx(0)});
  markStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
  EXPECT_FALSE(save(Func).PrologueSave);
}

TEST_P(MedPrologueSaves, OutOfFrameExtentsAreNotPrivateStorage) {
  for (BinaryFormat Format : {BinaryFormat::COFF, BinaryFormat::ELF})
    for (int64_t Offset :
         {std::numeric_limits<int64_t>::max(),
          std::numeric_limits<int64_t>::max() - 7,
          std::numeric_limits<int64_t>::max() - 8,
          std::numeric_limits<int64_t>::min(), int64_t{-9}, int64_t{0}}) {
      SCOPED_TRACE(testing::Message() << unsigned(Format) << ':' << Offset);
      MedFunc Func =
          pushedRbx(Format == BinaryFormat::COFF ? CallingConv::Win64
                                                 : CallingConv::SysV_AMD64);
      Func.Blocks[0].Ops[3] =
          op(NdOp::INT_ADD, sp(1),
             {sp(0), MedVar::makeConst(uint64_t(Offset), 8)});
      markStackAccesses(Func, Arch::X64, Format);
      EXPECT_FALSE(save(Func).PrologueSave);
    }
}

/// `mov [rsp+8], rbx; …; mov rbx, [rsp+8]; ret`: MSVC saves rbx in the home
/// slot Win64 hands the callee.
MedFunc homeSlotRbx(CallingConv Convention) {
  MedFunc Func;
  Func.Entry = 0x1000;
  Func.CC = Convention;
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Func.Entry;
  Block.Ops = {op(NdOp::COPY, sp(0), {sp(0)}),
               op(NdOp::COPY, rbx(0), {rbx(0)}),
               op(NdOp::COPY, rcx(0), {rcx(0)}),
               op(NdOp::INT_ADD, temp(11), {sp(0), MedVar::makeConst(8, 8)}),
               op(NdOp::STORE, none(), {temp(11), rbx(0)}),
               op(NdOp::COPY, rbx(1), {rcx(0)}),
               op(NdOp::INT_ADD, temp(12), {sp(0), MedVar::makeConst(8, 8)}),
               op(NdOp::LOAD, temp(13), {temp(12)}),
               op(NdOp::COPY, rbx(2), {temp(13)}),
               op(NdOp::RETURN, none(), {rbx(1)})};
  Func.Blocks.push_back(std::move(Block));
  return Func;
}

TEST_P(MedPrologueSaves, AWin64HomeSlotSaveIsPrologueStorage) {
  MedFunc Func = homeSlotRbx(CallingConv::Win64);
  markStackAccesses(Func, Arch::X64, BinaryFormat::COFF);
  EXPECT_TRUE(save(Func).PrologueSave);
}

TEST_P(MedPrologueSaves, OnlyWin64HandsTheCalleeHomeSlots) {
  // Above a System V callee's return address lies its caller's frame.
  MedFunc Func = homeSlotRbx(CallingConv::SysV_AMD64);
  markStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
  EXPECT_FALSE(save(Func).PrologueSave);
}

/// `mov rax, [rsp]; ret`: the function reads the return address its call
/// pushed.
MedFunc readsReturnAddress() {
  MedFunc Func;
  Func.Entry = 0x1000;
  Func.CC = CallingConv::SysV_AMD64;
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Func.Entry;
  Block.Ops = {op(NdOp::COPY, sp(0), {sp(0)}),
               op(NdOp::LOAD, temp(20), {sp(0)}),
               op(NdOp::RETURN, none(), {temp(20)})};
  Func.Blocks.push_back(std::move(Block));
  return Func;
}

const MedOp &read(const MedFunc &Func) { return Func.Blocks[0].Ops[1]; }

TEST_P(MedPrologueSaves, ALoadOfTheEntrySlotIsTheReturnAddress) {
  MedFunc Func = readsReturnAddress();
  markStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
  EXPECT_TRUE(read(Func).ReturnAddressRead);
}

TEST_P(MedPrologueSaves, AFunctionThatWritesTheSlotReadsItsOwnValue) {
  MedFunc Func = readsReturnAddress();
  auto &Ops = Func.Blocks[0].Ops;
  Ops.insert(Ops.begin() + 1, op(NdOp::STORE, none(), {sp(0), rcx(0)}));
  markStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
  EXPECT_FALSE(Func.Blocks[0].Ops[2].ReturnAddressRead);
}

TEST_P(MedPrologueSaves, ANarrowLoadOfTheEntrySlotIsNoReturnAddress) {
  MedFunc Func = readsReturnAddress();
  MedVar Narrow = temp(20);
  Narrow.Size = 4;
  Func.Blocks[0].Ops[1].Output = Narrow;
  markStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
  EXPECT_FALSE(read(Func).ReturnAddressRead);
}

TEST_P(MedPrologueSaves, AProcessEntryHasNoReturnAddress) {
  // The kernel starts `_start` with argc at the stack pointer.
  MedFunc Func = readsReturnAddress();
  markStackAccesses(Func, Arch::X64, BinaryFormat::ELF,
                    StackEntryKind::ProcessEntry);
  EXPECT_FALSE(read(Func).ReturnAddressRead);
}

TEST_P(MedPrologueSaves, AArch64KeepsItsReturnAddressInTheLinkRegister) {
  MedFunc Func = readsReturnAddress();
  markStackAccesses(Func, Arch::AArch64, BinaryFormat::ELF);
  EXPECT_FALSE(read(Func).ReturnAddressRead);
}

void consumeFrameRead(MedBlock &Block) {
  MedOp Call = op(NdOp::CALL, temp(20), {MedVar::makeConst(0x2000, 8)});
  auto Signature = std::make_shared<FixedIntegerCallArguments>();
  const auto Registers =
      getTargetRegInfo(Arch::X64).integerArgumentLayout(true);
  for (auto Register : Registers.Registers) {
    Signature->Parameters.push_back(
        {SourceABICarrierKind::IntegerRegister, Register, 0, 8});
    Call.addInput(MedVar::makeConst(0, 8));
  }
  Signature->Parameters.push_back({SourceABICarrierKind::Stack, 0, 40, 8});
  Call.addInput(temp(10));
  Call.IntegerCallSignature = std::move(Signature);
  Block.Ops.insert(Block.Ops.end() - 1, std::move(Call));
}

TEST(SourceFrameReads, MissingAndPartialPrivateBytesRemainUnavailable) {
  for (unsigned WrittenBytes : {0, 4, 8}) {
    SCOPED_TRACE(WrittenBytes);
    auto Func = pushedRbx();
    auto &Store = Func.Blocks[0].Ops[4];
    if (WrittenBytes)
      Store.Inputs[1].Size = WrittenBytes;
    else
      Store = op(NdOp::NOP, none(), {});
    consumeFrameRead(Func.Blocks.back());
    markSourceStackAccesses(Func, Arch::X64, BinaryFormat::COFF);
    const auto &Read = Func.Blocks[0].Ops[6];
    if (WrittenBytes == 8)
      EXPECT_EQ(Read.Opcode, NdOp::LOAD);
    else {
      ASSERT_EQ(Read.Opcode, NdOp::COPY);
      ASSERT_EQ(Read.NumInputs, 1);
      EXPECT_EQ(Read.Inputs[0].Kind, MedVar::Unspecified);
      EXPECT_EQ(Read.Inputs[0].Size, 8);
    }
  }
}

TEST(SourceFrameReads, PossibleWritersPreventAnUnavailableClaim) {
  for (auto Barrier :
       {NdOp::CALL, NdOp::INDIR_CALL, NdOp::INTRINSIC, NdOp::ATOMIC_XCHG,
        NdOp::ATOMIC_ADD, NdOp::ATOMIC_CMPXCHG, NdOp::STORE}) {
    SCOPED_TRACE(ndOpName(Barrier));
    auto Func = pushedRbx();
    Func.Blocks[0].Ops[4] =
        op(Barrier, none(), {rcx(0), MedVar::makeConst(0, 8)});
    consumeFrameRead(Func.Blocks.back());
    markSourceStackAccesses(Func, Arch::X64, BinaryFormat::COFF);
    EXPECT_EQ(Func.Blocks[0].Ops[6].Opcode, NdOp::LOAD);
  }
}

TEST(SourceFrameReads, AllPredecessorWritesAndQueryBackedgesAreConsidered) {
  for (unsigned Shape : {0, 1, 2, 3}) {
    SCOPED_TRACE(Shape);
    auto Func = pushedRbx();
    auto &Entry = Func.Blocks[0];
    MedBlock Query;
    Query.Id = 3;
    Query.Ops.assign(Entry.Ops.begin() + 6, Entry.Ops.end());
    Query.Preds = {1, 2};
    Entry.Ops.resize(4);
    Entry.Succs = {1, 2};
    MedBlock Left, Right;
    Left.Id = 1;
    Right.Id = 2;
    Left.Preds = Right.Preds = {0};
    Left.Succs = Right.Succs = {3};
    if (Shape == 1)
      Left.Ops.push_back(op(NdOp::STORE, none(), {sp(1), rbx(0)}));
    if (Shape == 2) {
      Query.Preds.push_back(3);
      Query.Succs = {3};
    }
    if (Shape == 3) // A stale/incomplete edge cannot establish an entry path.
      Query.Preds.clear();
    Func.Blocks.push_back(std::move(Left));
    Func.Blocks.push_back(std::move(Right));
    Func.Blocks.push_back(std::move(Query));
    consumeFrameRead(Func.Blocks.back());
    markSourceStackAccesses(Func, Arch::X64, BinaryFormat::COFF);
    EXPECT_EQ(Func.Blocks.back().Ops[0].Opcode,
              Shape ? NdOp::LOAD : NdOp::COPY);
  }
}

TEST(SourceFrameReads, GeneralPartialLoadsKeepTheirInitializedBits) {
  auto Func = pushedRbx();
  Func.Blocks[0].Ops[4].Inputs[1].Size = 4;
  // Only the low written word is returned. A source-only proof must not
  // replace the whole load with unavailable state merely for its upper word.
  auto LowWord =
      op(NdOp::SUBBYTES, temp(30), {temp(10), MedVar::makeConst(0, 4)});
  LowWord.Output.Size = 4;
  Func.Blocks[0].Ops.back().Inputs[0] = LowWord.Output;
  Func.Blocks[0].Ops.insert(Func.Blocks[0].Ops.end() - 1, LowWord);
  markSourceStackAccesses(Func, Arch::X64, BinaryFormat::COFF);
  EXPECT_EQ(Func.Blocks[0].Ops[6].Opcode, NdOp::LOAD);
}

TEST(SourceStackSpills, PhysicalCallsOverwriteOnlyTheirReturnAddressBytes) {
  for (Arch A : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
      for (unsigned Mode = 0; Mode < 12; ++Mode) {
        SCOPED_TRACE(testing::Message()
                     << unsigned(A) << ':' << unsigned(Format) << ':' << Mode);
        const auto &TRI = getTargetRegInfo(A);
        const auto Width = TRI.PointerSize;
        auto SP0 = reg(1, TRI.StackPointer, 0);
        SP0.TheArch = A;
        SP0.Size = Width;
        auto SP1 = SP0;
        SP1.SSAVer = 1;
        auto Address = temp(10);
        Address.TheArch = A;
        Address.Size = Width;
        auto Read = Address;
        Read.Id = 11;
        MedFunc Func;
        Func.Entry = 0x1000;
        Func.FrameSize = 64;
        MedBlock Block;
        Block.Id = 0;
        Block.StartAddr = Func.Entry;
        Block.Ops = {
            op(NdOp::COPY, SP0, {SP0}),
            op(NdOp::INT_SUB, SP1, {SP0, MedVar::makeConst(32, Width)}),
            op(NdOp::INT_SUB, Address,
               {SP1, MedVar::makeConst(Width + (Mode == 3), Width)}),
            op(NdOp::STORE, none(),
               {Address, MedVar::makeUnspecified(Width, A)}),
            op(Mode == 8 ? NdOp::INDIR_CALL : NdOp::CALL, none(),
               {MedVar::makeConst(0x2000, Width)}),
            op(NdOp::RETURN, none(), {MedVar::makeConst(19, Width)})};
        Block.Ops[4].CallReturnAddressBytes = Mode == 1   ? 0
                                              : Mode == 2 ? Width / 2
                                                          : Width;
        if (Mode == 4) {
          Block.Ops[4].addInput(Read);
          Block.Ops.insert(Block.Ops.begin() + 4,
                           op(NdOp::LOAD, Read, {Address}));
        }
        if (Mode == 5)
          Block.Ops.insert(
              Block.Ops.begin() + 4,
              op(NdOp::LOAD, Read, {MedVar::makeConst(0x8000, Width)}));
        if (Mode == 6) {
          auto Narrow = SP1;
          ++Narrow.SSAVer;
          Narrow.Size = 2;
          Block.Ops.insert(Block.Ops.begin() + 4,
                           op(NdOp::COPY, Narrow, {MedVar::makeConst(0, 2)}));
        }
        if (Mode == 7)
          Block.Ops[0] = op(NdOp::COPY, SP0, {Read});
        if (Mode == 9)
          Block.Ops[3].MemoryOrdering =
              NdMemoryOrdering::SequentiallyConsistent;
        if (Mode == 10)
          Block.ExceptionalSuccs.emplace_back(1, 0x3000);
        if (Mode == 11)
          // Read-only offset queries also admit an otherwise undefined SP.0
          // as the entry value; a conflicting definition above does not.
          Block.Ops[0] = op(NdOp::NOP, none(), {});
        Func.Blocks.push_back(std::move(Block));
        const auto Count = Func.Blocks[0].Ops.size();
        markSourceStackAccesses(Func, A, Format);
        EXPECT_EQ(Func.Blocks[0].Ops[3].PrologueSave,
                  (A == Arch::X86 || A == Arch::X64) &&
                      (Mode == 0 || Mode == 8 || Mode == 11));
        EXPECT_EQ(Func.Blocks[0].Ops.size(), Count);
      }
}

TEST(SourceStackSpills, ReturnAddressOverwriteRequiresACompleteReachingStack) {
  for (unsigned Mode = 0; Mode < 6; ++Mode) {
    SCOPED_TRACE(Mode);
    MedFunc Func = pushedRbx();
    Func.FrameSize = 32;
    auto &Entry = Func.Blocks[0];
    Entry.Ops.resize(5);
    Entry.Ops[4].Inputs[1] = MedVar::makeUnspecified(8, Arch::X64);
    Entry.Ops.emplace_back(
        op(NdOp::INT_ADD, sp(2), {sp(1), MedVar::makeConst(8, 8)}));
    Entry.Ops.emplace_back(
        op(NdOp::BRANCH, none(), {MedVar::makeConst(0x2000, 8)}));
    Entry.Succs = {1};
    MedBlock Next;
    Next.Id = 1;
    Next.StartAddr = 0x2000;
    Next.Preds = {0};
    Next.Ops = {op(NdOp::CALL, none(), {MedVar::makeConst(0x3000, 8)}),
                op(NdOp::RETURN, none(), {MedVar::makeConst(19, 8)})};
    Next.Ops[0].CallReturnAddressBytes = 8;
    if (Mode == 1)
      Next.Preds.clear();
    if (Mode == 2)
      Entry.Succs.clear();
    if (Mode == 3)
      Next.Preds.push_back(99);
    if (Mode == 4)
      Next.Id = 0;
    if (Mode == 5) {
      PhiNode Phi;
      Phi.Output = sp(3);
      Phi.Args = {{0, sp(2)}};
      Next.Phis.push_back(Phi);
    }
    Func.Blocks.push_back(std::move(Next));
    markSourceStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
    EXPECT_EQ(save(Func).PrologueSave, Mode == 0 || Mode == 5);
  }
}

TEST(SourceStackSpills, APhysicalCallCanReplaceTheIncomingReturnAddress) {
  MedFunc Func = pushedRbx();
  auto &Ops = Func.Blocks[0].Ops;
  Ops.resize(3);
  Ops.push_back(op(NdOp::INT_ADD, sp(1), {sp(0), MedVar::makeConst(8, 8)}));
  Ops.push_back(op(NdOp::CALL, none(), {MedVar::makeConst(0x2000, 8)}));
  Ops.back().CallReturnAddressBytes = 8;
  Ops.push_back(op(NdOp::LOAD, temp(10), {sp(0)}));
  Ops.push_back(op(NdOp::RETURN, none(), {temp(10)}));
  markSourceStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
  EXPECT_FALSE(Ops[5].ReturnAddressRead);
  Ops[4].CallReturnAddressBytes = 0;
  markSourceStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
  EXPECT_TRUE(Ops[5].ReturnAddressRead);
}

TEST(SourceStackSpills, OnlyCheckedNearCallOccurrencesPublishStackWrites) {
  for (Arch A : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (unsigned Mode = 0; Mode < 10; ++Mode) {
      SCOPED_TRACE(testing::Message() << unsigned(A) << ':' << Mode);
      const auto &TRI = getTargetRegInfo(A);
      BinaryImage Image;
      Image.Arch = A;
      Image.Format = BinaryFormat::ELF;
      Segment Text;
      Text.VA = 0x1000;
      Text.Data = {0xe8, 0xfb, 0x0f, 0x00, 0x00};
      if (Mode == 1)
        Text.Data = {0xff, 0xd0};
      if (Mode == 2)
        Text.Data[0] = 0xe9;
      if (Mode == 5)
        Text.Data = {0x66, 0xe8, 0x00, 0x00};
      if (Mode == 6)
        Text.Data = {0xff, 0x18};
      if (Mode == 7)
        Text.Data = {0x48, 0xff, 0xd0};
      Text.Size = Text.Data.size();
      Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
      Image.Segments.push_back(Text);
      ASSERT_NE(Image.readVA(Text.VA, Text.Size), nullptr);
      LowFunc Low;
      Low.Entry = Text.VA;
      LowBlock Block;
      Block.Id = 0;
      Block.StartAddr = Text.VA;
      Block.EndAddr = Text.VA + Text.Size;
      LowOp Call;
      Call.Opcode =
          Mode == 1 || Mode == 6 || Mode == 7 ? NdOp::INDIR_CALL : NdOp::CALL;
      Call.Addr = Text.VA;
      Call.Output = NdVar::reg(TRI.IntReturnReg, TRI.PointerSize);
      Call.addInput(Call.Opcode == NdOp::CALL
                        ? NdVar::cst(0x2000, TRI.PointerSize)
                        : NdVar::reg(TRI.IntReturnReg, TRI.PointerSize));
      Block.Ops.push_back(Call);
      LowInstructionBoundary Boundary;
      Boundary.Address = Text.VA;
      Boundary.Size = Text.Size;
      Boundary.OpCount = 1;
      Boundary.Control = LowInstructionControl::Call;
      Boundary.ControlFlags = LowInstructionControlFlag::Call;
      if (Call.Opcode == NdOp::INDIR_CALL)
        Boundary.ControlFlags |= LowInstructionControlFlag::Indirect;
      else
        Boundary.Immediate = 0x2000;
      if (Mode == 2) {
        Boundary.Control = LowInstructionControl::TailCall;
        Boundary.ControlFlags |= LowInstructionControlFlag::Return;
      }
      if (Mode == 3)
        ++Boundary.Address;
      if (Mode == 8) {
        Boundary.Control = LowInstructionControl::ConditionalCall;
        Boundary.ControlFlags |= LowInstructionControlFlag::Branch |
                                 LowInstructionControlFlag::Conditional;
      }
      if (Mode != 4)
        Block.InstructionBoundaries.push_back(Boundary);
      Low.Blocks.push_back(std::move(Block));
      LowToMedConverter Converter;
      if (Mode != 9)
        Converter.setBinaryImage(&Image);
      const auto Med = Converter.convert(Low, A);
      unsigned Calls = 0;
      for (const auto &B : Med.Blocks)
        for (const auto &Op : B.Ops)
          if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
            ++Calls;
            const bool Expected =
                ((A == Arch::X86 || A == Arch::X64) && Mode <= 1) ||
                (A == Arch::X64 && Mode == 7);
            EXPECT_EQ(Op.CallReturnAddressBytes,
                      Expected ? TRI.PointerSize : 0);
          }
      EXPECT_EQ(Calls, 1u);
    }
}

} // namespace
