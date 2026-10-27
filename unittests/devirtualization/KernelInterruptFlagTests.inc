//===- KernelInterruptFlagTests.inc - Supervisor IF semantics ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Independent native byte oracles for supervisor CLI and STI.

Program kernelInterruptOracle(std::initializer_list<uint8_t> Controls) {
  // Keep an independent entry snapshot and arbitrary RAX. Capture flags again
  // immediately after the controls, before the checking arithmetic changes any.
  Program P({0x9c, 0x41, 0x58, 0x49, 0x89, 0xc2});
  P.append(Controls);
  P.append({0x9c, 0x41, 0x59, 0x4c, 0x39, 0xd0, 0x75, 0});
  const auto FirstJump = P.Image.Segments.front().Data.size() - 1;
  // xor the two snapshots and discard only IF. Every other packed bit must
  // agree, including all scalar flags and the profile's symbolic NT/ID bits.
  P.append({0x4c, 0x89, 0xc0, 0x4c, 0x31, 0xc8, 0x25});
  P.immediate(0xfffffdff);
  P.append({0x85, 0xc0, 0x75, 0});
  const auto SecondJump = P.Image.Segments.front().Data.size() - 1;
  P.append({0x4c, 0x89, 0xc8, 0x25});
  P.immediate(0x200);
  P.requireEAX(*(Controls.end() - 1) == 0xfb ? 0x200 : 0);
  auto &Bytes = P.Image.Segments.front().Data;
  for (size_t At : {FirstJump, SecondJump}) {
    EXPECT_LT(Bytes.size() - At - 2, 128U);
    Bytes[At] = static_cast<uint8_t>(Bytes.size() - At - 2);
  }
  P.flagsProfile(InterpreterMachineStateProfile::KernelX64NoFaultV1);
  for (unsigned R = 1; R != 16; ++R)
    P.Contract.ReturnRegisters.push_back({R * 8, 8});
  for (auto Flag : {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF, x86reg::SF,
                    x86reg::DF, x86reg::OF})
    P.Contract.ReturnRegisters.push_back({Flag, 1});
  P.Contract.NativePreservedState = LowIRNativePreservationRequirement{
      LowPreservedStateSet::LegacyIntegerOpaqueV1,
      LowIRNativePreservationQuantifier::AllUndefinedChoices};
  return P;
}

TEST(OriginalBinaryUndefinedIndependence, KernelInterruptControlsChangeOnlyIF) {
  const std::initializer_list<uint8_t> Cases[] = {
      {0xfa}, {0xfb}, {0xfb, 0xfa}, {0xfa, 0xfb}, {0xfb, 0xfb}};
  for (const auto Controls : Cases) {
    auto P = kernelInterruptOracle(Controls);
    const auto Good = P.check();
    ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
    ASSERT_TRUE(Good.Certificate->LowIR.NativePreservation);
    EXPECT_EQ(Good.Certificate->LowIR.NativeFlagTransitions.size(),
              Controls.size() + 2);
  }
}

TEST(OriginalBinaryUndefinedIndependence,
     KernelInterruptControlsRequireExplicitSupervisorProfile) {
  for (uint8_t Opcode : {uint8_t{0xfa}, uint8_t{0xfb}}) {
    Program P({Opcode, 0xc3});
    P.flagsProfile(InterpreterMachineStateProfile::KernelX64NoFaultV1);
    const auto Good = P.recover();
    ASSERT_TRUE(Good.Independence.proved())
        << Good.Independence.Proof.Diagnostic;
    ASSERT_TRUE(Good.Recovery.complete()) << Good.Recovery.Diagnostic;
    unsigned Retained = 0;
    for (const auto &B : Good.Recovery.Residual.Blocks)
      for (const auto &O : B.Ops)
        if (O.Opcode == NdOp::INTRINSIC)
          ++Retained;
    EXPECT_EQ(Retained, 1U);
    P.flagsProfile();
    expectRefusal(P, Status::Unsupported);
    P.Options.X64FlagsProfile.reset();
    P.Contract.X64FlagsProfile.reset();
    expectRefusal(P, Status::Unsupported);
  }
}

TEST(OriginalBinaryUndefinedIndependence,
     KernelInterruptControlsChargeTransitionWork) {
  Program P({0xfb, 0xfa, 0xc3});
  P.flagsProfile(InterpreterMachineStateProfile::KernelX64NoFaultV1);
  const auto Good = P.check();
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_EQ(Good.Certificate->LowIR.NativeFlagTransitions.size(), 2U);
  LowIRIndependenceLimits Limits;
  Limits.MaxOperations = Good.Proof.Operations;
  ASSERT_TRUE(P.check(Limits).proved());
  --Limits.MaxOperations;
  expectRefusal(P, Status::BudgetExceeded, Limits);
}
