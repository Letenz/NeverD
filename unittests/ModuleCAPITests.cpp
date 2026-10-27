//===- ModuleCAPITests.cpp - Public module ownership and input contracts
//---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Public module ownership and input contracts.
///

#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

extern "C" int neverd_test_module_c_header(void);
namespace {
std::string take(const char *Text) {
  std::string Result = Text ? Text : "";
  neverd_free_string(Text);
  return Result;
}
std::string utf8(const std::filesystem::path &Path) {
  const auto Text = Path.u8string();
  return std::string(Text.begin(), Text.end());
}
std::string json(llvm::json::Value Value) {
  return llvm::formatv("{0}", Value).str();
}
class ModuleCAPI : public testing::Test {
protected:
  std::filesystem::path Root, Image;
  neverd_module_set_t Set = 0;
  void SetUp() override {
    llvm::SmallString<128> Directory;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-module-capi", Directory));
    Root = std::u8string(Directory.begin(), Directory.end());
    Image = elf();
  }
  void TearDown() override {
    if (Set)
      neverd_module_set_destroy(Set, nullptr);
    std::error_code EC;
    std::filesystem::remove_all(Root, EC);
  }
  std::filesystem::path text(const std::filesystem::path &Name,
                             llvm::StringRef Text) {
    auto Path = Root / Name;
    std::ofstream OS(Path, std::ios::binary);
    OS.write(Text.data(), Text.size());
    return Path;
  }
  std::filesystem::path elf(const std::filesystem::path &Name = "probe.elf",
                            uint8_t Value = 42) {
    using ELF = llvm::object::ELF64LE;
    ELF::Ehdr H{};
    std::memcpy(H.e_ident, "\177ELF", 4);
    H.e_ident[llvm::ELF::EI_CLASS] = llvm::ELF::ELFCLASS64;
    H.e_ident[llvm::ELF::EI_DATA] = llvm::ELF::ELFDATA2LSB;
    H.e_ident[llvm::ELF::EI_VERSION] = llvm::ELF::EV_CURRENT;
    H.e_type = llvm::ELF::ET_EXEC;
    H.e_machine = llvm::ELF::EM_X86_64;
    H.e_version = llvm::ELF::EV_CURRENT;
    H.e_entry = 0x401000;
    H.e_ehsize = sizeof(H);
    H.e_phoff = sizeof(H);
    H.e_phentsize = sizeof(ELF::Phdr);
    H.e_phnum = 1;
    H.e_shentsize = sizeof(ELF::Shdr);
    ELF::Phdr P{};
    P.p_type = llvm::ELF::PT_LOAD;
    P.p_flags = llvm::ELF::PF_R | llvm::ELF::PF_X;
    P.p_offset = 4096;
    P.p_vaddr = 0x401000;
    P.p_filesz = 6;
    P.p_memsz = 6;
    P.p_align = 4096;
    std::vector<uint8_t> Bytes(4102);
    std::memcpy(Bytes.data(), &H, sizeof(H));
    std::memcpy(Bytes.data() + sizeof(H), &P, sizeof(P));
    const uint8_t Code[] = {0xB8, Value, 0, 0, 0, 0xC3};
    std::memcpy(Bytes.data() + 4096, Code, sizeof(Code));
    const auto Path = Root / Name;
    std::ofstream OS(Path, std::ios::binary);
    OS.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    return Path;
  }
  static constexpr llvm::StringLiteral Map =
      "VMA LMA Size Align Out In Symbol\n"
      "0000000000401000 0000000000401000 00000006 16 .text\n"
      "0000000000401000 0000000000401000 00000006 1 probe.o:(.text)\n"
      "0000000000401000 0000000000401000 00000006 1 mapped_probe\n";
  std::string module(llvm::StringRef ID = "a",
                     std::filesystem::path Path = {}) {
    return json(llvm::json::Object{
        {"id", ID.str()}, {"image", utf8(Path.empty() ? Image : Path)}});
  }
  void start(const std::string &Configuration = "{}",
             const char *Base = nullptr) {
    neverd_module_set_options_v1 O{sizeof(O), Configuration.c_str(), Base};
    const char *Error = nullptr;
    Set = neverd_module_set_create_v1(&O, &Error);
    ASSERT_NE(Set, 0u) << take(Error);
    EXPECT_EQ(Error, nullptr);
  }
  llvm::json::Value inspect() {
    const char *Error = nullptr;
    auto Report = neverd_module_set_inspect_json(Set, &Error);
    EXPECT_NE(Report, nullptr) << take(Error);
    auto Value = llvm::json::parse(take(Report));
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return nullptr;
    }
    return std::move(*Value);
  }
  llvm::json::Value row() {
    auto Value = inspect();
    return (*Value.getAsObject()->getArray("modules"))[0];
  }
  void add(const std::string &Input) {
    const char *Error = nullptr;
    ASSERT_EQ(neverd_module_set_add_json(Set, Input.c_str(), &Error), 1)
        << take(Error);
    EXPECT_EQ(Error, nullptr);
  }
};

