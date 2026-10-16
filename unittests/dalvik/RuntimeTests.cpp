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
TEST(DalvikRuntime, FloatingStateHierarchyAndExceptionHandlersAreNotGuessed) {
  auto c = fixture({insn(0, "return-void")}, 0, "V");
  Field field;
  field.reference = {c.name, "floating", "F"};
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

Instruction typed(uint32_t pc, const char *op, std::vector<unsigned> regs,
                  std::string type) {
  auto i = insn(pc, op, std::move(regs));
  i.reference = std::move(type);
  return i;
}
Class objectFixture() {
  auto c = fixture({}, 3);
  Field f;
  f.reference = {c.name, "value", "I"};
  f.access = {"public"};
  c.fields.push_back(f);
  Method ctor;
  ctor.reference = {c.name, "<init>", {"I"}, "V"};
  ctor.access = {"public", "constructor"};
  ctor.registers = 2;
  auto base = insn(0, "invoke-direct", {0});
  base.reference = MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"};
  auto store = insn(3, "iput", {1, 0});
  store.reference = f.reference;
  ctor.instructions = {base, store, insn(5, "return-void")};
  c.methods.push_back(ctor);
  auto input = constant(3, 9);
  input.registers = {1};
  auto invoke = insn(5, "invoke-direct", {0, 1});
  invoke.reference = ctor.reference;
  auto read = insn(8, "iget", {1, 2});
  read.reference = f.reference;
  c.methods[0].instructions = {typed(0, "new-instance", {0}, c.name),
                               insn(2, "move-object", {2, 0}),
                               input,
                               invoke,
                               read,
                               insn(10, "return", {1})};
  return c;
}
TEST(DalvikRuntime, ConstructorsInitializeAliasedReferencesBeforeFieldUse) {
  auto c = objectFixture();
  auto result = run(c);
  EXPECT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  EXPECT_EQ(result.value, 9U);
  c.methods[1].instructions.erase(c.methods[1].instructions.begin());
  EXPECT_EQ(run(c).stop, ExecutionStop::Invalid);
  c = objectFixture();
  c.methods[0].instructions.erase(c.methods[0].instructions.begin() + 3);
  EXPECT_EQ(run(c).stop, ExecutionStop::Invalid);
  c = objectFixture();
  c.methods[0].instructions.insert(c.methods[0].instructions.begin() + 4,
                                   c.methods[0].instructions[3]);
  c.methods[0].instructions[4].pc = 7;
  EXPECT_EQ(run(c).stop, ExecutionStop::Invalid);
}
TEST(DalvikRuntime, ReferenceRegistersDoNotAcceptForgedIntegerIdentities) {
  auto zero = constant(0, 0);
  EXPECT_EQ(run(fixture({zero, insn(3, "return-object", {0})}, 1,
                        "Ljava/lang/Object;"))
                .stop,
            ExecutionStop::Returned);
  auto one = constant(0, 1);
  EXPECT_EQ(run(fixture({one, insn(3, "return-object", {0})}, 1,
                        "Ljava/lang/Object;"))
                .stop,
            ExecutionStop::Invalid);
  auto add = insn(3, "add-int/lit8", {0, 0});
  add.literal = int64_t(0);
  EXPECT_EQ(run(fixture({zero, add, insn(5, "return-object", {0})}, 1,
                        "Ljava/lang/Object;"))
                .stop,
            ExecutionStop::Invalid);
  auto c = objectFixture();
  c.methods[0].instructions[1] = insn(2, "move", {2, 0});
  EXPECT_EQ(run(c).stop, ExecutionStop::Invalid);
}
TEST(DalvikRuntime,
     JavaArrayExceptionsAreRecoverableButBudgetStopsAreTerminal) {
  auto length = constant(0, -1);
  auto code = std::vector{length, typed(3, "new-array", {0, 0}, "[I"),
                          insn(5, "return-object", {0})};
  auto c = fixture(code, 1, "[I");
  auto result = run(c);
  EXPECT_EQ(result.stop, ExecutionStop::Thrown);
  EXPECT_EQ(result.exception, "java/lang/NegativeArraySizeException");
  c.methods[0].instructions[0].literal = int64_t(0x7fffffff);
  auto ref = c.methods[0].reference;
  Program p({c});
  Runtime runtime(p, std::make_shared<Heap>(4096));
  result = call(runtime, *p.findStatic(ref));
  EXPECT_EQ(result.stop, ExecutionStop::HeapLimit);
  EXPECT_EQ(call(runtime, *p.findStatic(ref)).stop, ExecutionStop::HeapLimit);
}
TEST(DalvikRuntime, FieldAndArrayStorageShareBoundedObjectIdentity) {
  auto c = objectFixture();
  c.fields[0].reference.type = "B";
  Field link;
  link.reference = {c.name, "link", "Ljava/lang/Object;"};
  c.fields.push_back(link);
  Program p({c});
  auto heap = std::make_shared<Heap>();
  Runtime runtime(p, heap);
  auto admit = [] { return ExecutionStop::Returned; };
  auto a = runtime.allocate(c.name, true, admit, 65536);
  auto b = runtime.allocate(c.name, true, admit, 65536);
  ASSERT_EQ(a.stop, ExecutionStop::Returned);
  ASSERT_EQ(b.stop, ExecutionStop::Returned);
  const auto *byte = p.findField(c.fields[0].reference, false);
  const auto *ref = p.findField(link.reference, false);
  EXPECT_EQ(runtime.field(*byte, a.value, 0x1234ff, admit, 65536).value,
            0xffffffffU);
  EXPECT_EQ(runtime.field(*byte, a.value, {}, admit, 65536).value, 0xffffffffU);
  EXPECT_EQ(runtime.field(*byte, b.value, {}, admit, 65536).value, 0U);
  EXPECT_EQ(runtime.field(*ref, a.value, b.value, admit, 65536).value, b.value);
  EXPECT_EQ(runtime.field(*ref, b.value, a.value, admit, 65536).value, a.value);
  EXPECT_EQ(runtime.field(*ref, a.value, 0, admit, 65536).value, 0U);
  EXPECT_EQ(runtime.field(*ref, a.value, 0xffffff, admit, 65536).stop,
            ExecutionStop::Invalid);
}
TEST(DalvikRuntime, ArrayComponentTypesAndPayloadBoundsAreChecked) {
  auto n = constant(0, 1);
  auto c = fixture({n, typed(3, "new-array", {1, 0}, "[B"),
                    insn(5, "aget", {0, 1, 0}), insn(7, "return", {0})},
                   2);
  EXPECT_EQ(run(c).stop, ExecutionStop::Invalid);
  c.methods[0].instructions[2].opcode = "aget-byte";
  auto result = run(c);
  EXPECT_EQ(result.stop, ExecutionStop::Thrown);
  EXPECT_EQ(result.exception, "java/lang/ArrayIndexOutOfBoundsException");
  auto payload = insn(5, "fill-array-data", {1});
  payload.element_width = 1;
  payload.data = {1, 2};
  c.methods[0].instructions[2] = payload;
  EXPECT_EQ(run(c).stop, ExecutionStop::Thrown);
  c.methods[0].instructions[2].element_width = 2;
  EXPECT_EQ(run(c).stop, ExecutionStop::Invalid);
}
TEST(DalvikRuntime, ReferenceArrayCovarianceDoesNotCoercePrimitiveArrays) {
  EXPECT_TRUE(referenceAssignable("[[I", "[Ljava/lang/Object;"));
  EXPECT_TRUE(referenceAssignable("[Ltest/Box;", "[Ljava/lang/Object;"));
  EXPECT_FALSE(referenceAssignable("[I", "[J"));
  EXPECT_FALSE(referenceAssignable("[I", "[Ljava/lang/Object;"));
  EXPECT_FALSE(referenceAssignable("Ljava/lang/Object;", "[I"));
  EXPECT_FALSE(referenceAssignable("[Ljava/lang/Object;", "[Ltest/Box;"));
}
TEST(DalvikRuntime, NullArrayInClassInitializerRetainsErroneousClass) {
  auto c = counter();
  c.methods[1].instructions = {constant(0, 0), insn(3, "array-length", {0, 0}),
                               insn(4, "return-void")};
  Program p({c});
  Runtime runtime(p);
  auto method = p.findStatic(c.methods[0].reference);
  EXPECT_EQ(call(runtime, *method).exception,
            "java/lang/ExceptionInInitializerError");
  EXPECT_EQ(call(runtime, *method).exception, "java/lang/NoClassDefFoundError");
}

TEST(DalvikRuntime, AllocationCannotInitializeAnInaccessibleClass) {
  auto caller = fixture({typed(0, "new-instance", {0}, "Lhidden/Box;"),
                         insn(2, "return-object", {0})},
                        1, "Ljava/lang/Object;");
  Class other;
  other.name = "Lhidden/Box;";
  other.superclass = "Ljava/lang/Object;";
  auto ref = caller.methods[0].reference;
  Program p({caller, other});
  Runtime runtime(p);
  auto result = call(runtime, *p.findStatic(ref));
  EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
  EXPECT_NE(result.diagnostic.find("inaccessible allocation class"),
            std::string::npos);
}
TEST(DalvikRuntime, FinalInstanceStoresAreRestrictedToTheConstructingReceiver) {
  auto c = objectFixture();
  c.fields[0].access.insert("final");
  auto &ctor = c.methods[1];
  ctor.reference.parameters.push_back(c.name);
  ctor.registers = 3;
  ctor.instructions[1].registers = {1, 2};
  Program p({c});
  Runtime runtime(p);
  auto admit = [] { return ExecutionStop::Returned; };
  auto first = runtime.allocate(c.name, false, admit, 65536);
  auto other = runtime.allocate(c.name, true, admit, 65536);
  std::vector<uint64_t> args{first.value, 7, other.value};
  auto result =
      runtime.execute(*p.findMethod(ctor.reference, false), args, admit, 65536);
  EXPECT_EQ(result.stop, ExecutionStop::Invalid);
}
} // namespace
} // namespace neverd::mobile::dalvik
