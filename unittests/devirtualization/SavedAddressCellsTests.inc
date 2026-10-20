//===- SavedAddressCellsTests.inc - Saved frame cells
//-----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Preserve saved frame addresses only across individually separated writes.
///
//===----------------------------------------------------------------------===//

Provider savedFrameAddressProgram(int64_t WriteOffset = -64, uint64_t Mask = 15,
                                  uint16_t Width = 1) {
  Provider P;
  P.add(0x100, {op(NdOp::INT_SUB, reg(40), {reg(32), constant(96)}),
                op(NdOp::INT_SUB, reg(48), {reg(32), constant(160)}),
                op(NdOp::STORE, {}, {reg(40), reg(48)}),
                op(NdOp::COPY, reg(48), {constant(0)}),
                op(NdOp::INT_AND, reg(16), {reg(8), constant(Mask)}),
                op(NdOp::INT_ADD, reg(56),
                   {reg(32), constant(static_cast<uint64_t>(WriteOffset))}),
                op(NdOp::INT_ADD, reg(56), {reg(56), reg(16)}),
                op(NdOp::STORE, {}, {reg(56), constant(0x5a, Width)}),
                op(NdOp::LOAD, reg(48), {reg(40)}),
                op(NdOp::STORE, {}, {reg(48), constant(23)}),
                op(NdOp::LOAD, reg(0), {reg(48)}), ret()});
  return P;
}

TEST(InterpreterSpecialization, SavedFrameAddressSurvivesIndexedWrite) {
  auto P = savedFrameAddressProgram();
  const auto R = specializeInterpreter(P, {0x100}, separatedStoreOptions());
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_EQ(count(R.Residual, NdOp::STORE), 3u);
  for (uint64_t Root : {UINT64_C(8), UINT64_C(0x1008),
                        UINT64_C(0x8000000000000008), UINT64_MAX - 7})
    for (uint64_t Input : {UINT64_C(0), UINT64_C(1), UINT64_C(7), UINT64_C(15),
                           UINT64_C(16), UINT64_MAX}) {
      std::map<uint64_t, uint8_t> Initial;
      for (int64_t Offset = -192; Offset != 8; ++Offset)
        Initial[Root + static_cast<uint64_t>(Offset)] =
            static_cast<uint8_t>(Offset * 13);
      auto Expected = Initial;
      const uint64_t Pointer = Root - 160;
      for (unsigned Byte = 0; Byte != 8; ++Byte) {
        Expected[Root - 96 + Byte] =
            static_cast<uint8_t>(Pointer >> (Byte * 8));
        Expected[Root - 160 + Byte] = Byte == 0 ? 23 : 0;
      }
      Expected[Root - 64 + (Input & 15)] = 0x5a;
      std::map<uint64_t, uint8_t> Actual;
      EXPECT_EQ(execute(R.Residual, {{8, Input}, {32, Root}}, Initial, 0, {},
                        std::nullopt, {}, &Actual),
                23u);
      EXPECT_EQ(Actual, Expected);
    }
}

TEST(InterpreterSpecialization, SavedFrameAddressSurvivesConditionalWrite) {
  auto P = savedFrameAddressProgram();
  auto &Ops = P.Code.at(0x100).Ops;
  Ops[7] =
      atomicCmpxchg(reg(80, 1), reg(56), constant(0x11, 1), constant(0x5a, 1));
  P.add(0x100, Ops);
  const auto R = specializeInterpreter(P, {0x100}, separatedStoreOptions());
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  ASSERT_EQ(count(R.Residual, NdOp::ATOMIC_CMPXCHG), 1u);
  constexpr uint64_t Root = 0x1008;
  for (uint64_t Input = 0; Input != 16; ++Input)
    for (uint8_t Old : {0x11, 0x22}) {
      std::map<uint64_t, uint8_t> Initial;
      for (int64_t Offset = -192; Offset != 8; ++Offset)
        Initial[Root + static_cast<uint64_t>(Offset)] =
            static_cast<uint8_t>(Offset * 13);
      Initial[Root - 64 + Input] = Old;
      auto Expected = Initial;
      for (unsigned Byte = 0; Byte != 8; ++Byte) {
        Expected[Root - 96 + Byte] =
            static_cast<uint8_t>((Root - 160) >> (Byte * 8));
        Expected[Root - 160 + Byte] = Byte == 0 ? 23 : 0;
      }
      Expected[Root - 64 + Input] = Old == 0x11 ? 0x5a : Old;
      WideByteOracle Actual(llvm::endianness::little);
      Actual.word(reg(8), Input);
      Actual.word(reg(32), Root);
      Actual.Memory = Initial;
      ASSERT_TRUE(Actual.run(R.Residual)) << Actual.Diagnostic;
      EXPECT_EQ(Actual.number(reg(0)), 23u);
      EXPECT_EQ(Actual.Memory, Expected);
    }
}

