//===- WideImmutableReadTests.inc - Complete immutable reads ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independently check every output byte, address overlap and witness boundary.
//===----------------------------------------------------------------------===//

TEST(InterpreterSpecialization, WideImmutableReadsPreserveBytesAndEndian) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    Provider P;
    const auto Vector = reg(x86reg::XMM0, 16);
    for (unsigned I = 0; I < 16; ++I)
      P.Image[0x4000 + I] = uint8_t(7 + 31 * I);
    P.add(0x100, {op(NdOp::LOAD, Vector, {constant(0x4000)}),
                  op(NdOp::STORE, {}, {constant(0x5000), Vector}), ret()});
    auto O = wideOptions();
    O.ByteOrder = Order;
    const auto R = specializeInterpreter(P, {0x100}, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    EXPECT_EQ(count(R.Residual, NdOp::LOAD), 0u);
    ASSERT_EQ(R.Reads.size(), 1u);
    ASSERT_EQ(R.Reads[0].Bytes.size(), 16u);
    WideByteOracle Oracle(Order);
    // No runtime image bytes are supplied: the complete witness must suffice.
    ASSERT_TRUE(Oracle.run(R.Residual)) << Oracle.Diagnostic;
    for (unsigned I = 0; I < 16; ++I) {
      EXPECT_EQ(Oracle.Memory[0x5000 + I], P.Image[0x4000 + I]);
      EXPECT_EQ(R.Reads[0].Bytes[I], P.Image[0x4000 + I]);
    }
  }
}

TEST(InterpreterSpecialization, WideImmutableReadsCaptureOverlappingAddress) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big})
    for (uint64_t AddressOffset : {16u, 20u, 24u})
      for (bool Uniform : {false, true}) {
        Provider P;
        const auto Address = reg(AddressOffset), Vector = reg(16, 16);
        for (unsigned Choice = 0; Choice < 2; ++Choice)
          for (unsigned I = 0; I < 16; ++I)
            P.Image[0x4000 + 32 * Choice + I] =
                uint8_t(7 + 31 * I + (Uniform || I < 8 ? 0 : 97 * Choice));
        P.add(0x100, {op(NdOp::INT_AND, Address, {reg(0), constant(1)}),
                      op(NdOp::INT_MULT, Address, {Address, constant(32)}),
                      op(NdOp::INT_ADD, Address, {Address, constant(0x4000)}),
                      op(NdOp::LOAD, Vector, {Address}),
                      op(NdOp::STORE, {}, {constant(0x5000), Vector}), ret()});
        auto O = wideOptions();
        O.ByteOrder = Order;
        const auto R = specializeInterpreter(P, {0x100}, O);
        ASSERT_TRUE(R.complete()) << R.Diagnostic;
        EXPECT_EQ(count(R.Residual, NdOp::LOAD), 0u);
        EXPECT_EQ(count(R.Residual, NdOp::SELECT), Uniform ? 0u : 2u);
        ASSERT_EQ(R.Reads.size(), 2u);
        for (unsigned Choice = 0; Choice < 2; ++Choice) {
          WideByteOracle Oracle(Order);
          Oracle.word(reg(0), Choice);
          ASSERT_TRUE(Oracle.run(R.Residual)) << Oracle.Diagnostic;
          for (unsigned I = 0; I < 16; ++I)
            EXPECT_EQ(Oracle.Memory[0x5000 + I],
                      P.Image[0x4000 + 32 * Choice + I]);
        }
      }
}

TEST(InterpreterSpecialization, WideImmutableUpperHalfResolvesControl) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    Provider P;
    const auto Address = reg(16), Vector = reg(x86reg::XMM0, 16);
    for (unsigned Choice = 0; Choice < 2; ++Choice) {
      const uint64_t Target = 0x200 + 16 * Choice;
      for (unsigned I = 0; I < 16; ++I) {
        const unsigned Byte = Order == llvm::endianness::little ? I : 15 - I;
        P.Image[0x4000 + 32 * Choice + I] =
            Byte < 8 ? uint8_t(29 + 17 * Byte)
                     : uint8_t(Target >> (8 * (Byte - 8)));
      }
    }
    P.add(0x100, {op(NdOp::INT_AND, Address, {reg(0), constant(1)}),
                  op(NdOp::INT_MULT, Address, {Address, constant(32)}),
                  op(NdOp::INT_ADD, Address, {Address, constant(0x4000)}),
                  op(NdOp::LOAD, Vector, {Address}),
                  op(NdOp::SUBBYTES, reg(8), {Vector, constant(8)}),
                  op(NdOp::INDIR_BR, {}, {reg(8)})});
    P.add(0x200, {op(NdOp::COPY, reg(0), {constant(17)}), ret()});
    P.add(0x210, {op(NdOp::COPY, reg(0), {constant(31)}), ret()});
    auto O = wideOptions();
    O.ByteOrder = Order;
    const auto R = specializeInterpreter(P, {0x100}, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    EXPECT_EQ(count(R.Residual, NdOp::LOAD), 0u);
    for (unsigned Choice = 0; Choice < 2; ++Choice) {
      WideByteOracle Oracle(Order);
      Oracle.word(reg(0), Choice);
      ASSERT_TRUE(Oracle.run(R.Residual)) << Oracle.Diagnostic;
      EXPECT_EQ(Oracle.number(reg(0)), Choice ? 31u : 17u);
    }
  }
}

