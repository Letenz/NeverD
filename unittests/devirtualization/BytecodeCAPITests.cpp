#include "../lift/NeverDLiftFixture.h"

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/Support/JSON.h"

namespace {
class BytecodeCAPI : public NeverDLiftTest {
protected:
  const std::vector<unsigned char> Code{0x41, 0, 9, 0xfe};

  llvm::json::Value request() {
    auto Value = llvm::json::parse(R"({"schemaVersion":1,"base":16,
      "functions":[{"entry":16,"end":20,"name":"api_sum"}],
      "profile":{"version":1,"register_bytes":12,"byte_order":"little","encodings":[
        {"size":3,"match":[{"offset":0,"value":65}],"operations":[
          {"op":"INT_ADD","output":{"space":"reg","size":4,"value":{}},
           "inputs":[{"space":"reg","size":4,"value":{}},
             {"space":"const","size":4,"value":{"offset":2,"bytes":1}}]}]},
        {"size":1,"match":[{"offset":0,"value":254}],
         "operations":[{"op":"RETURN","inputs":[]}]}]}})");
    EXPECT_TRUE(bool(Value));
    return std::move(*Value);
  }

  llvm::json::Value run(const llvm::json::Value &Request,
                        llvm::ArrayRef<unsigned char> Bytes) {
    std::string Text;
    llvm::raw_string_ostream(Text) << Request;
    // The input length is authoritative, including a non-JSON trailing byte.
    const auto Size = Text.size();
    Text += 'x';
    const char *Owned = neverd_bytecode_recover_json_v1(
        Bytes.data(), Bytes.size(), Text.data(), Size);
    EXPECT_NE(Owned, nullptr);
    if (!Owned)
      return nullptr;
    auto Parsed = llvm::json::parse(Owned);
    neverd_free_string(Owned);
    EXPECT_TRUE(bool(Parsed));
    if (!Parsed) {
      llvm::consumeError(Parsed.takeError());
      return nullptr;
    }
    return std::move(*Parsed);
  }
};

TEST_F(BytecodeCAPI, BothSourceRoutesExecuteNarrowWritesWithCanaries) {
  for (unsigned Route : {0u, 1u, 2u}) {
    SCOPED_TRACE(Route);
    auto Request = request();
    (*Request.getAsObject())["output"] = Route ? "llvmc" : "highc";
    (*Request.getAsObject())["optimize"] = Route == 2;
    auto Result = run(Request, Code);
    const auto *Object = Result.getAsObject();
    ASSERT_NE(Object, nullptr);
    ASSERT_EQ(Object->getBoolean("ok"), true);
    EXPECT_EQ(Object->getInteger("functions"), 1);
    EXPECT_EQ(Object->getInteger("blocks"), 1);
    EXPECT_EQ(Object->getInteger("decoded_instructions"), 2);
    EXPECT_EQ(Object->getInteger("decoded_bytes"), 4);
    EXPECT_EQ(Object->getString("scope"), "state-c");
    auto Source = Object->getString("source");
    ASSERT_TRUE(Source);
    if (!hasCrossTargetClang())
      GTEST_SKIP() << "host Clang is unavailable";
    auto File = tmpFile("bytecode-api.c");
    std::ofstream(File) << Source->str() << R"(
#include <string.h>
int main(void) {
  unsigned char memory[14];
  for (unsigned i = 0; i < 1024; ++i) {
    uint32_t x = i * UINT32_C(0x1020304), expected = x + 9, got;
    memset(memory, 0x5a, sizeof(memory));
    memcpy(memory + 1, &x, 4);
    if (api_sum(memory + 1) != 0) return 1;
    memcpy(&got, memory + 1, 4);
    if (got != expected || memory[0] != 0x5a) return 2;
    for (unsigned j = 5; j < sizeof(memory); ++j)
      if (memory[j] != 0x5a) return 3;
  }
  return 0;
})";
    for (const auto *Optimization : {"-O0", "-O2"}) {
      auto Exe = tmpFile(std::string("bytecode-api") +
                         neverd::test::executableSuffix());
      auto Built = exec(NEVERD_TEST_CLANG,
                        {"-std=c11", Optimization, "-Werror",
                         "-fsanitize=undefined", "-fsanitize-trap=undefined",
                         File.string(), "-o", Exe.string()});
      ASSERT_TRUE(Built.ok()) << Built.err;
      EXPECT_TRUE(exec(Exe.string(), {}).ok());
    }
  }
}

