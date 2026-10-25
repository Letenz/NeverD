//===- RuntimeTests.cpp - Dalvik value and refusal boundaries -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Decoded-model adversarial cases, independent of the Java compiler oracle.
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/dalvik/Library.h"
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
TEST(DalvikRuntime, LibraryCatalogKeepsPublishedMethodPositions) {
  // JNI library method identities retain these published catalog positions.
  const auto methods = libraryMethods();
  ASSERT_GE(methods.size(), 91u);
  struct Expected {
    size_t index;
    const char *owner, *name, *signature;
    bool is_static;
  };
  const Expected expected[] = {
      {0, "java/lang/String", "intern", "()Ljava/lang/String;", false},
      {7, "java/lang/String", "length", "()I", false},
      {28, "java/lang/String", "valueOf", "(I)Ljava/lang/String;", true},
      {67, "java/lang/ClassNotFoundException", "<init>", "()V", false},
      {69, "java/lang/NoClassDefFoundError", "<init>", "()V", false},
      {81, "java/lang/ClassLoader", "getSystemClassLoader",
       "()Ljava/lang/ClassLoader;", true},
      {89, "java/io/File", "isAbsolute", "()Z", false},
      {90, "android/app/ActivityThread", "getInstrumentation",
       "()Landroid/app/Instrumentation;", false},
  };
  for (const auto &entry : expected) {
    SCOPED_TRACE(entry.index);
    EXPECT_STREQ(methods[entry.index].Class, entry.owner);
    EXPECT_STREQ(methods[entry.index].Name, entry.name);
    EXPECT_STREQ(methods[entry.index].Signature, entry.signature);
    EXPECT_EQ(methods[entry.index].Static, entry.is_static);
  }
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
     FloatingStateUnknownHierarchyAndMalformedHandlersFailClearly) {
  auto c = fixture({insn(0, "return-void")}, 0, "V");
  Field field;
  field.reference = {c.name, "floating", "F"};
  field.access = {"static"};
  c.fields.push_back(field);
  EXPECT_EQ(run(c).stop, ExecutionStop::Unsupported);
  c.fields.clear();
  c.methods[0].tries.push_back({0, 1, {}});
  EXPECT_EQ(run(c).stop, ExecutionStop::Invalid);
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
TEST(DalvikRuntime, CatchRethrowRetainsTheOriginalHeapObject) {
  auto c = fixture({insn(0, "throw", {1}), insn(1, "move-exception", {0}),
                    insn(2, "throw", {0})},
                   2, "V");
  c.methods[0].reference.parameters = {"Ljava/lang/RuntimeException;"};
  c.methods[0].tries = {{0, 1, {{"Ljava/lang/Exception;", 1}}}};
  Program program({c});
  auto heap = std::make_shared<Heap>();
  auto exception =
      heap->allocate(Object::Throwable, "java/lang/RuntimeException", 0);
  Runtime runtime(program, heap);
  uint64_t args[] = {exception->Identity};
  auto result = runtime.execute(
      *program.findStatic(c.methods[0].reference), args,
      [] { return ExecutionStop::Returned; }, 65536);
  EXPECT_EQ(result.stop, ExecutionStop::Thrown);
  EXPECT_EQ(result.throwable, exception->Identity);
  EXPECT_EQ(result.exception, "java/lang/RuntimeException");
}
TEST(DalvikRuntime, CatchCannotSwallowUnsupportedOrResourceStops) {
  auto c = fixture({insn(0, "monitor-enter", {0}),
                    insn(1, "move-exception", {0}), insn(2, "return-void")},
                   1, "V");
  c.methods[0].tries = {{0, 1, {{std::nullopt, 1}}}};
  EXPECT_EQ(run(c).stop, ExecutionStop::Unsupported);
  c.methods[0].instructions[0] = insn(0, "goto");
  c.methods[0].instructions[0].target = 0;
  EXPECT_EQ(run(c, 7).stop, ExecutionStop::InstructionLimit);
  c.methods[0].instructions[0] = insn(0, "move-exception", {0});
  EXPECT_EQ(run(c).stop, ExecutionStop::Invalid);
}
TEST(DalvikRuntime, HandlerOrderingAndIgnoredThrowableFollowDEXControlFlow) {
  auto c =
      fixture({constant(0, 0), insn(1, "throw", {0}), constant(2, 11),
               insn(3, "return", {0}), constant(4, 22), insn(5, "return", {0})},
              1);
  c.methods[0].tries = {{1,
                         2,
                         {{"Ljava/lang/RuntimeException;", 2},
                          {"Ljava/lang/NullPointerException;", 4}}}};
  auto result = run(c);
  EXPECT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  EXPECT_EQ(result.value, 11U);
  c.methods[0].tries[0].handlers[0].type = "Ljava/lang/ArithmeticException;";
  EXPECT_EQ(run(c).value, 22U);
}
TEST(DalvikRuntime, CyclicHierarchyAndUnmodeledInterfaceBodiesStop) {
  auto a = fixture({insn(0, "return-void")}, 0, "V");
  a.superclass = a.name;
  EXPECT_EQ(run(a).stop, ExecutionStop::Unsupported);
  a.superclass = "Ljava/lang/Object;";
  a.access = {"public", "interface"};
  EXPECT_EQ(run(a).stop, ExecutionStop::Unsupported);
}
TEST(DalvikRuntime, LiteralInterningRetainsUTF16UnitsAndWorkloadIdentity) {
  Heap heap;
  std::string text("A\0", 2);
  text += "\xf0\x9f\x98\x80\xed\xa0\x80";
  auto a = heap.literal(text), b = heap.literal(text);
  ASSERT_TRUE(a);
  EXPECT_EQ(a, b);
  EXPECT_EQ(a->Length, 5U);
  EXPECT_EQ(a->Bytes,
            (std::vector<uint8_t>{65, 0, 0, 0, 0x3d, 0xd8, 0, 0xde, 0, 0xd8}));
  auto copy =
      heap.allocate(Object::String, "java/lang/String", a->Bytes.size());
  copy->Bytes = a->Bytes;
  copy->Length = a->Length;
  EXPECT_EQ(heap.intern(copy), a);
  Heap fresh;
  EXPECT_NE(fresh.literal(text), a);
  EXPECT_THROW(javaStringBytes("\xc0\x80"), Error);
}
TEST(DalvikRuntime, InterfaceCannotBeInstantiatedEvenWithoutAbstractFlag) {
  Class c;
  c.name = "Ltest/Interface;";
  c.superclass = "Ljava/lang/Object;";
  c.access = {"public", "interface"};
  Program program({c});
  Runtime runtime(program);
  auto result = runtime.allocate(
      c.name, false, [] { return ExecutionStop::Returned; }, 65536);
  EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
}
TEST(DalvikRuntime, FinitePlatformMarkerInterfacesPreserveAssignability) {
  EXPECT_TRUE(
      referenceAssignable("Ljava/lang/String;", "Ljava/io/Serializable;"));
  EXPECT_TRUE(
      referenceAssignable("Ljava/lang/Class;", "Ljava/io/Serializable;"));
  EXPECT_TRUE(referenceAssignable("Ljava/lang/ArithmeticException;",
                                  "Ljava/io/Serializable;"));
  EXPECT_FALSE(
      referenceAssignable("Ljava/lang/Object;", "Ljava/io/Serializable;"));
  Class c;
  c.name = "Ltest/Problem;";
  c.superclass = "Ljava/lang/RuntimeException;";
  Program program({c});
  EXPECT_TRUE(program.assignable(c.name, "Ljava/io/Serializable;"));
}
TEST(DalvikRuntime, StringCharSequenceAssignabilityIsSharedAndCovariant) {
  Program program({}, std::map<std::string, std::string>{});
  for (const auto &[source, destination, expected] :
       std::vector<std::tuple<const char *, const char *, bool>>{
           {"Ljava/lang/String;", "Ljava/lang/CharSequence;", true},
           {"[Ljava/lang/String;", "[Ljava/lang/CharSequence;", true},
           {"[[Ljava/lang/String;", "[[Ljava/lang/CharSequence;", true},
           {"Ljava/lang/CharSequence;", "Ljava/lang/String;", false},
           {"Ljava/lang/Class;", "Ljava/lang/CharSequence;", false},
           {"Ljava/lang/Object;", "Ljava/lang/CharSequence;", false},
           {"[Ljava/lang/String;", "Ljava/lang/CharSequence;", false},
           {"[C", "[Ljava/lang/CharSequence;", false},
           {"[C", "Ljava/lang/CharSequence;", false}}) {
    SCOPED_TRACE(std::string(source) + " -> " + destination);
    EXPECT_EQ(referenceAssignable(source, destination), expected);
    EXPECT_EQ(program.assignable(source, destination), expected);
  }
}
class StringBuilderTest : public testing::Test {
protected:
  Program program{{}, std::map<std::string, std::string>{}};
  std::shared_ptr<Heap> heap = std::make_shared<Heap>();
  Runtime runtime{program, heap};
  const AdmitInstruction admit = [] { return ExecutionStop::Returned; };
  const LibraryMethod &method(const char *name, const char *signature) {
    auto [args, result] = prototype(signature);
    auto m =
        libraryMethod({"Ljava/lang/StringBuilder;", name, args, result}, false);
    if (!m)
      std::abort();
    return *m;
  }
  uint64_t create() {
    auto r = runtime.library(method("<init>", "()V"), 0, {}, admit);
    EXPECT_EQ(r.stop, ExecutionStop::Returned) << r.diagnostic;
    return r.value;
  }
  ExecutionResult append(uint64_t builder, uint64_t unit,
                         const AdmitInstruction &budget) {
    const uint64_t args[]{unit};
    return runtime.library(method("append", "(C)Ljava/lang/StringBuilder;"),
                           builder, args, budget);
  }
  ExecutionResult string(uint64_t builder) {
    return runtime.library(method("toString", "()Ljava/lang/String;"), builder,
                           {}, admit);
  }
};
TEST_F(StringBuilderTest, UTF16MutationRetainsIdentityAndIndependentStrings) {
  auto builder = create();
  EXPECT_EQ(string(builder).value, heap->literal("")->Identity);
  const std::vector<uint8_t> expected{0, 0, 0, 0xd8, 0, 0xdc, 0x2d, 0x4e};
  for (uint64_t unit : {0, 0xd800, 0xdc00, 0x4e2d}) {
    auto r = append(builder, unit, admit);
    ASSERT_EQ(r.stop, ExecutionStop::Returned) << r.diagnostic;
    EXPECT_EQ(r.value, builder);
  }
  auto a = string(builder), b = string(builder);
  ASSERT_EQ(a.stop, ExecutionStop::Returned);
  ASSERT_EQ(b.stop, ExecutionStop::Returned);
  EXPECT_NE(a.value, b.value);
  EXPECT_NE(a.value, builder);
  EXPECT_EQ(heap->get(a.value)->Type, Object::String);
  EXPECT_EQ(heap->get(a.value)->Bytes, expected);
  ASSERT_EQ(append(builder, 'X', admit).stop, ExecutionStop::Returned);
  EXPECT_EQ(heap->get(a.value)->Bytes, expected);
  EXPECT_EQ(heap->get(b.value)->Bytes, expected);
  EXPECT_EQ(heap->get(builder)->Length, 5u);
  auto other = create();
  EXPECT_NE(other, builder);
  EXPECT_TRUE(heap->get(other)->Bytes.empty());
}
TEST_F(StringBuilderTest, ExhaustedWorkDoesNotPublishAPartialAppend) {
  for (unsigned available : {0u, 1u, 2u}) {
    auto storage = std::make_shared<Heap>();
    Runtime state(program, storage);
    auto builder = state.library(method("<init>", "()V"), 0, {}, admit).value;
    const uint64_t a[]{'A'}, b[]{'B'};
    auto &add = method("append", "(C)Ljava/lang/StringBuilder;");
    ASSERT_EQ(state.library(add, builder, a, admit).stop,
              ExecutionStop::Returned);
    auto before = storage->get(builder)->Bytes;
    auto r = state.library(add, builder, b, [&] {
      if (!available)
        return ExecutionStop::InstructionLimit;
      --available;
      return ExecutionStop::Returned;
    });
    EXPECT_EQ(r.stop, ExecutionStop::InstructionLimit);
    EXPECT_EQ(storage->get(builder)->Bytes, before);
    EXPECT_EQ(state.library(add, builder, b, admit).stop, r.stop);
  }
}
TEST_F(StringBuilderTest, AllocationLimitPreservesMutableAndImmutableState) {
  auto storage = std::make_shared<Heap>(65536, 1);
  Runtime state(program, storage);
  auto builder = state.library(method("<init>", "()V"), 0, {}, admit);
  ASSERT_EQ(builder.stop, ExecutionStop::Returned);
  const uint64_t unit[]{0xdfff};
  ASSERT_EQ(state
                .library(method("append", "(C)Ljava/lang/StringBuilder;"),
                         builder.value, unit, admit)
                .stop,
            ExecutionStop::Returned);
  auto r = state.library(method("toString", "()Ljava/lang/String;"),
                         builder.value, {}, admit);
  EXPECT_EQ(r.stop, ExecutionStop::HeapLimit);
  EXPECT_EQ(storage->get(builder.value)->Bytes,
            (std::vector<uint8_t>{0xff, 0xdf}));
  EXPECT_TRUE(state.snapshot().terminal);
}
TEST_F(StringBuilderTest, ReceiverAndUnimplementedOperationsFailClearly) {
  auto null = append(0, 'A', admit);
  EXPECT_EQ(null.stop, ExecutionStop::Thrown);
  EXPECT_EQ(null.exception, "java/lang/NullPointerException");
  EXPECT_FALSE(
      libraryMethod({"Ljava/lang/StringBuilder;", "capacity", {}, "I"}, false));
  auto wrong = heap->literal("value");
  EXPECT_EQ(append(wrong->Identity, 'B', admit).stop, ExecutionStop::Invalid);
  Runtime fresh(program, heap);
  auto raw = heap->allocate(Object::Instance, "java/lang/StringBuilder", 0);
  raw->Initialized = false;
  EXPECT_EQ(fresh
                .library(method("toString", "()Ljava/lang/String;"),
                         raw->Identity, {}, admit)
                .stop,
            ExecutionStop::Invalid);
}
TEST_F(StringBuilderTest, DexConstructionAndAppendShareTheValueOwner) {
  auto allocation = insn(0, "new-instance", {0});
  allocation.reference = std::string("Ljava/lang/StringBuilder;");
  auto constructor = insn(2, "invoke-direct", {0});
  constructor.reference =
      MethodRef{"Ljava/lang/StringBuilder;", "<init>", {}, "V"};
  auto unit = insn(5, "const", {1});
  unit.literal = int64_t(0xd800);
  auto add = insn(8, "invoke-virtual", {0, 1});
  add.reference = MethodRef{"Ljava/lang/StringBuilder;",
                            "append",
                            {"C"},
                            "Ljava/lang/StringBuilder;"};
  auto text = insn(12, "invoke-virtual", {2});
  text.reference = MethodRef{
      "Ljava/lang/StringBuilder;", "toString", {}, "Ljava/lang/String;"};
  auto c = fixture({allocation, constructor, unit, add,
                    insn(11, "move-result-object", {2}), text,
                    insn(15, "move-result-object", {1}),
                    insn(16, "return-object", {1})},
                   3, "Ljava/lang/String;");
  Program code({c}, std::map<std::string, std::string>{});
  Runtime state(code, heap);
  auto result =
      state.execute(*code.findStatic(c.methods[0].reference), {}, admit, 65536);
  ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  EXPECT_EQ(heap->get(result.value)->Bytes, (std::vector<uint8_t>{0, 0xd8}));
  for (auto parent :
       {"Ljava/io/Serializable;", "Ljava/lang/Appendable;",
        "Ljava/lang/CharSequence;", "Ljava/lang/AbstractStringBuilder;"})
    EXPECT_TRUE(code.assignable("Ljava/lang/StringBuilder;", parent));
}
} // namespace
} // namespace neverd::mobile::dalvik
