//===- WideImmutableImageTests.inc - Complete PE read witnesses ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Fixed-image permissions, complete spans and upper-byte integrity.
// Included in the fixed-PE fixture's test namespace.

TEST_F(PEFixedImageTest, RawCodeWideReadRequiresCompleteImmutableSpan) {
  for (unsigned Case = 0; Case != 3; ++Case) {
    SCOPED_TRACE(Case);
    auto Bytes = fixture();
    // movdqu xmm0,[rip+data]; ret. No relocation writes the vector or code.
    directory(Bytes, BASE_RELOCATION_TABLE, 0, 0);
    const uint8_t Code[] = {0xf3, 0x0f, 0x6f, 0x05, 0, 0, 0, 0, 0xc3};
    std::copy(std::begin(Code), std::end(Code), Bytes.begin() + Text);
    const unsigned Offset = Case == 2 ? 0x1f8 : 0x80;
    put32(Bytes, Text + 4, 0x2000 + Offset - 0x1008);
    for (unsigned I = 0; I != 16 && Offset + I < SectionBytes; ++I)
      Bytes[RData + Offset + I] = uint8_t(19 + 47 * I);
    if (Case == 1)
      put32(Bytes, SectionTable + 40 + 36,
            IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE |
                IMAGE_SCN_CNT_INITIALIZED_DATA);
    auto Loaded = load(Bytes);
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    auto O = rawOptions();
    const auto R = specializeBinaryInterpreter(*Loaded, Base + 0x1000, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    auto Error = validateSpecializationRawEnvironment(R, O);
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    unsigned WideLoads = 0;
    for (const auto &Block : R.Residual.Blocks)
      for (const auto &Op : Block.Ops)
        WideLoads += Op.Opcode == NdOp::LOAD && Op.Output.Size == 16;
    if (Case) {
      EXPECT_EQ(WideLoads, 1U);
      EXPECT_TRUE(R.Reads.empty());
      continue;
    }
    EXPECT_EQ(WideLoads, 0U);
    ASSERT_EQ(R.Reads.size(), 1U);
    EXPECT_EQ(R.Reads[0].Address, Base + 0x2000 + Offset);
    ASSERT_EQ(R.Reads[0].Bytes.size(), 16U);
    for (unsigned I = 0; I != 16; ++I)
      EXPECT_EQ(R.Reads[0].Bytes[I], Bytes[RData + Offset + I]);
    auto Changed = R;
    Changed.Reads[0].Bytes[15] ^= 1;
    Error = validateSpecializationRawEnvironment(Changed, O);
    ASSERT_TRUE(bool(Error));
    EXPECT_NE(llvm::toString(std::move(Error)).find("evidence"),
              std::string::npos);
  }
}
