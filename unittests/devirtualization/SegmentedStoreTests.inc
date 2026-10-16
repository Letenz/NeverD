//===- SegmentedStoreTests.inc - Runtime-base write recovery ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Independent byte-memory and alias controls for segmented stores.

SpecializationOptions segmentedWriteOptions() {
  auto Options = segmentedReadOptions();
  Options.X64SegmentedReads = false;
  Options.X64SegmentedWrites = true;
  return Options;
}

LowOp segmentedStore(NdMemoryAddressSpace Space, NdVar Offset, NdVar Value) {
  auto Store = op(NdOp::STORE, {}, {Offset, Value});
  Store.MemoryAddressSpace = Space;
  return Store;
}

TEST(InterpreterSpecialization, SegmentedWriteRequiresIndependentContract) {
  for (auto Space :
       {NdMemoryAddressSpace::X86FS, NdMemoryAddressSpace::X86GS}) {
    Provider P;
    P.add(0x100, {segmentedStore(Space, constant(16), constant(7)),
                  op(NdOp::COPY, reg(0), {constant(1)}), ret()});
    EXPECT_EQ(specializeInterpreter(P, {0x100}, segmentedReadOptions()).Status,
              SpecializationStatus::Unsupported);
    auto Options = segmentedWriteOptions();
    const auto Good = specializeInterpreter(P, {0x100}, Options);
    ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
    EXPECT_EQ(count(Good.Residual, NdOp::STORE), 1U);
    for (unsigned Missing = 0; Missing != 2; ++Missing) {
      auto Bad = Options;
      if (Missing)
        Bad.NormalNonfaultingExecution = false;
      else
        Bad.ExplicitMachineState = false;
      const auto R = specializeInterpreter(P, {0x100}, Bad);
      EXPECT_EQ(R.Status, SpecializationStatus::InvalidInput);
      EXPECT_TRUE(R.Residual.Blocks.empty());
    }
    P.add(0x100, {segmentedLoad(Space, reg(0), constant(16)), ret()});
    EXPECT_EQ(specializeInterpreter(P, {0x100}, Options).Status,
              SpecializationStatus::Unsupported);
  }
}

TEST(InterpreterSpecialization,
     SegmentedWritePreservesCompleteBytesAndFrameAliases) {
  for (auto Space : {NdMemoryAddressSpace::X86FS, NdMemoryAddressSpace::X86GS})
    for (uint16_t Width : {1, 2, 4, 8})
      for (uint32_t Chain : {0, 16}) {
        SCOPED_TRACE(Width);
        SCOPED_TRACE(Chain);
        Provider P;
        P.add(0x100,
              {op(NdOp::INT_ADD, reg(40), {reg(32), constant(16)}),
               op(NdOp::STORE, {}, {reg(40), constant(0x1122334455667788)}),
               jump(0x110)});
        P.add(0x110,
              {segmentedStore(Space, reg(40), reg(8, Width)), jump(0x120)});
        P.add(0x120, {op(NdOp::LOAD, reg(0), {reg(40)}), ret()});
        auto Options = segmentedWriteOptions();
        Options.FrameBaseRegister = SymRegisterRange{32, 8};
        Options.MaxChainedTransfers = Chain;
        const auto R = specializeInterpreter(P, {0x100}, Options);
        ASSERT_TRUE(R.complete()) << R.Diagnostic;
        EXPECT_EQ(count(R.Residual, NdOp::STORE), 2U);
        EXPECT_TRUE(R.Reads.empty());
        for (uint64_t Base : {uint64_t{0}, uint64_t{0x100}, UINT64_MAX - 15})
          for (uint64_t Value :
               {uint64_t{0}, UINT64_MAX, uint64_t{0xfedcba9876543210}}) {
            std::map<uint64_t, uint8_t> Memory;
            for (uint64_t Address = 0x1ff0; Address != 0x2130; ++Address)
              Memory[Address] = static_cast<uint8_t>(Address * 37);
            auto Expected = Memory;
            for (unsigned B = 0; B != 8; ++B)
              Expected[0x2010 + B] = 0x1122334455667788ULL >> (8 * B);
            for (unsigned B = 0; B != Width; ++B)
              Expected[Base + 0x2010 + B] = Value >> (8 * B);
            uint64_t ExpectedReturn = 0;
            for (unsigned B = 0; B != 8; ++B)
              ExpectedReturn |= uint64_t{Expected[0x2010 + B]} << (8 * B);
            std::map<uint64_t, uint8_t> Actual;
            EXPECT_EQ(execute(R.Residual, {{32, 0x2000}, {8, Value}}, Memory, 0,
                              {{Space, Base}}, std::nullopt, {}, &Actual),
                      ExpectedReturn);
            EXPECT_EQ(Actual, Expected);
          }
        auto Exact = Options;
        Exact.MaxOperations = R.EvaluatedOperations;
        EXPECT_TRUE(specializeInterpreter(P, {0x100}, Exact).complete());
        ASSERT_GT(Exact.MaxOperations, 1U);
        --Exact.MaxOperations;
        const auto Short = specializeInterpreter(P, {0x100}, Exact);
        EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded);
        EXPECT_TRUE(Short.Residual.Blocks.empty());
      }
}

