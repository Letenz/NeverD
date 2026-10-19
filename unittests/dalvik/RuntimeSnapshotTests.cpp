//===- RuntimeSnapshotTests.cpp - Observation without Java execution ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/dalvik/Library.h"

#include <algorithm>

namespace neverd::mobile::dalvik {
namespace {
using Phase = AndroidApplicationSnapshot::Phase;
using Loader = AndroidApplicationSnapshot::ClassLookupFailure::Loader;
using Init = JavaClassState::Initialization;
const AdmitInstruction Admit = [] { return ExecutionStop::Returned; };
const LibraryMethod &operation(LibraryOperation op) {
  for (const auto &method : libraryMethods())
    if (method.Op == op)
      return method;
  std::abort();
}
Class declaration() {
  Class type;
  type.name = "Ltest/Observed;";
  type.superclass = "Ljava/lang/Object;";
  type.access = {"public"};
  // A supplied label is not authority to claim a platform definition.
  type.source_id = "android-api28-framework";
  return type;
}
JavaClassState row(const RuntimeSnapshot &state, std::string_view name) {
  auto it = std::find_if(state.classes.begin(), state.classes.end(),
                         [&](const auto &c) { return c.descriptor == name; });
  EXPECT_NE(it, state.classes.end());
  return it == state.classes.end() ? JavaClassState{} : *it;
}
TEST(DalvikSnapshot, SeparatesSuppliedOwnershipFromLabelsAndLoaderAssumptions) {
  Program program({declaration()}, std::map<std::string, std::string>{});
  for (auto phase :
       {std::optional<Phase>{}, std::optional{Phase::Unattached},
        std::optional{Phase::Attached}, std::optional{Phase::Application}}) {
    auto heap = std::make_shared<Heap>();
    Runtime runtime(program, heap,
                    phase ? std::optional{AndroidApplicationSnapshot{*phase}}
                          : std::nullopt);
    auto before = runtime.snapshot();
    EXPECT_EQ(before, runtime.snapshot());
    EXPECT_EQ(before.framework.declared_phase, phase);
    EXPECT_EQ(before.framework.activity_thread, 0u);
    EXPECT_EQ(before.framework.application, 0u);
    EXPECT_EQ(before.framework.application_loader, 0u);
    EXPECT_FALSE(before.framework.instrumentation);
    EXPECT_FALSE(before.terminal);
    auto supplied = row(before, "Ltest/Observed;");
    EXPECT_TRUE(supplied.supplied);
    EXPECT_EQ(supplied.initialization, Init::NotStarted);
    EXPECT_EQ(supplied.class_object, 0u);
    EXPECT_EQ(supplied.projected_loader,
              phase == Phase::Application ? std::optional{Loader::Application}
                                          : std::nullopt);
    auto framework = row(before, "Landroid/app/ActivityThread;");
    EXPECT_FALSE(framework.supplied);
    EXPECT_EQ(supplied.source_id, framework.source_id);
    EXPECT_EQ(framework.projected_loader,
              phase ? std::optional{Loader::Boot} : std::nullopt);
    EXPECT_TRUE(std::is_sorted(before.classes.begin(), before.classes.end(),
                               [](const auto &a, const auto &b) {
                                 return a.descriptor < b.descriptor;
                               }));
    // Inspection allocated no class, loader, string or framework object.
    EXPECT_EQ(heap->allocate(Object::Instance, "java/lang/Object", 0)->Identity,
              1u);
    auto object = heap->classObject("test/Observed");
    auto materialized = row(runtime.snapshot(), "Ltest/Observed;");
    EXPECT_EQ(materialized.class_object, object->Identity);
    EXPECT_EQ(materialized.initialization, Init::NotStarted);
    EXPECT_EQ(runtime.initialize("Ltest/Observed;", Admit, 65536).stop,
              ExecutionStop::Returned);
    EXPECT_EQ(row(runtime.snapshot(), "Ltest/Observed;").initialization,
              Init::Initialized);
  }
}
TEST(DalvikSnapshot, RetainsIdentitiesAndInstrumentationWritesWithoutReads) {
  Program program({}, std::map<std::string, std::string>{});
  auto heap = std::make_shared<Heap>();
  AndroidApplicationSnapshot input{Phase::Application};
  input.instrumentation = AndroidApplicationSnapshot::Instrumentation::Platform;
  Runtime runtime(program, heap, input);
  auto call = [&](LibraryOperation op, uint64_t receiver = 0) {
    auto r = runtime.library(operation(op), receiver, {}, Admit);
    EXPECT_EQ(r.stop, ExecutionStop::Returned) << r.diagnostic;
    return r.value;
  };
  auto thread = call(CurrentActivityThread);
  auto app = call(ThreadApplication, thread);
  auto loader = call(ContextClassLoader, app);
  auto boot = call(LoaderParent, loader);
  auto context = call(BaseContext, app);
  auto system = call(SystemClassLoader);
  auto before = runtime.snapshot();
  EXPECT_EQ(before.framework.activity_thread, thread);
  EXPECT_EQ(before.framework.application, app);
  EXPECT_EQ(before.framework.context, context);
  EXPECT_EQ(before.framework.application_loader, loader);
  EXPECT_EQ(before.framework.boot_loader, boot);
  EXPECT_EQ(before.framework.system_loader, system);
  EXPECT_FALSE(before.framework.instrumentation); // Supplied, but not read.
  auto instrument = call(ThreadInstrumentation, thread);
  EXPECT_EQ(runtime.snapshot().framework.instrumentation, instrument);
  const auto *field =
      program.findField({"Landroid/app/ActivityThread;", "mInstrumentation",
                         "Landroid/app/Instrumentation;"},
                        false);
  ASSERT_NE(field, nullptr);
  ASSERT_EQ(runtime.field(*field, thread, 0, Admit, 65536).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(runtime.snapshot().framework.instrumentation, 0u);
  EXPECT_EQ(call(ThreadInstrumentation, thread), 0u);
  EXPECT_EQ(call(CurrentApplication), app);
}
TEST(DalvikSnapshot, RecordsFailedAndInterruptedInitializersWithoutRetrying) {
  for (bool terminal : {false, true}) {
    auto type = declaration();
    Method init;
    init.reference = {type.name, "<clinit>", {}, "V"};
    init.access = {"static", "constructor"};
    init.registers = 1;
    Instruction zero, failure;
    zero.pc = 0;
    zero.opcode = "const/4";
    zero.registers = {0};
    zero.literal = int64_t(0);
    failure.pc = 1;
    failure.opcode = "throw";
    failure.registers = {0};
    init.instructions = {zero, failure};
    type.methods.push_back(init);
    Program program({type});
    Runtime runtime(program);
    unsigned ticks = 0;
    bool saw_initializing = false;
    auto result = runtime.initialize(
        type.name,
        [&] {
          ++ticks;
          saw_initializing |=
              row(runtime.snapshot(), type.name).initialization ==
              Init::Initializing;
          return terminal ? ExecutionStop::InstructionLimit
                          : ExecutionStop::Returned;
        },
        65536);
    EXPECT_TRUE(saw_initializing);
    EXPECT_GT(ticks, 0u);
    EXPECT_EQ(result.stop, terminal ? ExecutionStop::InstructionLimit
                                    : ExecutionStop::Thrown);
    auto stopped = runtime.snapshot();
    EXPECT_EQ(stopped.terminal, terminal);
    EXPECT_EQ(row(stopped, type.name).initialization,
              terminal ? Init::Initializing : Init::Erroneous);
    EXPECT_EQ(runtime.snapshot(), stopped);
    auto retry = runtime.initialize(type.name, Admit, 65536);
    EXPECT_EQ(retry.stop, result.stop);
    if (!terminal)
      EXPECT_EQ(retry.exception, "java/lang/NoClassDefFoundError");
    EXPECT_EQ(runtime.snapshot(), stopped);
  }
}
TEST(DalvikSnapshot, ObservationDoesNotChangeBudgetOrHeapExhaustion) {
  Program program({}, std::map<std::string, std::string>{});
  auto execute = [&](bool observe) {
    auto heap = std::make_shared<Heap>(512, 1);
    Runtime runtime(program, heap,
                    AndroidApplicationSnapshot{Phase::Application});
    unsigned ticks = 0;
    auto admit = [&] {
      ++ticks;
      return ExecutionStop::Returned;
    };
    if (observe)
      (void)runtime.snapshot();
    auto app = runtime.library(operation(CurrentApplication), 0, {}, admit);
    EXPECT_EQ(app.stop, ExecutionStop::Returned);
    if (observe)
      (void)runtime.snapshot();
    auto loader =
        runtime.library(operation(ContextClassLoader), app.value, {}, admit);
    EXPECT_EQ(loader.stop, ExecutionStop::HeapLimit);
    return std::pair{runtime.snapshot(), ticks};
  };
  EXPECT_EQ(execute(false), execute(true));
}
TEST(DalvikSnapshot, ExplicitApplicationLoaderCanPrecedeApplicationCreation) {
  Program program({declaration()}, std::map<std::string, std::string>{});
  for (auto established :
       {std::optional<bool>{}, std::optional{false}, std::optional{true}}) {
    auto heap = std::make_shared<Heap>();
    AndroidApplicationSnapshot input{Phase::Attached};
    input.application_loader_established = established;
    if (established == true)
      input.class_lookup_failures = {{Loader::Application, "test.Absent", {}}};
    Runtime runtime(program, heap, input);
    auto klass = heap->classObject("test/Observed");
    auto loader =
        runtime.library(operation(ClassLoaderOf), klass->Identity, {}, Admit);
    EXPECT_EQ(loader.stop, established == true ? ExecutionStop::Returned
                                               : ExecutionStop::Unsupported);
    auto snapshot = runtime.snapshot();
    EXPECT_EQ(snapshot.framework.application_loader_established, established);
    EXPECT_EQ(snapshot.framework.application, 0u);
    EXPECT_EQ(row(snapshot, "Ltest/Observed;").initialization,
              Init::NotStarted);
    if (established != true)
      continue;
    EXPECT_NE(loader.value, 0u);
    EXPECT_EQ(snapshot.framework.application_loader, loader.value);
    EXPECT_EQ(row(snapshot, "Ltest/Observed;").projected_loader,
              Loader::Application);
    auto boot =
        runtime.library(operation(LoaderParent), loader.value, {}, Admit);
    ASSERT_EQ(boot.stop, ExecutionStop::Returned);
    EXPECT_NE(boot.value, loader.value);
    const uint64_t known[]{heap->literal("test.Observed")->Identity};
    auto found =
        runtime.library(operation(LoadClass), loader.value, known, Admit);
    EXPECT_EQ(found.stop, ExecutionStop::Returned);
    EXPECT_EQ(found.value, klass->Identity);
    const uint64_t missing[]{heap->literal("test.Absent")->Identity};
    auto absent =
        runtime.library(operation(LoadClass), loader.value, missing, Admit);
    EXPECT_EQ(absent.stop, ExecutionStop::Thrown);
    EXPECT_EQ(absent.exception, "java/lang/ClassNotFoundException");
    auto app = runtime.library(operation(CurrentApplication), 0, {}, Admit);
    EXPECT_EQ(app.stop, ExecutionStop::Returned);
    EXPECT_EQ(app.value, 0u);
    EXPECT_FALSE(runtime.snapshot().terminal);
  }
  for (auto phase : {Phase::Unattached, Phase::Application}) {
    AndroidApplicationSnapshot input{phase};
    input.application_loader_established = phase == Phase::Unattached;
    EXPECT_THROW(Runtime(program, {}, input), Error);
  }
}
} // namespace
} // namespace neverd::mobile::dalvik
