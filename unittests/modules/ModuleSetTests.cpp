//===- ModuleSetTests.cpp - Generic input identity and lifetime -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Generic input identity and lifetime.
///

#include "gtest/gtest.h"

#include "neverd/debug/DebugContext.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/modules/ModuleSet.h"
#include "neverd/support/BinaryLoading.h"
#include "neverd/support/FilePath.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace neverd;
using namespace neverd::modules;

namespace {
class ModuleFiles : public testing::Test {
protected:
  std::filesystem::path Root;
  void SetUp() override {
    llvm::SmallString<128> Directory;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-module-test", Directory));
    const auto Text = Directory.str();
    Root = std::u8string(Text.begin(), Text.end());
  }
  void TearDown() override {
    std::error_code EC;
    std::filesystem::remove_all(Root, EC);
  }
  std::filesystem::path write(llvm::StringRef Name,
                              llvm::ArrayRef<uint8_t> Bytes) {
    const auto Path = Root / Name.str();
    std::ofstream OS(Path, std::ios::binary);
    OS.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    return Path;
  }
  std::filesystem::path text(llvm::StringRef Name, llvm::StringRef Value) {
    return write(Name,
                 llvm::ArrayRef(reinterpret_cast<const uint8_t *>(Value.data()),
                                Value.size()));
  }
  std::filesystem::path elf(llvm::StringRef Name = "probe.elf") {
    using ELF = llvm::object::ELF64LE;
    ELF::Ehdr Header{};
    std::memcpy(Header.e_ident, "\177ELF", 4);
    Header.e_ident[llvm::ELF::EI_CLASS] = llvm::ELF::ELFCLASS64;
    Header.e_ident[llvm::ELF::EI_DATA] = llvm::ELF::ELFDATA2LSB;
    Header.e_ident[llvm::ELF::EI_VERSION] = llvm::ELF::EV_CURRENT;
    Header.e_type = llvm::ELF::ET_EXEC;
    Header.e_machine = llvm::ELF::EM_X86_64;
    Header.e_version = llvm::ELF::EV_CURRENT;
    Header.e_entry = 0x401000;
    Header.e_ehsize = sizeof(Header);
    Header.e_phoff = sizeof(Header);
    Header.e_phentsize = sizeof(ELF::Phdr);
    Header.e_phnum = 1;
    Header.e_shentsize = sizeof(ELF::Shdr);
    ELF::Phdr Program{};
    Program.p_type = llvm::ELF::PT_LOAD;
    Program.p_flags = llvm::ELF::PF_R | llvm::ELF::PF_X;
    Program.p_offset = 0x1000;
    Program.p_vaddr = 0x401000;
    Program.p_filesz = 6;
    Program.p_memsz = 6;
    Program.p_align = 0x1000;
    std::vector<uint8_t> Bytes(0x1006);
    std::memcpy(Bytes.data(), &Header, sizeof(Header));
    std::memcpy(Bytes.data() + sizeof(Header), &Program, sizeof(Program));
    const uint8_t Code[] = {0xB8, 42, 0, 0, 0, 0xC3};
    std::memcpy(Bytes.data() + 0x1000, Code, sizeof(Code));
    return write(Name, Bytes);
  }
  ModuleInput input(std::string ID, const std::filesystem::path &Path,
                    std::optional<LoadPolicy> Policy = {}) {
    return {std::move(ID), Path, {}, Policy};
  }
  std::unique_ptr<ModuleSet> catalogue(std::vector<ModuleInput> Inputs,
                                       ModuleLimits Limits = {}) {
    ModuleOptions O;
    O.Modules = std::move(Inputs);
    O.Limits = Limits;
    auto M = ModuleSet::create(std::move(O));
    if (!M) {
      ADD_FAILURE() << llvm::toString(M.takeError());
      return {};
    }
    return std::move(*M);
  }
  template <class T> std::string error(llvm::Expected<T> Result) {
    if (Result) {
      ADD_FAILURE() << "expected failure";
      return {};
    }
    return llvm::toString(Result.takeError());
  }
  static constexpr llvm::StringLiteral Map =
      "VMA LMA Size Align Out In Symbol\n"
      "0000000000401000 0000000000401000 00000006 16 .text\n"
      "0000000000401000 0000000000401000 00000006 1 probe.o:(.text)\n"
      "0000000000401000 0000000000401000 00000006 1 mapped_probe\n";
};

TEST_F(ModuleFiles, RegistrationAndInspectionDoNotOpenInputs) {
  auto M = catalogue({input("later", Root / "not-yet.sys", LoadPolicy::Lazy)});
  ASSERT_TRUE(M);
  ASSERT_EQ(M->inspect().size(), 1U);
  EXPECT_FALSE(M->inspect()[0].Prepared);
  EXPECT_EQ(M->inspect()[0].Load, LoadPolicy::Lazy);
  EXPECT_EQ(M->retainedSourceBytes(), 0U);
  EXPECT_FALSE(M->prepareEager());
  EXPECT_EQ(M->revision(), 0U);
  auto J = llvm::json::parse(moduleSetJSON(*M));
  ASSERT_TRUE(bool(J));
  EXPECT_FALSE(M->prepared("later"));
}

TEST_F(ModuleFiles, NamedConfigurationUsesItsParentOrAnExplicitJsonOrigin) {
  const auto Config =
      text("modules.json", R"({"modules":[{"id":"a","image":"not-yet.sys"}]})");
  auto Options = moduleOptionsFromFile(Config);
  ASSERT_TRUE(bool(Options)) << llvm::toString(Options.takeError());
  EXPECT_EQ(Options->BaseDirectory, Root);
  ASSERT_EQ(Options->Modules.size(), 1u);
  EXPECT_EQ(Options->Modules[0].Image, Root / "not-yet.sys");
  std::filesystem::create_directory(Root / "configs");
  const auto Contents =
      llvm::formatv(
          "{0}",
          llvm::json::Value(llvm::json::Object{
              {"base_directory", pathToUTF8(Root)},
              {"modules", llvm::json::Array{llvm::json::Object{
                              {"id", "a"}, {"image", "elsewhere.sys"}}}}}))
          .str();
  const auto Other = text("configs/explicit.json", Contents);
  auto Explicit = moduleOptionsFromFile(Other);
  ASSERT_TRUE(bool(Explicit)) << llvm::toString(Explicit.takeError());
  EXPECT_EQ(Explicit->BaseDirectory, Root);
  EXPECT_EQ(Explicit->Modules[0].Image, Root / "elsewhere.sys");
}

TEST_F(ModuleFiles, NamedConfigurationRejectsInvalidFilesAndExcessiveJson) {
  EXPECT_FALSE(error(moduleOptionsFromFile(Root)).empty());
  EXPECT_FALSE(error(moduleOptionsFromFile(Root / "missing.json")).empty());
  EXPECT_FALSE(error(moduleOptionsFromFile(text("array.json", "[]"))).empty());
  const std::string Excess(input_limit::JSONBytes + 1, ' ');
  EXPECT_NE(
      error(moduleOptionsFromFile(text("large.json", Excess))).find("2 MiB"),
      std::string::npos);
}

TEST_F(ModuleFiles, ExplicitOriginAndPolicyPrecedence) {
  const auto Config = R"({"module_defaults":{"load":"lazy"},"modules":[
    {"id":"a","image":"a.sys"},
    {"id":"b","image":"b.sys","load":"eager"}]})";
  EXPECT_NE(error(moduleOptionsFromJSON(Config)).find("base_directory"),
            std::string::npos);
  auto O = moduleOptionsFromJSON(Config, Root);
  ASSERT_TRUE(bool(O));
  auto M = ModuleSet::create(std::move(*O));
  ASSERT_TRUE(bool(M));
  const auto Rows = (*M)->inspect();
  ASSERT_EQ(Rows.size(), 2U);
  EXPECT_EQ(Rows[0].Load, LoadPolicy::Lazy);
  EXPECT_EQ(Rows[1].Load, LoadPolicy::Eager);
  EXPECT_EQ(Rows[0].Input.Image, Root / "a.sys");
}

