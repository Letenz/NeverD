//===- LibraryFeatureTests.cpp - Library feature contract tests ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/sigs/SignatureDB.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace neverd;
using namespace neverd::sigs;

namespace {

const std::filesystem::path FeatureDirectory(NEVERD_LIBRARY_FEATURE_DIR);
constexpr const char *Libcxx =
    "libcxx-23git-arm64-macos-abi1-alternate-clang22";

class LibraryFeatureTest : public testing::Test {
protected:
  void SetUp() override {
    llvm::SmallString<128> Temp;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-features", Temp));
    Root = Temp.str().str();
    std::error_code EC;
    std::filesystem::copy(FeatureDirectory.parent_path(), Root / "features",
                          std::filesystem::copy_options::recursive, EC);
    ASSERT_FALSE(EC) << EC.message();
    Path = Root / "features/rules" / (std::string(Libcxx) + ".json");
    auto Buffer = llvm::MemoryBuffer::getFile(Path.string());
    ASSERT_TRUE(Buffer);
    auto Parsed = llvm::json::parse((*Buffer)->getBuffer());
    ASSERT_TRUE(static_cast<bool>(Parsed))
        << llvm::toString(Parsed.takeError());
    Pack = std::move(*Parsed);
  }

  void TearDown() override {
    std::error_code EC;
    std::filesystem::remove_all(Root, EC);
  }

  llvm::json::Object &rule() {
    return *Pack.getAsObject()->getArray("rules")->front().getAsObject();
  }

  llvm::json::Object &pattern() { return *rule().getObject("pattern"); }

  void save() {
    std::ofstream Out(Path, std::ios::binary);
    Out << llvm::formatv("{0:2}", Pack).str() << '\n';
    ASSERT_TRUE(Out.good());
  }

  void rejected(llvm::StringRef Diagnostic) {
    save();
    SignatureDB DB;
    auto Error = DB.loadFeaturePack(Path);
    ASSERT_TRUE(static_cast<bool>(Error));
    EXPECT_NE(llvm::toString(std::move(Error)).find(Diagnostic.str()),
              std::string::npos);
    EXPECT_TRUE(DB.featurePacks().empty());
    EXPECT_EQ(DB.featureGeneration(), 0u);
  }

