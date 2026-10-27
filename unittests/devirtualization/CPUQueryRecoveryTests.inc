//===- CPUQueryRecoveryTests.inc - Runtime CPU-query recovery ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Runtime CPU-query recovery, state preservation and refusal regressions.
///
//===----------------------------------------------------------------------===//

void addCPUQuery(Provider &P, va_t Address) {
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  D.setStrict(true);
  const uint8_t Bytes[]{0x0f, 0xa2};
  DecodedInsn I{};
  ASSERT_EQ(D.decodeOneForLift(Bytes, sizeof(Bytes), Address, I),
            sizeof(Bytes));
  std::vector<LowOp> Ops;
  LowInstructionUndefinedEffects Effects;
  D.liftToLow(I, Ops, {}, {}, &Effects);
  P.add(Address, std::move(Ops), Address + sizeof(Bytes));
  auto &Instruction = P.Code.at(Address);
  Instruction.Origin.Size = sizeof(Bytes);
  Instruction.NativeBytes.assign(std::begin(Bytes), std::end(Bytes));
  Instruction.UndefinedEffects = std::move(Effects);
}

std::optional<uint64_t>
executeCPUQueries(const LowFunc &Function, std::map<uint64_t, uint64_t> Inputs,
                  llvm::ArrayRef<CPUQueryObservation> Queries) {
  return execute(Function, std::move(Inputs), {}, 0, {}, std::nullopt, {},
                 nullptr, std::nullopt, 0xffbf, nullptr, Queries);
}

TEST(InterpreterSpecialization, CPUQueryRetainsSelectorsAndFourResults) {
  Provider P;
  addCPUQuery(P, 0x100);
  P.add(0x102, {op(NdOp::INT_XOR, reg(0), {reg(0), reg(x86reg::RBX)}),
                op(NdOp::INT_XOR, reg(0), {reg(0), reg(x86reg::RCX)}),
                op(NdOp::INT_XOR, reg(0), {reg(0), reg(x86reg::RDX)}),
                op(NdOp::INT_XOR, reg(0), {reg(0), reg(x86reg::R8)}), ret()});
  const auto R = specializeInterpreter(P, {0x100}, wideOptions());
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_EQ(count(R.Residual, NdOp::INTRINSIC), 1u);
  for (uint32_t Leaf : {0u, 7u, 0x80000001u, UINT32_MAX})
    for (uint32_t Subleaf : {0u, 3u, UINT32_MAX}) {
      const std::array<uint32_t, 4> Answers{
          Leaf ^ 0x91827364u, Subleaf ^ 0xfedcba98u, 0x80000000u, UINT32_MAX};
      const uint64_t Saved = UINT64_C(0x123456789abcdef0);
      const CPUQueryObservation Query{Leaf, Subleaf, Answers};
      const uint64_t Expected =
          uint64_t(Answers[0] ^ Answers[1] ^ Answers[2] ^ Answers[3]) ^ Saved;
      EXPECT_EQ(executeCPUQueries(
                    R.Residual,
                    {{x86reg::RAX, UINT64_C(0x8765432100000000) | Leaf},
                     {x86reg::RCX, UINT64_C(0xabcdef1200000000) | Subleaf},
                     {x86reg::R8, Saved}},
                    {Query}),
                Expected);
    }
}

TEST(InterpreterSpecialization, CPUQueryAnswersRemainIndependent) {
  Provider P;
  addCPUQuery(P, 0x100);
  P.add(0x102,
        {op(NdOp::COPY, reg(x86reg::R8), {reg(x86reg::RBX)}),
         op(NdOp::COPY, reg(x86reg::RAX), {constant(7)}),
         op(NdOp::COPY, reg(x86reg::RCX), {constant(3)})},
        0x110);
  addCPUQuery(P, 0x110);
  P.add(0x112,
        {op(NdOp::INT_EQUAL, reg(80, 1), {reg(x86reg::R8), reg(x86reg::RBX)}),
         op(NdOp::COND_BR, {}, {constant(0x120), reg(80, 1)})},
        0x130);
  P.add(0x120, {op(NdOp::COPY, reg(0), {constant(17)}), ret()});
  P.add(0x130, {op(NdOp::COPY, reg(0), {constant(23)}), ret()});
  const auto R = specializeInterpreter(P, {0x100}, wideOptions());
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_EQ(count(R.Residual, NdOp::INTRINSIC), 2u);
  EXPECT_GT(count(R.Residual, NdOp::COND_BR), 0u);
  for (uint32_t First : {0u, 0x80000000u, UINT32_MAX})
    for (uint32_t Second : {First, First ^ 1u, First ^ 0x80000000u}) {
      const CPUQueryObservation Queries[]{{7, 3, {1, First, 2, 3}},
                                          {7, 3, {4, Second, 5, 6}}};
      EXPECT_EQ(executeCPUQueries(
                    R.Residual, {{x86reg::RAX, 7}, {x86reg::RCX, 3}}, Queries),
                First == Second ? 17u : 23u);
    }
}