TEST_F(ModuleFiles, MalformedConfigurationFailsBeforeIO) {
  for (const auto *JSON :
       {"[]", "{\"unexpected\":true}", "{\"modules\":null}",
        "{\"module_defaults\":{\"load\":\"auto\"}}",
        "{\"limits\":{\"max_modules\":0}}",
        "{\"modules\":[{\"id\":\"a\",\"image\":\"x\",\"load\":false}]}",
        "{\"modules\":[{\"id\":\"a\",\"image\":\"x\",\"debug\":{\"kind\":"
        "\"pdb\"}}]}",
        "{\"modules\":[{\"id\":\"a\",\"image\":\"x\",\"debug\":{\"kind\":"
        "\"guess\",\"path\":\"y\"}}]}"}) {
    EXPECT_FALSE(error(moduleOptionsFromJSON(JSON, Root)).empty()) << JSON;
  }
}

TEST_F(ModuleFiles, DuplicateAddPreservesCatalogueAndRevision) {
  auto M = catalogue({input("same", Root / "one.sys")});
  ASSERT_TRUE(M);
  const auto Before = moduleSetJSON(*M);
  auto E = M->add(input("same", Root / "two.sys"));
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find("duplicate"), std::string::npos);
  EXPECT_EQ(moduleSetJSON(*M), Before);
}

