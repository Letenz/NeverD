//===- RegistrationStateTestUtils.cpp - x86 EH state fixtures -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateTestUtils.h"

#include "neverd/lift/X86Regs.h"

namespace neverd::registration_test {

void emitOp(LowBlock &Block, va_t Address, NdOp Opcode, NdVar Output,
            std::initializer_list<NdVar> Inputs, NdMemoryAddressSpace Space) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Seq = Block.Ops.size();
  Op.Output = Output;
  Op.Addr = Address;
  Op.MemoryAddressSpace = Space;
  for (NdVar Input : Inputs)
    Op.addInput(Input);
  Block.Ops.push_back(Op);
}

void addSlotStore(LowBlock &Block, int32_t Value, uint16_t Width) {
  LowOp Address;
  Address.Opcode = NdOp::INT_ADD;
  Address.Output = NdVar::tmp(0, 4);
  Address.addInput(NdVar::reg(x86reg::RBP, 4));
  Address.addInput(NdVar::cst(uint32_t(-4), 4));
  Address.Addr = Block.StartAddr;
  Block.Ops.push_back(Address);
  LowOp Store;
  Store.Opcode = NdOp::STORE;
  Store.addInput(Address.Output);
  Store.addInput(NdVar::cst(uint32_t(Value), Width));
  Store.Addr = Block.StartAddr;
  Block.Ops.push_back(Store);
}

