//===- AbsoluteMemoryFactsTests.inc - Current memory fact recovery -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Checks proved per-write separation and intersection of current bytes.
///
//===----------------------------------------------------------------------===//

TEST(InterpreterSpecialization, WrittenAbsoluteTargetsSurviveNodeProjection) {
  Provider P;
  P.add(0x100, {op(NdOp::STORE, {}, {constant(0x4080), constant(0x120)}),
                jump(0x110)});
  P.add(0x110, {op(NdOp::LOAD, reg(16), {constant(0x4080)}),
                op(NdOp::INDIR_BR, {}, {reg(16)})});
  P.add(0x120, {op(NdOp::COPY, reg(0), {constant(7)}), ret()});
  for (auto Order : {llvm::endianness::little, llvm::endianness::big})
    for (uint32_t Chain : {0, 8}) {
      SpecializationOptions O;
      O.ByteOrder = Order;
      O.MaxChainedTransfers = Chain;
      const auto R = specializeInterpreter(P, {0x100}, O);
      ASSERT_TRUE(R.complete()) << R.Diagnostic;
      EXPECT_EQ(count(R.Residual, NdOp::STORE), 1u);
      EXPECT_EQ(count(R.Residual, NdOp::LOAD), 1u);
      WideByteOracle Actual(Order);
      ASSERT_TRUE(Actual.run(R.Residual)) << Actual.Diagnostic;
      EXPECT_EQ(Actual.number(reg(0)), 7u);
    }
}

TEST(InterpreterSpecialization, AbsoluteFactsJoinByIntersection) {
  for (bool Agree : {false, true}) {
    Provider P;
    P.add(0x100, {op(NdOp::COND_BR, {}, {constant(0x110), reg(8, 1)})}, 0x120);
    P.add(0x110, {op(NdOp::STORE, {}, {constant(0x4080), constant(0x140)}),
                  jump(0x130)});
    P.add(0x120, {op(NdOp::STORE, {},
                     {constant(0x4080), constant(Agree ? 0x140 : 0x150)}),
                  jump(0x130)});
    P.add(0x130, {op(NdOp::LOAD, reg(16), {constant(0x4080)}),
                  op(NdOp::INDIR_BR, {}, {reg(16)})});
    P.add(0x140, {op(NdOp::COPY, reg(0), {constant(7)}), ret()});
    P.add(0x150, {op(NdOp::COPY, reg(0), {constant(9)}), ret()});
    const auto R = specializeInterpreter(P, {0x100});
    if (Agree) {
      ASSERT_TRUE(R.complete()) << R.Diagnostic;
      for (uint64_t Choice : {0, 1})
        EXPECT_EQ(execute(R.Residual, {{8, Choice}}), 7u);
    } else {
      EXPECT_EQ(R.Status, SpecializationStatus::UnresolvedControl);
      EXPECT_TRUE(R.Residual.Blocks.empty());
    }
  }
}

TEST(InterpreterSpecialization, AliasingWritesRetireAbsoluteTargets) {
  for (bool Absolute : {false, true}) {
    Provider P;
    P.add(0x100, {op(NdOp::STORE, {}, {constant(0x4080), constant(0x120)}),
                  jump(0x110)});
    P.add(0x110, {op(NdOp::STORE, {},
                     {Absolute ? constant(0x407f) : reg(8), reg(24, 2)}),
                  op(NdOp::LOAD, reg(16), {constant(0x4080)}),
                  op(NdOp::INDIR_BR, {}, {reg(16)})});
    P.add(0x120, {ret()});
    const auto R = specializeInterpreter(P, {0x100});
    EXPECT_EQ(R.Status, SpecializationStatus::UnresolvedControl)
        << R.Diagnostic;
    EXPECT_TRUE(R.Residual.Blocks.empty());
  }
}

TEST(InterpreterSpecialization, ProvedStackWritesRetainAbsoluteTargets) {
  Provider P;
  P.add(0x100, {op(NdOp::STORE, {}, {constant(0x4080), constant(0x120)}),
                op(NdOp::INT_SUB, reg(40), {reg(32), constant(16)}),
                op(NdOp::STORE, {}, {reg(40), constant(7)}), jump(0x110)});
  P.add(0x110, {op(NdOp::LOAD, reg(0), {reg(40)}),
                op(NdOp::LOAD, reg(16), {constant(0x4080)}),
                op(NdOp::INDIR_BR, {}, {reg(16)})});
  P.add(0x120, {ret()});
  SpecializationOptions O;
  O.FrameBaseRegister = SymRegisterRange{32, 8};
  O.EntryFrameBounds = SpecializationEntryFrameBounds{-32, 8};
  O.EntryFrameBounds->ExcludedAddressRanges = {{0x4000, 0x4100}};
  EXPECT_FALSE(specializeInterpreter(P, {0x100}, O).complete());
  O.ProveStoreFrameSeparation = true;
  for (auto Order : {llvm::endianness::little, llvm::endianness::big})
    for (uint32_t Chain : {0, 8}) {
      O.ByteOrder = Order;
      O.MaxChainedTransfers = Chain;
      const auto R = specializeInterpreter(P, {0x100}, O);
      ASSERT_TRUE(R.complete()) << R.Diagnostic;
      for (uint64_t Stack : {0x3ff8, 0x4128}) {
        WideByteOracle Actual(Order);
        Actual.word(reg(32), Stack);
        ASSERT_TRUE(Actual.run(R.Residual)) << Actual.Diagnostic;
        EXPECT_EQ(Actual.number(reg(0)), 7u);
      }
      auto MayAlias = O;
      MayAlias.EntryFrameBounds->ExcludedAddressRanges.clear();
      const auto Lost = specializeInterpreter(P, {0x100}, MayAlias);
      EXPECT_EQ(Lost.Status, SpecializationStatus::UnresolvedControl);
      EXPECT_TRUE(Lost.Residual.Blocks.empty());
      auto Short = O;
      Short.MaxOperations = R.EvaluatedOperations - 1;
      const auto Limited = specializeInterpreter(P, {0x100}, Short);
      EXPECT_EQ(Limited.Status, SpecializationStatus::BudgetExceeded);
      EXPECT_TRUE(Limited.Residual.Blocks.empty());
    }
}