TEST_F(ModuleFiles, UnpreparedInputCorrectionRetainsIdentityPolicyAndNoIO) {
  auto M = catalogue({input("later", Root / "missing.elf", LoadPolicy::Lazy)});
  ASSERT_TRUE(M);
  EXPECT_FALSE(error(M->prepare("later")).empty());
  EXPECT_EQ(M->revision(), 0u);
  const auto Corrected = Root / "corrected.elf";
  ASSERT_EQ(llvm::toString(M->reviseUnprepared("later", Corrected)), "");
  EXPECT_EQ(M->revision(), 1u);
  EXPECT_EQ(M->retainedSourceBytes(), 0u);
  const auto Rows = M->inspect();
  ASSERT_EQ(Rows.size(), 1u);
  EXPECT_EQ(Rows[0].Input.ID, "later");
  EXPECT_EQ(Rows[0].Input.Image, Corrected);
  EXPECT_EQ(Rows[0].Load, LoadPolicy::Lazy);
  EXPECT_FALSE(Rows[0].Prepared);
  EXPECT_FALSE(std::filesystem::exists(Corrected));
  elf("corrected.elf");
  ASSERT_EQ(llvm::toString(M->prepareEager()), "");
  EXPECT_FALSE(M->prepared("later"));
  auto Loaded = M->prepare("later");
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  EXPECT_EQ(M->revision(), 2u);
}

TEST_F(ModuleFiles, FailedCompanionCanBeCorrectedBeforeFirstObservation) {
  const auto P = elf();
  auto Input = input("a", P);
  Input.Debug = DebugInput{DebugKind::Map, Root / "missing.map"};
  auto M = catalogue({Input});
  ASSERT_TRUE(M);
  EXPECT_FALSE(error(M->prepare("a")).empty());
  EXPECT_FALSE(M->prepared("a"));
  const auto D = text("fixed.map", Map);
  ASSERT_EQ(llvm::toString(
                M->reviseUnprepared("a", P, DebugInput{DebugKind::Map, D})),
            "");
  EXPECT_EQ(M->retainedSourceBytes(), 0u);
  auto Loaded = M->prepare("a");
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  EXPECT_NE((*Loaded)->debug(), nullptr);
  auto Symbol = (*Loaded)->lookupSymbol("mapped_probe");
  ASSERT_TRUE(bool(Symbol));
  ASSERT_TRUE(Symbol->has_value());
  EXPECT_EQ((**Symbol).Address, 0x401000u);
}

