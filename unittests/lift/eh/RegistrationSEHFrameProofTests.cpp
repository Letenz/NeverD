//===- RegistrationSEHFrameProofTests.cpp - Source call frame privacy
//------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/llvm/X86/MedLLVMRegistrationSEHProof.h"
#include "gtest/gtest.h"

using namespace neverd;

namespace {

MedFunc cookieCall() {
  MedFunc Function;
  Function.ExceptionMetadata.emplace();
  Function.ExceptionMetadata->Registration.emplace();
  Function.RegistrationStates.emplace();
  auto &States = *Function.RegistrationStates;
  States.SecurityCookiesComplete = true;
  States.CookieCheckVA = 0x2000;
  States.CookieChecks.push_back({0x1010, 0x1015, 3});
  MedBlock Block;
  Block.Id = 1;
  MedOp Root;
  Root.Opcode = NdOp::COPY;
  Root.Addr = 0x1000;
  Root.OriginSeq = 2;
  Root.Output.Kind = MedVar::Temp;
  Root.Output.Id = 12;
  Root.Output.SSAVer = 1;
  Root.Output.Size = 4;
  Block.Ops.push_back(Root);
  States.FrameValues.push_back({Root.Addr, Root.OriginSeq, std::nullopt});
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = 0x1010;
  Call.OriginSeq = 3;
  Call.addInput(MedVar::makeConst(0x2000, 4));
  Block.Ops.push_back(Call);
  Function.Blocks.push_back(Block);
  MedCallInfo Info;
  Info.BlockId = 1;
  Info.OpIdx = 1;
  Info.Args.push_back(Root.Output);
  Function.CallInfos.push_back(Info);
  return Function;
}

bool provesFramePrivacy(const MedFunc &Function) {
  return x86_registration::getPrivateSEHFrameValues(
             Function, *Function.RegistrationStates,
             [](const MedVar &, uint16_t) { return false; })
      .has_value();
}

TEST(RegistrationSEHFrameProof, CheckedDecodedCookieIsNotAFrameEscape) {
  auto Function = cookieCall();
  EXPECT_TRUE(provesFramePrivacy(Function));
}

TEST(RegistrationSEHFrameProof, CookieExemptionRequiresItsExactSourceCall) {
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Function = cookieCall();
    auto &States = *Function.RegistrationStates;
    auto &Call = Function.Blocks.front().Ops.back();
    switch (Mutation) {
    case 0:
      States.SecurityCookiesComplete = false;
      break;
    case 1:
      States.CookieChecks.clear();
      break;
    case 2:
      Call.Addr += 1;
      break;
    case 3:
      Call.OriginSeq += 1;
      break;
    case 4:
      Call.Inputs[0].ConstVal += 1;
      break;
    case 5:
      Function.CallInfos.front().Args.front().Size = 8;
      break;
    case 6:
      Function.CallInfos.front().Args.push_back(MedVar::makeConst(0, 4));
      break;
    case 7:
      Function.CallInfos.front().OpIdx = 2;
      break;
    }
    EXPECT_FALSE(provesFramePrivacy(Function));
  }
}

} // namespace
