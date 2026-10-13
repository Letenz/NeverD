//===- SymbolStoreTests.cpp - Symbol search paths and stores
//---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// _NT_SYMBOL_PATH parsing, the files and URLs a PDB is looked for at, and the
/// order discovery tries them in.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/debug/SymbolStore.h"
#include "neverd/loader/BinaryImage.h"

using namespace neverd;

namespace {

const std::filesystem::path Cache = "/cache/symbols";

std::vector<std::string>
strings(const std::vector<std::filesystem::path> &Paths) {
  std::vector<std::string> Out;
  for (const auto &Path : Paths)
    Out.push_back(Path.generic_string());
  return Out;
}

BinaryImage pe(const char *Recorded, PDBIdentityState State) {
  BinaryImage Img;
  Img.Format = BinaryFormat::COFF;
  Img.DynInfo.PDBPath = Recorded;
  Img.DynInfo.CodeViewPDBIdentityState = State;
  PDBBuildIdentity Identity;
  Identity.Guid = {0x78, 0x56, 0x34, 0x12, 0xbc, 0x9a, 0xf0, 0xde,
                   0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0};
  Identity.Age = 1;
  Img.DynInfo.CodeViewPDBIdentity = Identity;
  return Img;
}

} // namespace

TEST(SymbolStore, ServerElementsNameTheirStores) {
  // The downstream store keeps what the server after it serves.
  SymbolPath Path = parseSymbolPath(
      "srv*/local/sym*https://msdl.microsoft.com/download/symbols", Cache);
  EXPECT_EQ(strings(Path.Stores), std::vector<std::string>{"/local/sym"});
  ASSERT_EQ(Path.Servers.size(), 1u);
  EXPECT_EQ(Path.Servers[0].URL, "https://msdl.microsoft.com/download/symbols");
  EXPECT_EQ(Path.Servers[0].Cache.generic_string(), "/local/sym");
  EXPECT_TRUE(Path.Directories.empty());

  // A server without a store of its own fills the default one; an empty
  // store stands for it too.
  for (const char *Text :
       {"srv*https://symbols.example/x", "SRV**https://symbols.example/x"}) {
    SCOPED_TRACE(Text);
    Path = parseSymbolPath(Text, Cache);
    EXPECT_EQ(strings(Path.Stores), std::vector<std::string>{"/cache/symbols"});
    ASSERT_EQ(Path.Servers.size(), 1u);
    EXPECT_EQ(Path.Servers[0].Cache, Cache);
  }

  // "srv*" alone is Microsoft's public server, as WinDbg's .symfix sets it.
  Path = parseSymbolPath("srv*", Cache);
  ASSERT_EQ(Path.Servers.size(), 1u);
  EXPECT_EQ(Path.Servers[0].URL, "https://msdl.microsoft.com/download/symbols");
  EXPECT_EQ(Path.Servers[0].Cache, Cache);

  // symsrv*<library>*... reads as srv*...
  Path = parseSymbolPath("symsrv*symsrv.dll*/s*https://symbols.example", Cache);
  ASSERT_EQ(Path.Servers.size(), 1u);
  EXPECT_EQ(Path.Servers[0].Cache.generic_string(), "/s");

  // A store that is no server is only searched.
  Path = parseSymbolPath("srv*/a*/b", Cache);
  EXPECT_EQ(strings(Path.Stores), (std::vector<std::string>{"/a", "/b"}));
  EXPECT_TRUE(Path.Servers.empty());
}

TEST(SymbolStore, CacheElementsAndDirectories) {
  // cache* keeps what later servers without a store of their own serve.
  const SymbolPath Path = parseSymbolPath(
      ";cache*/c;/plain/dir ; srv*https://one.example;srv*/own*https://two",
      Cache);
  EXPECT_EQ(strings(Path.Directories), std::vector<std::string>{"/plain/dir"});
  EXPECT_EQ(strings(Path.Stores), (std::vector<std::string>{"/c", "/own"}));
  ASSERT_EQ(Path.Servers.size(), 2u);
  EXPECT_EQ(Path.Servers[0].Cache.generic_string(), "/c");
  EXPECT_EQ(Path.Servers[1].Cache.generic_string(), "/own");
}

TEST(SymbolStore, StorePathsAndServerURLs) {
  const PDBSymbolRequest Request{"kernel name.pdb", "ABC1"};
  EXPECT_EQ(symbolStorePath("/s", Request).generic_string(),
            "/s/kernel name.pdb/ABC1/kernel name.pdb");
  EXPECT_EQ(symbolServerURL({"https://symbols.example/x/", Cache}, Request),
            "https://symbols.example/x/kernel%20name.pdb/ABC1/"
            "kernel%20name.pdb");
}

TEST(SymbolStore, CandidatesFollowTheSearchOrder) {
  SymbolPath Path;
  Path.Directories = {"/dir"};
  Path.Stores = {"/store"};
  const std::filesystem::path Recorded =
      std::filesystem::temp_directory_path() / "build" / "Kernel.pdb";
  const BinaryImage Img =
      pe(Recorded.string().c_str(), PDBIdentityState::Unique);
  std::vector<std::pair<std::string, PDBLocation>> Seen;
  for (const PDBCandidate &C : pdbCandidates("/in/app.exe", Img, Path))
    Seen.emplace_back(C.Path.generic_string(), C.Location);
  const std::vector<std::pair<std::string, PDBLocation>> Expected{
      {Recorded.generic_string(), PDBLocation::Recorded},
      {"/in/Kernel.pdb", PDBLocation::Beside},
      {"/in/app.pdb", PDBLocation::Beside},
      {"/dir/Kernel.pdb", PDBLocation::Directory},
      {"/store/kernel.pdb/123456789ABCDEF0123456789ABCDEF01/kernel.pdb",
       PDBLocation::Store}};
  EXPECT_EQ(Seen, Expected);

  // A bare name is looked for beside the image, not in the working
  // directory.
  for (const PDBCandidate &C : pdbCandidates(
           "/in/app.exe", pe("Kernel.pdb", PDBIdentityState::Unique), Path))
    EXPECT_NE(C.Location, PDBLocation::Recorded) << C.Path.generic_string();

  // Without a unique identity no store can key the PDB.
  const auto Ambiguous = pdbCandidates(
      "/in/app.exe", pe("Kernel.pdb", PDBIdentityState::Ambiguous), Path);
  for (const PDBCandidate &C : Ambiguous)
    EXPECT_NE(C.Location, PDBLocation::Store) << C.Path.generic_string();
}
