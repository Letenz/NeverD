//===- RuntimeResumeTests.cpp - Java activation and external return state ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/dalvik/Runtime.h"

namespace neverd::mobile::dalvik {
namespace {
Instruction op(unsigned pc, const char *name, std::vector<unsigned> regs = {}) {
  Instruction i;
  i.pc = pc;
  i.opcode = name;
  i.registers = std::move(regs);
  return i;
}
Instruction literal(unsigned pc, int value) {
  auto i = op(pc, "const", {0});
  i.literal = int64_t(value);
  return i;
}
constexpr const char *Owner = "Lresume/Example;";
MethodRef nativeRef{Owner, "nativeValue", {}, "I"};
FieldRef countRef{Owner, "count", "I"};
MethodRef runRef{Owner, "run", {}, "I"};
Class fixture(bool loading = false) {
  Class c;
  c.name = Owner;
  c.superclass = "Ljava/lang/Object;";
  c.access = {"public"};
  Field f;
  f.reference = countRef;
  f.access = {"public", "static"};
  c.fields.push_back(f);
  Method n;
  n.reference = nativeRef;
  n.access = {"public", "static", "native"};
  c.methods.push_back(n);
  auto store = op(1, "sput", {0});
  store.reference = countRef;
  auto call = op(2, "invoke-static");
  call.reference = nativeRef;
  Method run;
  run.reference = runRef;
  run.access = {"public", "static"};
  run.registers = 2;
  run.instructions = {literal(0, 1),
                      store,
                      call,
                      op(3, "move-result", {1}),
                      call,
                      op(5, "move-result", {0}),
                      op(6, "add-int", {0, 0, 1}),
                      op(7, "return", {0})};
  run.instructions[4].pc = 4;
  c.methods.push_back(run);
  if (loading) {
    Method init;
    init.reference = {Owner, "<clinit>", {}, "V"};
    init.access = {"static"};
    init.registers = 1;
    auto name = op(2, "const-string", {0});
    name.literal = std::string("sample");
    auto load = op(3, "invoke-static", {0});
    load.reference = MethodRef{
        "Ljava/lang/System;", "loadLibrary", {"Ljava/lang/String;"}, "V"};
    auto after = store;
    after.pc = 5;
    init.instructions = {
        literal(0, 1),       store, name, load, literal(4, 2), after,
        op(6, "return-void")};
    c.methods.push_back(init);
  }
  return c;
}
AdmitInstruction budget(uint64_t &remaining) {
  return [&] {
    if (!remaining)
      return ExecutionStop::InstructionLimit;
    --remaining;
    return ExecutionStop::Returned;
  };
}
TEST(DalvikResume, PreservesRegistersEffectsAndOccurrenceTokens) {
  Program p({fixture()});
  Runtime r(p);
  uint64_t left = 100;
  auto admit = budget(left);
  auto a = r.execute(*p.findStatic(runRef), {}, admit, 65536, 7);
  ASSERT_EQ(a.stop, ExecutionStop::Suspended) << a.diagnostic;
  ASSERT_TRUE(a.external);
  EXPECT_EQ(a.external->method, p.findStatic(nativeRef));
  EXPECT_FALSE(r.snapshot().terminal);
  const auto *field = p.findStaticField(countRef);
  EXPECT_EQ(r.field(*field, 0, {}, admit, 65536, 7).value, 1u);
  ASSERT_EQ(r.field(*field, 0, 9, admit, 65536, 7).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(r.resume(a.continuation, 8, {}).stop, ExecutionStop::Invalid);
  auto b = r.resume(a.continuation, 7, {ExecutionStop::Returned, 13});
  ASSERT_EQ(b.stop, ExecutionStop::Suspended);
  EXPECT_NE(a.continuation, b.continuation);
  EXPECT_EQ(r.resume(a.continuation, 7, {}).stop, ExecutionStop::Invalid);
  auto done = r.resume(b.continuation, 7, {ExecutionStop::Returned, 29});
  EXPECT_EQ(done.stop, ExecutionStop::Returned) << done.diagnostic;
  EXPECT_EQ(done.value, 42u);
  EXPECT_EQ(r.field(*field, 0, {}, admit, 65536, 7).value, 9u);
  EXPECT_EQ(left, 92u); // Eight original instructions; no replay.
  EXPECT_EQ(r.resume(b.continuation, 7, {}).stop, ExecutionStop::Invalid);
}
TEST(DalvikResume, ClassInitializerRetainsItsOwnerAndFinishesAfterLoad) {
  Program p({fixture(true)});
  Runtime r(p);
  uint64_t left = 100;
  auto admit = budget(left);
  auto a = r.initialize(Owner, admit, 65536, 7);
  ASSERT_EQ(a.stop, ExecutionStop::Suspended) << a.diagnostic;
  ASSERT_TRUE(a.external);
  EXPECT_EQ(a.external->kind, ExternalCall::LoadLibrary);
  EXPECT_EQ(a.external->library, "sample");
  EXPECT_EQ(a.external->caller, Owner);
  EXPECT_EQ(r.field(*p.findStaticField(countRef), 0, {}, admit, 65536, 7).value,
            1u);
  EXPECT_EQ(r.initialize(Owner, admit, 65536, 7).stop, ExecutionStop::Returned);
  auto done = r.resume(a.continuation, 7, {});
  EXPECT_EQ(done.stop, ExecutionStop::Returned) << done.diagnostic;
  EXPECT_EQ(r.field(*p.findStaticField(countRef), 0, {}, admit, 65536, 7).value,
            2u);
  auto remaining = left;
  EXPECT_EQ(r.initialize(Owner, admit, 65536, 7).stop, ExecutionStop::Returned);
  EXPECT_EQ(left, remaining);
}
TEST(DalvikResume, OtherThreadCannotReadPartiallyInitializedClass) {
  Program p({fixture(true)});
  Runtime r(p);
  uint64_t left = 100;
  auto admit = budget(left);
  ASSERT_EQ(r.initialize(Owner, admit, 65536, 7).stop,
            ExecutionStop::Suspended);
  auto foreign = r.field(*p.findStaticField(countRef), 0, {}, admit, 65536, 8);
  EXPECT_EQ(foreign.stop, ExecutionStop::Unsupported);
  EXPECT_TRUE(r.snapshot().terminal);
}
TEST(DalvikResume, ThrowingLoadMarksClassErroneousAndPreservesThrowable) {
  Program p({fixture(true)});
  auto heap = std::make_shared<Heap>();
  Runtime r(p, heap);
  uint64_t left = 100;
  auto admit = budget(left);
  auto a = r.initialize(Owner, admit, 65536, 7);
  ASSERT_EQ(a.stop, ExecutionStop::Suspended);
  auto thrown =
      heap->allocate(Object::Throwable, "java/lang/UnsatisfiedLinkError", 0);
  ExecutionResult error{ExecutionStop::Thrown, 0, thrown->ClassName};
  error.throwable = thrown->Identity;
  auto done = r.resume(a.continuation, 7, error);
  EXPECT_EQ(done.stop, ExecutionStop::Thrown);
  EXPECT_EQ(done.throwable, thrown->Identity);
  EXPECT_FALSE(r.snapshot().terminal);
  auto again = r.initialize(Owner, admit, 65536, 7);
  EXPECT_EQ(again.exception, "java/lang/NoClassDefFoundError");
}
TEST(DalvikResume, NativeExceptionEntersOriginalDexHandler) {
  auto c = fixture();
  auto &m = c.methods[1];
  m.instructions = {op(0, "invoke-static"), op(1, "move-result", {0}),
                    op(2, "return", {0}),   op(3, "move-exception", {1}),
                    literal(4, 73),         op(5, "return", {0})};
  m.instructions[0].reference = nativeRef;
  m.tries = {
      {0, 1, {{std::string("Ljava/lang/IllegalArgumentException;"), 3}}}};
  Program p({c});
  Runtime r(p);
  uint64_t left = 100;
  auto a = r.execute(*p.findStatic(runRef), {}, budget(left), 65536, 7);
  ASSERT_EQ(a.stop, ExecutionStop::Suspended);
  auto done = r.resume(
      a.continuation, 7,
      {ExecutionStop::Thrown, 0, "java/lang/IllegalArgumentException"});
  EXPECT_EQ(done.stop, ExecutionStop::Returned) << done.diagnostic;
  EXPECT_EQ(done.value, 73u);
  EXPECT_EQ(left, 96u);
}
TEST(DalvikResume, NestedOwnerCallsMustFinishBeforeTheirParent) {
  Program p({fixture()});
  Runtime r(p);
  uint64_t left = 100;
  auto a = r.execute(*p.findStatic(nativeRef), {}, budget(left), 65536, 7);
  auto b = r.execute(*p.findStatic(nativeRef), {}, budget(left), 65536, 7);
  ASSERT_EQ(a.stop, ExecutionStop::Suspended);
  ASSERT_EQ(b.stop, ExecutionStop::Suspended);
  EXPECT_EQ(r.resume(a.continuation, 7, {}).stop, ExecutionStop::Invalid);
  EXPECT_EQ(r.resume(b.continuation, 7, {ExecutionStop::Returned, 3}).value,
            3u);
  EXPECT_EQ(r.resume(a.continuation, 7, {ExecutionStop::Returned, 4}).value,
            4u);
}
TEST(DalvikResume, BudgetAndReturnValidationRemainTerminalAfterSuspension) {
  for (bool invalid : {false, true}) {
    Program p({fixture()});
    Runtime r(p);
    uint64_t left = 3;
    auto a = r.execute(*p.findStatic(runRef), {}, budget(left), 65536, 7);
    ASSERT_EQ(a.stop, ExecutionStop::Suspended);
    auto done = r.resume(a.continuation, 7,
                         {ExecutionStop::Returned, invalid ? UINT64_MAX : 0});
    EXPECT_EQ(done.stop, invalid ? ExecutionStop::Invalid
                                 : ExecutionStop::InstructionLimit);
    EXPECT_TRUE(r.snapshot().terminal);
  }
}
TEST(DalvikResume, SynchronousCallDoesNotClaimNativeExecution) {
  Program p({fixture()});
  uint64_t left = 100;
  auto result = p.execute(*p.findStatic(runRef), {}, budget(left), 65536);
  EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
}
} // namespace
} // namespace neverd::mobile::dalvik