TEST(InterpreterSpecialization, SegmentedWriteInvalidatesAffineFrameCells) {
  for (auto Space : {NdMemoryAddressSpace::X86FS, NdMemoryAddressSpace::X86GS})
    for (uint32_t Chain : {0, 16}) {
      Provider P;
      P.add(0x100, {op(NdOp::INT_ADD, reg(40), {reg(32), constant(16)}),
                    op(NdOp::INT_ADD, reg(48), {reg(32), constant(32)}),
                    op(NdOp::STORE, {}, {reg(40), reg(48)}), jump(0x110)});
      P.add(0x110, {segmentedStore(Space, constant(0), constant(0x7000)),
                    jump(0x120)});
      P.add(0x120, {op(NdOp::LOAD, reg(56), {reg(40)}),
                    op(NdOp::LOAD, reg(0, 1), {reg(56)}),
                    op(NdOp::INT_ZEXT, reg(0), {reg(0, 1)}), ret()});
      auto Options = segmentedWriteOptions();
      Options.FrameBaseRegister = SymRegisterRange{32, 8};
      Options.MaxChainedTransfers = Chain;
      const auto R = specializeInterpreter(P, {0x100}, Options);
      ASSERT_TRUE(R.complete()) << R.Diagnostic;
      for (uint64_t Base : {0x2010U, 0x3000U})
        EXPECT_EQ(execute(R.Residual, {{32, 0x2000}},
                          {{0x2020, 7}, {0x7000, 19}}, 0, {{Space, Base}}),
                  Base == 0x2010 ? 19U : 7U);
    }
}

TEST(InterpreterSpecialization, SegmentedWritesAndReadsShareLinearMemory) {
  Provider P;
  P.add(0x100,
        {segmentedStore(NdMemoryAddressSpace::X86FS, constant(16), reg(8)),
         segmentedLoad(NdMemoryAddressSpace::X86GS, reg(0), constant(16)),
         ret()});
  auto Options = segmentedWriteOptions();
  Options.X64SegmentedReads = true;
  const auto R = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_EQ(count(R.Residual, NdOp::LOAD), 1U);
  for (uint64_t FS : {0x2000U, 0x3000U})
    for (uint64_t GS : {0x2000U, 0x3000U}) {
      std::map<uint64_t, uint8_t> Memory;
      for (uint64_t Base : {0x2000U, 0x3000U})
        for (unsigned B = 0; B != 8; ++B)
          Memory[Base + 16 + B] = B ? 0 : 17;
      EXPECT_EQ(execute(R.Residual, {{8, 29}}, Memory, 0,
                        {{NdMemoryAddressSpace::X86FS, FS},
                         {NdMemoryAddressSpace::X86GS, GS}}),
                FS == GS ? 29U : 17U);
    }
}

TEST(InterpreterSpecialization,
     SegmentedWriteCannotBorrowOffsetReturnOrFrameContracts) {
  for (auto Space : {NdMemoryAddressSpace::X86FS, NdMemoryAddressSpace::X86GS})
    for (bool FrameOffset : {false, true})
      for (bool Disjoint : {false, true})
        for (bool Prove : {false, true}) {
          Provider P;
          const auto Stack = reg(x86reg::RSP);
          P.add(0x100, {op(NdOp::INT_ADD, reg(64), {Stack, constant(16)}),
                        segmentedStore(Space, FrameOffset ? reg(64) : reg(8),
                                       constant(7)),
                        op(NdOp::COPY, reg(0), {constant(1)}), ret()});
          auto Options = segmentedWriteOptions();
          Options.FrameBaseRegister = SymRegisterRange{x86reg::RSP, 8};
          Options.RequireRestoredFrameAtReturn = true;
          Options.ExternalStoresPreserveEntryReturnSlot = true;
          Options.ProveStoreReturnSlotSeparation = Prove;
          Options.EntryFrameBounds = SpecializationEntryFrameBounds{-64, 64};
          Options.ExternalStoresDisjointEntryFrame = Disjoint;
          const auto R = specializeInterpreter(P, {0x100}, Options);
          EXPECT_EQ(R.Status, SpecializationStatus::Unsupported)
              << R.Diagnostic;
          EXPECT_NE(R.Diagnostic.find("address origin"), std::string::npos);
          EXPECT_TRUE(R.Residual.Blocks.empty());
        }
}