TEST(InterpreterSpecialization, CPUQueryRepeatedOccurrenceAndExactBudget) {
  Provider P;
  P.add(0xf0,
        {op(NdOp::COPY, reg(80), {constant(0)}),
         op(NdOp::COPY, reg(88), {constant(3)})},
        0x100);
  P.add(0x100,
        {op(NdOp::COPY, reg(x86reg::RAX), {constant(7)}),
         op(NdOp::COPY, reg(x86reg::RCX), {constant(3)})},
        0x110);
  addCPUQuery(P, 0x110);
  P.add(0x112,
        {op(NdOp::INT_ADD, reg(80), {reg(80), reg(0)}),
         op(NdOp::INT_SUB, reg(88), {reg(88), constant(1)}),
         op(NdOp::COND_BR, {}, {constant(0x100), reg(88)})},
        0x120);
  P.add(0x120, {op(NdOp::COPY, reg(0), {reg(80)}), ret()});
  auto O = wideOptions();
  O.ControlRegisters = {{88, 8}};
  const auto R = specializeInterpreter(P, {0xf0}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  const CPUQueryObservation Queries[]{{7, 3, {1, 2, 3, 4}},
                                      {7, 3, {UINT32_MAX, 5, 6, 7}},
                                      {7, 3, {0x80000000u, 8, 9, 10}}};
  EXPECT_EQ(executeCPUQueries(R.Residual, {}, Queries), UINT64_C(0x180000000));
  O.MaxOperations = R.EvaluatedOperations;
  EXPECT_TRUE(specializeInterpreter(P, {0xf0}, O).complete());
  --O.MaxOperations;
  const auto Short = specializeInterpreter(P, {0xf0}, O);
  EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_TRUE(Short.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, CPUQueryTransitionMatchesCompleteStateOracle) {
  Provider P;
  addCPUQuery(P, 0x100);
  const auto &I = P.Code.at(0x100);
  const auto Sample = NdVar::tmp(0x1000, 16);
  const auto Transition = lowerRuntimeRegisterRead(I, wideOptions(), Sample);
  ASSERT_TRUE(Transition);
  ASSERT_EQ(Transition->SampleBytes, 16);
  const uint64_t Registers[]{x86reg::RAX, x86reg::RBX, x86reg::RCX,
                             x86reg::RDX};
  for (uint32_t Seed : {0u, 1u, 0x80000000u, 0xfedcba98u, UINT32_MAX}) {
    SymContext C;
    SymState S(C);
    const auto Canary = [](uint64_t Offset) {
      return UINT64_C(0xdeadbeef87654321) ^ Offset;
    };
    for (uint64_t Offset = 0; Offset < 16 * 8; Offset += 8)
      S.write(SymSpace::Register, Offset, C.mkConst(64, Canary(Offset)));
    for (uint64_t Flag : x64::ModelledFlags)
      S.write(SymSpace::Register, Flag, C.mkConst(8, 1));
    const auto Vector = llvm::APInt(128, Canary(0), true);
    S.write(SymSpace::Register, x86reg::XMM0, C.mkConst(Vector));
    const auto FS = C.mkConst(64, Canary(0x100));
    const auto GS = C.mkConst(64, Canary(0x200));
    ASSERT_TRUE(
        S.setX64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS, FS));
    ASSERT_TRUE(
        S.setX64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS, GS));
    ASSERT_TRUE(S.store(C.mkConst(64, 0x400), C.mkConst(64, Canary(0x400))));
    const uint32_t Answers[]{Seed, Seed ^ UINT32_MAX, Seed ^ 0x80000001u,
                             Seed ^ 0x12345678u};
    const uint64_t Words[]{uint64_t(Answers[0]) | (uint64_t(Answers[1]) << 32),
                           uint64_t(Answers[2]) | (uint64_t(Answers[3]) << 32)};
    S.write(SymSpace::Temporary, Sample.Offset,
            C.mkConst(llvm::APInt(128, Words)));
    SymExec E(C, S);
    for (const auto &Op : Transition->Ops)
      ASSERT_EQ(E.step(Op), StepResult::Continue);
    for (const auto &Op : llvm::ArrayRef(I.Ops).drop_front())
      ASSERT_EQ(E.step(Op), StepResult::Continue);
    for (uint64_t Offset = 0; Offset < 16 * 8; Offset += 8) {
      uint64_t Expected = Canary(Offset);
      for (unsigned Index = 0; Index != 4; ++Index)
        if (Offset == Registers[Index])
          Expected = Answers[Index];
      const auto Actual = C.asConst(S.read(SymSpace::Register, Offset, 8));
      ASSERT_TRUE(Actual);
      EXPECT_EQ(Actual->getZExtValue(), Expected) << Offset;
    }
    for (uint64_t Flag : x64::ModelledFlags)
      EXPECT_EQ(C.asConst(S.read(SymSpace::Register, Flag, 1))->getZExtValue(),
                1u);
    EXPECT_EQ(*C.asConst(S.read(SymSpace::Register, x86reg::XMM0, 16)), Vector);
    EXPECT_EQ(C.asConst(S.load(C.mkConst(64, 0x400), 8))->getZExtValue(),
              Canary(0x400));
    EXPECT_EQ(S.x64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS), FS);
    EXPECT_EQ(S.x64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS), GS);
    EXPECT_EQ(E.opaqueOperationCount(), 0u);
  }
}

