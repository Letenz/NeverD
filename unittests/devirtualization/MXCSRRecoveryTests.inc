//===- MXCSRRecoveryTests.inc - Explicit control-state recovery ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// MXCSR recovery, retained effects and reserved-bit proof regressions.
//===----------------------------------------------------------------------===//

SpecializationOptions mxcsrOptions(uint32_t Mask = 0xffbf) {
  SpecializationOptions O;
  O.ExplicitMachineState = true;
  O.NormalNonfaultingExecution = true;
  O.X64MXCSRMask = Mask;
  return O;
}

LowOp mxcsrLoad(NdVar Address) {
  return op(NdOp::INTRINSIC, {},
            {constant(uint64_t(Intrinsic::Ldmxcsr), 2), Address});
}
LowOp mxcsrRead(NdVar Output) {
  return op(NdOp::INTRINSIC, Output,
            {constant(uint64_t(Intrinsic::X86ReadMXCSR), 2)});
}
LowOp mxcsrWrite(NdVar Value) {
  return op(NdOp::INTRINSIC, {},
            {constant(uint64_t(Intrinsic::X86WriteMXCSR), 2), Value});
}

TEST(InterpreterSpecialization, MXCSRRequiresExplicitArchitectureAndMask) {
  Provider P;
  P.add(0x100, {mxcsrRead(reg(8, 4)), ret()});
  EXPECT_EQ(specializeInterpreter(P, {0x100}).Status,
            SpecializationStatus::Unsupported);
  for (unsigned Case = 0; Case != 8; ++Case) {
    SCOPED_TRACE(Case);
    auto O = mxcsrOptions();
    switch (Case) {
    case 0:
      O.ExplicitMachineState = false;
      break;
    case 1:
      O.NormalNonfaultingExecution = false;
      break;
    case 2:
      O.X64MXCSRMask = 0;
      break;
    case 3:
      O.X64MXCSRMask = 0x10000;
      break;
    case 4:
      O.X64MXCSRMask = 0xff00;
      break;
    case 5:
      O.Architecture = Arch::AArch64;
      break;
    case 6:
      O.Architecture = Arch::X86;
      break;
    case 7:
      O.ByteOrder = llvm::endianness::big;
      break;
    }
    const auto R = specializeInterpreter(P, {0x100}, O);
    EXPECT_EQ(R.Status, Case == 5 || Case == 6
                            ? SpecializationStatus::Unsupported
                            : SpecializationStatus::InvalidInput)
        << R.Diagnostic;
    EXPECT_TRUE(R.Residual.Blocks.empty());
  }
}

TEST(InterpreterSpecialization, MXCSRIncomingStateIsBoundedButArbitrary) {
  for (uint32_t Mask : {0xffbfU, 0xffffU}) {
    Provider P;
    P.add(0x100,
          {mxcsrRead(reg(8, 4)),
           op(NdOp::INT_AND, reg(16, 4),
              {reg(8, 4), constant(uint32_t(~Mask), 4)}),
           op(NdOp::INT_NOTEQUAL, reg(24, 1), {reg(16, 4), constant(0, 4)}),
           op(NdOp::COND_BR, {}, {constant(0x200), reg(24, 1)})},
          0x110);
    P.add(0x110, {op(NdOp::INT_ZEXT, reg(0), {reg(8, 4)}), ret()});
    const auto R = specializeInterpreter(P, {0x100}, mxcsrOptions(Mask));
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    EXPECT_EQ(count(R.Residual, NdOp::INTRINSIC), 1U);
    for (uint32_t Entry : {0U, 0x1f80U, 0x8000U, 0xffbfU, Mask}) {
      uint32_t Final = UINT32_MAX;
      EXPECT_EQ(execute(R.Residual, {}, {}, 0, {}, {}, {}, nullptr, Entry, Mask,
                        &Final),
                Entry);
      EXPECT_EQ(Final, Entry);
    }
  }
}

TEST(InterpreterSpecialization, MXCSRLoadPreservesExactMemoryValueAndEffects) {
  for (uint32_t Mask : {0xffbfU, 0xffffU}) {
    Provider P;
    P.add(0x100, {op(NdOp::INT_AND, reg(16, 4), {reg(8, 4), constant(Mask, 4)}),
                  op(NdOp::STORE, {}, {reg(24), reg(16, 4)})});
    P.add(0x101, {mxcsrLoad(reg(24))});
    P.add(0x102, {mxcsrRead(reg(32, 4)),
                  op(NdOp::INT_ZEXT, reg(0), {reg(32, 4)}), ret()});
    const auto R = specializeInterpreter(P, {0x100}, mxcsrOptions(Mask));
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    EXPECT_EQ(count(R.Residual, NdOp::INTRINSIC), 2U);
    EXPECT_EQ(count(R.Residual, NdOp::STORE), 1U);
    for (uint32_t Word : {0U, 1U, 0x40U, 0x1f80U, 0xffffU, UINT32_MAX})
      for (uint64_t Address : {0x1000ULL, 0x1001ULL, 0x1003ULL}) {
        uint32_t Final = UINT32_MAX;
        std::map<uint64_t, uint8_t> Memory;
        EXPECT_EQ(execute(R.Residual, {{8, Word}, {24, Address}}, {}, 0, {}, {},
                          {}, &Memory, 0x1f80, Mask, &Final),
                  Word & Mask);
        EXPECT_EQ(Final, Word & Mask);
        ASSERT_EQ(Memory.size(), 4U);
        for (unsigned Byte = 0; Byte != 4; ++Byte)
          EXPECT_EQ(Memory.at(Address + Byte),
                    uint8_t((Word & Mask) >> (8 * Byte)));
      }
    auto Exact = mxcsrOptions(Mask);
    Exact.MaxOperations = R.EvaluatedOperations;
    EXPECT_TRUE(specializeInterpreter(P, {0x100}, Exact).complete());
    --Exact.MaxOperations;
    EXPECT_EQ(specializeInterpreter(P, {0x100}, Exact).Status,
              SpecializationStatus::BudgetExceeded);
  }
}