TEST(ModuleCAPIHeader, PureCTranslationUnitLinksAndCallsTheSharedLibrary) {
  EXPECT_EQ(neverd_test_module_c_header(), 0);
}
TEST_F(ModuleCAPI, EmptyCatalogueNeedsNoDecompilerSessionOrEmulation) {
  const char *Error = "previous";
  Set = neverd_module_set_create_v1(nullptr, &Error);
  ASSERT_NE(Set, 0u);
  EXPECT_EQ(Error, nullptr);
  EXPECT_TRUE(inspect().getAsObject()->getArray("modules")->empty());
  EXPECT_EQ(neverd_module_set_prepare_eager(Set, &Error), 1);
  EXPECT_EQ(Error, nullptr);
}
TEST_F(ModuleCAPI,
       TruncatedAndUnknownVersionPrefixesAreRejectedBeforeOtherFields) {
  size_t Prefix = sizeof(size_t);
  const char *Error = nullptr;
  EXPECT_EQ(neverd_module_set_create_v1(
                reinterpret_cast<const neverd_module_set_options_v1 *>(&Prefix),
                &Error),
            0u);
  EXPECT_NE(take(Error).find("exact v1"), std::string::npos);
  neverd_module_set_options_v1 O{};
  O.struct_size = sizeof(O) + 1;
  EXPECT_EQ(neverd_module_set_create_v1(&O, &Error), 0u);
  EXPECT_NE(take(Error).find("exact v1"), std::string::npos);
}
TEST_F(ModuleCAPI,
       HandlesAreNotReusedAndStaleOperationsCannotAccessAnotherSet) {
  start();
  const auto Old = Set;
  ASSERT_EQ(neverd_module_set_destroy(Set, nullptr), 1);
  Set = 0;
  start();
  EXPECT_GT(Set, Old);
  const char *Error = nullptr;
  EXPECT_EQ(neverd_module_set_inspect_json(Old, &Error), nullptr);
  EXPECT_NE(take(Error).find("handle"), std::string::npos);
  EXPECT_EQ(neverd_module_set_destroy(Old, &Error), 0);
  EXPECT_NE(take(Error).find("handle"), std::string::npos);
  EXPECT_EQ(neverd_module_set_inspect_json(UINT64_MAX, &Error), nullptr);
  EXPECT_NE(take(Error).find("handle"), std::string::npos);
  EXPECT_EQ(neverd_module_set_destroy(0, &Error), 1);
  EXPECT_EQ(Error, nullptr);
}
TEST_F(ModuleCAPI, RegistrationBorrowsNoInputAndDoesNotOpenEagerFiles) {
  start();
  {
    std::string Input = module("a", Root / "missing.sys");
    add(Input);
    Input.assign(Input.size(), '?');
  }
  auto R = row();
  EXPECT_EQ(R.getAsObject()->getString("id"), "a");
  EXPECT_EQ(R.getAsObject()->getString("load"), "eager");
  EXPECT_EQ(R.getAsObject()->getBoolean("prepared"), false);
  const char *Error = nullptr;
  EXPECT_EQ(neverd_module_set_prepare_eager(Set, &Error), 0);
  EXPECT_FALSE(take(Error).empty());
  EXPECT_EQ(inspect().getAsObject()->getInteger("revision"), 1);
}
TEST_F(ModuleCAPI, EagerPreparationHonorsPerModulePolicyAndDefaultInheritance) {
  const auto Config = json(llvm::json::Object{
      {"module_defaults", llvm::json::Object{{"load", "lazy"}}},
      {"modules",
       llvm::json::Array{
           llvm::json::Object{{"id", "lazy"},
                              {"image", utf8(Root / "missing.elf")}},
           llvm::json::Object{
               {"id", "eager"}, {"image", utf8(Image)}, {"load", "eager"}}}}});
  start(Config);
  const char *Error = nullptr;
  ASSERT_EQ(neverd_module_set_prepare_eager(Set, &Error), 1) << take(Error);
  auto Report = inspect();
  const auto &Rows = *Report.getAsObject()->getArray("modules");
  ASSERT_EQ(Rows.size(), 2u);
  EXPECT_EQ(Rows[0].getAsObject()->getBoolean("prepared"), true);
  EXPECT_EQ(Rows[1].getAsObject()->getBoolean("prepared"), false);
}
TEST_F(ModuleCAPI, QueryPreparesLazyInputAndMissingSymbolIsAnOwnedSuccess) {
  start(R"({"module_defaults":{"load":"lazy"}})");
  add(module());
  EXPECT_EQ(row().getAsObject()->getBoolean("prepared"), false);
  const char *Error = nullptr;
  auto Report =
      neverd_module_set_lookup_symbol_json(Set, "a", "absent", &Error);
  ASSERT_NE(Report, nullptr) << take(Error);
  auto Value = llvm::json::parse(take(Report));
  ASSERT_TRUE(bool(Value));
  EXPECT_EQ(Value->getAsObject()->getBoolean("found"), false);
  EXPECT_EQ(Error, nullptr);
  EXPECT_EQ(row().getAsObject()->getBoolean("prepared"), true);
}
TEST_F(ModuleCAPI, FailedEagerBatchRetainsTheUnpreparedCatalogue) {
  start();
  add(module("a"));
  add(module("b", Root / "missing.elf"));
  const auto Before = json(inspect());
  const char *Error = nullptr;
  EXPECT_EQ(neverd_module_set_prepare_eager(Set, &Error), 0);
  EXPECT_FALSE(take(Error).empty());
  EXPECT_EQ(json(inspect()), Before);
}
TEST_F(ModuleCAPI, DuplicateAndMalformedAddDoNotChangeTheCatalogue) {
  start();
  add(module());
  const auto Before = json(inspect());
  const char *Error = nullptr;
  for (const auto &Input :
       {module(), std::string("[]"), std::string("{}"),
        std::string(R"({"id":"b","image":"x","extra":true})")}) {
    EXPECT_EQ(neverd_module_set_add_json(Set, Input.c_str(), &Error), 0);
    EXPECT_FALSE(take(Error).empty());
    EXPECT_EQ(json(inspect()), Before);
  }
}
TEST_F(ModuleCAPI, UnpreparedRevisionRepairsTheFileWithoutChangingLoadPolicy) {
  start(R"({"module_defaults":{"load":"lazy"}})");
  add(module("a", Root / "missing.elf"));
  const char *Error = nullptr;
  EXPECT_EQ(neverd_module_set_prepare(Set, "a", &Error), 0);
  EXPECT_FALSE(take(Error).empty());
  ASSERT_EQ(
      neverd_module_set_revise_unprepared_json(Set, module().c_str(), &Error),
      1)
      << take(Error);
  EXPECT_EQ(row().getAsObject()->getString("load"), "lazy");
  EXPECT_EQ(row().getAsObject()->getBoolean("prepared"), false);
  ASSERT_EQ(neverd_module_set_prepare(Set, "a", &Error), 1) << take(Error);
  const auto Before = json(inspect());
  EXPECT_EQ(
      neverd_module_set_revise_unprepared_json(Set, module().c_str(), &Error),
      0);
  EXPECT_FALSE(take(Error).empty());
  EXPECT_EQ(json(inspect()), Before);
}
TEST_F(ModuleCAPI, RevisionCannotSilentlyDiscardALoadChange) {
  start();
  add(module());
  const auto Before = json(inspect());
  const auto Input = json(llvm::json::Object{
      {"id", "a"}, {"image", utf8(Image)}, {"load", "lazy"}});
  const char *Error = nullptr;
  EXPECT_EQ(
      neverd_module_set_revise_unprepared_json(Set, Input.c_str(), &Error), 0);
  EXPECT_NE(take(Error).find("omit load"), std::string::npos);
  EXPECT_EQ(json(inspect()), Before);
}
TEST_F(ModuleCAPI,
       PreparedBytesAndOwnedReportsSurviveSourceReplacementAndDestroy) {
  start();
  add(module());
  const char *Error = nullptr;
  ASSERT_EQ(neverd_module_set_prepare(Set, "a", &Error), 1) << take(Error);
  const auto Before = json(inspect());
  elf("probe.elf", 73);
  ASSERT_EQ(neverd_module_set_prepare(Set, "a", &Error), 1) << take(Error);
  EXPECT_EQ(json(inspect()), Before);
  const auto Report = neverd_module_set_inspect_json(Set, &Error);
  ASSERT_NE(Report, nullptr);
  ASSERT_EQ(neverd_module_set_destroy(Set, &Error), 1) << take(Error);
  Set = 0;
  EXPECT_EQ(json(llvm::cantFail(llvm::json::parse(take(Report)))), Before);
}
TEST_F(ModuleCAPI, LateDebugUsesOriginalAddressesAndNamesOnlyMapTrust) {
  start();
  add(module());
  text("probe.map", Map);
  const char *Error = nullptr;
  ASSERT_EQ(neverd_module_set_prepare(Set, "a", &Error), 1) << take(Error);
  const auto Debug = json(
      llvm::json::Object{{"kind", "map"}, {"path", utf8(Root / "probe.map")}});
  ASSERT_EQ(
      neverd_module_set_attach_debug_json(Set, "a", Debug.c_str(), &Error), 1)
      << take(Error);
  auto R = row();
  EXPECT_EQ(R.getAsObject()->getBoolean("debug_loaded"), true);
  EXPECT_EQ(R.getAsObject()->getBoolean("debug_identity_verified"), false);
  EXPECT_EQ(R.getAsObject()->getBoolean("authenticated_function_signatures"),
            false);
  auto Query =
      neverd_module_set_lookup_symbol_json(Set, "a", "mapped_probe", &Error);
  ASSERT_NE(Query, nullptr) << take(Error);
  auto Result = llvm::json::parse(take(Query));
  ASSERT_TRUE(bool(Result));
  EXPECT_EQ(Result->getAsObject()->getBoolean("found"), true);
  EXPECT_EQ(Result->getAsObject()->getObject("symbol")->getString("address"),
            "0x401000");
}
TEST_F(ModuleCAPI,
       FailedCompanionDoesNotReplaceOrFallBackFromThePreparedImage) {
  start();
  add(module());
  const char *Error = nullptr;
  ASSERT_EQ(neverd_module_set_prepare(Set, "a", &Error), 1) << take(Error);
  text("probe.map", Map);
  const auto Before = json(inspect());
  const auto Debug = json(llvm::json::Object{
      {"kind", "pdb"}, {"path", utf8(Root / "missing.pdb")}});
  EXPECT_EQ(
      neverd_module_set_attach_debug_json(Set, "a", Debug.c_str(), &Error), 0);
  EXPECT_FALSE(take(Error).empty());
  EXPECT_EQ(json(inspect()), Before);
}
TEST_F(ModuleCAPI, RelativeAddsUseTheExplicitOriginAndRejectAnUnstatedOrigin) {
  const auto Base = utf8(Root);
  start("{}", Base.c_str());
  add(R"({"id":"a","image":"probe.elf"})");
  const char *Error = nullptr;
  ASSERT_EQ(neverd_module_set_prepare(Set, "a", &Error), 1) << take(Error);
  ASSERT_EQ(neverd_module_set_destroy(Set, nullptr), 1);
  Set = 0;
  start();
  EXPECT_EQ(neverd_module_set_add_json(Set, R"({"id":"a","image":"probe.elf"})",
                                       &Error),
            0);
  EXPECT_NE(take(Error).find("base_directory"), std::string::npos);
}
TEST_F(ModuleCAPI,
       ConfigurationFileSuppliesItsParentOriginWithoutOpeningModules) {
  auto Path =
      text("modules.json", R"({"modules":[{"id":"a","image":"probe.elf"}]})");
  const char *Error = nullptr;
  Set = neverd_module_set_from_file(utf8(Path).c_str(), &Error);
  ASSERT_NE(Set, 0u) << take(Error);
  EXPECT_EQ(row().getAsObject()->getBoolean("prepared"), false);
  ASSERT_EQ(neverd_module_set_prepare_eager(Set, &Error), 1) << take(Error);
}
TEST_F(ModuleCAPI, ExplicitJSONOriginOverridesTheConfigurationFileDirectory) {
  const auto Directory = Root / "configs";
  std::filesystem::create_directory(Directory);
  const auto Config = json(llvm::json::Object{
      {"base_directory", utf8(Root)},
      {"modules", llvm::json::Array{llvm::json::Object{
                      {"id", "a"}, {"image", "probe.elf"}}}}});
  const auto Path = text("configs/modules.json", Config);
  const char *Error = nullptr;
  Set = neverd_module_set_from_file(utf8(Path).c_str(), &Error);
  ASSERT_NE(Set, 0u) << take(Error);
  ASSERT_EQ(neverd_module_set_prepare_eager(Set, &Error), 1) << take(Error);
}
TEST_F(ModuleCAPI, DirectJSONRejectsConflictingExplicitOrigins) {
  const auto Config = json(llvm::json::Object{{"base_directory", utf8(Root)}});
  const auto Other = utf8(Root / "other");
  neverd_module_set_options_v1 O{sizeof(O), Config.c_str(), Other.c_str()};
  const char *Error = nullptr;
  EXPECT_EQ(neverd_module_set_create_v1(&O, &Error), 0u);
  EXPECT_NE(take(Error).find("conflicting"), std::string::npos);
}
TEST_F(ModuleCAPI, ConfigurationAndCStringBoundsAreEnforcedBeforeParsing) {
  const std::string Large(NEVERD_MODULE_JSON_LIMIT + 1, ' ');
  neverd_module_set_options_v1 O{sizeof(O), Large.c_str(), nullptr};
  const char *Error = nullptr;
  EXPECT_EQ(neverd_module_set_create_v1(&O, &Error), 0u);
  EXPECT_NE(take(Error).find("bound"), std::string::npos);
  const auto Path = text("too-large.json", Large);
  EXPECT_EQ(neverd_module_set_from_file(utf8(Path).c_str(), &Error), 0u);
  EXPECT_NE(take(Error).find("2 MiB"), std::string::npos);
  O.configuration_json = "\xC0\xAF";
  EXPECT_EQ(neverd_module_set_create_v1(&O, &Error), 0u);
  EXPECT_NE(take(Error).find("UTF-8"), std::string::npos);
  EXPECT_EQ(neverd_module_set_from_file(nullptr, &Error), 0u);
  EXPECT_FALSE(take(Error).empty());
}
TEST_F(ModuleCAPI,
       UnknownModuleAndMissingRequiredStringsReturnOwnedDiagnostics) {
  start();
  const char *Error = nullptr;
  EXPECT_EQ(neverd_module_set_prepare(Set, nullptr, &Error), 0);
  EXPECT_FALSE(take(Error).empty());
  EXPECT_EQ(neverd_module_set_prepare(Set, "unknown", &Error), 0);
  EXPECT_FALSE(take(Error).empty());
  EXPECT_EQ(neverd_module_set_lookup_symbol_json(Set, "a", "", &Error),
            nullptr);
  EXPECT_FALSE(take(Error).empty());
  const std::string Long(NEVERD_MODULE_SYMBOL_LIMIT + 1, 'x');
  EXPECT_EQ(
      neverd_module_set_lookup_symbol_json(Set, "a", Long.c_str(), &Error),
      nullptr);
  EXPECT_FALSE(take(Error).empty());
  EXPECT_EQ(neverd_module_set_add_json(Set, nullptr, nullptr), 0);
  EXPECT_EQ(neverd_module_set_prepare_eager(0, nullptr), 0);
}
TEST_F(ModuleCAPI, UnicodeConfigAndResourcePathsUseUtf8AcrossTheCABI) {
  const auto Resource = elf(std::filesystem::path(u8"资源.elf"));
  auto Path =
      text(std::filesystem::path(u8"模块.json"),
           json(llvm::json::Object{
               {"modules",
                llvm::json::Array{llvm::json::Object{
                    {"id", "a"}, {"image", utf8(Resource.filename())}}}}}));
  const char *Error = nullptr;
  Set = neverd_module_set_from_file(utf8(Path).c_str(), &Error);
  ASSERT_NE(Set, 0u) << take(Error);
  ASSERT_EQ(neverd_module_set_prepare(Set, "a", &Error), 1) << take(Error);
  EXPECT_EQ(row().getAsObject()->getString("image"), utf8(Resource));
}
TEST_F(ModuleCAPI, RealPDBMatchingAndMismatchUseTheSameEngineIdentityPolicy) {
#ifndef NEVERD_MODULE_CAPI_FIXTURES
  GTEST_SKIP() << "Clang/lld-link PE/PDB fixtures unavailable";
#else
  const auto Fixtures = std::filesystem::path(NEVERD_MODULE_CAPI_FIXTURES);
  start();
  add(module("good", Fixtures / "first.sys"));
  add(module("bad", Fixtures / "first.sys"));
  const auto Good = json(llvm::json::Object{
      {"kind", "pdb"}, {"path", utf8(Fixtures / "first.pdb")}});
  const auto Bad = json(llvm::json::Object{
      {"kind", "pdb"}, {"path", utf8(Fixtures / "second.pdb")}});
  const char *Error = nullptr;
  EXPECT_EQ(
      neverd_module_set_attach_debug_json(Set, "bad", Bad.c_str(), &Error), 0);
  EXPECT_FALSE(take(Error).empty());
  ASSERT_EQ(
      neverd_module_set_attach_debug_json(Set, "good", Good.c_str(), &Error), 1)
      << take(Error);
  auto Query =
      neverd_module_set_lookup_symbol_json(Set, "good", "module_probe", &Error);
  ASSERT_NE(Query, nullptr) << take(Error);
  auto Result = llvm::json::parse(take(Query));
  ASSERT_TRUE(bool(Result));
  EXPECT_EQ(Result->getAsObject()->getBoolean("found"), true);
#endif
}

