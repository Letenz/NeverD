#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>

namespace {
#ifdef __APPLE__
std::string read(const std::filesystem::path &Path) {
  std::ifstream Input(Path);
  return {std::istreambuf_iterator<char>(Input), {}};
}

void write(const std::filesystem::path &Path, const std::string &Text) {
  std::ofstream Output(Path);
  Output << Text;
  Output.close();
  ASSERT_TRUE(Output) << Path;
}

void run(const std::vector<std::string> &Arguments,
         const std::filesystem::path &Log) {
  std::vector<llvm::StringRef> Refs(Arguments.begin(), Arguments.end());
  const std::string Out = Log.string() + ".out";
  const std::string Err = Log.string() + ".err";
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Out, Err};
  std::string Error;
  const int Status = llvm::sys::ExecuteAndWait(Refs.front(), Refs, std::nullopt,
                                               Redirects, 120, 0, &Error);
  ASSERT_EQ(Status, 0) << Error << '\n' << read(Err);
}

enum class RuntimeFixture {
  ARC,
  Associations,
  SwiftCalls,
  ConstantStrings,
  UnfairLocks,
  SwiftStrings,
  DiagnosticReports,
  Protocols,
  NativePointers
};

void verifyRuntime(bool Chained,
                   RuntimeFixture FixtureKind = RuntimeFixture::ARC,
                   bool Profiled = false) {
  const bool DiagnosticReports =
      FixtureKind == RuntimeFixture::DiagnosticReports;
  const bool SwiftStrings = FixtureKind == RuntimeFixture::SwiftStrings;
  const bool Associations = FixtureKind == RuntimeFixture::Associations;
  const bool SwiftCalls = FixtureKind == RuntimeFixture::SwiftCalls;
  const bool NativePointers = FixtureKind == RuntimeFixture::NativePointers;
  const bool ConstantStrings =
      FixtureKind == RuntimeFixture::ConstantStrings || NativePointers;
  const bool UnfairLocks = FixtureKind == RuntimeFixture::UnfairLocks;
  const bool Protocols = FixtureKind == RuntimeFixture::Protocols;
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-objc-arc", Directory));
  const std::filesystem::path Work(Directory.str().str());
  const auto Cleanup = llvm::make_scope_exit([&] {
    std::error_code Error;
    std::filesystem::remove_all(Work, Error);
  });
  const std::filesystem::path Fixtures(NEVERD_MOBILE_FIXTURE_DIR);
  const char *Fixture = Protocols           ? "ObjCProtocols.m"
                        : DiagnosticReports ? "ObjCDiagnosticReports.m"
                        : SwiftStrings      ? "ObjCSwiftString.m"
                        : UnfairLocks       ? "ObjCUnfairLocks.m"
                        : ConstantStrings   ? "ObjCConstantStrings.m"
                        : SwiftCalls        ? "ObjCSwiftRuntime.m"
                        : Associations      ? "ObjCAssociations.m"
                                            : "ObjCARC.m";
  const char *Harness = Protocols           ? "ObjCProtocolsHarness.m"
                        : DiagnosticReports ? "ObjCDiagnosticReportsHarness.m"
                        : SwiftStrings      ? "ObjCSwiftStringHarness.m"
                        : UnfairLocks       ? "ObjCUnfairLocksHarness.m"
                        : ConstantStrings   ? "ObjCConstantStringsHarness.m"
                        : SwiftCalls        ? "ObjCSwiftRuntimeHarness.m"
                        : Associations      ? "ObjCAssociationsHarness.m"
                                            : "ObjCARCHarness.m";
  const auto Original = (Work / "original.dylib").string();
  const std::string Compiler = NEVERD_TEST_CLANG;
#if defined(__aarch64__) || defined(__arm64__)
  const std::string HostArch = "arm64";
#else
  const std::string HostArch = "x86_64";
#endif
  std::vector<std::string> Compile{Compiler,      "-arch",
                                   HostArch,      "-O2",
                                   "-g0",         "-fobjc-arc",
                                   "-dynamiclib", "-framework",
                                   "Foundation",  (Fixtures / Fixture).string(),
                                   "-o",          Original};
  if (!Chained)
    Compile.push_back("-Wl,-no_fixup_chains");
  if (NativePointers)
    Compile.push_back("-DNEVERD_NATIVE_POINTERS");
  if (SwiftCalls || SwiftStrings || DiagnosticReports)
    Compile.insert(Compile.end(), {"-L/usr/lib/swift", "-lswiftCore",
                                   "-Wl,-rpath,/usr/lib/swift"});
  if (SwiftStrings)
    Compile.push_back("-lswiftFoundation");
  if (DiagnosticReports)
    Compile.insert(Compile.end(), {"-iwithsysroot", "/usr/lib"});
  if (Profiled)
    Compile.push_back("-fprofile-instr-generate=" +
                      (Work / "counters.profraw").string());
  ASSERT_NO_FATAL_FAILURE(run(Compile, Work / "compile-original"));

  std::unique_ptr<void, decltype(&neverd_session_destroy)> Session(
      neverd_session_create(), neverd_session_destroy);
  ASSERT_TRUE(Session);
  ASSERT_TRUE(neverd_session_load(Session.get(), Original.c_str()));
  std::unique_ptr<const char, decltype(&neverd_free_string)> Raw(
      neverd_objc_methods_json(Session.get(), 0), neverd_free_string);
  ASSERT_TRUE(Raw);
  auto JSON = llvm::json::parse(Raw.get());
  ASSERT_TRUE(bool(JSON));
  const auto *Object = JSON->getAsObject();
  ASSERT_NE(Object, nullptr);
  const auto *Methods = Object->getArray("methods");
  ASSERT_NE(Methods, nullptr);
  ASSERT_EQ(Methods->size(), Protocols           ? 3U
                             : DiagnosticReports ? 5U
                             : SwiftStrings      ? 2U
                             : SwiftCalls        ? 9U
                                                 : 7U);
  std::set<std::string> Remaining{"item",         "setItem:", "observer",
                                  "setObserver:", "title",    "setTitle:",
                                  ".cxx_destruct"};
  if (Associations)
    Remaining = {"objectForKey:",
                 "storeObject:forKey:policy:",
                 "clearAssociatedObjects",
                 "objectForStaticKey",
                 "storeObjectForStaticKey:",
                 "objectForInteriorKey",
                 "storeObjectForInteriorKey:"};
  if (SwiftCalls)
    Remaining = {"keep:",
                 "drop:",
                 "weakInitialize:object:",
                 "weakAssign:object:",
                 "weakRead:",
                 "weakDestroy:",
                 "objectType:",
                 "begin:scratch:flags:",
                 "end:"};
  if (ConstantStrings)
    Remaining = {"ascii", "alias", "unicode", "embedded",
                 "empty", "first", "second"};
  if (UnfairLocks)
    Remaining = {"add:",   "value",       "tryAdd:",       "lock",
                 "unlock", "assertOwner", "assertNotOwner"};
  if (SwiftStrings)
    Remaining = {"bridgeWord:storage:", "roundTrip:"};
  if (DiagnosticReports)
    Remaining = {"initializer", "initializerInFile", "fatal", "fatalInFile",
                 "terminal"};
  if (Protocols) {
    Remaining = {
        "enumerate:state:objects:count:", "metricOf:", "isNegativeMetric:"};
    const auto *Metadata = Object->getObject("objc_metadata");
    ASSERT_NE(Metadata, nullptr);
    const auto *Declarations = Metadata->getArray("protocols");
    ASSERT_NE(Declarations, nullptr);
    bool Found = false;
    for (const auto &Value : *Declarations) {
      const auto *Protocol = Value.getAsObject();
      ASSERT_NE(Protocol, nullptr);
      if (Protocol->getString("name") != "NSFastEnumeration")
        continue;
      const auto *Members = Protocol->getArray("methods");
      ASSERT_NE(Members, nullptr);
      for (const auto &Member : *Members) {
        const auto *Method = Member.getAsObject();
        ASSERT_NE(Method, nullptr);
        if (Method->getString("selector") ==
            "countByEnumeratingWithState:objects:count:") {
          EXPECT_EQ(Method->getString("status"), "supported");
          EXPECT_EQ(Method->get("implementation"), nullptr);
          Found = true;
        }
      }
    }
    ASSERT_TRUE(Found);
  }
  std::string Declarations;
  std::string Install = "static void installRecovered(void) {\n"
                        "Class cls = objc_getClass(\"" +
                        std::string(Protocols           ? "NDProtocolCalls"
                                    : DiagnosticReports ? "NDDiagnosticReports"
                                    : SwiftStrings      ? "NDSwiftString"
                                    : UnfairLocks       ? "NDUnfairLocks"
                                    : ConstantStrings   ? "NDConstantStrings"
                                    : SwiftCalls        ? "NDSwiftRuntimeCalls"
                                                        : "NDARCBox") +
                        "\");\n";
  std::vector<std::string> Sources;
  std::map<std::string, std::string> IdentityHelpers;
  std::string NativeHelper;
  std::set<std::string> StorageNames;
  for (const auto &Value : *Methods) {
    const auto *Method = Value.getAsObject();
    ASSERT_NE(Method, nullptr);
    ASSERT_EQ(Method->getString("status"), "recovered") << Raw.get();
    auto Selector = Method->getString("selector");
    auto Name = Method->getString("function_name");
    auto Source = Method->getString("source");
    ASSERT_TRUE(Selector && Name && Source);
    ASSERT_EQ(Remaining.erase(Selector->str()), 1U);
    std::string MethodSource = Source->str();
    if (NativePointers && (*Selector == "ascii" || *Selector == "unicode")) {
      EXPECT_NE(MethodSource.find("NDForwardPointer(void* native_arg0)"),
                std::string::npos)
          << MethodSource;
      EXPECT_NE(MethodSource.find("objc_retain"), std::string::npos);
      // Each C API method includes its complete native dependency group.
      // Link the common helper once when combining these two independent units.
      const auto Begin = MethodSource.find(
          "\nint64_t NDForwardPointer(void* native_arg0) {\n");
      ASSERT_NE(Begin, std::string::npos);
      const auto End = MethodSource.find("\n}", Begin);
      ASSERT_NE(End, std::string::npos);
      const auto Definition = MethodSource.substr(Begin, End + 2 - Begin);
      if (NativeHelper.empty()) {
        NativeHelper = Definition;
      } else {
        EXPECT_EQ(NativeHelper, Definition);
        MethodSource.erase(Begin, End + 2 - Begin);
      }
    }
    for (const char *Inventory :
         {"shared_identity_functions", "shared_storage_functions"})
      if (const auto *Helpers = Method->getArray(Inventory)) {
        for (const auto &Value : *Helpers) {
          const auto Helper = Value.getAsString();
          ASSERT_TRUE(Helper);
          if (llvm::StringRef(Inventory) == "shared_storage_functions")
            StorageNames.insert(Helper->str());
          const auto Begin =
              MethodSource.find("\nuintptr_t " + Helper->str() + "(void) {\n");
          ASSERT_NE(Begin, std::string::npos);
          const auto End = MethodSource.find("\n}", Begin);
          ASSERT_NE(End, std::string::npos);
          const auto Definition = MethodSource.substr(Begin, End + 2 - Begin);
          const auto [It, Added] =
              IdentityHelpers.emplace(Helper->str(), Definition);
          EXPECT_EQ(It->second, Definition);
          MethodSource.erase(Begin, End + 2 - Begin);
        }
      }
    const auto Path =
        Work / ("recovered-" + std::to_string(Sources.size()) + ".c");
    ASSERT_NO_FATAL_FAILURE(write(Path, MethodSource));
    Sources.push_back(Path.string());
    Declarations += "extern void " + Name->str() + "(void);\n";
    Install += "class_replaceMethod(cls, sel_registerName(\"" +
               Selector->str() + "\"), (IMP)" + Name->str() +
               ", method_getTypeEncoding(class_getInstanceMethod(cls, "
               "sel_registerName(\"" +
               Selector->str() + "\"))));\n";
  }
  ASSERT_TRUE(Remaining.empty());
  EXPECT_EQ(StorageNames.size(), Profiled ? 1U : 0U);
  if (DiagnosticReports)
    EXPECT_FALSE(IdentityHelpers.empty());
  else
    EXPECT_EQ(IdentityHelpers.size(), (Associations      ? 2U
                                       : ConstantStrings ? 6U
                                                         : 0U) +
                                          StorageNames.size());
  if (!IdentityHelpers.empty()) {
    std::string Shared = "#include <stdint.h>\n";
    for (const auto &[Name, Definition] : IdentityHelpers)
      Shared += Definition + "\n";
    const auto Path = Work / "shared-identities.c";
    ASSERT_NO_FATAL_FAILURE(write(Path, Shared));
    Sources.push_back(Path.string());
  }
  ASSERT_NO_FATAL_FAILURE(
      write(Work / "replacements.h", Declarations + Install + "}\n"));
  const auto Baseline = (Work / "baseline").string();
  Compile = {Compiler,
             "-arch",
             HostArch,
             "-O2",
             "-fno-objc-arc",
             "-fblocks",
             "-framework",
             "Foundation",
             "-I" + Work.string(),
             (Fixtures / Harness).string(),
             Original,
             "-o",
             Baseline};
  if (SwiftCalls || SwiftStrings || DiagnosticReports)
    Compile.insert(Compile.end() - 2, {"-L/usr/lib/swift", "-lswiftCore",
                                       "-Wl,-rpath,/usr/lib/swift"});
  if (SwiftStrings) {
    const auto Words = (Work / "words.dylib").string();
    ASSERT_NO_FATAL_FAILURE(
        run({"/usr/bin/swiftc", "-O", "-target", HostArch + "-apple-macosx13.0",
             "-emit-library",
             (Fixtures / "ObjCSwiftStringWords.swift").string(), "-o", Words},
            Work / "compile-swift-words"));
    Compile.insert(Compile.end() - 2, {"-lswiftFoundation", Words});
  }
  ASSERT_NO_FATAL_FAILURE(run(Compile, Work / "link-baseline"));
  ASSERT_NO_FATAL_FAILURE(run({Baseline}, Work / "baseline"));
  const auto Recovered = (Work / "recovered").string();
  Compile.back() = Recovered;
  Compile.push_back("-DNEVERD_RECOVERED_ARC");
  Compile.insert(Compile.end(), Sources.begin(), Sources.end());
  ASSERT_NO_FATAL_FAILURE(run(Compile, Work / "link-recovered"));
  ASSERT_NO_FATAL_FAILURE(run({Recovered}, Work / "recovered"));
  EXPECT_EQ(read(Work / "baseline.out"), read(Work / "recovered.out"));
  if (DiagnosticReports) {
    EXPECT_EQ(read(Work / "baseline.err"), read(Work / "recovered.err"));
    EXPECT_NE(
        read(Work / "recovered.err").find("Use of unimplemented initializer"),
        std::string::npos);
  }
  EXPECT_EQ(read(Work / "recovered.out"),
            Protocols ? "enumerated=132096\ninteger-bits=4096\n"
            : DiagnosticReports
                ? "diagnostic-runtime=pass\ncontents=pass\ntrap=pass\n"
            : SwiftStrings ? "swift-string=pass\ncontents=pass\nlifetime=pass\n"
            : UnfairLocks  ? "unfair-locks=pass\ntrylock=pass\nownership=pass\n"
                             "concurrency=pass\n"
            : ConstantStrings ? "constant-strings=pass\nunicode=pass\nidentity="
                                "pass\nlifetime=pass\n"
            : SwiftCalls
                ? "swift-runtime=pass\nweak=pass\naccess=pass\ndestroyed=128\n"
            : Associations
                ? "associations=pass\nretain=pass\ncopy=pass\nstatic-"
                  "keys=pass\nclear="
                  "pass\ndestroyed=1\n"
                : "strong=pass\nweak=pass\ncopy=pass\ndestructor="
                  "pass\ndestroyed=3\n");
}
#endif

