//===- RuntimeTests.cpp - Dalvik value and refusal boundaries -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Decoded-model adversarial cases, independent of the Java compiler oracle.
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/dalvik/Runtime.h"

namespace neverd::mobile::dalvik {
namespace {
Instruction insn(uint32_t pc, const char *op, std::vector<unsigned> regs = {}) {
  Instruction i;
  i.pc = pc;
  i.opcode = op;
  i.registers = std::move(regs);
  return i;
}
Class fixture(std::vector<Instruction> code, unsigned registers = 3,
              std::string result = "I") {
  Class c;
  c.name = "Ltest/Values;";
  c.superclass = "Ljava/lang/Object;";
  Method m;
  m.reference = {c.name, "run", {}, std::move(result)};
  m.access = {"public", "static"};
  m.registers = registers;
  m.instructions = std::move(code);
  c.methods.push_back(std::move(m));
  return c;
}
ExecutionResult run(Class c, uint64_t budget = 32, uint64_t stack = 65536) {
  auto reference = c.methods.front().reference;
  Program p({std::move(c)});
  return p.execute(
      *p.findStatic(reference), {},
      [&] {
        if (!budget)
          return ExecutionStop::InstructionLimit;
        --budget;
        return ExecutionStop::Returned;
      },
      stack);
}
TEST(DalvikRuntime, UninitializedNarrowAndWideRegistersAreNotZero) {
  EXPECT_EQ(run(fixture({insn(0, "return", {0})})).stop,
            ExecutionStop::Invalid);
  EXPECT_EQ(run(fixture({insn(0, "return-wide", {0})}, 2, "J")).stop,
            ExecutionStop::Invalid);
}
TEST(DalvikRuntime, OverlappingWideMoveReadsBothHalvesBeforeWriting) {
  auto c = insn(0, "const-wide", {0});
  c.literal = int64_t(0x123456789abcdef0);
  auto r = run(fixture(
      {c, insn(5, "move-wide", {1, 0}), insn(6, "return-wide", {1})}, 3, "J"));
  EXPECT_EQ(r.stop, ExecutionStop::Returned) << r.diagnostic;
  EXPECT_EQ(r.value, 0x123456789abcdef0ULL);
  c.registers = {1};
  r = run(fixture(
      {c, insn(5, "move-wide", {0, 1}), insn(6, "return-wide", {0})}, 3, "J"));
  EXPECT_EQ(r.value, 0x123456789abcdef0ULL);
}
TEST(DalvikRuntime, WritingOneWideHalfInvalidatesThePair) {
  auto c = insn(0, "const-wide", {0});
  c.literal = int64_t(7);
  auto d = insn(5, "const/4", {1});
  d.literal = int64_t(9);
  EXPECT_EQ(run(fixture({c, d, insn(6, "return-wide", {0})}, 2, "J")).stop,
            ExecutionStop::Invalid);
  EXPECT_EQ(run(fixture({c, insn(5, "return", {1})})).stop,
            ExecutionStop::Invalid);
}
TEST(DalvikRuntime, JumpLoopConsumesSharedBudget) {
  auto loop = insn(0, "goto");
  loop.target = 0;
  auto r = run(fixture({loop}), 7);
  EXPECT_EQ(r.stop, ExecutionStop::InstructionLimit);
  EXPECT_NE(r.diagnostic.find("code-unit 0"), std::string::npos);
}
TEST(DalvikRuntime, ResourceAndInstructionBoundariesFailExplicitly) {
  auto c = fixture({insn(0, "return-void")}, 0, "V");
  auto ref = c.methods[0].reference;
  Program p({c});
  EXPECT_EQ(
      p.execute(
           *p.findStatic(ref), {}, [] { return ExecutionStop::Timeout; }, 1024)
          .stop,
      ExecutionStop::Timeout);
  EXPECT_EQ(run(c, 3, 1).stop, ExecutionStop::StackLimit);
  EXPECT_EQ(run(fixture({insn(0, "monitor-enter", {0})})).stop,
            ExecutionStop::Unsupported);
}
TEST(DalvikRuntime,
     NoObjectStateClassInitializationOrExceptionHandlersAreGuessed) {
  auto c = fixture({insn(0, "return-void")}, 0, "V");
  auto init = c.methods[0];
  init.reference.name = "<clinit>";
  c.methods.push_back(init);
  EXPECT_EQ(run(c).stop, ExecutionStop::Unsupported);
  c.methods.pop_back();
  c.methods[0].tries.push_back({0, 1, {}});
  EXPECT_EQ(run(c).stop, ExecutionStop::Unsupported);
  c.methods[0].tries.clear();
  c.superclass = "Lunknown/Base;";
  EXPECT_EQ(run(c).stop, ExecutionStop::Unsupported);
  c.superclass = "Ljava/lang/Object;";
  c.name = "Ljava/lang/String;";
  EXPECT_THROW(Program({c}), Error);
}
TEST(DalvikRuntime, ResultMustFollowCompatibleInvocation) {
  EXPECT_EQ(
      run(fixture({insn(0, "move-result", {0}), insn(1, "return", {0})})).stop,
      ExecutionStop::Invalid);
}
TEST(DalvikRuntime, NarrowReturnDoesNotHideMalformedRegisterValues) {
  auto c = insn(0, "const", {0});
  c.literal = int64_t(256);
  EXPECT_EQ(run(fixture({c, insn(3, "return", {0})}, 1, "B")).stop,
            ExecutionStop::Invalid);
  EXPECT_EQ(run(fixture({c, insn(3, "return", {0})}, 1, "Z")).stop,
            ExecutionStop::Invalid);
}
TEST(DalvikRuntime, DivisionOverflowAndZeroPreserveJavaBehavior) {
  auto a = insn(0, "const", {0}), b = insn(3, "const/4", {1});
  a.literal = int64_t(-2147483648LL);
  b.literal = int64_t(-1);
  auto code =
      std::vector{a, b, insn(4, "div-int", {2, 0, 1}), insn(6, "return", {2})};
  auto r = run(fixture(code));
  EXPECT_EQ(r.stop, ExecutionStop::Returned);
  EXPECT_EQ(r.value, 0x80000000U);
  code[2].opcode = "rem-int";
  EXPECT_EQ(run(fixture(code)).value, 0U);
  code[1].literal = int64_t(0);
  r = run(fixture(code));
  EXPECT_EQ(r.stop, ExecutionStop::Thrown);
  EXPECT_EQ(r.exception, "java/lang/ArithmeticException");
}
} // namespace
} // namespace neverd::mobile::dalvik
