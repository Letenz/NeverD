//===- JsonTests.cpp - Transport parser compatibility
//----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Checks acceleration against the transport's numeric and admission contract.
///
//===----------------------------------------------------------------------===//

#include "common/transport/Json.h"
#include "gtest/gtest.h"

using namespace neverd::transport;

namespace {
void rejects(const std::string &Text, const char *Code,
             const char *Message = nullptr) {
  try {
    (void)parseJson(Text);
    FAIL() << "accepted " << Text;
  } catch (const Error &E) {
    EXPECT_EQ(E.code, Code);
    if (Message)
      EXPECT_STREQ(E.what(), Message);
  }
}
} // namespace

TEST(AcceleratedJson, NestedValuesPreserveNumericStorageAndOwnedStrings) {
  const std::vector<std::string> Numbers = {"0",
                                            "-0",
                                            "-1",
                                            "1",
                                            "9223372036854775807",
                                            "9223372036854775808",
                                            "18446744073709551615",
                                            "18446744073709551616",
                                            "-9223372036854775808",
                                            "-9223372036854775809",
                                            "1.0",
                                            "-0.0",
                                            "1e-1000",
                                            "2.2250738585072014e-308",
                                            "4.9406564584124654e-324",
                                            "9007199254740993.0",
                                            "1.7976931348623157e308"};
  for (const auto &Number : Numbers) {
    SCOPED_TRACE(Number);
    std::string Text = "{\"nested\":[{\"value\":" + Number +
                       ",\"string\":\"a\\u0000b\",\"\\u0000key\":true},null]}";
    const auto Want = Json::parse(Text);
    const auto Got = parseJson(Text);
    Text.assign(Text.size(), 'x');
    EXPECT_EQ(Got, Want);
    EXPECT_EQ(Got.dump(), Want.dump());
    EXPECT_EQ(Got["nested"][0]["value"].type(),
              Want["nested"][0]["value"].type());
  }
}

TEST(AcceleratedJson, PreservesNumbersStringsAndRepresentation) {
  for (const std::string Text : {"null",
                                 "true",
                                 "-0",
                                 "0",
                                 "-0.0",
                                 "1e0",
                                 "1e-1000",
                                 "9223372036854775807",
                                 "9223372036854775808",
                                 "18446744073709551615",
                                 "18446744073709551616",
                                 "-9223372036854775808",
                                 "-9223372036854775809",
                                 "1.7976931348623157e308",
                                 "4.9406564584124654e-324",
                                 "\xef\xbb\xbf{\"bom\":true}",
                                 "{\"\\u0000x\":\"a\\u0000b\"}",
                                 "[1,{\"x\":1},{\"x\":2}]",
                                 "{\"x\":{\"x\":1}}",
                                 "{\"a\":\"\\ud83d\\ude00\",\"b\":\"中文\"}"}) {
    SCOPED_TRACE(Text);
    const auto Want = Json::parse(Text);
    const auto Got = parseJson(Text);
    EXPECT_EQ(Got, Want);
    EXPECT_EQ(Got.type(), Want.type());
    EXPECT_EQ(Got.dump(), Want.dump());
  }
}

TEST(AcceleratedJson, DecodedDuplicatesKeepErrorPrecedence) {
  for (const std::string Text :
       {"{\"x\":1,\"x\":2}", "{\"x\":1,\"\\u0078\":2}",
        "{\"\\u0000x\":1,\"\\u0000x\":2}", "{\"😀\":1,\"\\ud83d\\ude00\":2}",
        "[{}, {\"x\":1,\"x\":2}]", "{\"x\":1,\"x\": [broken"})
    rejects(Text, "invalid_json", "Duplicate object key in JSON frame");
  EXPECT_NO_THROW(parseJson("{\"\\u0000x\":1,\"\\u0000y\":2}"));
}

TEST(AcceleratedJson, RejectsMalformedAndOversizedFrames) {
  for (const std::string Text :
       {"{", "[1,]", "{} {}", "01", "1e1000", "NaN", "{\"x\":\"\xff\"}",
        "\"\\ud800\"", "\"\\udc00\"", "\"\\ud800x\"", "{\"x\":1}tail"})
    rejects(Text, "invalid_json");
  rejects("", "invalid_frame");
  EXPECT_THROW(parseJson("null", 3), Error);
  EXPECT_NO_THROW(parseJson(std::string(64, '[') + "0" + std::string(64, ']')));
  rejects(std::string(65, '[') + "0" + std::string(65, ']'), "invalid_frame");
}

TEST(AcceleratedJson, RepeatedLargeAndSmallFramesRetainNoBorrowedKeys) {
  for (unsigned I = 0; I < 8; ++I) {
    const auto Text =
        Json({{"large", std::string(100000 + I, 'a')}, {"sequence", I}}).dump();
    auto Parsed = parseJson(Text);
    EXPECT_EQ(parseJson("{\"small\":true}").at("small"), true);
    EXPECT_EQ(Parsed.at("large").get<std::string>().size(), 100000u + I);
    EXPECT_EQ(Parsed.at("sequence"), I);
  }
}