TEST(ObjCRuntimeSource, RecompiledARCMethodsPreserveActualObjectLifetimes) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained ? "default fixups" : "classic fixups");
    ASSERT_NO_FATAL_FAILURE(verifyRuntime(Chained));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation and Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, RecompiledAssociatedObjectsPreserveLifetimeAndPolicy) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained ? "default fixups" : "classic fixups");
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::Associations));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation and Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, RecompiledProfiledMethodsKeepSharedCounterStorage) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained ? "default fixups" : "classic fixups");
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::Associations, true));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation and Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource,
     RecompiledSwiftRuntimeCallsPreserveWeakObjectLifetimes) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained ? "default fixups" : "classic fixups");
    ASSERT_NO_FATAL_FAILURE(verifyRuntime(Chained, RuntimeFixture::SwiftCalls));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation and Swift runtime";
#endif
}

TEST(ObjCRuntimeSource, RecompiledConstantStringsPreserveContentsAndIdentity) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained ? "default fixups" : "classic fixups");
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::ConstantStrings));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation and Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, NativePointerForwardingPreservesStringsAndOwnership) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained ? "default fixups" : "classic fixups");
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::NativePointers));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation and Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource,
     RecompiledUnfairLocksPreserveOwnershipAndConcurrentUpdates) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained ? "default fixups" : "classic fixups");
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::UnfairLocks));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation and os_unfair_lock";
#endif
}
TEST(ObjCRuntimeSource,
     RecompiledSwiftStringBridgePreservesContentsAndLifetime) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained ? "default fixups" : "classic fixups");
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::SwiftStrings));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation and Swift runtime";
#endif
}
TEST(ObjCRuntimeSource, RecompiledProtocolCallsPreserveEnumerationState) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained ? "default fixups" : "classic fixups");
    ASSERT_NO_FATAL_FAILURE(verifyRuntime(Chained, RuntimeFixture::Protocols));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation";