TEST(InterpreterSpecialization, MXCSRWriteCanUseAProvedBranchDomain) {
  Provider P;
  P.add(0x100,
        {op(NdOp::INT_LESS, reg(16, 1), {reg(8, 4), constant(16, 4)}),
         op(NdOp::COND_BR, {}, {constant(0x110), reg(16, 1)})},
        0x120);
  P.add(0x110, {mxcsrWrite(reg(8, 4)), mxcsrRead(reg(24, 4)),
                op(NdOp::INT_ZEXT, reg(0), {reg(24, 4)}), ret()});
  P.add(0x120, {op(NdOp::COPY, reg(0), {constant(99)}), ret()});
  auto O = mxcsrOptions();
  O.ControlRegisters = {{8, 4}};
  const auto R = specializeInterpreter(P, {0x100}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  for (uint32_t Input : {0U, 4U, 15U, 16U, 255U, UINT32_MAX}) {
    uint32_t Final = UINT32_MAX;
    EXPECT_EQ(execute(R.Residual, {{8, Input}}, {}, 0, {}, {}, {}, nullptr,
                      0x1f80, 0xffbf, &Final),
              Input < 16 ? Input : 99);
    EXPECT_EQ(Final, Input < 16 ? Input : 0x1f80U);
  }
  ASSERT_GT(R.SolverQueries, 1U);
  O.MaxSolverQueries = R.SolverQueries;
  EXPECT_TRUE(specializeInterpreter(P, {0x100}, O).complete());
  --O.MaxSolverQueries;
  const auto Short = specializeInterpreter(P, {0x100}, O);
  EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded)
      << Short.Diagnostic;
  EXPECT_TRUE(Short.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, MXCSRDoesNotInferValidityFromANonfaultingFlag) {
  for (bool Load : {false, true})
    for (auto Value : {reg(8, 4), constant(0x10000, 4), constant(UINT32_MAX, 4),
                       constant(0x40, 4)}) {
      Provider P;
      if (Load)
        P.add(0x100, {op(NdOp::STORE, {}, {reg(16), Value}), mxcsrLoad(reg(16)),
                      ret()});
      else
        P.add(0x100, {mxcsrWrite(Value), ret()});
      const auto R = specializeInterpreter(P, {0x100}, mxcsrOptions());
      EXPECT_EQ(R.Status, SpecializationStatus::Unsupported);
      EXPECT_NE(R.Diagnostic.find("reserved bits"), std::string::npos);
      EXPECT_TRUE(R.Residual.Blocks.empty());
    }
  Provider P;
  P.add(0x100, {mxcsrLoad(reg(16)), ret()});
  const auto Unknown = specializeInterpreter(P, {0x100}, mxcsrOptions(0xffff));
  EXPECT_EQ(Unknown.Status, SpecializationStatus::Unsupported);
  EXPECT_NE(Unknown.Diagnostic.find("reserved bits"), std::string::npos);
}

TEST(InterpreterSpecialization, MXCSRLoadKeepsOrdinaryAliasInvalidation) {
  for (bool SameAddress : {false, true}) {
    Provider P;
    P.add(0x100,
          {op(NdOp::STORE, {}, {reg(8), constant(0x1f80, 4)}),
           op(NdOp::STORE, {}, {SameAddress ? reg(8) : reg(16), reg(24, 4)}),
           mxcsrLoad(reg(8)), ret()});
    const auto R = specializeInterpreter(P, {0x100}, mxcsrOptions());
    EXPECT_EQ(R.Status, SpecializationStatus::Unsupported);
    EXPECT_NE(R.Diagnostic.find("reserved bits"), std::string::npos);
    EXPECT_TRUE(R.Residual.Blocks.empty());
  }
}

TEST(InterpreterSpecialization, MXCSRDAZRequiresItsDeclaredCapability) {
  Provider P;
  P.add(0x100, {mxcsrWrite(constant(0x40, 4)), mxcsrRead(reg(8, 4)),
                op(NdOp::INT_ZEXT, reg(0), {reg(8, 4)}), ret()});
  EXPECT_EQ(specializeInterpreter(P, {0x100}, mxcsrOptions()).Status,
            SpecializationStatus::Unsupported);
  const auto R = specializeInterpreter(P, {0x100}, mxcsrOptions(0xffff));
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_EQ(execute(R.Residual, {}, {}, 0, {}, {}, {}, nullptr, 0, 0xffff),
            0x40U);
  EXPECT_EQ(count(R.Residual, NdOp::INTRINSIC), 2U);
}

TEST(InterpreterSpecialization, MXCSRRejectsMalformedAndUnmodeledOperations) {
  std::vector<LowOp> Cases{
      mxcsrLoad(reg(8, 4)), mxcsrWrite(reg(8)), mxcsrRead(reg(8)),
      op(NdOp::INTRINSIC, {},
         {constant(uint64_t(Intrinsic::Stmxcsr), 2), reg(8)})};
  auto Bad = mxcsrLoad(reg(8));
  Bad.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
  Cases.push_back(Bad);
  Bad = mxcsrLoad(reg(8));
  Bad.Output = reg(16, 4);
  Cases.push_back(Bad);
  Bad = mxcsrRead(reg(8, 4));
  Bad.Inputs[0].Size = 1;
  Cases.push_back(Bad);
  Bad = mxcsrWrite(constant(0, 4));
  Bad.addInput(constant(0));
  Cases.push_back(Bad);
  for (size_t I = 0; I != Cases.size(); ++I) {
    SCOPED_TRACE(I);
    Provider P;
    P.add(0x100, {Cases[I], ret()});
    const auto R = specializeInterpreter(P, {0x100}, mxcsrOptions());
    // The canonical memory validator owns the malformed address/output cases.
    EXPECT_EQ(R.Status, I == 0 || I == 5 ? SpecializationStatus::InvalidInput
                                         : SpecializationStatus::Unsupported)
        << R.Diagnostic;
    EXPECT_TRUE(R.Residual.Blocks.empty());
  }
}

TEST(InterpreterSpecialization, MXCSRProjectionRetainsStateOperations) {
  Provider P;
  P.add(0x100, {mxcsrWrite(constant(0x1f80, 4)), jump(0x110)});
  P.add(0x110,
        {mxcsrRead(reg(8, 4)), op(NdOp::INT_ZEXT, reg(0), {reg(8, 4)}), ret()});
  auto O = mxcsrOptions();
  O.MaxChainedTransfers = 0;
  const auto R = specializeInterpreter(P, {0x100}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_GE(R.Residual.Blocks.size(), 2U);
  EXPECT_EQ(count(R.Residual, NdOp::INTRINSIC), 2U);
  for (uint32_t Entry : {0U, 0x1f80U, 0xffbfU})
    EXPECT_EQ(execute(R.Residual, {}, {}, 0, {}, {}, {}, nullptr, Entry),
              0x1f80U);
}

TEST(InterpreterSpecialization, MXCSRTransitionPreservesStateOnRejectedWrites) {
  for (uint32_t Mask : {0xffbfU, 0xffffU}) {
    SymContext C;
    SymState S(C);
    SymExec E(C, S);
    auto Controls = seedRuntimeControlState(C, mxcsrOptions(Mask));
    ASSERT_EQ(Controls.Values.size(), 1U);
    const auto Old = Controls.Values[0];
    const auto Word = C.mkVar("incoming_word", 32);
    S.write(SymSpace::Register, 8, Word);
    uint64_t Next = 0x1000;
    const auto T =
        lowerRuntimeControlOperation(mxcsrWrite(reg(8, 4)), S, Controls,
                                     mxcsrOptions(Mask), [&](uint16_t Bytes) {
                                       const auto V = NdVar::tmp(Next, Bytes);
                                       Next += Bytes;
                                       return V;
                                     });
    ASSERT_TRUE(T);
    ASSERT_TRUE(T->Rejected);
    ASSERT_TRUE(T->OutputState);
    for (const auto &Op : T->Ops)
      ASSERT_EQ(E.step(Op), StepResult::Continue);
    const auto Bad = E.operandValue(*T->Rejected);
    const auto After = E.operandValue(T->OutputState->second);
    const auto Invalid = C.mkNot(
        C.mkEq(C.mkAnd(Word, C.mkConst(32, uint32_t(~Mask))), C.mkZero(32)));
    // Independent architectural postconditions, for every 32-bit input and
    // every declared entry state. Refusal must precede a state change.
    solver::BitVectorSolver Fault(C), Success(C), Guard(C);
    ASSERT_TRUE(Fault.assertTrue(Invalid));
    ASSERT_TRUE(Fault.assertDistinct(After, Old));
    EXPECT_EQ(Fault.check(), solver::SatResult::Unsat);
    ASSERT_TRUE(Success.assertFalse(Invalid));
    ASSERT_TRUE(Success.assertDistinct(After, Word));
    EXPECT_EQ(Success.check(), solver::SatResult::Unsat);
    ASSERT_TRUE(Guard.assertDistinct(C.mkExtract(Bad, 0, 1), Invalid));
    EXPECT_EQ(Guard.check(), solver::SatResult::Unsat);
  }
}