TEST_F(ModuleCAPI,
       ConcurrentPublicAddsAreSerializedOrReportBusyWithoutLostEntries) {
  start();
  std::atomic<unsigned> Ready{0}, Failed{0};
  std::atomic<bool> Begin{false};
  std::vector<std::thread> Threads;
  for (unsigned Thread = 0; Thread < 4; ++Thread)
    Threads.emplace_back([&, Thread] {
      ++Ready;
      while (!Begin.load())
        std::this_thread::yield();
      for (unsigned Item = 0; Item < 16; ++Item) {
        const auto Input = module("module-" + std::to_string(Thread) + "-" +
                                  std::to_string(Item));
        bool Added = false;
        for (unsigned Retry = 0; Retry < 10000; ++Retry) {
          const char *Error = nullptr;
          if (neverd_module_set_add_json(Set, Input.c_str(), &Error)) {
            if (Error)
              ++Failed;
            neverd_free_string(Error);
            Added = true;
            break;
          }
          if (take(Error).find("busy") == std::string::npos) {
            ++Failed;
            break;
          }
          std::this_thread::yield();
        }
        if (!Added)
          ++Failed;
      }
    });
  while (Ready.load() != Threads.size())
    std::this_thread::yield();
  Begin.store(true);
  for (auto &Thread : Threads)
    Thread.join();
  EXPECT_EQ(Failed.load(), 0u);
  auto Report = inspect();
  EXPECT_EQ(Report.getAsObject()->getArray("modules")->size(), 64u);
  EXPECT_EQ(Report.getAsObject()->getInteger("revision"), 64);
}
TEST_F(ModuleCAPI, LongDiagnosticsRemainBoundedValidUtf8) {
  std::string Name;
  for (unsigned I = 0; I < 30000; ++I)
    Name += "\xe8\xb5\x84";
  const auto Configuration = json(llvm::json::Object{{Name, 1}});
  neverd_module_set_options_v1 O{sizeof(O), Configuration.c_str(), nullptr};
  const char *Error = nullptr;
  EXPECT_EQ(neverd_module_set_create_v1(&O, &Error), 0u);
  ASSERT_NE(Error, nullptr);
  const auto Message = take(Error);
  EXPECT_LE(Message.size(), 65536u);
  EXPECT_TRUE(llvm::json::isUTF8(Message));
  EXPECT_NE(Message.find("unknown field"), std::string::npos);
}
} // namespace
