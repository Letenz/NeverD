//===- RegistrationSEHScalarSpillTests.cpp - Runtime scalar spills -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/llvm/X86/MedLLVMRegistrationSEHProof.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"

using namespace neverd;
using namespace neverd::x86_registration;

namespace {

MedVar value(int Id) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = Id;
  V.SSAVer = 1;
  V.Size = 4;
  return V;
}

SEHFrameValues::Key key(const MedVar &V) { return {V.Kind, V.Id, V.SSAVer}; }

MedFunc scalarSpill() {
  MedFunc F;
  F.ExceptionMetadata.emplace();
  F.ExceptionMetadata->Registration.emplace();
  F.ExceptionMetadata->Registration->Scopes.emplace_back();
  F.ExceptionMetadata->Registration->Scopes.back().FilterVA = 0x1000;
  F.RegistrationStates.emplace();
  auto &States = *F.RegistrationStates;
  States.Complete = States.CallbackStatesComplete =
      States.RegistrationLifetimeComplete = true;
  States.SEHExceptionAccesses.push_back(
      {RegistrationSEHExceptionAccess::Kind::ExceptionCode, 0x1000, 0, 0x1000});
  F.Blocks.emplace_back();
  F.Blocks.back().Id = 1;
  auto &Ops = F.Blocks.back().Ops;
  MedOp Code;
  Code.Opcode = NdOp::LOAD;
  Code.Output = value(1);
  Code.addInput(MedVar::makeConst(0x6000, 4));
  Code.Addr = 0x1000;
  Code.OriginSeq = 0;
  Ops.push_back(Code);
  MedOp Address;
  Address.Opcode = NdOp::COPY;
  Address.Output = value(2);
  Address.addInput(MedVar::makeConst(0, 4));
  Address.Addr = 0x1004;
  Address.OriginSeq = 0;
  Ops.push_back(Address);
  MedOp Store;
  Store.Opcode = NdOp::STORE;
  Store.addInput(Address.Output);
  Store.addInput(Code.Output);
  Store.Addr = 0x1004;
  Store.OriginSeq = 1;
  Ops.push_back(Store);
  Address.Output = value(3);
  Address.Addr = 0x1008;
  Ops.push_back(Address);
  MedOp Reload;
  Reload.Opcode = NdOp::LOAD;
  Reload.Output = value(4);
  Reload.addInput(Address.Output);
  Reload.Addr = 0x1008;
  Reload.OriginSeq = 1;
  Ops.push_back(Reload);
  MedOp Branch;
  Branch.Opcode = NdOp::COND_BR;
  Branch.addInput(Reload.Output);
  Ops.push_back(Branch);
  States.FrameValues = {{0x1000, 0, std::nullopt},
                        {0x1004, 0, -36},
                        {0x1008, 0, -36},
                        {0x1008, 1, std::nullopt}};
  return F;
}

bool provesSpill(const MedFunc &F) {
  const auto Offsets = getSEHFrameOffsets(F);
  if (!Offsets)
    return false;
  std::set<SEHFrameValues::Key> Scalars{key(value(1))};
  size_t Work = 0;
  return extendSEHScalarSpills(F, *Offsets, Scalars, Work) &&
         Scalars.count(key(value(4)));
}

TEST(RegistrationSEHScalarSpill, CarriesAnExactDWORDWithinItsBlock) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = scalarSpill();
    auto &Ops = F.Blocks.front().Ops;
    if (Mutation == 1) {
      // A disjoint store must not destroy the fact about this slot.
      auto Other = Ops[1];
      Other.Addr = 0x1006;
      Other.Output = value(5);
      F.RegistrationStates->FrameValues.push_back({Other.Addr, 0, -32});
      auto Write = Ops[2];
      Write.Addr = Other.Addr;
      Write.Inputs[0] = Other.Output;
      Write.Inputs[1] = MedVar::makeConst(42, 4);
      Ops.insert(Ops.begin() + 3, {Other, Write});
    }
    if (Mutation == 2)
      Ops[1].Output.Kind = Ops[2].Inputs[0].Kind = MedVar::Reg;
    if (Mutation == 3) {
      F.Blocks.insert(F.Blocks.begin(), MedBlock{});
      F.Blocks.front().Id = 0;
    }
    EXPECT_TRUE(provesSpill(F));
  }
}