TEST_F(ModuleFiles, InvalidCorrectionDoesNotPublishAnyCatalogueChange) {
  auto M = catalogue({input("a", Root / "missing.elf")});
  ASSERT_TRUE(M);
  const auto Before = moduleSetJSON(*M);
  EXPECT_NE(llvm::toString(M->reviseUnprepared("a", {})), "");
  EXPECT_NE(llvm::toString(M->reviseUnprepared(
                "a", Root / "valid.elf",
                DebugInput{static_cast<DebugKind>(999), Root / "x"})),
            "");
  EXPECT_NE(llvm::toString(M->reviseUnprepared("unknown", Root / "other.elf")),
            "");
  EXPECT_EQ(moduleSetJSON(*M), Before);
  EXPECT_EQ(M->retainedSourceBytes(), 0u);
}

TEST_F(ModuleFiles, PreparedInputCorrectionCannotReplaceAPinnedObservation) {
  auto M = catalogue({input("a", elf())});
  ASSERT_TRUE(M);
  auto Loaded = M->prepare("a");
  ASSERT_TRUE(bool(Loaded));
  const auto Before = moduleSetJSON(*M);
  const auto Pinned = *Loaded;
  const auto Alternate = elf("alternate.elf");
  EXPECT_NE(llvm::toString(M->reviseUnprepared("a", Alternate)), "");
  EXPECT_EQ(moduleSetJSON(*M), Before);
  EXPECT_EQ(M->prepared("a"), Pinned);
  EXPECT_EQ(Pinned->image().Entry, 0x401000u);
}

TEST_F(ModuleFiles, EagerSkipsLazyAndQueryLoadsItOnDemand) {
  const auto P = elf();
  auto M = catalogue({input("eager", P), input("lazy", P, LoadPolicy::Lazy)});
  ASSERT_TRUE(M);
  ASSERT_FALSE(M->prepareEager());
  EXPECT_TRUE(M->prepared("eager"));
  EXPECT_FALSE(M->prepared("lazy"));
  auto Report = llvm::json::parse(moduleSetJSON(*M));
  ASSERT_TRUE(bool(Report));
  auto Base =
      (*Report->getAsObject()->getArray("modules"))[0].getAsObject()->getString(
          "preferred_base");
  ASSERT_TRUE(Base.has_value());
  uint64_t Address = 0;
  EXPECT_FALSE(Base->getAsInteger(0, Address));
  EXPECT_EQ(Address, M->prepared("eager")->image().Base);
  auto S = M->lookupSymbol("lazy", "absent");
  ASSERT_TRUE(bool(S));
  EXPECT_FALSE(S->has_value());
  EXPECT_TRUE(M->prepared("lazy"));
}

TEST_F(ModuleFiles, EagerBatchFailurePublishesNothingAndCanRetry) {
  const auto P = elf();
  auto M = catalogue({input("a", P), input("z", Root / "later.elf")});
  ASSERT_TRUE(M);
  auto E = M->prepareEager();
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  EXPECT_EQ(M->revision(), 0U);
  EXPECT_EQ(M->retainedSourceBytes(), 0U);
  EXPECT_FALSE(M->prepared("a"));
  elf("later.elf");
  ASSERT_FALSE(M->prepareEager());
  EXPECT_TRUE(M->prepared("a"));
  EXPECT_TRUE(M->prepared("z"));
}

TEST_F(ModuleFiles,
       PreparedSnapshotSurvivesSourceReplacementAndSetDestruction) {
  const auto P = elf();
  auto M = catalogue({input("a", P)});
  ASSERT_TRUE(M);
  auto Loaded = M->prepare("a");
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto Image = *Loaded;
  const auto Digest = Image->imageDigest();
  const auto Snapshot = Image->preparedImagePath();
  text("probe.elf", "replaced source");
  EXPECT_EQ(Image->bytes()[0x1001], 42);
  EXPECT_EQ(Image->imageDigest(), Digest);
  auto Reload = loadBinary(Snapshot);
  ASSERT_TRUE(bool(Reload));
  EXPECT_EQ(Reload->Entry, 0x401000U);
  Loaded = std::shared_ptr<const ModuleImage>();
  M.reset();
  EXPECT_TRUE(std::filesystem::exists(Snapshot));
  Image.reset();
  EXPECT_FALSE(std::filesystem::exists(Snapshot));
}