TEST(InterpreterSpecialization,
     SegmentedWriteInvalidatesUnsafeStoredPointerOrigins) {
  for (uint32_t Chain : {0, 16}) {
    Provider P;
    const auto Stack = reg(x86reg::RSP);
    P.add(0x100, {op(NdOp::INT_ADD, reg(64), {Stack, constant(16)}),
                  op(NdOp::STORE, {}, {reg(64), reg(8)}), jump(0x110)});
    P.add(0x110,
          {segmentedStore(NdMemoryAddressSpace::X86GS, constant(0), Stack),
           jump(0x120)});
    P.add(0x120, {op(NdOp::STORE, {}, {Stack, constant(23)}),
                  op(NdOp::LOAD, reg(72), {reg(64)}),
                  op(NdOp::STORE, {}, {reg(72), constant(7)}),
                  op(NdOp::LOAD, reg(0), {Stack}), ret()});
    auto Options = segmentedWriteOptions();
    Options.FrameBaseRegister = SymRegisterRange{x86reg::RSP, 8};
    Options.MaxChainedTransfers = Chain;
    Options.EntryFrameBounds = SpecializationEntryFrameBounds{0, 32};
    Options.ExternalStoresDisjointEntryFrame = true;
    // The later loaded pointer can be the frame root. Its original external
    // origin cannot survive the runtime-based overwrite and preserve 23.
    const auto R = specializeInterpreter(P, {0x100}, Options);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    std::map<uint64_t, uint8_t> Actual;
    const auto Value = execute(R.Residual, {{x86reg::RSP, 0x2000}, {8, 0x7000}},
                               {}, 0, {{NdMemoryAddressSpace::X86GS, 0x2010}},
                               std::nullopt, {}, &Actual);
    EXPECT_EQ(Value, 7U);
    ASSERT_EQ(Actual.size(), 16U);
    for (unsigned B = 0; B != 8; ++B) {
      EXPECT_EQ(Actual[0x2000 + B], B ? 0U : 7U);
      EXPECT_EQ(Actual[0x2010 + B], B == 1 ? 0x20U : 0U);
    }
  }
}

TEST(InterpreterSpecialization, SegmentedWriteRejectsUnsupportedShapes) {
  const auto Space = NdMemoryAddressSpace::X86GS;
  std::vector<LowOp> Bad{segmentedStore(Space, reg(8, 4), reg(16)),
                         segmentedStore(Space, reg(8), reg(16, 16)),
                         segmentedStore(Space, reg(8), reg(16, 3)),
                         op(NdOp::STORE, reg(0), {reg(8), reg(16)}),
                         op(NdOp::STORE, {}, {reg(8)}),
                         op(NdOp::STORE, {}, {reg(0), reg(8), reg(16)}),
                         op(NdOp::ATOMIC_XCHG, reg(0), {reg(8), reg(16)})};
  for (auto &Op : Bad)
    Op.MemoryAddressSpace = Space;
  for (auto Ordering :
       {NdMemoryOrdering::Release, NdMemoryOrdering::SequentiallyConsistent}) {
    auto Ordered = segmentedStore(Space, reg(8), reg(16));
    Ordered.MemoryOrdering = Ordering;
    Bad.push_back(Ordered);
  }
  Bad.push_back(
      segmentedStore(static_cast<NdMemoryAddressSpace>(255), reg(8), reg(16)));
  for (const auto &Store : Bad) {
    Provider P;
    P.add(0x100, {Store, ret()});
    const auto R = specializeInterpreter(P, {0x100}, segmentedWriteOptions());
    EXPECT_FALSE(R.complete());
    EXPECT_TRUE(R.Residual.Blocks.empty());
  }
}