TEST(InterpreterSpecialization, WideImmutablePartialDomainKeepsWholeRead) {
  for (bool LimitAddresses : {false, true}) {
    Provider P;
    const auto Address = reg(16), Vector = reg(x86reg::XMM0, 16);
    std::map<uint64_t, uint8_t> Actual;
    for (unsigned Choice = 0; Choice < 2; ++Choice)
      for (unsigned I = 0; I < 16; ++I) {
        const uint64_t At = 0x4000 + 32 * Choice + I;
        Actual[At] = uint8_t(7 + 31 * I + 97 * Choice);
        if (LimitAddresses || Choice == 0 || I < 15)
          P.Image[At] = Actual[At];
      }
    P.add(0x100, {op(NdOp::INT_AND, Address, {reg(0), constant(1)}),
                  op(NdOp::INT_MULT, Address, {Address, constant(32)}),
                  op(NdOp::INT_ADD, Address, {Address, constant(0x4000)}),
                  op(NdOp::LOAD, Vector, {Address}),
                  op(NdOp::STORE, {}, {constant(0x5000), Vector}), ret()});
    auto O = wideOptions();
    if (LimitAddresses)
      O.MaxImmutableReadAddresses = 1;
    const auto R = specializeInterpreter(P, {0x100}, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    EXPECT_EQ(count(R.Residual, NdOp::LOAD), 1u);
    EXPECT_TRUE(R.Reads.empty());
    for (unsigned Choice = 0; Choice < 2; ++Choice) {
      WideByteOracle Oracle(llvm::endianness::little);
      Oracle.Memory = Actual;
      Oracle.word(reg(0), Choice);
      ASSERT_TRUE(Oracle.run(R.Residual)) << Oracle.Diagnostic;
      for (unsigned I = 0; I < 16; ++I)
        EXPECT_EQ(Oracle.Memory[0x5000 + I], Actual[0x4000 + 32 * Choice + I]);
    }
  }
}

TEST(InterpreterSpecialization, WideImmutableMalformedExtentRefuses) {
  class SizedProvider : public Provider {
  public:
    unsigned CertificateBytes = 0;
    std::optional<SpecializationImmutableRead>
    immutableRead(va_t Address, uint16_t Size) override {
      auto R = Provider::immutableRead(Address, Size);
      if (R)
        R->Bytes.resize(CertificateBytes);
      return R;
    }
  };
  for (unsigned Bytes : {0u, 8u, 15u, 17u}) {
    SizedProvider P;
    P.CertificateBytes = Bytes;
    for (unsigned I = 0; I < 16; ++I)
      P.Image[0x4000 + I] = uint8_t(I);
    P.add(0x100,
          {op(NdOp::LOAD, reg(x86reg::XMM0, 16), {constant(0x4000)}), ret()});
    const auto R = specializeInterpreter(P, {0x100}, wideOptions());
    EXPECT_EQ(R.Status, SpecializationStatus::InvalidInput);
    EXPECT_NE(R.Diagnostic.find("invalid byte extent"), std::string::npos);
    EXPECT_TRUE(R.Residual.Blocks.empty());
    EXPECT_TRUE(R.Reads.empty());
  }
}

TEST(InterpreterSpecialization, WideImmutableLoweringChargesEveryPart) {
  for (bool Multiple : {false, true}) {
    Provider P;
    for (unsigned I = 0; I < 48; ++I)
      P.Image[0x4000 + I] = uint8_t(19 + 31 * I);
    const auto Address = reg(16);
    std::vector<LowOp> Ops;
    if (Multiple) {
      Ops.push_back(op(NdOp::INT_AND, Address, {reg(0), constant(1)}));
      Ops.push_back(op(NdOp::INT_MULT, Address, {Address, constant(32)}));
      Ops.push_back(op(NdOp::INT_ADD, Address, {Address, constant(0x4000)}));
    }
    Ops.push_back(op(NdOp::LOAD, reg(x86reg::XMM0, 16),
                     {Multiple ? Address : constant(0x4000)}));
    Ops.push_back(ret());
    P.add(0x100, std::move(Ops));
    auto O = wideOptions();
    const auto R = specializeInterpreter(P, {0x100}, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    ASSERT_EQ(count(R.Residual, NdOp::LOAD), 0u);
    // Constant: two scalar copies and RETURN. Finite: three address operations,
    // captured address, two initial copies, comparison, two selects and RETURN.
    const uint64_t ExpectedOperations = Multiple ? 10 : 3;
    EXPECT_EQ(R.EvaluatedOperations, ExpectedOperations);
    O.MaxOperations = ExpectedOperations;
    EXPECT_TRUE(specializeInterpreter(P, {0x100}, O).complete());
    --O.MaxOperations;
    const auto Short = specializeInterpreter(P, {0x100}, O);
    EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded);
    EXPECT_TRUE(Short.Residual.Blocks.empty());
    EXPECT_TRUE(Short.Reads.empty());
  }
}