  std::filesystem::path Root;
  std::filesystem::path Path;
  llvm::json::Value Pack = nullptr;
};

TEST(LibraryFeaturePacks, PublishedPacksLoadWithoutPublishingUngatedNames) {
  SignatureDB DB;
  auto Error = DB.loadFeatureDirectory(FeatureDirectory);
  ASSERT_FALSE(static_cast<bool>(Error)) << llvm::toString(std::move(Error));
  ASSERT_EQ(DB.featurePacks().size(), 5u);
  size_t Rules = 0, BytePatterns = 0, ComPatterns = 0;
  for (const auto &[ID, Pack] : DB.featurePacks()) {
    EXPECT_EQ(Pack.Id, ID);
    EXPECT_EQ(Pack.SHA256.size(), 64u);
    EXPECT_EQ(Pack.ProfileSHA256.size(), 64u);
    EXPECT_EQ(Pack.EvidenceSHA256.size(), 64u);
    EXPECT_FALSE(Pack.SourceOrigin.empty());
    EXPECT_FALSE(Pack.SourceLicense.empty());
    BinaryImage Image;
    Image.Arch = Pack.Architecture;
    Image.Format = Pack.Format;
    Image.Bits = Bitness::Bits64;
    EXPECT_TRUE(Pack.accepts(Image));
    Image.Arch = Arch::ARM;
    EXPECT_FALSE(Pack.accepts(Image));
    Rules += Pack.Rules.size();
    for (const auto &Rule : Pack.Rules) {
      BytePatterns += Rule.PatternKind == LibraryFeatureRule::Kind::BytePattern;
      ComPatterns += Rule.PatternKind == LibraryFeatureRule::Kind::ComLifetime;
      if (Rule.PatternKind == LibraryFeatureRule::Kind::Expression)
        EXPECT_LT(Rule.Result, Rule.Nodes.size());
    }
  }
  EXPECT_EQ(Rules, 59u);
  EXPECT_EQ(BytePatterns, 15u);
  EXPECT_EQ(ComPatterns, 6u);
  EXPECT_EQ(DB.moduleCount(), 0u);
  EXPECT_TRUE(DB.buildNameMap().empty());
}

TEST_F(LibraryFeatureTest, RejectsUnsupportedSchema) {
  (*Pack.getAsObject())["schema_version"] = 2;
  rejected("schema_version");
}

TEST_F(LibraryFeatureTest, RejectsUnknownOperator) {
  (*pattern().getArray("nodes"))[1].getAsObject()->operator[]("op") = "call";
  rejected("unsupported expression operator");
}

TEST_F(LibraryFeatureTest, RejectsForwardAndCyclicNodes) {
  (*pattern().getArray("nodes"))[1].getAsObject()->operator[]("args") =
      llvm::json::Array{"value"};
  rejected("forward expression argument");
}

TEST_F(LibraryFeatureTest, RejectsUnsupportedWidth) {
  (*pattern().getArray("nodes"))[0].getAsObject()->operator[]("bits") = 63;
  rejected("unsupported expression width");
}

TEST_F(LibraryFeatureTest, RejectsOverflowingConstant) {
  (*pattern().getArray("nodes"))[2].getAsObject()->operator[]("value") =
      "0x100";
  rejected("invalid expression constant");
}

TEST_F(LibraryFeatureTest, RejectsUnreachableEvidenceNodes) {
  pattern().getArray("nodes")->push_back(llvm::json::Object{
      {"id", "unused"}, {"op", "input"}, {"bits", 64}, {"role", "argument"}});
  rejected("use every node");
}

TEST_F(LibraryFeatureTest, RejectsMemoryOrderingExtension) {
  (*pattern().getArray("nodes"))[1].getAsObject()->operator[]("ordering") =
      "volatile";
  rejected("unsupported field");
}

TEST_F(LibraryFeatureTest, RejectsReceiverAndLayoutDisagreement) {
  rule()["receiver_type"] = "user::lookalike";
  rejected("receiver contradicts");
}

TEST_F(LibraryFeatureTest, RejectsShapeAsLibraryIdentity) {
  rule()["identity_evidence"] = "similar-layout";
  rejected("unsupported library identity evidence");
}

TEST_F(LibraryFeatureTest, RejectsScopeWithoutCompiledWitness) {
  rule()["scope"] = llvm::json::Array{"whole-function"};
  rejected("every scope needs");
}

TEST_F(LibraryFeatureTest, RejectsProfileTraversal) {
  auto *Ref = Pack.getAsObject()->getObject("profile");
  (*Ref)["path"] = "../outside.json";
  rejected("invalid feature reference path");
}

TEST_F(LibraryFeatureTest, RejectsChangedProfileBytes) {
  const auto Profile =
      Root / "features/profiles" / (std::string(Libcxx) + ".json");
  std::ofstream(Profile, std::ios::app) << ' ';
  rejected("digest mismatch");
}

TEST_F(LibraryFeatureTest, RejectsDuplicateEscapedKeys) {
  save();
  const std::string Text = llvm::formatv("{0}", Pack).str();
  std::ofstream(Path) << "{\"schema_vers\\u0069on\":1," << Text.substr(1);
  auto Result = readLibraryFeaturePack(Path);
  ASSERT_FALSE(static_cast<bool>(Result));
  EXPECT_NE(
      llvm::toString(Result.takeError()).find("duplicate feature JSON key"),
      std::string::npos);
}

TEST_F(LibraryFeatureTest, FailedReloadRetainsPriorPacksAndByteSignatures) {
  SignatureDB DB;
  auto Error = DB.loadFeatureDirectory(Root / "features/rules");
  ASSERT_FALSE(static_cast<bool>(Error)) << llvm::toString(std::move(Error));
  ASSERT_FALSE(DB.loadPatternText("AABB 00 0000 0002 :0000 existing\n", "old"));
  const uint64_t Generation = DB.featureGeneration();
  const std::string Hash = DB.featurePacks().at(Libcxx).SHA256;
  rule()["revision"] = 0;
  save();
  Error = DB.loadFeatureDirectory(Root / "features/rules");
  ASSERT_TRUE(static_cast<bool>(Error));
  llvm::consumeError(std::move(Error));
  EXPECT_EQ(DB.featureGeneration(), Generation);
  EXPECT_EQ(DB.featurePacks().size(), 5u);
  EXPECT_EQ(DB.featurePacks().at(Libcxx).SHA256, Hash);
  EXPECT_EQ(DB.moduleCount(), 1u);
}

} // namespace