TEST_F(ModuleFiles, SeparateCataloguesShareNoMutableFileLifetime) {
  const auto P = elf();
  auto A = catalogue({input("a", P)}), B = catalogue({input("a", P)});
  ASSERT_TRUE(A && B);
  auto X = A->prepare("a"), Y = B->prepare("a");
  ASSERT_TRUE(bool(X) && bool(Y));
  EXPECT_EQ((*X)->imageDigest(), (*Y)->imageDigest());
  EXPECT_NE((*X)->preparedImagePath(), (*Y)->preparedImagePath());
}

TEST_F(ModuleFiles, SourceByteLimitsRejectWithoutPublishing) {
  const auto P = elf();
  ModuleLimits Limits;
  Limits.TotalSourceBytes = 0x1005;
  auto M = catalogue({input("a", P)}, Limits);
  ASSERT_TRUE(M);
  EXPECT_NE(error(M->prepare("a")).find("limit"), std::string::npos);
  EXPECT_EQ(M->retainedSourceBytes(), 0U);
  EXPECT_EQ(M->revision(), 0U);
}

TEST_F(ModuleFiles, MissingExplicitCompanionDoesNotPublishImageOrUseNearbyMap) {
  const auto P = elf();
  text("probe.map", Map);
  auto I = input("a", P);
  I.Debug = DebugInput{DebugKind::PDB, Root / "missing.pdb"};
  auto M = catalogue({I});
  ASSERT_TRUE(M);
  EXPECT_FALSE(error(M->prepare("a")).empty());
  EXPECT_FALSE(M->prepared("a"));
  EXPECT_EQ(M->retainedSourceBytes(), 0U);
}

TEST_F(ModuleFiles, AmbientDebugFilesAreNotDiscovered) {
  const auto P = elf();
  text("probe.map", Map);
  auto M = catalogue({input("a", P)});
  ASSERT_TRUE(M);
  auto Loaded = M->prepare("a");
  ASSERT_TRUE(bool(Loaded));
  EXPECT_EQ((*Loaded)->debug(), nullptr);
  auto S = M->lookupSymbol("a", "mapped_probe");
  ASSERT_TRUE(bool(S));
  EXPECT_FALSE(S->has_value());
}

TEST_F(ModuleFiles, LateMapAttachmentRetainsOldObservationAndNamesOnlyTrust) {
  const auto P = elf(), MapPath = text("probe.map", Map);
  auto M = catalogue({input("a", P)});
  ASSERT_TRUE(M);
  auto Before = M->prepare("a");
  ASSERT_TRUE(bool(Before));
  auto After = M->attachDebug("a", {DebugKind::Map, MapPath});
  ASSERT_TRUE(bool(After)) << llvm::toString(After.takeError());
  EXPECT_EQ((*Before)->debug(), nullptr);
  EXPECT_NE((*After)->debug(), nullptr);
  EXPECT_EQ((*Before)->preparedImagePath(), (*After)->preparedImagePath());
  EXPECT_FALSE((*After)->hasAuthenticatedFunctionSignatures());
  EXPECT_FALSE((*After)->hasAuthenticatedObjectExtents());
  auto Report = llvm::json::parse(moduleSetJSON(*M));
  ASSERT_TRUE(bool(Report));
  auto *Row = (*Report->getAsObject()->getArray("modules"))[0].getAsObject();
  EXPECT_EQ(Row->getBoolean("debug_loaded"), true);
  EXPECT_EQ(Row->getBoolean("debug_identity_verified"), false);
  auto Symbol = (*After)->lookupSymbol("mapped_probe");
  ASSERT_TRUE(bool(Symbol));
  ASSERT_TRUE(Symbol->has_value());
  EXPECT_EQ((**Symbol).Address, 0x401000U);
  text("probe.map", "source replaced");
  EXPECT_TRUE(bool((*After)->lookupSymbol("mapped_probe")));
}

