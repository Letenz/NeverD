//===- SavedScalarCellsTests.inc - Proved frame-value preservation
//---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

Provider savedScalarProgram(uint16_t Width = 8, int64_t WriteOffset = -64) {
  Provider P;
  std::vector<LowOp> Ops{
      op(NdOp::INT_SUB, reg(40), {reg(32), constant(99)}),
      op(NdOp::STORE, {}, {reg(40), reg(8, Width)}),
      op(NdOp::COPY, reg(72, Width), {reg(8, Width)}),
      op(NdOp::INT_AND, reg(24), {reg(16), constant(15)}),
      op(NdOp::INT_ADD, reg(56),
         {reg(32), constant(static_cast<uint64_t>(WriteOffset))}),
      op(NdOp::INT_ADD, reg(56), {reg(56), reg(24)}),
      op(NdOp::STORE, {}, {reg(56), constant(0xa5, 1)}),
      op(NdOp::LOAD, reg(48, Width), {reg(40)}),
      op(NdOp::INT_XOR, reg(48, Width), {reg(48, Width), reg(72, Width)})};
  if (Width != 8)
    Ops.push_back(op(NdOp::INT_ZEXT, reg(48), {reg(48, Width)}));
  Ops.push_back(op(NdOp::INT_ADD, reg(48), {reg(48), constant(0x110)}));
  Ops.push_back(op(NdOp::INDIR_BR, {}, {reg(48)}));
  P.add(0x100, Ops);
  P.add(0x110, {op(NdOp::COPY, reg(0), {constant(29)}), ret()});
  return P;
}

TEST(InterpreterSpecialization, SavedScalarSurvivesIndexedWrite) {
  for (uint16_t Width : {1, 2, 4, 8}) {
    auto P = savedScalarProgram(Width);
    const auto R = specializeInterpreter(P, {0x100}, separatedStoreOptions());
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    EXPECT_EQ(count(R.Residual, NdOp::INDIR_BR), 0u);
    for (uint64_t Root : {UINT64_C(8), UINT64_C(0x1008), UINT64_MAX - 7})
      for (uint64_t Value :
           {UINT64_C(0), UINT64_C(0x123456789abcdef0), UINT64_MAX})
        for (uint64_t Index : {UINT64_C(0), UINT64_C(7), UINT64_C(15)}) {
          std::map<uint64_t, uint8_t> Initial;
          for (int64_t Offset = -128; Offset != 8; ++Offset)
            Initial[Root + static_cast<uint64_t>(Offset)] =
                static_cast<uint8_t>(Offset * 13);
          auto Expected = Initial;
          for (unsigned Byte = 0; Byte != Width; ++Byte)
            Expected[Root - 99 + Byte] =
                static_cast<uint8_t>(Value >> (Byte * 8));
          Expected[Root - 64 + Index] = 0xa5;
          std::map<uint64_t, uint8_t> Actual;
          EXPECT_EQ(execute(R.Residual, {{8, Value}, {16, Index}, {32, Root}},
                            Initial, 0, {}, std::nullopt, {}, &Actual),
                    29u);
          EXPECT_EQ(Actual, Expected);
        }
  }
}

TEST(InterpreterSpecialization, SavedScalarSurvivesConditionalWrite) {
  auto P = savedScalarProgram();
  auto Ops = P.Code.at(0x100).Ops;
  Ops[6] =
      atomicCmpxchg(reg(80, 1), reg(56), constant(0x11, 1), constant(0xa5, 1));
  P.add(0x100, Ops);
  const auto R = specializeInterpreter(P, {0x100}, separatedStoreOptions());
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  ASSERT_EQ(count(R.Residual, NdOp::ATOMIC_CMPXCHG), 1u);
  constexpr uint64_t Root = 0x1008;
  constexpr uint64_t Value = UINT64_C(0x123456789abcdef0);
  for (uint64_t Index = 0; Index != 16; ++Index)
    for (uint8_t Old : {0x11, 0x22}) {
      WideByteOracle Actual(llvm::endianness::little);
      Actual.word(reg(8), Value);
      Actual.word(reg(16), Index);
      Actual.word(reg(32), Root);
      for (int64_t Offset = -128; Offset != 8; ++Offset)
        Actual.Memory[Root + static_cast<uint64_t>(Offset)] =
            static_cast<uint8_t>(Offset * 13);
      Actual.Memory[Root - 64 + Index] = Old;
      auto Expected = Actual.Memory;
      for (unsigned Byte = 0; Byte != 8; ++Byte)
        Expected[Root - 99 + Byte] = static_cast<uint8_t>(Value >> (Byte * 8));
      Expected[Root - 64 + Index] = Old == 0x11 ? 0xa5 : Old;
      ASSERT_TRUE(Actual.run(R.Residual)) << Actual.Diagnostic;
      EXPECT_EQ(Actual.number(reg(0)), 29u);
      EXPECT_EQ(Actual.Memory, Expected);
    }
}

