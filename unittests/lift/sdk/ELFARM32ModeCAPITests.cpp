//===- ELFARM32ModeCAPITests.cpp - Metadata-only mixed-mode sessions
//-------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include "neverd/sdk/NeverDCAPI.h"

namespace {

struct SessionOwner {
  neverd_session_t Value = neverd_session_create();
  ~SessionOwner() { neverd_session_destroy(Value); }
};

std::string takeString(const char *Value) {
  if (!Value)
    return {};
  std::string Result(Value);
  neverd_free_string(Value);
  return Result;
}

class ELFARM32ModeCAPITest : public NeverDLiftTest {};

TEST_F(ELFARM32ModeCAPITest,
       MixedMetadataReplacesAndRestoresAConcreteDecoderInTheSameSession) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "ARM mode fixture requires cross-target clang";
  const std::string Thumb = R"(
.syntax unified
.text
.thumb
.globl thumb32_add
.type thumb32_add,%function
.thumb_func
thumb32_add:
  adds r0, r0, r1
  bx lr
.size thumb32_add, .-thumb32_add
)";
  const std::string ARM = R"(
.arm
.p2align 2
.globl arm32_add
.type arm32_add,%function
arm32_add:
  add r0, r0, r1
  bx lr
.size arm32_add, .-arm32_add
)";
  for (bool Mixed : {false, true}) {
    const auto Source = tmpFile(Mixed ? "mixed.s" : "thumb.s");
    std::ofstream(Source) << Thumb << (Mixed ? ARM : "");
    const auto Object = tmpFile(Mixed ? "mixed.o" : "thumb.o");
    const auto Compiled =
        exec(NEVERD_TEST_CLANG, {"-target", "armv7-linux-gnueabi", "-c",
                                 Source.string(), "-o", Object.string()});
    ASSERT_TRUE(Compiled.ok()) << Compiled.err;
  }
  SessionOwner Session;
  ASSERT_NE(Session.Value, nullptr);
  for (bool Mixed : {false, true, false}) {
    SCOPED_TRACE(Mixed);
    ASSERT_EQ(neverd_session_load(
                  Session.Value,
                  tmpFile(Mixed ? "mixed.o" : "thumb.o").string().c_str()),
              1)
        << neverd_last_error(Session.Value);
    const auto Headers = takeString(neverd_headers_json(Session.Value));
    EXPECT_NE(Headers.find(Mixed ? "mixed_arm_thumb" : "thumb"),
              std::string::npos)
        << Headers;
    const auto Symbols = takeString(neverd_symbols_json(Session.Value));
    EXPECT_NE(Symbols.find("thumb32_add"), std::string::npos) << Symbols;
    if (Mixed)
      EXPECT_NE(Symbols.find("arm32_add"), std::string::npos) << Symbols;
    const auto Disassembly =
        takeString(neverd_disasm_json(Session.Value, 0, 1));
    if (Mixed) {
      EXPECT_EQ(Disassembly, "[]");
      EXPECT_STREQ(neverd_last_error(Session.Value),
                   "mixed ARM/Thumb decoding unsupported");
      EXPECT_EQ(neverd_disasm_text(Session.Value, "thumb32_add", 0), nullptr);
      EXPECT_STREQ(neverd_last_error(Session.Value),
                   "mixed ARM/Thumb decoding unsupported");
      EXPECT_TRUE(takeString(neverd_decompile(Session.Value, 0)).empty());
      EXPECT_STREQ(neverd_last_error(Session.Value),
                   "mixed ARM/Thumb decoding unsupported");
      EXPECT_TRUE(takeString(neverd_decompile_llvm(Session.Value, 0)).empty());
      EXPECT_STREQ(neverd_last_error(Session.Value),
                   "mixed ARM/Thumb decoding unsupported");
      EXPECT_NE(takeString(neverd_headers_json(Session.Value))
                    .find("mixed_arm_thumb"),
                std::string::npos);
    } else {
      EXPECT_NE(Disassembly.find("adds"), std::string::npos) << Disassembly;
    }
  }
}

} // namespace
