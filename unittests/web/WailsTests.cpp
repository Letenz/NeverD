//===- WailsTests.cpp - Offline Wails layout contracts --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Atomic directory/pointer/hash admission, exact budgets and producer capture.
///
//===----------------------------------------------------------------------===//

#include "Internal.h"
#include "WailsFixture.h"
#include "gtest/gtest.h"

#include "neverd/web/Wails.h"

#include "llvm/Support/FileSystem.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {
using namespace neverd::web;
using namespace neverd::web::wails_test;
class WebWails : public ::testing::Test {
protected:
  std::filesystem::path Root;
  void SetUp() override {
    if (!wailsAvailable())
      GTEST_SKIP() << "pinned native Unicode path policy unavailable";
    llvm::SmallString<128> D;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-wails", D));
    Root = D.str().str();
  }
  void TearDown() override {
    std::error_code EC;
    std::filesystem::remove_all(Root, EC);
  }
  Artifact input(std::string_view Bytes) {
    const auto Path = Root / "input";
    {
      std::ofstream F(Path, std::ios::binary);
      F.write(Bytes.data(), Bytes.size());
    }
    return capture(Path.string(), Limits{}).Artifacts.at(0);
  }
  void refuse(const std::string &B, const char *Code) {
    try {
      (void)extractWails(input(B), WailsProfile);
      FAIL() << Code;
    } catch (const Error &E) {
      EXPECT_STREQ(E.what(), Code);
    }
  }
};
TEST_F(WebWails, ExactByteEvidenceAndIndependentTableNamespaces) {
  auto B = elf({entries(), entries()});
  // These shapes occur in real Go runtime type descriptors. They cannot be
  // treated as filesystems solely because they have a self-referential slice.
  for (const auto At : {0x1800U, 0x1880U}) {
    put(B, At, VM + At + 24);
    put(B, At + 8, 1);
    put(B, At + 16, 1);
  }
  put(B, 0x1880 + 32, UINT64_MAX);
  const auto A = input(B);
  const auto E = extractWails(A, WailsProfile);
  ASSERT_EQ(E.Tables.size(), 2U);
  ASSERT_EQ(E.Members.size(), 6U);
  EXPECT_EQ(E.BuildInfoOffset, BuildOffset);
  EXPECT_EQ(E.Tables[0].Offset, 0x2000U);
  EXPECT_EQ(E.Tables[0].Size, 168U);
  EXPECT_NE(E.Tables[0].ID, E.Tables[1].ID);
  for (unsigned I = 0; I < E.Members.size(); ++I) {
    const auto &M = E.Members[I];
    EXPECT_EQ(M.Path + (M.Directory ? "/" : ""), entries()[I % 3].Path);
    if (!M.Directory) {
      EXPECT_EQ(M.Content.read(0, M.Content.size()), entries()[I % 3].Contents);
      EXPECT_EQ(M.BlobHash, sha256(entries()[I % 3].Contents));
      EXPECT_EQ(M.Content.read(0, M.Content.size()),
                B.substr(M.Offset, M.Content.size()));
    }
  }
  EXPECT_EQ(E.ID, extractWails(A, WailsProfile).ID);
  EXPECT_EQ(A.Content.digest(), E.BlobHash);
  for (unsigned I = 0; I < 2; ++I) {
    const auto S = wailsNamespace(E, I);
    ASSERT_EQ(S.Artifacts.size(), 4U);
    EXPECT_EQ(S.ID, E.Tables[I].ID);
    EXPECT_EQ(S.Artifacts[2].ID, E.Members[I * 3 + 1].ID);
  }
  EXPECT_THROW(wailsNamespace(E, 2), Error);
}
TEST_F(WebWails, VersionsAndBuildDeclarationsAreExplicit) {
  auto B = elf();
  buildInfo(B, "go1.25.8");
  refuse(B, "wails_unsupported_go_declaration");
  B = elf();
  buildInfo(B, "go1.25.7", moduleInfo() + "build\tGOOS=windows\n");
  refuse(B, "wails_conflicting_build_declaration");
  B = elf();
  auto Mod = moduleInfo();
  Mod.insert(Mod.find("build\t"),
             "=>\tPRIVATE_REPLACEMENT_CANARY\tv2.11.0\t\n");
  buildInfo(B, "go1.25.7", Mod);
  refuse(B, "wails_replaced_framework_declaration");
  B = elf();
  buildInfo(B, "go1.25.7",
            moduleInfo() + "dep\tgithub.com/wailsapp/wails/v2\tv2.11.0\tx\n");
  refuse(B, "wails_unsupported_framework_declaration");
  B = elf();
  B[BuildOffset + 15] = 0;
  refuse(B, "wails_unsupported_build_info_layout");
  EXPECT_THROW(extractWails(input(elf()), "wails-latest"), Error);
}
TEST_F(WebWails, NativeMappingAndPointersNeverGuess) {
  auto B = elf();
  put(B, 18, 183, 2);
  refuse(B, "wails_unsupported_container");
  B = elf();
  put(B, 16, 3, 2);
  refuse(B, "wails_unsupported_container");
  B = elf();
  put(B, 64 + 56 + 16, VM);
  refuse(B, "wails_ambiguous_mapping");
  B = elf();
  put(B, 0x2000 + 24, UINT64_MAX);
  refuse(B, "wails_range_out_of_bounds");
  B = elf();
  put(B, 0x2000 + 24 + 48 + 16, 0x500010);
  refuse(B, "wails_ambiguous_mapping");
  B = elf();
  put(B, 0x2000 + 16, 2);
  refuse(B, "wails_embed_table_not_found");
  for (const auto Size : {0U, 4U, 63U, 128U, 0x2000U, 0x3100U})
    EXPECT_THROW(extractWails(input(elf().substr(0, Size)), WailsProfile),
                 Error);
}
TEST_F(WebWails, GoDirectoryOrderAndPortablePathsMustBothHold) {
  auto Items = entries();
  std::swap(Items[1], Items[2]);
  refuse(elf({Items}), "wails_invalid_embed_order");
  refuse(elf({{{"missing/file.js", "x"}}}), "wails_missing_embed_directory");
  refuse(elf({{{"../file.js", "x"}}}), "unsafe_member_name");
  refuse(elf({{{"CON.js", "x"}}}), "unsafe_member_name");
  refuse(elf({{{"A.js", "x"}, {"a.js", "y"}}}),
         "package_archive_path_collision");
  auto B = elf();
  B[0x2000 + 24 + 32] = 1;
  refuse(B, "wails_invalid_directory_record");
}
TEST_F(WebWails, EveryHashAndMetadataAliasIsCheckedBeforePublication) {
  auto B = elf();
  B[0x1400] ^= 1;
  refuse(B, "wails_embed_hash_mismatch");
  B = elf();
  B[0x2000 + 24 + 48 + 32] ^= 1;
  refuse(B, "wails_embed_hash_mismatch");
  B = elf({{{"same.js", "x"}, {"second.js", "y"}}});
  const auto Second = 0x2000 + 24 + 48;
  put(B, Second + 16, VM + 0x1400);
  B.replace(Second + 32, 16, prefix("x"));
  const auto E = extractWails(input(B), WailsProfile);
  EXPECT_EQ(E.Members[0].BlobHash, E.Members[1].BlobHash);
  EXPECT_EQ(E.HashedBytes, B.size() + 1);
  B = elf({{{"same.js", "same.js"}}});
  put(B, 0x2000 + 24 + 16, VM + 0x1000);
  refuse(B, "wails_payload_metadata_overlap");
  B = elf({{{"a.js", "abc"}, {"b.js", "bc"}}});
  put(B, Second + 16, VM + 0x1401);
  refuse(B, "wails_partial_payload_overlap");
}
TEST_F(WebWails, StoredHashChangesAtTheExactCompilerSizeBoundary) {
  for (const auto Size : {0U, 1U, 1024U, 1025U}) {
    const std::string Text(Size, 'a');
    auto B = elf({{{"app.js", Text}}});
    const auto E = extractWails(input(B), WailsProfile);
    ASSERT_EQ(E.Members.size(), 1U);
    EXPECT_EQ(E.Members[0].BlobHash, sha256(Text));
    EXPECT_EQ(E.HashedBytes,
              B.size() + Size * (Size > 1024 ? 2 : 1) + (Size > 1024 ? 1 : 0));
    // Plain SHA256's prefix is not the compiler's stored digest, even though
    // embed.go's stale field comment calls it a truncated SHA256 hash.
    llvm::SHA256 Plain;
    Plain.update(llvm::StringRef(Text));
    const auto Bytes = Plain.final();
    B.replace(0x2000 + 24 + 32, 16,
              reinterpret_cast<const char *>(Bytes.data()), 16);
    refuse(B, "wails_embed_hash_mismatch");
  }
}
TEST_F(WebWails, ExactAndShortIndependentBudgetsAreAtomic) {
  const auto A = input(elf());
  const auto E = extractWails(A, WailsProfile);
  WailsLimits L;
  L.Tables = E.Tables.size();
  L.Members = E.Members.size();
  L.Steps = E.Steps;
  L.PayloadBytes = E.PayloadBytes;
  L.PrivateBytes = E.PrivateBytes;
  EXPECT_EQ(extractWails(A, WailsProfile, L).ID, E.ID);
  for (unsigned I = 0; I < 5; ++I) {
    auto Short = L;
    switch (I) {
    case 0:
      --Short.Tables;
      break;
    case 1:
      --Short.Members;
      break;
    case 2:
      --Short.Steps;
      break;
    case 3:
      --Short.PayloadBytes;
      break;
    case 4:
      --Short.PrivateBytes;
      break;
    }
    EXPECT_THROW(extractWails(A, WailsProfile, Short), Error) << I;
  }
  L = {};
  ++L.Members;
  EXPECT_THROW(extractWails(A, WailsProfile, L), Error);
  EXPECT_EQ(A.Content.digest(), A.BlobHash);
}
TEST_F(WebWails, Zentro030CapturedWailsAssetsWhenSupplied) {
  const auto *Path = std::getenv("NEVERD_WAILS_ZENTRO_030_ELF");
  if (!Path || !*Path)
    GTEST_SKIP() << "captured Zentro v0.3.0 Linux x64 release required";
  const auto A = capture(Path, Limits{}).Artifacts.at(0);
  ASSERT_EQ(A.Content.size(), 28832896U);
  ASSERT_EQ(A.BlobHash,
            "ed0932139a8735f403f8c237234d5fb958b4cf5f0a571499475809d330768145");
  const auto E = extractWails(A, WailsProfile);
  ASSERT_EQ(E.Tables.size(), 1U);
  ASSERT_EQ(E.Members.size(), 14U);
  EXPECT_EQ(E.Tables[0].Offset, 19580128U);
  EXPECT_EQ(E.BuildInfoOffset, 27987728U);
  EXPECT_EQ(E.PayloadBytes, 4700610U);
  EXPECT_EQ(E.Members[5].Path, "frontend/dist/index.html");
  EXPECT_EQ(E.Members[5].Kind, "html");
  EXPECT_EQ(E.Members[7].Path, "frontend/dist/assets/index-BaRbvfp3.js");
  EXPECT_EQ(E.Members[7].Kind, "javascript");
  EXPECT_EQ(E.Members[7].Offset, 16727680U);
  EXPECT_EQ(E.Members[7].Content.size(), 1552742U);
  EXPECT_EQ(E.Members[7].BlobHash,
            "550cdaae8e65f0cbd2a75132a0dc5073fc8a7883811d0d7abdd6a59f71bd0e1e");
  for (const auto &M : E.Members)
    if (!M.Directory)
      EXPECT_EQ(M.Content.digest(), M.BlobHash);
}
} // namespace
