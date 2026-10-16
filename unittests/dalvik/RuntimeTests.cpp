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

#include <tuple>

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
TEST(DalvikRuntime, NoObjectStateHierarchyOrExceptionHandlersAreGuessed) {
  auto c = fixture({insn(0, "return-void")}, 0, "V");
  Field field;
  field.reference = {c.name, "object", "Ljava/lang/Object;"};
  field.access = {"static"};
  c.fields.push_back(field);
  EXPECT_EQ(run(c).stop, ExecutionStop::Unsupported);
  c.fields.clear();
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

Instruction constant(uint32_t pc, int64_t value) {
  auto i = insn(pc, "const", {0});
  i.literal = value;
  return i;
}
Instruction fieldAccess(uint32_t pc, const char *op, const FieldRef &field) {
  auto i = insn(pc, op, {0});
  i.reference = field;
  return i;
}
Class counter() {
  Field field;
  field.reference = {"Ltest/Values;", "count", "I"};
  field.access = {"static"};
  field.value = int64_t(7);
  auto add = insn(2, "add-int/lit8", {0, 0});
  add.literal = int64_t(1);
  auto c = fixture({fieldAccess(0, "sget", field.reference), add,
                    fieldAccess(4, "sput", field.reference),
                    insn(6, "return", {0})});
  c.fields.push_back(field);
  Method init;
  init.reference = {c.name, "<clinit>", {}, "V"};
  init.access = {"static", "constructor"};
  init.registers = 1;
  init.instructions = {fieldAccess(0, "sget", field.reference), add,
                       fieldAccess(4, "sput", field.reference),
                       insn(6, "return-void")};
  c.methods.push_back(init);
  return c;
}
ExecutionResult call(Runtime &runtime, const Method &method) {
  return runtime.execute(
      method, {}, [] { return ExecutionStop::Returned; }, 65536);
}
TEST(DalvikRuntime, StaticStatePersistsOnlyWithinItsWorkload) {
  auto c = counter();
  auto ref = c.methods[0].reference;
  Program p({c});
  Runtime a(p), b(p);
  auto method = p.findStatic(ref);
  EXPECT_EQ(call(a, *method).value, 9U);
  EXPECT_EQ(call(a, *method).value, 10U);
  EXPECT_EQ(call(b, *method).value, 9U);
}
TEST(DalvikRuntime, FailedInitializationIsNotRetried) {
  auto c = counter();
  auto zero = insn(5, "const/4", {1});
  zero.literal = int64_t(0);
  c.methods[1].registers = 2;
  c.methods[1].instructions = {constant(0, 1), zero,
                               insn(6, "div-int", {0, 0, 1}),
                               insn(8, "return-void")};
  auto ref = c.methods[0].reference;
  Program p({c});
  Runtime runtime(p);
  auto first = call(runtime, *p.findStatic(ref));
  EXPECT_EQ(first.stop, ExecutionStop::Thrown);
  EXPECT_EQ(first.exception, "java/lang/ExceptionInInitializerError");
  EXPECT_NE(first.diagnostic.find("code-unit 6"), std::string::npos);
  auto again = call(runtime, *p.findStatic(ref));
  EXPECT_EQ(again.stop, ExecutionStop::Thrown);
  EXPECT_EQ(again.exception, "java/lang/NoClassDefFoundError");
}
TEST(DalvikRuntime, InterruptedInitializationCannotResumeWithPartialState) {
  for (auto stop : {ExecutionStop::InstructionLimit, ExecutionStop::Timeout,
                    ExecutionStop::Unsupported}) {
    auto c = counter();
    auto ref = c.methods[0].reference;
    Program p({c});
    Runtime runtime(p);
    unsigned admitted = 0;
    auto first = runtime.execute(
        *p.findStatic(ref), {},
        [&] { return ++admitted > 3 ? stop : ExecutionStop::Returned; }, 65536);
    EXPECT_EQ(first.stop, stop);
    auto again = call(runtime, *p.findStatic(ref));
    EXPECT_EQ(again.stop, stop);
    EXPECT_EQ(again.diagnostic, first.diagnostic);
  }
}
TEST(DalvikRuntime, StaticFieldWidthAndFinalWritesAreChecked) {
  auto c = counter();
  c.methods[0].instructions[0].opcode = "sget-wide";
  EXPECT_EQ(run(c).stop, ExecutionStop::Invalid);
  c = counter();
  c.fields[0].access.insert("final");
  EXPECT_EQ(run(c).stop, ExecutionStop::Invalid);
  c = counter();
  c.fields[0].access.insert("volatile");
  EXPECT_EQ(run(c).stop, ExecutionStop::Unsupported);
  c = counter();
  c.fields[0].access.erase("static");
  EXPECT_EQ(run(c).stop, ExecutionStop::Unsupported);
}
TEST(DalvikRuntime, EncodedValuesAreCanonicalAndFieldStoresNarrow) {
  for (auto [type, op, input, output] :
       {std::tuple{"B", "byte", 0x1234ff, uint64_t(0xffffffff)},
        std::tuple{"S", "short", 0x12ffff, uint64_t(0xffffffff)},
        std::tuple{"C", "char", 0x12ffff, uint64_t(0xffff)}}) {
    auto c = counter();
    c.fields[0].reference.type = type;
    c.fields[0].value = std::monostate{};
    c.methods.pop_back();
    auto field = c.fields[0].reference;
    c.methods[0].instructions = {
        constant(0, input),
        fieldAccess(3, (std::string("sput-") + op).c_str(), field),
        fieldAccess(5, (std::string("sget-") + op).c_str(), field),
        insn(7, "return", {0})};
    auto result = run(c);
    EXPECT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    EXPECT_EQ(result.value, output);
    c.fields[0].value = int64_t(65536);
    EXPECT_EQ(run(c).stop, ExecutionStop::Invalid);
  }
}
TEST(DalvikRuntime, ClassInitializersCannotBeCalledExplicitly) {
  auto c = counter();
  Program p({c});
  Runtime runtime(p);
  auto result = call(runtime, *p.findStatic(c.methods[1].reference));
  EXPECT_EQ(result.stop, ExecutionStop::Invalid);
}
TEST(DalvikRuntime, PublicMembersDoNotExposeAnInaccessibleDeclaringClass) {
  Class target;
  target.name = "Lhidden/Other;";
  target.superclass = "Ljava/lang/Object;";
  Field field;
  field.reference = {target.name, "value", "I"};
  field.access = {"public", "static"};
  field.value = int64_t(7);
  target.fields.push_back(field);
  auto caller = fixture(
      {fieldAccess(0, "sget", field.reference), insn(2, "return", {0})});
  auto ref = caller.methods[0].reference;
  for (bool visible : {false, true}) {
    if (visible)
      target.access.insert("public");
    Program p({caller, target});
    Runtime runtime(p);
    auto result = call(runtime, *p.findStatic(ref));
    EXPECT_EQ(result.stop,
              visible ? ExecutionStop::Returned : ExecutionStop::Unsupported);
    if (visible)
      EXPECT_EQ(result.value, 7U);
  }
}
} // namespace
} // namespace neverd::mobile::dalvik