TEST_F(ModuleFiles, FailedDebugAttachmentLeavesPreparedObservationUntouched) {
  auto M = catalogue({input("a", elf())});
  ASSERT_TRUE(M);
  auto Before = M->prepare("a");
  ASSERT_TRUE(bool(Before));
  const auto Report = moduleSetJSON(*M);
  EXPECT_FALSE(
      error(M->attachDebug("a", {DebugKind::PDB, Root / "missing.pdb"}))
          .empty());
  EXPECT_EQ(M->prepared("a"), *Before);
  EXPECT_EQ(moduleSetJSON(*M), Report);
}

TEST_F(ModuleFiles, TypedInputRejectsInvalidEnumsAndExcessiveLimits) {
  ModuleOptions O;
  O.DefaultLoad = static_cast<LoadPolicy>(999);
  EXPECT_FALSE(error(ModuleSet::create(O)).empty());
  O.DefaultLoad = LoadPolicy::Eager;
  O.Limits.Modules = 4097;
  EXPECT_FALSE(error(ModuleSet::create(O)).empty());
}

TEST_F(ModuleFiles, ExplicitDwarfCompanionFailsWithoutMapFallback) {
  const auto P = elf();
  text("probe.map", Map);
  const auto D = text("broken.debug", "not DWARF");
  auto I = input("a", P);
  I.Debug = DebugInput{DebugKind::DWARF, D};
  auto M = catalogue({I});
  ASSERT_TRUE(M);
  EXPECT_NE(error(M->prepare("a")).find("broken.debug"), std::string::npos);
  EXPECT_FALSE(M->prepared("a"));
}

TEST_F(ModuleFiles, AmbiguousDebugNameDoesNotChooseTheFirstAddress) {
  const auto P = elf();
  const auto D =
      text("ambiguous.map",
           Map.str() +
               "0000000000401004 0000000000401004 00000001 1 mapped_probe\n");
  auto I = input("a", P);
  I.Debug = DebugInput{DebugKind::Map, D};
  auto M = catalogue({I});
  ASSERT_TRUE(M);
  EXPECT_NE(error(M->lookupSymbol("a", "mapped_probe")).find("ambiguous"),
            std::string::npos);
}

TEST_F(ModuleFiles, SymbolBudgetDoesNotReturnAnIncompleteSearchResult) {
  auto I = input("a", elf());
  const auto D =
      text("many.map",
           Map.str() +
               "0000000000401004 0000000000401004 00000001 1 another_probe\n");
  I.Debug = DebugInput{DebugKind::Map, D};
  ModuleLimits Limits;
  Limits.Symbols = 1;
  auto M = catalogue({I}, Limits);
  ASSERT_TRUE(M);
  EXPECT_NE(error(M->lookupSymbol("a", "absent")).find("limit"),
            std::string::npos);
}

TEST_F(ModuleFiles, DebugBudgetFailureDoesNotAlterTheOriginalObservation) {
  auto I = input("a", elf());
  const auto D = text("large.map", Map);
  ModuleLimits Limits;
  Limits.DebugBytes = 1;
  auto M = catalogue({I}, Limits);
  ASSERT_TRUE(M);
  auto Before = M->prepare("a");
  ASSERT_TRUE(bool(Before));
  const auto Report = moduleSetJSON(*M);
  EXPECT_NE(error(M->attachDebug("a", {DebugKind::Map, D})).find("limit"),
            std::string::npos);
  EXPECT_EQ(moduleSetJSON(*M), Report);
  EXPECT_EQ(M->prepared("a"), *Before);
}

TEST_F(ModuleFiles, ModulesAddedLaterCanPrepareWithoutChangingEarlierOnes) {
  const auto P = elf();
  auto M = catalogue({input("first", P)});
  ASSERT_TRUE(M);
  auto First = M->prepare("first");
  ASSERT_TRUE(bool(First));
  ASSERT_FALSE(M->add(input("late", P, LoadPolicy::Lazy)));
  EXPECT_EQ(M->prepared("first"), *First);
  EXPECT_FALSE(M->prepared("late"));
  auto Late = M->prepare("late");
  ASSERT_TRUE(bool(Late));
  EXPECT_EQ((*First)->imageDigest(), (*Late)->imageDigest());
}