TEST(InterpreterSpecialization, SavedScalarRejectsPossiblePartialOverlap) {
  auto P = savedScalarProgram(8, -100);
  const auto R = specializeInterpreter(P, {0x100}, separatedStoreOptions());
  EXPECT_FALSE(R.complete());
  EXPECT_TRUE(R.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, SavedScalarDoesNotResurrectOverwrittenValue) {
  auto P = savedScalarProgram();
  auto Ops = P.Code.at(0x100).Ops;
  Ops.insert(Ops.begin() + 7, op(NdOp::STORE, {}, {reg(40), constant(1)}));
  P.add(0x100, Ops);
  const auto R = specializeInterpreter(P, {0x100}, separatedStoreOptions());
  EXPECT_FALSE(R.complete());
  EXPECT_TRUE(R.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, SavedScalarConstantSurvivesProjection) {
  auto P = savedScalarProgram();
  auto Ops = P.Code.at(0x100).Ops;
  Ops[1] = op(NdOp::STORE, {}, {reg(40), constant(0x110)});
  Ops.resize(7);
  Ops.push_back(jump(0x120));
  P.add(0x100, Ops);
  P.add(0x120, {op(NdOp::INT_SUB, reg(40), {reg(32), constant(99)}),
                op(NdOp::LOAD, reg(48), {reg(40)}),
                op(NdOp::INDIR_BR, {}, {reg(48)})});
  auto O = separatedStoreOptions();
  O.MaxChainedTransfers = 0;
  const auto R = specializeInterpreter(P, {0x100}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  for (uint64_t Index = 0; Index != 16; ++Index)
    EXPECT_EQ(execute(R.Residual, {{8, UINT64_MAX}, {16, Index}, {32, 0x1008}}),
              29u);
}

TEST(InterpreterSpecialization, SavedScalarDoesNotGrantExternalAliasContract) {
  auto P = savedScalarProgram();
  auto Ops = P.Code.at(0x100).Ops;
  Ops.resize(8);
  Ops.push_back(op(NdOp::STORE, {}, {reg(48), constant(0x33)}));
  Ops.push_back(op(NdOp::COPY, reg(0), {constant(29)}));
  Ops.push_back(ret());
  P.add(0x100, Ops);
  auto O = separatedStoreOptions();
  const auto Strict = specializeInterpreter(P, {0x100}, O);
  EXPECT_FALSE(Strict.complete());
  EXPECT_EQ(Strict.Status, SpecializationStatus::Unsupported);
  EXPECT_NE(Strict.Diagnostic.find("nonalias contract"), std::string::npos);
  O.ExternalStoresPreserveEntryReturnSlot = true;
  const auto Conditional = specializeInterpreter(P, {0x100}, O);
  ASSERT_TRUE(Conditional.complete()) << Conditional.Diagnostic;
  std::map<uint64_t, uint8_t> Actual;
  EXPECT_EQ(execute(Conditional.Residual, {{8, 0x3000}, {16, 7}, {32, 0x1008}},
                    {}, 0, {}, std::nullopt, {}, &Actual),
            29u);
  EXPECT_EQ(Actual.at(0x3000), 0x33);
  for (unsigned Byte = 1; Byte != 8; ++Byte)
    EXPECT_EQ(Actual.at(0x3000 + Byte), 0);
}

TEST(InterpreterSpecialization, SavedScalarChargesExactBudgets) {
  auto P = savedScalarProgram();
  auto O = separatedStoreOptions();
  const auto R = specializeInterpreter(P, {0x100}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  ASSERT_GT(R.SolverQueries, 0u);
  O.MaxOperations = R.EvaluatedOperations;
  O.MaxSolverQueries = R.SolverQueries;
  EXPECT_TRUE(specializeInterpreter(P, {0x100}, O).complete());
  --O.MaxOperations;
  const auto ShortWork = specializeInterpreter(P, {0x100}, O);
  EXPECT_EQ(ShortWork.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_TRUE(ShortWork.Residual.Blocks.empty());
  ++O.MaxOperations;
  --O.MaxSolverQueries;
  const auto ShortQueries = specializeInterpreter(P, {0x100}, O);
  EXPECT_EQ(ShortQueries.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_TRUE(ShortQueries.Residual.Blocks.empty());
}