TEST(InterpreterSpecialization, CPUQueryRejectsIncompleteBundleAndDomain) {
  Provider P;
  addCPUQuery(P, 0x100);
  P.add(0x102, {ret()});
  ASSERT_EQ(P.Code.at(0x100).Ops.size(), 9u);
  auto O = wideOptions();
  ASSERT_TRUE(specializeInterpreter(P, {0x100}, O).complete());
  for (unsigned Mutation = 0; Mutation != 22; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Bad = P;
    auto &I = Bad.Code.at(0x100);
    if (Mutation < 4)
      I.Ops[1 + Mutation].Output = reg(x86reg::R8, 4);
    else if (Mutation < 8)
      I.Ops[5 + Mutation - 4].Inputs[0] = reg(x86reg::R8, 4);
    else {
      switch (Mutation) {
      case 8:
        I.NativeBytes.clear();
        break;
      case 9:
        I.NativeBytes[1] = 0x31;
        break;
      case 10:
        I.Ops[0].Inputs[1] = reg(x86reg::RDX, 4);
        break;
      case 11:
        I.Ops[0].Inputs[2] = reg(x86reg::RCX, 8);
        break;
      case 12:
        I.Ops[0].NumInputs = 2;
        break;
      case 13:
        I.Ops[0].Output = reg(x86reg::RAX, 4);
        break;
      case 14:
        I.Ops[4].Inputs[0] = I.Ops[1].Inputs[0];
        break;
      case 15:
        I.Ops[2].Inputs[0].Offset = UINT64_MAX - 1;
        break;
      case 16:
        I.Ops[3].Inputs[0] = reg(0x500, 4);
        break;
      case 17:
        I.Ops[4].MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
        break;
      case 18:
        I.Ops[0].MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
        break;
      case 19:
        I.IsNativeCall = true;
        break;
      case 20:
        I.Fallthrough.Address += 1;
        break;
      case 21:
        I.Ops[7].Seq += 1;
        break;
      }
    }
    I.UndefinedEffects.OperationDigest = lowUndefinedOperationDigest(I.Ops);
    const auto R = specializeInterpreter(Bad, {0x100}, O);
    EXPECT_FALSE(R.complete());
    EXPECT_TRUE(R.Residual.Blocks.empty());
  }
  const auto Sample = NdVar::tmp(0x1000, 16);
  for (unsigned Missing = 0; Missing != 4; ++Missing) {
    auto Bad = O;
    if (Missing == 0)
      Bad.ExplicitMachineState = false;
    if (Missing == 1)
      Bad.NormalNonfaultingExecution = false;
    if (Missing == 2)
      Bad.ByteOrder = llvm::endianness::big;
    if (Missing == 3)
      Bad.Architecture = Arch::AArch64;
    EXPECT_FALSE(lowerRuntimeRegisterRead(P.Code.at(0x100), Bad, Sample));
  }
  for (auto Invalid :
       {reg(0, 16), NdVar::tmp(0x1000, 8), NdVar::tmp(UINT64_MAX - 7, 16),
        NdVar::tmp(P.Code.at(0x100).Ops[1].Inputs[0].Offset, 16)})
    EXPECT_FALSE(lowerRuntimeRegisterRead(P.Code.at(0x100), O, Invalid));
}

TEST(InterpreterSpecialization, CPUQueryAddressRemainsUnknownOrigin) {
  Provider P;
  addCPUQuery(P, 0x100);
  P.add(0x102, {ret()});
  auto O = ordinaryReturnOptions();
  O.ExplicitMachineState = O.NormalNonfaultingExecution = true;
  ASSERT_TRUE(specializeInterpreter(P, {0x100}, O).complete());
  P.add(0x102,
        {op(NdOp::INT_LEFT, reg(x86reg::RDX), {reg(x86reg::RDX), constant(32)}),
         op(NdOp::INT_OR, reg(0), {reg(0), reg(x86reg::RDX)}),
         op(NdOp::STORE, {}, {reg(0), constant(7)}), ret()});
  const auto R = specializeInterpreter(P, {0x100}, O);
  EXPECT_EQ(R.Status, SpecializationStatus::Unsupported);
  EXPECT_NE(R.Diagnostic.find("unknown address origin"), std::string::npos);
  EXPECT_TRUE(R.Residual.Blocks.empty());
}
