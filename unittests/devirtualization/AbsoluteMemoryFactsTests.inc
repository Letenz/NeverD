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

TEST(InterpreterSpecialization, AbsoluteRangeUnionsRespectBoundedWork) {
  for (uint64_t Stride : {8u, 16u}) {
    Provider P;
    std::vector<LowOp> Writes;
    for (uint64_t I = 0; I != 64; ++I)
      Writes.push_back(op(NdOp::STORE, {},
                          {constant(0x4000 + I * Stride), constant(I + 1)}));
    Writes.push_back(op(NdOp::INT_SUB, reg(40), {reg(32), constant(16)}));
    Writes.push_back(op(NdOp::STORE, {}, {reg(40), constant(7)}));
    Writes.push_back(jump(0x110));
    P.add(0x100, std::move(Writes));
    std::vector<LowOp> Reads{op(NdOp::COPY, reg(0), {constant(0)})};
    for (uint64_t I = 0; I != 64; ++I) {
      Reads.push_back(op(NdOp::LOAD, reg(16), {constant(0x4000 + I * Stride)}));
      Reads.push_back(op(NdOp::INT_ADD, reg(0), {reg(0), reg(16)}));
    }
    Reads.push_back(op(NdOp::INDIR_BR, {}, {reg(0)}));
    P.add(0x110, std::move(Reads));
    P.add(2080,
          {ret()}); // Sum of the independently stored values 1 through 64.
    SpecializationOptions O;
    O.FrameBaseRegister = SymRegisterRange{32, 8};
    O.EntryFrameBounds = SpecializationEntryFrameBounds{-32, 8};
    O.EntryFrameBounds->ExcludedAddressRanges = {{0x4000, 0x4400}};
    O.ProveStoreFrameSeparation = true;
    // A bounded batch of current cells must not repeatedly scan the whole
    // memory bank through independent descriptors for the same base.
    O.MaxOperations = 24576;
    for (auto Order : {llvm::endianness::little, llvm::endianness::big})
      for (uint32_t Chain : {0, 8}) {
        O.ByteOrder = Order;
        O.MaxChainedTransfers = Chain;
        const auto R = specializeInterpreter(P, {0x100}, O);
        ASSERT_TRUE(R.complete()) << R.Diagnostic;
        for (uint64_t Stack : {0x3ff8, 0x4428}) {
          WideByteOracle Actual(Order);
          Actual.word(reg(32), Stack);
          ASSERT_TRUE(Actual.run(R.Residual)) << Actual.Diagnostic;
          EXPECT_EQ(Actual.number(reg(0)), 2080u);
        }
        auto MayAlias = O;
        MayAlias.EntryFrameBounds->ExcludedAddressRanges.clear();
        EXPECT_FALSE(specializeInterpreter(P, {0x100}, MayAlias).complete());
      }
  }
}

TEST(InterpreterSpecialization,
     FrameExclusionsRequireCompleteWriteAndCellBounds) {
  struct Case {
    int64_t Displacement;
    uint64_t ExcludedBegin;
    uint64_t ExcludedEnd;
    uint64_t CounterexampleStack;
  };
  // Each seed obeys the frame exclusion but lets the write overlap the target.
  const Case Cases[] = {
      {-40, 0x4000, 0x4008, 0x4028}, {8, 0x4000, 0x4008, 0x3ff8},
      {-36, 0x4000, 0x4008, 0x4028}, {4, 0x4000, 0x4008, 0x3ff8},
      {-32, 0x4000, 0x4004, 0x4024}, {0, 0x4004, 0x4008, 0x3ffc}};
  for (const auto &C : Cases) {
    const uint64_t Store = C.CounterexampleStack + C.Displacement;
    ASSERT_TRUE(C.CounterexampleStack + 8 <= C.ExcludedBegin ||
                C.CounterexampleStack - 32 >= C.ExcludedEnd);
    ASSERT_TRUE(Store < 0x4008 && Store + 8 > 0x4000);
    Provider P;
    P.add(0x100,
          {op(NdOp::STORE, {}, {constant(0x4000), constant(0x120)}),
           op(NdOp::INT_ADD, reg(40),
              {reg(32), constant(static_cast<uint64_t>(C.Displacement))}),
           op(NdOp::STORE, {}, {reg(40), reg(8)}), jump(0x110)});
    P.add(0x110, {op(NdOp::LOAD, reg(16), {constant(0x4000)}),
                  op(NdOp::INDIR_BR, {}, {reg(16)})});
    P.add(0x120, {ret()});
    SpecializationOptions O;
    O.FrameBaseRegister = SymRegisterRange{32, 8};
    O.EntryFrameBounds = SpecializationEntryFrameBounds{-32, 8};
    O.EntryFrameBounds->ExcludedAddressRanges = {
        {C.ExcludedBegin, C.ExcludedEnd}};
    O.ProveStoreFrameSeparation = true;
    for (auto Order : {llvm::endianness::little, llvm::endianness::big})
      for (uint32_t Chain : {0, 8}) {
        O.ByteOrder = Order;
        O.MaxChainedTransfers = Chain;
        const auto R = specializeInterpreter(P, {0x100}, O);
        EXPECT_EQ(R.Status, SpecializationStatus::UnresolvedControl)
            << C.Displacement << ": " << R.Diagnostic;
        EXPECT_TRUE(R.Residual.Blocks.empty());
      }
  }
}
