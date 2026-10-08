//===- UnpackPublicTests.cpp - Recovery through the shared SDK and CLI ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../TestProcess.h"
#include "UnpackLibraryTestSupport.h"
#include "UnpackTestSupport.h"

#include "neverd/sdk/NeverDCAPI.h"
#include "neverd/unpack/UnpackCLIStrings.h"
#include "neverd/unpack/UnpackStrings.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

namespace {
using namespace neverd;
using namespace neverd::unpack::test;
namespace text = neverd::unpack::strings;

class UnpackPublic : public testing::Test {
protected:
  neverd_session_t Session = nullptr;
  std::filesystem::path Directory;
  void SetUp() override {
    Session = neverd_session_create();
    ASSERT_NE(Session, nullptr);
    llvm::SmallString<128> Path;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-unpack", Path));
    Directory = Path.str().str();
  }
  void TearDown() override {
    std::error_code Ignored;
    std::filesystem::remove_all(Directory, Ignored);
    if (Session)
      neverd_session_destroy(Session);
  }
  /// The report, or nothing with the session error left for the caller.
  std::optional<llvm::json::Object> api(const std::string &Input,
                                        const std::string &Output,
                                        const char *Options) {
    const char *Report =
        neverd_unpack_json(Session, Input.c_str(), Output.c_str(), Options);
    if (!Report)
      return std::nullopt;
    auto Free = llvm::scope_exit([&] { neverd_free_string(Report); });
    auto Parsed = llvm::json::parse(Report);
    if (!Parsed) {
      ADD_FAILURE() << llvm::toString(Parsed.takeError());
      return std::nullopt;
    }
    return *Parsed->getAsObject();
  }
  /// Run the command and return its exit code and report text.
  std::pair<int, std::string> cli(const std::string &Input,
                                  const std::string &Output,
                                  const std::string &Options) {
    const auto Report = (Directory / StandardOutput).string();
    const auto Command =
        test::shellQuote(NEVERD_UNPACK_CLI) + " " + unpack_cli::Command + " " +
        test::shellQuote(Input) + " -" + unpack_cli::OutputOption + " " +
        test::shellQuote(Output) + " --" + unpack_cli::OptionsOption + "=" +
        test::shellQuote(Options) + test::redirectStdout(Report) +
        test::silenceStderr();
    const int Status = test::systemExitCode(test::runShellCommand(Command));
    const auto Bytes = readFile(Report);
    return {Status, std::string(Bytes.begin(), Bytes.end())};
  }
};

TEST_F(UnpackPublic, CAPIAndCLIWriteTheSameImageAndReport) {
  const auto Input = fixture(PlainPacked).string();
  const auto First = (Directory / FirstOutput).string();
  const auto Second = (Directory / SecondOutput).string();
  auto Report = api(Input, First, nullptr);
  if (!Report) {
    // Without any CPU transport there is nothing to compare.
    const std::string Reason = neverd_last_error(Session);
    if (Reason.find(Unavailable) != std::string::npos ||
        Reason == text::Disabled)
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  EXPECT_EQ(Report->getString(text::OutcomeField), text::UnpackedOutcome);
  const Image Original = readImage(fixture(Plain));
  EXPECT_EQ(Report->getString(text::EntryField),
            llvm::utohexstr(Original.Entry, true));
  const auto *Output = Report->getObject(text::OutputField);
  ASSERT_NE(Output, nullptr);
  EXPECT_EQ(Output->getString(text::PathField), First);
  const auto Written = readFile(First);
  EXPECT_EQ(Output->getInteger(text::SizeField), int64_t(Written.size()));
  EXPECT_EQ(readImage(Written).Entry, Original.Entry);

  const auto [Status, Text] = cli(Input, Second, text::EmptyOptions);
  EXPECT_EQ(Status, unpack_cli::Success);
  EXPECT_EQ(readFile(Second), Written);
  auto Parsed = llvm::json::parse(Text);
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  auto Other = *Parsed->getAsObject();
  // Only the requested path differs between the two reports.
  (*Other.getObject(text::OutputField))[text::PathField] = First;
  EXPECT_EQ(llvm::json::Value(std::move(Other)),
            llvm::json::Value(std::move(*Report)));
}

TEST_F(UnpackPublic, RunWithoutAnEntryWritesNothingAndIsIncomplete) {
  const auto Input = fixture(PlainPacked).string();
  const auto Output = (Directory / FirstOutput).string();
  auto Report = api(Input, Output, TinyLimitOptions);
  if (!Report)
    GTEST_SKIP() << neverd_last_error(Session);
  EXPECT_EQ(Report->getString(text::OutcomeField), text::NoEntryOutcome);
  EXPECT_EQ(Report->get(text::OutputField)->kind(), llvm::json::Value::Null);
  EXPECT_FALSE(std::filesystem::exists(Output));
  const auto [Status, Text] = cli(Input, Output, TinyLimitOptions);
  EXPECT_EQ(Status, unpack_cli::Incomplete);
  EXPECT_FALSE(std::filesystem::exists(Output));
  EXPECT_NE(Text.find(text::NoEntryOutcome), std::string::npos);
}

TEST_F(UnpackPublic, CAPIAndCLIPreserveDLLExportsAndTLS) {
#ifndef NEVERD_UNPACK_LIBRARY_FIXTURE_DIR
  GTEST_SKIP() << test::library::MissingTools;
#else
  namespace library = unpack::test::library;
  const auto Fixtures =
      std::filesystem::path(NEVERD_UNPACK_LIBRARY_FIXTURE_DIR) / "X64";
  const auto Original = readImage(Fixtures / library::InputFile);
  const auto Input = (Directory / library::InputFile).string();
  const auto First = (Directory / "first.dll").string();
  const auto Second = (Directory / "second.dll").string();
  ASSERT_TRUE(library::write(Input, library::pack(Original, library::TLSMode)));
  llvm::json::Object Options{
      {"windows",
       llvm::json::Object{
           {"modules",
            llvm::json::Array{llvm::json::Object{
                {"name", library::DependencyFile},
                {"path", (Fixtures / library::DependencyFile).string()}}}}}}};
  const std::string Encoded =
      llvm::formatv("{0}", llvm::json::Value(std::move(Options))).str();
  auto Report = api(Input, First, Encoded.c_str());
  if (!Report) {
    const std::string Reason = neverd_last_error(Session);
    if (Reason.find(Unavailable) != std::string::npos ||
        Reason == text::Disabled)
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  EXPECT_EQ(Report->getString(text::OutcomeField), text::UnpackedOutcome);
  const auto Image = readImage(readFile(First));
  EXPECT_TRUE(Image.FileCharacteristics & llvm::COFF::IMAGE_FILE_DLL);
  EXPECT_EQ(Image.Entry, Original.Entry);
  EXPECT_EQ(Image.Exports, Original.Exports);
  const auto [Status, Text] = cli(Input, Second, Encoded);
  EXPECT_EQ(Status, unpack_cli::Success);
  EXPECT_EQ(readFile(Second), readFile(First));
  auto Parsed = llvm::json::parse(Text);
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  EXPECT_EQ(Parsed->getAsObject()->getString(text::OutcomeField),
            text::UnpackedOutcome);
#endif
}

TEST_F(UnpackPublic, SetupFailuresAreErrorsNotReports) {
  const auto Input = fixture(PlainPacked).string();
  const auto Output = (Directory / FirstOutput).string();
  EXPECT_EQ(neverd_unpack_json(Session, nullptr, Output.c_str(), nullptr),
            nullptr);
  EXPECT_STREQ(neverd_last_error(Session), text::PathRequired);
  EXPECT_EQ(neverd_unpack_json(Session, Input.c_str(), "", nullptr), nullptr);
  EXPECT_STREQ(neverd_last_error(Session), text::OutputRequired);
  EXPECT_EQ(neverd_unpack_json(nullptr, Input.c_str(), Output.c_str(), nullptr),
            nullptr);
  // An image may never replace the input it was recovered from.
  EXPECT_FALSE(api(Input, Input, nullptr));
  const std::string Same = neverd_last_error(Session);
  EXPECT_TRUE(Same == text::SamePath || Same == text::Disabled) << Same;
  EXPECT_FALSE(api(Input, Output, UnknownOption));
  EXPECT_FALSE(api((Directory / Missing).string(), Output, nullptr));
  EXPECT_FALSE(std::filesystem::exists(Output));
  EXPECT_EQ(cli(Input, Output, UnknownOption).first, unpack_cli::Error);
  EXPECT_EQ(
      cli((Directory / Missing).string(), Output, text::EmptyOptions).first,
      unpack_cli::Error);
  EXPECT_FALSE(std::filesystem::exists(Output));
}
} // namespace