LowFunc makeBranchingFrame() {
  LowFunc F;
  F.Entry = 0x1000;
  F.ExceptionMetadata.emplace();
  F.ExceptionMetadata->CodeRange = {0x1000, 0x2000};
  F.ExceptionMetadata->Personality = ExceptionPersonality::ExceptHandler3;
  F.ExceptionMetadata->Encoding = ExceptionEncoding::X86ScopeTableEH3;
  RegistrationChainInfo &Chain = F.ExceptionMetadata->Registration.emplace();
  Chain.SeededTryLevel = -1;
  Chain.TryLevelOffset = -4;
  Chain.RegistrationOffset = -16;
  Chain.ChainInstallVA = 0x1000;
  Chain.Scopes.push_back({-1, 0x1800, 0x1900, false});
  Chain.TryLevelStores = {{0x1010, 0x1017, 0}, {0x1020, 0x1027, -1}};
  F.Blocks.resize(4);
  for (int I = 0; I < 4; ++I) {
    LowBlock &B = F.Blocks[I];
    B.Id = I;
    B.StartAddr = 0x1000 + I * 0x10;
    B.EndAddr = B.StartAddr + 7;
    B.InstructionBoundaries.push_back({B.StartAddr, 7});
  }
  F.Blocks[0].Succs = {1, 2};
  F.Blocks[1].Succs = {3};
  F.Blocks[2].Succs = {3};
  auto Emit = [&](NdOp Opcode, NdVar Output,
                  std::initializer_list<NdVar> Inputs,
                  NdMemoryAddressSpace Space = NdMemoryAddressSpace::Default) {
    LowOp Op;
    Op.Opcode = Opcode;
    Op.Seq = F.Blocks[0].Ops.size();
    Op.Output = Output;
    Op.Addr = 0x1000;
    Op.MemoryAddressSpace = Space;
    for (NdVar Input : Inputs)
      Op.addInput(Input);
    F.Blocks[0].Ops.push_back(Op);
  };
  Emit(NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
       {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
  Emit(NdOp::COPY, NdVar::reg(x86reg::RBP, 4), {NdVar::reg(x86reg::RSP, 4)});
  Emit(NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
       {NdVar::reg(x86reg::RSP, 4), NdVar::cst(16, 4)});
  Emit(NdOp::LOAD, NdVar::tmp(8, 4), {NdVar::cst(0, 8)},
       NdMemoryAddressSpace::X86FS);
  Emit(NdOp::STORE, {}, {NdVar::reg(x86reg::RSP, 4), NdVar::tmp(8, 4)});
  LowOp Install;
  Install.Opcode = NdOp::STORE;
  Install.Seq = F.Blocks[0].Ops.size();
  Install.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  Install.addInput(NdVar::cst(0, 8));
  Install.addInput(NdVar::reg(x86reg::RSP, 4));
  Install.Addr = 0x1000;
  F.Blocks[0].Ops.push_back(Install);
  addSlotStore(F.Blocks[1], 0);
  addSlotStore(F.Blocks[2], -1);
  return F;
}

LowFunc makeCxxCatchContinuation(bool IncludeResume) {
  auto F = makeBranchingFrame();
  auto &EH = *F.ExceptionMetadata;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  EH.Encoding = ExceptionEncoding::X86CxxFuncInfo;
  auto &Chain = *EH.Registration;
  Chain.RegistrationOffset = -12;
  Chain.Scopes.clear();
  Chain.TryLevelStores = {
      {0x1016, 0x101d, 0}, {0x1020, 0x1027, -1}, {0x1900, 0x1907, -1}};
  auto &Cxx = EH.Cxx.emplace();
  Cxx.MaxState = 2;
  Cxx.UnwindMap = {{-1, 0, CxxUnwindAction::ActionKind::None},
                   {-1, 0, CxxUnwindAction::ActionKind::None}};
  CxxTryBlock Try;
  Try.TryLow = Try.TryHigh = 0;
  Try.CatchHigh = 1;
  CxxCatchHandler Catch;
  Catch.HandlerVA = 0x1800;
  Try.Handlers.push_back(Catch);
  Cxx.TryBlocks.push_back(Try);

  F.Blocks.resize(IncludeResume ? 7 : 6);
  F.Blocks[0].Ops[2].Inputs[1] = NdVar::cst(12, 4);
  F.Blocks[0].Succs = {1};
  for (size_t I = 1; I < F.Blocks.size(); ++I) {
    F.Blocks[I] = LowBlock{};
    F.Blocks[I].Id = I;
    F.Blocks[I].StartAddr = 0x1000 + I * 0x10;
    F.Blocks[I].EndAddr = F.Blocks[I].StartAddr + 7;
  }
  auto &Body = F.Blocks[1];
  Body.EndAddr = 0x101d;
  Body.InstructionBoundaries = {{0x1010, 3}, {0x1013, 3}, {0x1016, 7}};
  Body.Succs = {2};
  emitOp(Body, 0x1010, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
         {NdVar::reg(x86reg::RSP, 4), NdVar::cst(16, 4)});
  emitOp(Body, 0x1013, NdOp::INT_ADD, NdVar::tmp(40, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-16), 4)});
  emitOp(Body, 0x1013, NdOp::STORE, {},
         {NdVar::tmp(40, 4), NdVar::reg(x86reg::RSP, 4)});
  LowBlock StateStore;
  StateStore.StartAddr = 0x1016;
  addSlotStore(StateStore, 0);
  Body.Ops.insert(Body.Ops.end(), StateStore.Ops.begin(), StateStore.Ops.end());

  F.Blocks[2].InstructionBoundaries = {{0x1020, 7}};
  F.Blocks[2].Succs = {3};
  addSlotStore(F.Blocks[2], -1);
  auto &Unlink = F.Blocks[3];
  Unlink.InstructionBoundaries = {{0x1030, 7}};
  Unlink.Succs = {4};
  emitOp(Unlink, 0x1030, NdOp::INT_ADD, NdVar::tmp(50, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-12), 4)});
  emitOp(Unlink, 0x1030, NdOp::LOAD, NdVar::tmp(51, 4), {NdVar::tmp(50, 4)});
  emitOp(Unlink, 0x1030, NdOp::STORE, {}, {NdVar::cst(0, 4), NdVar::tmp(51, 4)},
         NdMemoryAddressSpace::X86FS);
  auto AddReturn = [&](LowBlock &Block, va_t Address) {
    LowInstructionBoundary Return;
    Return.Address = Address;
    Return.Size = 1;
    Return.Control = LowInstructionControl::Return;
    Block.InstructionBoundaries.push_back(Return);
    Block.EndAddr = Address + 1;
    emitOp(Block, Address, NdOp::RETURN, {}, {NdVar::reg(x86reg::RAX, 4)});
  };
  AddReturn(F.Blocks[4], 0x1040);
  auto &Handler = F.Blocks[5];
  Handler.StartAddr = 0x1800;
  Handler.InstructionBoundaries = {{0x1800, 5}};
  emitOp(Handler, 0x1800, NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
         {NdVar::cst(0x1900, 4)});
  AddReturn(Handler, 0x1805);
  if (IncludeResume) {
    auto &Resume = F.Blocks[6];
    Resume.StartAddr = 0x1900;
    Resume.EndAddr = 0x1907;
    Resume.InstructionBoundaries = {{0x1900, 7}};
    Resume.Succs = {3};
    addSlotStore(Resume, -1);
  }
  return F;
}

} // namespace neverd::registration_test