TEST_F(BytecodeCAPI, CheckReportsReachableCoverageWithoutPartialSource) {
  auto Request = request();
  (*Request.getAsObject())["output"] = "check";
  auto Bytes = Code;
  Bytes.push_back(0);
  auto Result = run(Request, Bytes);
  ASSERT_NE(Result.getAsObject(), nullptr);
  EXPECT_EQ(Result.getAsObject()->getBoolean("ok"), true);
  EXPECT_EQ(Result.getAsObject()->getInteger("input_bytes"), 5);
  EXPECT_EQ(Result.getAsObject()->getInteger("decoded_bytes"), 4);
  EXPECT_EQ(Result.getAsObject()->getString("source"), "");
  EXPECT_EQ(Result.getAsObject()->getString("scope"), "cfg");
}

TEST_F(BytecodeCAPI, InvalidRulesAndUnknownInstructionsFailWithoutSource) {
  for (unsigned Case = 0; Case != 8; ++Case) {
    auto Request = request();
    auto &Object = *Request.getAsObject();
    auto Bytes = Code;
    switch (Case) {
    case 0:
      Object["extra"] = true;
      break;
    case 1:
      Object["output"] = "rust";
      break;
    case 2:
      Object["optimize"] = true;
      break;
    case 3:
      Object["unaligned_pointers"] = 1;
      break;
    case 4:
      Object["base"] = -1;
      break;
    case 5:
      Object["bindings"] = true;
      break;
    case 6:
      Bytes[0] = 0;
      break;
    case 7:
      Bytes.pop_back();
      break;
    }
    auto Result = run(Request, Bytes);
    ASSERT_NE(Result.getAsObject(), nullptr);
    EXPECT_EQ(Result.getAsObject()->getBoolean("ok"), false) << Case;
    EXPECT_TRUE(Result.getAsObject()->getString("error")) << Case;
    EXPECT_EQ(Result.getAsObject()->get("source"), nullptr) << Case;
  }
}

TEST_F(BytecodeCAPI, RejectsBufferLengthsBeforeAccess) {
  for (unsigned Case = 0; Case != 4; ++Case) {
    const char *Result = nullptr;
    switch (Case) {
    case 0:
      Result = neverd_bytecode_recover_json_v1(nullptr, 1, "{}", 2);
      break;
    case 1:
      Result = neverd_bytecode_recover_json_v1(Code.data(), 1, nullptr, 2);
      break;
    case 2:
      Result = neverd_bytecode_recover_json_v1(Code.data(), 67108865, "{}", 2);
      break;
    case 3:
      Result = neverd_bytecode_recover_json_v1(Code.data(), 1, "{}", 67108865);
      break;
    }
    ASSERT_NE(Result, nullptr);
    auto Parsed = llvm::json::parse(Result);
    neverd_free_string(Result);
    ASSERT_TRUE(bool(Parsed));
    ASSERT_NE(Parsed->getAsObject(), nullptr);
    EXPECT_EQ(Parsed->getAsObject()->getBoolean("ok"), false);
  }
}

TEST_F(BytecodeCAPI, MalformedJSONReturnsAnOwnedUTF8Error) {
  for (const std::string &Text :
       {std::string("[]"), std::string("{\"schemaVersion\":2}"),
        std::string("{\"schemaVersion\":1}\0extra", 25),
        std::string("{\"schemaVersion\":\xff}")}) {
    const char *Owned = neverd_bytecode_recover_json_v1(
        Code.data(), Code.size(), Text.data(), Text.size());
    ASSERT_NE(Owned, nullptr);
    auto Parsed = llvm::json::parse(Owned);
    neverd_free_string(Owned);
    ASSERT_TRUE(bool(Parsed));
    ASSERT_NE(Parsed->getAsObject(), nullptr);
    EXPECT_EQ(Parsed->getAsObject()->getBoolean("ok"), false);
    EXPECT_TRUE(Parsed->getAsObject()->getString("error"));
    EXPECT_EQ(Parsed->getAsObject()->get("source"), nullptr);
  }
}
} // namespace