TEST_F(ModuleFiles, UnicodePathsUseTheExplicitUtf8ConfigurationBoundary) {
  const auto Original = elf();
  const auto Unicode = Root / std::filesystem::path(u8"模块 文件.elf");
  std::filesystem::rename(Original, Unicode);
  llvm::json::Array Rows;
  Rows.push_back(
      llvm::json::Object{{"id", "unicode"}, {"image", pathToUTF8(Unicode)}});
  const auto JSON = llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                             {"modules", std::move(Rows)}}))
                        .str();
  auto M = ModuleSet::fromJSON(JSON);
  ASSERT_TRUE(bool(M));
  auto Loaded = (*M)->prepare("unicode");
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  EXPECT_EQ((*Loaded)->sourcePath(), Unicode);
  EXPECT_EQ((*Loaded)->image().Entry, 0x401000U);
}

TEST_F(ModuleFiles, RealPDBCompanionAndWrongCompanionUseSharedIdentityChecks) {
#ifndef NEVERD_MODULE_FIXTURES
  GTEST_SKIP() << "Clang/lld-link PE/PDB fixtures unavailable";
#else
  const auto Fixtures = std::filesystem::path(NEVERD_MODULE_FIXTURES);
  auto I = input("driver", Fixtures / "first.sys");
  I.Debug = DebugInput{DebugKind::PDB, Fixtures / "first.pdb"};
  auto M = catalogue({I});
  ASSERT_TRUE(M);
  auto Loaded = M->prepare("driver");
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  EXPECT_EQ((*Loaded)->debugKind(), DebugKind::PDB);
  EXPECT_FALSE((*Loaded)->debugDigest().empty());
  auto Symbol = (*Loaded)->lookupSymbol("module_probe");
  ASSERT_TRUE(bool(Symbol));
  ASSERT_TRUE(Symbol->has_value());
  EXPECT_NE((**Symbol).Address, 0U);
  I.Debug->Path = Fixtures / "second.pdb";
  auto Wrong = catalogue({I});
  ASSERT_TRUE(Wrong);
  EXPECT_NE(error(Wrong->prepare("driver")).find("does not match"),
            std::string::npos);
  EXPECT_EQ(Wrong->retainedSourceBytes(), 0U);
#endif
}

TEST_F(ModuleFiles, PreparedPDBRemainsUsableAfterItsOriginalIsReplaced) {
#ifndef NEVERD_MODULE_FIXTURES
  GTEST_SKIP() << "Clang/lld-link PE/PDB fixtures unavailable";
#else
  const auto Fixtures = std::filesystem::path(NEVERD_MODULE_FIXTURES);
  const auto Original = Root / "caller.pdb";
  std::filesystem::copy_file(Fixtures / "first.pdb", Original);
  auto I = input("driver", Fixtures / "first.sys");
  I.Debug = DebugInput{DebugKind::PDB, Original};
  auto M = catalogue({I});
  ASSERT_TRUE(M);
  auto Prepared = M->prepare("driver");
  ASSERT_TRUE(bool(Prepared)) << llvm::toString(Prepared.takeError());
  const auto Digest = (*Prepared)->debugDigest();
  const auto Private = (*Prepared)->preparedDebugPath();
  text("caller.pdb", "caller replaced its PDB");
  M.reset();
  auto Symbol = (*Prepared)->lookupSymbol("module_probe");
  ASSERT_TRUE(bool(Symbol));
  ASSERT_TRUE(Symbol->has_value());
  EXPECT_EQ((*Prepared)->debugDigest(), Digest);
  EXPECT_TRUE(std::filesystem::exists(Private));
  Prepared = std::shared_ptr<const ModuleImage>();
  EXPECT_FALSE(std::filesystem::exists(Private));
#endif
}
} // namespace
