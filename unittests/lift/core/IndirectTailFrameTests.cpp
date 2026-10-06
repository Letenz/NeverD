//===- IndirectTailFrameTests.cpp - Native tail-transfer frame guards -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "IndirectTailFrame.h"
#include "NeverDLiftFixture.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/Support/Error.h"

#include <iterator>

namespace {

class IndirectTailFrame : public NeverDLiftTest {
protected:
  void SetUp() override {
    NeverDLiftTest::SetUp();
    if (!hasCrossTargetClang())
      GTEST_SKIP() << "cross-target Clang is unavailable";
  }

  fs::path compileTailFrameFixture() {
    const auto Source =
        fs::path(TEST_SOURCE_DIR) / "aarch64/test_indirect_tail_frame_a64.S";
    const auto Object = tmpFile("tail-frame.o");
    const auto Built =
        exec(NEVERD_TEST_CLANG, {"-target", "aarch64-linux-gnu", "-c",
                                 Source.string(), "-o", Object.string()});
    EXPECT_TRUE(Built.ok()) << Built.err;
    return Object;
  }

  static neverd::LowFunc build(const neverd::BinaryImage &Image,
                               const std::string &Name) {
    const auto *Function = Image.findSymbol(Name);
    EXPECT_NE(Function, nullptr);
    if (!Function)
      return {};
    neverd::Decoder Decoder;
    EXPECT_TRUE(Decoder.init(Image.Arch, Image.Mode));
    neverd::CFGBuilder Builder;
    return Builder.build(Image, Decoder, Function->Addr, Function->Name);
  }

  static bool hasOpcode(const neverd::LowFunc &Function, neverd::NdOp Opcode) {
    for (const auto &Block : Function.Blocks)
      for (const auto &Op : Block.Ops)
        if (Op.Opcode == Opcode)
          return true;
    return false;
  }
};

TEST_F(IndirectTailFrame, ActiveOrUnknownTailFrameRetainsItsIndirectBranch) {
  auto Image = neverd::loadBinary(compileTailFrameFixture().string());
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  for (const char *Name :
       {"tail_frame_live", "tail_frame_unknown", "tail_frame_join",
        "tail_link_changed", "tail_link_unrestored", "tail_link_slot_changed",
        "tail_link_slot_partial", "tail_unknown_alias", "tail_frame_borrowed",
        "tail_stack_target", "tail_link_target", "tail_frame_loop",
        "tail_link_loop", "tail_independent_root"}) {
    SCOPED_TRACE(Name);
    const auto Low = build(*Image, Name);
    EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::INDIR_BR));
    EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));
  }
}

TEST_F(IndirectTailFrame,
       RestoredNativeFrameRetainsItsFunctionPointerTailCall) {
  auto Image = neverd::loadBinary(compileTailFrameFixture().string());
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  for (const char *Name :
       {"tail_leaf", "tail_frame_balanced", "tail_frame_diamond",
        "tail_link_restored", "tail_loop_balanced"}) {
    SCOPED_TRACE(Name);
    const auto Low = build(*Image, Name);
    EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_BR));
    EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));
    EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::RETURN));
  }
}

TEST_F(IndirectTailFrame, UnresolvedLiveFrameCannotPublishACallReturn) {
  const auto Object = compileTailFrameFixture();
  for (unsigned Route : {0u, 1u, 2u}) {
    SCOPED_TRACE(Route);
    const auto File = tmpFile("tail-frame-" + std::to_string(Route) + ".c");
    std::vector<std::string> Args = {"decompile", Object.string(),
                                     "--func",    "tail_frame_live",
                                     "-o",        File.string()};
    if (Route)
      Args.push_back("--llvm");
    if (Route == 2)
      Args.push_back("--no-opt");
    const auto Recovered = exec(ndBin(), Args);
    ASSERT_TRUE(Recovered.ok()) << Recovered.err;
    std::ifstream Input(File);
    const std::string Source((std::istreambuf_iterator<char>(Input)), {});
    EXPECT_NE(Source.find("__builtin_trap"), std::string::npos) << Source;
    const auto Compiled =
        exec(NEVERD_TEST_CLANG, {"-fsyntax-only", "-Werror", File.string()});
    EXPECT_TRUE(Compiled.ok()) << Compiled.err << "\n" << Source;
  }
}

TEST_F(IndirectTailFrame, IncompleteOrExhaustedGuardPublishesNoCandidate) {
  using namespace neverd;
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  LowFunc Function;
  Function.Entry = 0x1000;
  Function.DecodedInstructionCount = Function.LiftedInstructionCount = 1;
  Function.ModuleAnalysisRoots.insert(Function.Entry);
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = Function.Entry;
  Block.EndAddr = Function.Entry + 4;
  LowOp Branch;
  Branch.Opcode = NdOp::INDIR_BR;
  Branch.Addr = Function.Entry;
  Branch.addInput(NdVar::reg(TRI.IntParamRegs.front(), 8));
  Block.Ops.push_back(Branch);
  LowInstructionBoundary Boundary;
  Boundary.Address = Function.Entry;
  Boundary.Size = 4;
  Boundary.OpCount = 1;
  Boundary.Control = LowInstructionControl::Branch;
  Boundary.ControlFlags =
      LowInstructionControlFlag::Branch | LowInstructionControlFlag::Indirect;
  Block.InstructionBoundaries.push_back(Boundary);
  Function.Blocks.push_back(std::move(Block));
  EXPECT_EQ(restoredAArch64IndirectTailFrames(Function, BinaryFormat::ELF),
            (std::set<va_t>{Function.Entry}));
  for (size_t Budget : {size_t{0}, size_t{1}})
    EXPECT_TRUE(
        restoredAArch64IndirectTailFrames(Function, BinaryFormat::ELF, Budget)
            .empty());

  auto Incomplete = Function;
  Incomplete.Blocks[0].InstructionBoundaries[0].OpCount = 0;
  EXPECT_TRUE(
      restoredAArch64IndirectTailFrames(Incomplete, BinaryFormat::ELF).empty());
  Incomplete = Function;
  Incomplete.ModuleAnalysisRoots.insert(Function.Entry + 1);
  EXPECT_TRUE(
      restoredAArch64IndirectTailFrames(Incomplete, BinaryFormat::ELF).empty());
  Incomplete = Function;
  Incomplete.Blocks[0].Succs.push_back(1);
  EXPECT_TRUE(
      restoredAArch64IndirectTailFrames(Incomplete, BinaryFormat::ELF).empty());
}

} // namespace