TEST(RegistrationSEHScalarSpill,
     RefusesChangedCoordinatesEffectsAndDefinitions) {
  for (unsigned Mutation = 0; Mutation != 36; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = scalarSpill();
    auto &Ops = F.Blocks.front().Ops;
    auto &States = *F.RegistrationStates;
    switch (Mutation) {
    case 0:
      States.FrameValues[2].EstablishedFrameOffset = -32;
      break;
    case 1:
      States.FrameValues[2].EstablishedFrameOffset.reset();
      break;
    case 2:
      Ops[4].Inputs[0].Size = 8;
      break;
    case 3:
      ++Ops[4].Inputs[0].SSAVer;
      break;
    case 4:
      Ops[4].Output.Size = 2;
      break;
    case 5:
      Ops[2].Inputs[1].Size = 2;
      break;
    case 6:
      Ops[4].MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case 7:
      Ops[2].MemoryOrdering = NdMemoryOrdering::Release;
      break;
    case 8:
      Ops[4].MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 9:
      Ops[2].MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
      break;
    case 10:
      Ops[4].Dead = true;
      break;
    case 11:
      Ops[2].Dead = true;
      break;
    case 12:
      F.SkippedSSA = true;
      break;
    case 13:
      States.Complete = false;
      break;
    case 14:
      Ops[2].Inputs[1] = value(9);
      break;
    case 15: {
      auto Overwrite = Ops[2];
      Overwrite.Inputs[1] = MedVar::makeConst(1, 2);
      Ops.insert(Ops.begin() + 3, Overwrite);
      break;
    }
    case 16: {
      auto Overwrite = Ops[2];
      Overwrite.Inputs[0] = value(9);
      Ops.insert(Ops.begin() + 3, Overwrite);
      break;
    }
    case 17:
    case 18:
    case 19:
    case 20:
    case 21: {
      MedOp Effect;
      const NdOp Effects[] = {NdOp::CALL, NdOp::INDIR_CALL, NdOp::INTRINSIC,
                              NdOp::ATOMIC_ADD, NdOp::COND_BR};
      Effect.Opcode = Effects[Mutation - 17];
      Ops.insert(Ops.begin() + 3, Effect);
      break;
    }
    case 22: {
      auto Duplicate = Ops[4];
      ++Duplicate.Addr;
      Ops.push_back(Duplicate);
      break;
    }
    case 23:
      F.Blocks.front().Phis.emplace_back();
      F.Blocks.front().Phis.back().Output = Ops[4].Output;
      break;
    case 24:
      F.CallClobbers.emplace_back();
      F.CallClobbers.back().Value = Ops[4].Output;
      break;
    case 25:
      Ops.front().IntrinsicOutputs.push_back(Ops[4].Output);
      break;
    case 26: {
      auto Duplicate = Ops[0];
      ++Duplicate.Addr;
      Ops.push_back(Duplicate);
      break;
    }
    case 27:
      States.FrameValues[3].EstablishedFrameOffset = -40;
      break;
    case 28: {
      MedBlock Next;
      Next.Id = 2;
      Next.Ops.assign(Ops.begin() + 3, Ops.end());
      Ops.erase(Ops.begin() + 3, Ops.end());
      F.Blocks.push_back(Next);
      break;
    }
    case 29:
      Ops[4].Output.SSAVer = 0;
      break;
    case 30:
      Ops[2].Inputs[1].Size = 0;
      break;
    case 31:
      Ops[0].Output.Size = 8;
      break;
    case 32:
    case 33: {
      auto Other = Ops[1];
      Other.Addr = 0x1006;
      Other.Output = value(5);
      States.FrameValues.push_back({Other.Addr, 0, Mutation == 32 ? -37 : -33});
      auto Write = Ops[2];
      Write.Addr = Other.Addr;
      Write.Inputs[0] = Other.Output;
      Write.Inputs[1] = MedVar::makeConst(42, 4);
      Ops.insert(Ops.begin() + 3, {Other, Write});
      break;
    }
    case 34:
      Ops[0].Dead = true;
      break;
    case 35:
      Ops[0].Output.SSAVer = 0;
      break;
    }
    EXPECT_FALSE(provesSpill(F));
  }
}

TEST(RegistrationSEHScalarSpill, RequiresTheFullBudget) {
  const auto F = scalarSpill();
  const auto Offsets = getSEHFrameOffsets(F);
  ASSERT_TRUE(Offsets);
  for (bool Enough : {false, true}) {
    std::set<SEHFrameValues::Key> Scalars{key(value(1))};
    size_t Work = limits::kMaxRegistrationEHStateWork - 6 + !Enough;
    EXPECT_EQ(extendSEHScalarSpills(F, *Offsets, Scalars, Work), Enough);
  }
}

TEST(RegistrationSEHScalarSpill, OnlyRuntimeDWORDsDischargeFramePrivacy) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = scalarSpill();
    auto &Access = F.RegistrationStates->SEHExceptionAccesses.front();
    if (Mutation == 1)
      Access.Field = RegistrationSEHExceptionAccess::Kind::ExceptionFlags;
    if (Mutation == 2)
      Access.Field = RegistrationSEHExceptionAccess::Kind::RecordPointer;
    if (Mutation == 3)
      ++Access.OpSeq;
    if (Mutation == 4)
      F.Blocks.front().Ops[2].Inputs[1] = value(2);
    EXPECT_EQ(getPrivateSEHFrameValues(F, *F.RegistrationStates,
                                       [](const MedOp &) { return true; })
                  .has_value(),
              Mutation <= 1);
  }
}

} // namespace
