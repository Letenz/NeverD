//===- AndroidVersionTests.cpp - Shared platform field state -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent scalar, identity, DEX/JNI storage and refusal cases.
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/dalvik/Runtime.h"

namespace neverd::mobile::dalvik {
namespace {
constexpr auto Version = "Landroid/os/Build$VERSION;";
const AdmitInstruction Admit = [] { return ExecutionStop::Returned; };
using Properties = std::map<std::string, std::string>;
const Field *field(const Program &program, const char *name,
                   const char *type = "Ljava/lang/String;") {
  return program.findStaticField({Version, name, type});
}
TEST(AndroidVersion, ExplicitPlatformAndUnreplaceableDeclarations) {
  Program ordinary({});
  EXPECT_FALSE(ordinary.containsClass(Version));
  Program android({}, Properties{});
  ASSERT_TRUE(android.containsClass(Version));
  EXPECT_TRUE(field(android, "RELEASE"));
  EXPECT_FALSE(field(android, "RELEASE", "I"));
  EXPECT_FALSE(field(android, "ACTIVE_CODENAMES", "[Ljava/lang/String;"));
  EXPECT_FALSE(android.findField({Version, "SDK_INT", "I"}, false));
  Class replacement;
  replacement.name = Version;
  EXPECT_THROW(Program({replacement}, Properties{}), Error);
  Field forged = *field(android, "RELEASE");
  EXPECT_EQ(android.platformValue(forged), nullptr);
}
TEST(AndroidVersion, PropertyStringsAreStableButAreNotInternedLiterals) {
  Program program(
      {}, Properties{{"ro.build.version.release", "9\xf0\x9f\x98\x80"},
                     {"ro.build.version.incremental", "9\xf0\x9f\x98\x80"}});
  auto heap = std::make_shared<Heap>();
  Runtime runtime(program, heap);
  auto read = [&](const char *name) {
    auto result = runtime.field(*field(program, name), 0, {}, Admit, 65536);
    EXPECT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    return result.value;
  };
  auto release = read("RELEASE");
  auto incremental = read("INCREMENTAL");
  ASSERT_TRUE(heap->get(release));
  EXPECT_EQ(read("RELEASE"), release);
  EXPECT_NE(release, incremental);
  EXPECT_EQ(heap->get(release)->Bytes, heap->get(incremental)->Bytes);
  EXPECT_EQ(heap->get(release)->Bytes,
            (std::vector<uint8_t>{'9', 0, 0x3d, 0xd8, 0, 0xde}));
  EXPECT_NE(release, heap->literal("9\xf0\x9f\x98\x80")->Identity);
  EXPECT_EQ(read("BASE_OS"), heap->literal("")->Identity);
  EXPECT_EQ(read("CODENAME"), heap->literal("unknown")->Identity);
  EXPECT_EQ(read("SDK"), read("CODENAME"));
}
TEST(AndroidVersion, IntegerParsingUsesAndroidPrefixesAndSignedBounds) {
  const std::pair<const char *, uint32_t> cases[] = {
      {"28", 28},
      {"028", 28},
      {"0x1c", 28},
      {"0X1C", 28},
      {" +28", 28},
      {"\t-1", 0xffffffff},
      {"-2147483648", 0x80000000},
      {"2147483647", 0x7fffffff},
      {"2147483648", 0},
      {"-2147483649", 0},
      {"0xffffffff", 0},
      {"0x80000000", 0},
      {"+0x1c", 0},
      {" 0x1c", 0},
      {"-0x1c", 0},
      {"28 ", 0},
      {"28tail", 0},
      {"", 0},
      {"+", 0},
      {"0x", 0},
      {"999999999999999999999999999999", 0}};
  for (auto [text, expected] : cases) {
    SCOPED_TRACE(text);
    Program program(
        {}, Properties{{"ro.build.version.sdk", text},
                       {"ro.product.first_api_level", text},
                       {"ro.build.version.preview_sdk", text},
                       {"ro.build.version.min_supported_target_sdk", text}});
    Runtime runtime(program);
    for (const char *name : {"SDK_INT", "FIRST_SDK_INT", "PREVIEW_SDK_INT",
                             "MIN_SUPPORTED_TARGET_SDK_INT"}) {
      auto result =
          runtime.field(*field(program, name, "I"), 0, {}, Admit, 65536);
      EXPECT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
      EXPECT_EQ(result.value, expected);
    }
  }
}
TEST(AndroidVersion, StringFieldsUseIndependentNamedProperties) {
  Program program({},
                  Properties{{"ro.build.version.incremental", "build-123"},
                             {"ro.build.version.release", "9.2"},
                             {"ro.build.version.base_os", "base-os"},
                             {"ro.build.version.security_patch", "2018-08-05"},
                             {"ro.build.version.sdk", "28"},
                             {"ro.build.version.codename", "REL"}});
  auto heap = std::make_shared<Heap>();
  Runtime runtime(program, heap);
  for (const auto &[name, expected] : {std::pair{"INCREMENTAL", "build-123"},
                                       {"RELEASE", "9.2"},
                                       {"BASE_OS", "base-os"},
                                       {"SECURITY_PATCH", "2018-08-05"},
                                       {"SDK", "28"},
                                       {"CODENAME", "REL"}}) {
    SCOPED_TRACE(name);
    auto result = runtime.field(*field(program, name), 0, {}, Admit, 65536);
    ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    auto object = heap->get(result.value);
    ASSERT_TRUE(object);
    EXPECT_EQ(object->Bytes, javaStringBytes(expected));
  }
}
TEST(AndroidVersion, DexReadsTheSameObjectAsDirectFieldAccess) {
  Class consumer;
  consumer.name = "Ltest/VersionReader;";
  consumer.access = {"public"};
  consumer.superclass = "Ljava/lang/Object;";
  Method read;
  read.reference = {consumer.name, "read", {}, "Ljava/lang/String;"};
  read.access = {"public", "static"};
  read.registers = 1;
  Instruction get;
  get.opcode = "sget-object";
  get.registers = {0};
  get.reference = FieldRef{Version, "RELEASE", "Ljava/lang/String;"};
  Instruction ret;
  ret.pc = 2;
  ret.opcode = "return-object";
  ret.registers = {0};
  read.instructions = {get, ret};
  consumer.methods.push_back(read);
  Program program({consumer}, Properties{{"ro.build.version.release", "9.2"}});
  Runtime runtime(program);
  auto direct = runtime.field(*field(program, "RELEASE"), 0, {}, Admit, 65536);
  auto dex =
      runtime.execute(*program.findStatic(read.reference), {}, Admit, 65536);
  EXPECT_EQ(direct.stop, ExecutionStop::Returned) << direct.diagnostic;
  EXPECT_EQ(dex.stop, ExecutionStop::Returned) << dex.diagnostic;
  EXPECT_NE(direct.value, 0u);
  EXPECT_EQ(dex.value, direct.value);
}
TEST(AndroidVersion, RefusalsDoNotCreateAReplacementOrRestartTheHeap) {
  EXPECT_THROW(Program({}, Properties{{"ro.build.version.release",
                                       std::string(92, 'x')}}),
               Error);
  EXPECT_THROW(Program({}, Properties{{"ro.build.version.release",
                                       std::string("a\0b", 3)}}),
               Error);
  EXPECT_THROW(Program({}, Properties{{"ro.build.version.release", "\xff"}}),
               Error);
  Program program({}, Properties{});
  Runtime runtime(program);
  auto denied =
      runtime.field(*field(program, "SDK_INT", "I"), 0, 999, Admit, 65536);
  EXPECT_EQ(denied.stop, ExecutionStop::Invalid);
  EXPECT_NE(denied.diagnostic.find("final field"), std::string::npos);
  EXPECT_EQ(
      runtime.field(*field(program, "SDK_INT", "I"), 0, {}, Admit, 65536).stop,
      ExecutionStop::Invalid);
  Runtime bounded(program, std::make_shared<Heap>(16, 1));
  auto exhausted =
      bounded.field(*field(program, "RELEASE"), 0, {}, Admit, 65536);
  EXPECT_EQ(exhausted.stop, ExecutionStop::HeapLimit);
  EXPECT_EQ(
      bounded.field(*field(program, "RELEASE"), 0, {}, Admit, 65536).diagnostic,
      exhausted.diagnostic);
}
} // namespace
} // namespace neverd::mobile::dalvik
