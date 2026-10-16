//===- KernelInterruptRefinementTests.inc - Mandatory IF observations ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Supervisor flag changes remain visible to original/residual proofs.

TEST(BinaryLowIRRefinement, KernelInterruptControlsCompareMandatoryFinalIF) {
  for (uint8_t Opcode : {uint8_t{0xfa}, uint8_t{0xfb}}) {
    Program P({Opcode, 0xc3});
    P.Options.X64FlagsProfile = P.Contract.X64FlagsProfile =
        InterpreterMachineStateProfile::KernelX64NoFaultV1;
    auto R = P.recover();
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    P.Contract.ReturnRegisters.clear();
    P.Contract.ObserveWrittenFrameBytes = false;
    const auto Identity = P.check(R.Residual);
    ASSERT_TRUE(Identity.proved()) << Identity.Proof.Diagnostic;
    unsigned Changed = 0;
    for (auto &B : R.Residual.Blocks)
      for (auto &O : B.Ops)
        if (O.Opcode == NdOp::INTRINSIC) {
          O.Inputs[0] = NdVar::scalar(
              static_cast<uint64_t>(Opcode == 0xfa ? Intrinsic::Sti
                                                   : Intrinsic::Cli),
              2);
          ++Changed;
        }
    ASSERT_EQ(Changed, 1U);
    const auto Different = P.check(R.Residual);
    refused(Different, Status::Different);
    EXPECT_NE(Different.Proof.Diagnostic.find("final system flags"),
              std::string::npos);
  }
}

TEST(BinaryLowIRRefinement, KernelInterruptControlsRejectMalformedCandidate) {
  Program P({0xfa, 0xc3});
  P.Options.X64FlagsProfile = P.Contract.X64FlagsProfile =
      InterpreterMachineStateProfile::KernelX64NoFaultV1;
  const auto Good = P.recover();
  ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
  for (unsigned Kind = 0; Kind != 6; ++Kind) {
    auto Candidate = Good.Residual;
    unsigned Changed = 0;
    for (auto &B : Candidate.Blocks)
      for (auto &O : B.Ops)
        if (O.Opcode == NdOp::INTRINSIC) {
          if (Kind == 0)
            O.Output = NdVar::reg(x86reg::RAX, 8);
          else if (Kind == 1)
            O.addInput(NdVar::scalar(0, 8));
          else if (Kind == 2)
            O.Inputs[0].Size = 1;
          else if (Kind == 3)
            O.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
          else if (Kind == 4)
            O.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
          else
            O.Inputs[0] = NdVar::reg(x86reg::RAX, 2);
          ++Changed;
        }
    ASSERT_EQ(Changed, 1U);
    const auto Bad = P.check(Candidate);
    EXPECT_FALSE(Bad.proved()) << "kind=" << Kind;
    EXPECT_FALSE(Bad.Certificate);
    EXPECT_FALSE(Bad.Proof.Certificate);
  }
}