#endif
}
} // namespace

TEST(ObjCRuntimeSource, RecompiledDiagnosticsPreserveMessagesAndTerminalTrap) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained);
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::DiagnosticReports));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation and Swift runtime";
#endif
}

namespace {
#ifdef __APPLE__
void verifySwiftStorage(bool Chained) {
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-swift-storage", Directory));
  const std::filesystem::path Work(Directory.str().str());
  const auto Cleanup = llvm::make_scope_exit([&] {
    std::error_code Error;
    std::filesystem::remove_all(Work, Error);
  });
  const std::filesystem::path Fixtures(NEVERD_MOBILE_FIXTURE_DIR);
#if defined(__aarch64__) || defined(__arm64__)
  const std::string HostArch = "arm64";
#else
  const std::string HostArch = "x86_64";
#endif
  const auto Original = (Work / "original.dylib").string();
  std::vector<std::string> Compile{
      "/usr/bin/swiftc",
      "-O",
      "-emit-library",
      "-enable-library-evolution",
      "-module-name",
      "NDStorage",
      "-target",
      HostArch + "-apple-macosx13.0",
      (Fixtures / "ObjCSwiftStorage.swift").string(),
      "-o",
      Original};
  if (!Chained)
    Compile.insert(Compile.end(), {"-Xlinker", "-no_fixup_chains"});
  ASSERT_NO_FATAL_FAILURE(run(Compile, Work / "compile-original"));
  std::unique_ptr<void, decltype(&neverd_session_destroy)> Session(
      neverd_session_create(), neverd_session_destroy);
  ASSERT_TRUE(Session);
  ASSERT_TRUE(neverd_session_load(Session.get(), Original.c_str()));
  std::unique_ptr<const char, decltype(&neverd_free_string)> Raw(
      neverd_objc_methods_json(Session.get(), 0), neverd_free_string);
  ASSERT_TRUE(Raw);
  auto JSON = llvm::json::parse(Raw.get());
  ASSERT_TRUE(bool(JSON));
  const auto *Object = JSON->getAsObject();
  ASSERT_NE(Object, nullptr);
  const auto *Methods = Object->getArray("methods");
  ASSERT_NE(Methods, nullptr);
  std::set<std::string> Remaining{"value", "count"};
  std::vector<std::string> Sources;
  std::string Declarations,
      Install = "static void installRecovered(void) {\n"
                "Class cls = objc_getClass(\"NDSwiftStorage\");\n";
  for (const auto &Value : *Methods) {
    const auto *Method = Value.getAsObject();
    ASSERT_NE(Method, nullptr);
    const auto Selector = Method->getString("selector");
    ASSERT_TRUE(Selector);
    if (!Remaining.count(Selector->str()))
      continue;
    ASSERT_EQ(Method->getString("status"), "recovered") << Raw.get();
    const auto Name = Method->getString("function_name");
    const auto Source = Method->getString("source");
    ASSERT_TRUE(Name && Source);
    EXPECT_TRUE(Source->contains("ivar_getOffset"));
    const auto Path = Work / (Selector->str() + ".c");
    ASSERT_NO_FATAL_FAILURE(write(Path, Source->str()));
    Sources.push_back(Path.string());
    Declarations += "extern void " + Name->str() + "(void);\n";
    Install += "class_replaceMethod(cls, sel_registerName(\"" +
               Selector->str() + "\"), (IMP)" + Name->str() +
               ", method_getTypeEncoding(class_getInstanceMethod(cls, "
               "sel_registerName(\"" +
               Selector->str() + "\"))));\n";
    Remaining.erase(Selector->str());
  }
  ASSERT_TRUE(Remaining.empty());
  ASSERT_NO_FATAL_FAILURE(
      write(Work / "replacements.h", Declarations + Install + "}\n"));
  Compile = {NEVERD_TEST_CLANG,
             "-arch",
             HostArch,
             "-O2",
             "-fno-objc-arc",
             "-framework",
             "Foundation",
             "-I" + Work.string(),
             (Fixtures / "ObjCSwiftStorageHarness.m").string(),
             Original,
             "-o",
             (Work / "baseline").string()};
  ASSERT_NO_FATAL_FAILURE(run(Compile, Work / "link-baseline"));
  ASSERT_NO_FATAL_FAILURE(run({Compile.back()}, Work / "baseline"));
  Compile.back() = (Work / "recovered").string();
  Compile.push_back("-DNEVERD_RECOVERED_STORAGE");
  Compile.insert(Compile.end(), Sources.begin(), Sources.end());
  ASSERT_NO_FATAL_FAILURE(run(Compile, Work / "link-recovered"));
  ASSERT_NO_FATAL_FAILURE(
      run({(Work / "recovered").string()}, Work / "recovered"));
  EXPECT_EQ(read(Work / "baseline.out"), read(Work / "recovered.out"));
  EXPECT_EQ(read(Work / "recovered.out"),
            "swift-storage=pass\ngetter-calls=2048\n");
}
#endif
} // namespace

TEST(ObjCRuntimeSource, RecompiledSwiftStoredPropertiesUseRuntimeIvarOffsets) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained);
    ASSERT_NO_FATAL_FAILURE(verifySwiftStorage(Chained));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation and Swift runtime";
#endif
}