TEST(InterpreterSpecialization,
     SavedFrameAddressRejectsPossiblePartialOverlap) {
  for (const auto &[Offset, Mask, Width] :
       {std::tuple<int64_t, uint64_t, uint16_t>{-104, 7, 2},
        std::tuple<int64_t, uint64_t, uint16_t>{-100, 15, 4}}) {
    auto P = savedFrameAddressProgram(Offset, Mask, Width);
    const auto R = specializeInterpreter(P, {0x100}, separatedStoreOptions());
    EXPECT_FALSE(R.complete());
    EXPECT_EQ(R.Status, SpecializationStatus::Unsupported);
    EXPECT_TRUE(R.Residual.Blocks.empty());
  }
}

TEST(InterpreterSpecialization,
     SavedFrameAddressDoesNotResurrectOverwrittenCell) {
  auto P = savedFrameAddressProgram();
  auto &Ops = P.Code.at(0x100).Ops;
  Ops.insert(Ops.begin() + 8, op(NdOp::STORE, {}, {reg(40), constant(1)}));
  P.add(0x100, Ops);
  const auto R = specializeInterpreter(P, {0x100}, separatedStoreOptions());
  EXPECT_FALSE(R.complete());
  EXPECT_EQ(R.Status, SpecializationStatus::Unsupported);
  EXPECT_TRUE(R.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, SavedFrameAddressKeepsCellsButNotTheirGap) {
  Provider P;
  constexpr uint64_t Initial = UINT64_C(0x1122334455667788);
  P.add(0x100,
        {op(NdOp::INT_SUB, reg(40), {reg(32), constant(128)}),
         op(NdOp::STORE, {}, {reg(40), reg(32)}),
         op(NdOp::INT_SUB, reg(48), {reg(32), constant(64)}),
         op(NdOp::INT_SUB, reg(56), {reg(32), constant(160)}),
         op(NdOp::STORE, {}, {reg(48), reg(56)}),
         op(NdOp::INT_SUB, reg(64), {reg(32), constant(96)}),
         op(NdOp::STORE, {}, {reg(64), constant(Initial)}),
         op(NdOp::INT_AND, reg(16), {reg(8), constant(7)}),
         op(NdOp::INT_ADD, reg(72), {reg(64), reg(16)}),
         op(NdOp::STORE, {}, {reg(72), constant(0xa5, 1)}),
         op(NdOp::LOAD, reg(56), {reg(48)}),
         op(NdOp::STORE, {}, {reg(56), constant(77)}),
         op(NdOp::LOAD, reg(32), {reg(40)}), op(NdOp::LOAD, reg(80), {reg(64)}),
         op(NdOp::INT_EQUAL, reg(88, 1), {reg(80), constant(Initial)}),
         op(NdOp::COND_BR, {}, {constant(0x110), reg(88, 1)})},
        0x120);
  P.add(0x110, {op(NdOp::COPY, reg(0), {constant(1)}), ret()});
  P.add(0x120, {op(NdOp::COPY, reg(0), {constant(2)}), ret()});
  const auto R = specializeInterpreter(P, {0x100}, separatedStoreOptions());
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_GT(count(R.Residual, NdOp::COND_BR), 0u);
  for (uint64_t Input = 0; Input != 8; ++Input)
    EXPECT_EQ(execute(R.Residual, {{8, Input}, {32, 0x1008}}), 2u);
}

TEST(InterpreterSpecialization, SavedFrameAddressChargesExactBudgets) {
  auto P = savedFrameAddressProgram();
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

TEST(InterpreterSpecialization, SavedFrameAddressSurvivesProjectionAndRestore) {
  Provider P;
  P.add(0x100,
        {op(NdOp::INT_SUB, reg(40), {reg(32), constant(64)}),
         op(NdOp::STORE, {}, {reg(40), reg(32)}),
         op(NdOp::INT_AND, reg(16), {reg(8), constant(15)}),
         op(NdOp::INT_SUB, reg(48), {reg(32), constant(32)}),
         op(NdOp::INT_ADD, reg(48), {reg(48), reg(16)}),
         op(NdOp::STORE, {}, {reg(48), constant(0x5a, 1)}), jump(0x110)});
  P.add(0x110, {op(NdOp::INT_SUB, reg(40), {reg(32), constant(64)}),
                op(NdOp::LOAD, reg(32), {reg(40)}),
                op(NdOp::COPY, reg(0), {constant(29)}), ret()});
  for (uint32_t Chain : {0, 8}) {
    auto O = separatedStoreOptions();
    O.MaxChainedTransfers = Chain;
    const auto R = specializeInterpreter(P, {0x100}, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    for (uint64_t Input = 0; Input != 16; ++Input)
      EXPECT_EQ(execute(R.Residual, {{8, Input}, {32, 0x1008}}), 29u);
  }
}
