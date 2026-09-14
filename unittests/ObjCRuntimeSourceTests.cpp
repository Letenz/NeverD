#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"

#include <algorithm>
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
  NativePointers,
  Foundation,
  BlockLifetimes,
  CoreData,
  ScalarConstants,
  SwiftLiterals,
  StoredStrings,
  Graphics,
  DarwinDeclarations
};

void verifyRuntime(bool Chained,
                   RuntimeFixture FixtureKind = RuntimeFixture::ARC,
                   bool Profiled = false, bool ManualBlocks = false) {
  const bool DarwinDeclarations =
      FixtureKind == RuntimeFixture::DarwinDeclarations;
  const bool CoreData = FixtureKind == RuntimeFixture::CoreData;
  const bool ScalarConstants = FixtureKind == RuntimeFixture::ScalarConstants;
  const bool SwiftLiterals = FixtureKind == RuntimeFixture::SwiftLiterals;
  const bool StoredStrings = FixtureKind == RuntimeFixture::StoredStrings;
  const bool Graphics = FixtureKind == RuntimeFixture::Graphics;
  const bool BlockLifetimes = FixtureKind == RuntimeFixture::BlockLifetimes;
  const bool Foundation = FixtureKind == RuntimeFixture::Foundation;
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
  const char *Fixture = DarwinDeclarations  ? "ObjCDarwinDeclarations.m"
                        : CoreData          ? "ObjCCoreDataCalls.m"
                        : ScalarConstants   ? "ObjCScalarConstants.m"
                        : SwiftLiterals     ? "ObjCSwiftLiteralStrings.m"
                        : StoredStrings     ? "ObjCStoredStrings.m"
                        : Graphics          ? "ObjCGraphicsCalls.m"
                        : BlockLifetimes    ? "ObjCBlockLifetimes.m"
                        : Foundation        ? "ObjCFoundationCalls.m"
                        : Protocols         ? "ObjCProtocols.m"
                        : DiagnosticReports ? "ObjCDiagnosticReports.m"
                        : SwiftStrings      ? "ObjCSwiftString.m"
                        : UnfairLocks       ? "ObjCUnfairLocks.m"
                        : ConstantStrings   ? "ObjCConstantStrings.m"
                        : SwiftCalls        ? "ObjCSwiftRuntime.m"
                        : Associations      ? "ObjCAssociations.m"
                                            : "ObjCARC.m";
  const char *Harness = DarwinDeclarations  ? "ObjCDarwinDeclarationsHarness.m"
                        : CoreData          ? "ObjCCoreDataCallsHarness.m"
                        : ScalarConstants   ? "ObjCScalarConstantsHarness.m"
                        : SwiftLiterals     ? "ObjCSwiftLiteralStringsHarness.m"
                        : StoredStrings     ? "ObjCStoredStringsHarness.m"
                        : Graphics          ? "ObjCGraphicsCallsHarness.m"
                        : BlockLifetimes    ? "ObjCBlockLifetimesHarness.m"
                        : Foundation        ? "ObjCFoundationCallsHarness.m"
                        : Protocols         ? "ObjCProtocolsHarness.m"
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
  if (ManualBlocks) {
    ASSERT_TRUE(BlockLifetimes);
    auto Flag = std::find(Compile.begin(), Compile.end(), "-fobjc-arc");
    ASSERT_NE(Flag, Compile.end());
    *Flag = "-fno-objc-arc";
    Compile.push_back("-DNEVERD_MANUAL_BLOCKS");
  }
  if (CoreData)
    Compile.insert(Compile.end(), {"-framework", "CoreData"});
  if (Graphics)
    Compile.insert(Compile.end(),
                   {"-framework", "CoreGraphics", "-framework", "ImageIO"});
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
  if (SwiftLiterals) {
    const auto Base = (Work / "literal-base.o").string();
    ASSERT_NO_FATAL_FAILURE(
        run({Compiler, "-arch", HostArch, "-O2", "-fobjc-arc", "-c",
             (Fixtures / Fixture).string(), "-o", Base},
            Work / "compile-base"));
    Compile = {"/usr/bin/swiftc",
               "-O",
               "-target",
               HostArch + "-apple-macosx13.0",
               "-emit-library",
               "-import-objc-header",
               (Fixtures / "ObjCSwiftLiteralStrings.h").string(),
               (Fixtures / "ObjCSwiftLiteralStrings.swift").string(),
               Base,
               "-o",
               Original};
    if (!Chained)
      Compile.insert(Compile.end(), {"-Xlinker", "-no_fixup_chains"});
  }
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
  ASSERT_EQ(Methods->size(), DarwinDeclarations  ? 19U
                             : CoreData          ? 3U
                             : ScalarConstants   ? 6U
                             : SwiftLiterals     ? 3U
                             : StoredStrings     ? 4U
                             : Graphics          ? 6U
                             : BlockLifetimes    ? (ManualBlocks ? 6U : 5U)
                             : Foundation        ? 13U
                             : Protocols         ? 6U
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
    Remaining = {"enumerate:state:objects:count:",
                 "metricOf:",
                 "isNegativeMetric:",
                 "countObjects:",
                 "reportMutation:",
                 "checkRuntimeGuard:"};
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
  if (Foundation)
    Remaining = {"lookup:key:",
                 "lengthOf:",
                 "copyObject:",
                 "append:to:",
                 "numberValue:",
                 "put:forKey:in:",
                 "makeDictionary:keys:count:",
                 "formatObject:number:fraction:",
                 "formatPosition:fraction:",
                 "formatEmpty",
                 "formatWide:small:",
                 "logObject:count:fraction:",
                 "logEmpty"};
  if (CoreData)
    Remaining = {
        "fetchFromContext:request:error:", "countInContext:request:error:",
        "registeredObjectsInContext:"};
  if (ScalarConstants)
    Remaining = {"finiteDouble", "negativeZeroDouble", "payloadDouble",
                 "finiteFloat",  "negativeZeroFloat",  "payloadFloat"};
  if (Graphics)
    Remaining = {"widthOfImage:", "heightOfImage:",     "retainImage:",
                 "alphaOfColor:", "componentsInColor:", "imageCountInSource:"};
  if (StoredStrings)
    Remaining = {"arrayWithValue:", "dictionaryWithValue:", "writeLiteralTo:",
                 "literal"};
  if (SwiftLiterals)
    Remaining = {"asciiLiteral", "sameAsciiLiteral", "unicodeLiteral"};
  if (DarwinDeclarations)
    Remaining = {"nameOfClass:",
                 "classNamed:",
                 "nameOfSelector:",
                 "selectorNamed:",
                 "incrementWithLock:counter:",
                 "incrementWithObject:counter:",
                 "compare:with:",
                 "time:delta:",
                 "lastError",
                 "remainder:divisor:",
                 "defaultMode",
                 "modeStorage",
                 "descriptionKey",
                 "mainQueue",
                 "timerType",
                 "defaultPriority",
                 "foundationVersion",
                 "belongs:to:",
                 "responds:selector:"};
  if (BlockLifetimes)
    Remaining = {"makeCounterForArray:", "makeCounterForArray:other:offset:",
                 "duplicateBlock:", "releaseBlock:",
                 "synchronouslyAppend:toArray:queue:"};
  if (ManualBlocks)
    Remaining.insert("holderForBlock:");
  std::string Declarations;
  std::string Install = "static void installRecovered(void) {\n"
                        "Class cls = objc_getClass(\"" +
                        std::string(DarwinDeclarations ? "NDDarwinDeclarations"
                                    : CoreData         ? "NDCoreDataCalls"
                                    : ScalarConstants  ? "NDScalarConstants"
                                    : SwiftLiterals    ? "NDSwiftLiteralStrings"
                                    : StoredStrings    ? "NDStoredStrings"
                                    : Graphics         ? "NDGraphicsCalls"
                                    : BlockLifetimes   ? "NDBlockFactory"
                                    : Foundation       ? "NDFoundationCalls"
                                    : Protocols        ? "NDProtocolCalls"
                                    : DiagnosticReports ? "NDDiagnosticReports"
                                    : SwiftStrings      ? "NDSwiftString"
                                    : UnfairLocks       ? "NDUnfairLocks"
                                    : ConstantStrings   ? "NDConstantStrings"
                                    : SwiftCalls        ? "NDSwiftRuntimeCalls"
                                                        : "NDARCBox") +
                        "\");\n";
  std::vector<std::string> Sources;
  std::map<std::string, std::string> IdentityHelpers;
  std::map<std::string, std::string> BlockHelpers;
  unsigned RepeatedBlockHelpers = 0;
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
    // Each C API unit is independently compilable. Its inventory identifies
    // functions whose address must be shared when units are linked together.
    if (const auto *Helpers = Method->getArray("shared_block_functions"))
      for (const auto &Value : *Helpers) {
        const auto Helper = Value.getAsString();
        ASSERT_TRUE(Helper);
        auto Name = MethodSource.find(" " + Helper->str() + "(");
        ASSERT_NE(Name, std::string::npos);
        auto Begin = MethodSource.rfind('\n', Name);
        ASSERT_NE(Begin, std::string::npos);
        // Skip prototypes and select the emitted top-level definition.
        while (MethodSource.find('{', Begin) > MethodSource.find(';', Begin)) {
          Name = MethodSource.find(" " + Helper->str() + "(", Name + 1);
          ASSERT_NE(Name, std::string::npos);
          Begin = MethodSource.rfind('\n', Name);
        }
        const auto End = MethodSource.find("\n}", Begin);
        ASSERT_NE(End, std::string::npos);
        const auto Definition = MethodSource.substr(Begin, End + 2 - Begin);
        const auto [It, Added] =
            BlockHelpers.emplace(Helper->str(), Definition);
        EXPECT_EQ(It->second, Definition);
        if (!Added) {
          ++RepeatedBlockHelpers;
          MethodSource.replace(Begin, End + 2 - Begin,
                               Definition.substr(0, Definition.find('{')) +
                                   ";");
        }
      }
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
  if (BlockLifetimes)
    EXPECT_GE(RepeatedBlockHelpers, 2U);
  EXPECT_EQ(StorageNames.size(), Profiled ? 1U : 0U);
  if (DiagnosticReports)
    EXPECT_FALSE(IdentityHelpers.empty());
  else
    EXPECT_EQ(IdentityHelpers.size(), (Associations      ? 2U
                                       : ConstantStrings ? 6U
                                       : Foundation      ? 6U
                                       : SwiftLiterals   ? 2U
                                       : StoredStrings   ? 4U
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
  if (ManualBlocks)
    Compile.insert(Compile.end() - 2, "-DNEVERD_MANUAL_BLOCKS");
  if (CoreData)
    Compile.insert(Compile.end() - 2, {"-framework", "CoreData"});
  if (Graphics)
    Compile.insert(Compile.end() - 2,
                   {"-framework", "CoreGraphics", "-framework", "ImageIO"});
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
  if (Protocols) {
    const auto Probe = (Work / "stack-failure-probe.dylib").string();
    ASSERT_NO_FATAL_FAILURE(
        run({Compiler, "-arch", HostArch, "-O2", "-dynamiclib",
             (Fixtures / "ObjCStackFailureProbe.c").string(), "-o", Probe},
            Work / "compile-stack-probe"));
    Compile.insert(Compile.end() - 2, Probe);
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
  if (Foundation) {
    auto Messages = [](const std::string &Text) {
      std::vector<std::string> Result;
      size_t Position = 0;
      while ((Position = Text.find("ND_FORMAT:", Position)) !=
             std::string::npos) {
        const auto End = Text.find('\n', Position);
        Result.push_back(Text.substr(Position, End - Position));
        Position = End == std::string::npos ? Text.size() : End + 1;
      }
      return Result;
    };
    const auto BaselineMessages = Messages(read(Work / "baseline.err"));
    const auto RecoveredMessages = Messages(read(Work / "recovered.err"));
    ASSERT_EQ(BaselineMessages.size(), 4U);
    EXPECT_EQ(RecoveredMessages, BaselineMessages);
  }
  if (DiagnosticReports) {
    EXPECT_EQ(read(Work / "baseline.err"), read(Work / "recovered.err"));
    EXPECT_NE(
        read(Work / "recovered.err").find("Use of unimplemented initializer"),
        std::string::npos);
  }
  EXPECT_EQ(
      read(Work / "recovered.out"),
      DarwinDeclarations ? "darwin-declarations=2048\nlocked-updates="
                           "8192\nsynchronized-updates=8192\n"
      : CoreData
          ? "core-data-fetches=1024\ncontext-identity=pass\nfetch-count=16\n"
            "nil-context=pass\n"
      : ScalarConstants
          ? "scalar-bit-checks=6144\nsigned-zero=pass\nnan-payload=pass\n"
      : SwiftLiterals ? "swift-literals=3072\nutf8=pass\nlifetime="
                        "pass\nidentical-objects=0\n"
      : StoredStrings
          ? "stored-strings=4096\nobject-identity=pass\nlifetime=pass\n"
      : Graphics ? "graphics-queries=6144\nimage-identity=pass\npng-count=1\n"
      : BlockLifetimes
          ? "escaping-blocks=1024\ncopy-dispose=pass\nmutated-captures=pass\n"
            "conditional-invokes=1024\nconditional-construction=pass\n"
            "synchronous-mutations=1024\n"
      : Foundation ? "variadic-formats=2276\nframework-iterations=2048\narray-"
                     "dictionaries=17\nnil-"
                     "dispatch=pass\n"
      : Protocols  ? "enumerated=132096\nmutations=2048\nloop-mutations=4\n"
                     "integer-bits=4096\nguard-check=pass\n"
      : DiagnosticReports
          ? "diagnostic-runtime=pass\ncontents=pass\ntrap=pass\n"
      : SwiftStrings    ? "swift-string=pass\ncontents=pass\nlifetime=pass\n"
      : UnfairLocks     ? "unfair-locks=pass\ntrylock=pass\nownership=pass\n"
                          "concurrency=pass\n"
      : ConstantStrings ? "constant-strings=pass\nunicode=pass\nidentity="
                          "pass\nlifetime=pass\n"
      : SwiftCalls
          ? "swift-runtime=pass\nweak=pass\naccess=pass\ndestroyed=128\n"
      : Associations ? "associations=pass\nretain=pass\ncopy=pass\nstatic-"
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
TEST(ObjCRuntimeSource,
     RecompiledFrameworkCallsPreserveValuesAndObjectIdentity) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained ? "chained" : "classic");
    ASSERT_NO_FATAL_FAILURE(verifyRuntime(Chained, RuntimeFixture::Foundation));
  }
#else
  GTEST_SKIP() << "requires the Darwin Objective-C runtime";
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

TEST(ObjCRuntimeSource,
     RecompiledBlocksPreserveEscapingCapturesAndOwnershipHelpers) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained ? "default fixups" : "classic fixups");
    for (bool Manual : {false, true}) {
      SCOPED_TRACE(Manual ? "manual reference counting" : "ARC");
      for (bool Profiled : {false, true}) {
        SCOPED_TRACE(Profiled ? "instrumented counters" : "uninstrumented");
        ASSERT_NO_FATAL_FAILURE(verifyRuntime(
            Chained, RuntimeFixture::BlockLifetimes, Profiled, Manual));
      }
    }
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation and Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource,
     RecompiledDarwinDeclarationsPreserveIdentitySynchronizationAndScalars) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained ? "default fixups" : "classic fixups");
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::DarwinDeclarations));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation and Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, RecompiledFrameworkFetchesPreserveContextObjects) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained ? "default fixups" : "classic fixups");
    ASSERT_NO_FATAL_FAILURE(verifyRuntime(Chained, RuntimeFixture::CoreData));
  }
#else
  GTEST_SKIP() << "Requires macOS CoreData and Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, RecompiledImmutableScalarsPreserveFloatingBitPatterns) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained);
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::ScalarConstants));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation";
#endif
}

TEST(ObjCRuntimeSource, RecompiledGraphicsCallsPreserveValuesAndImageIdentity) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained);
    ASSERT_NO_FATAL_FAILURE(verifyRuntime(Chained, RuntimeFixture::Graphics));
  }
#else
  GTEST_SKIP() << "Requires macOS CoreGraphics and ImageIO";
#endif
}

TEST(ObjCRuntimeSource, RecompiledSwiftLiteralsPreserveUTF8AndObjectLifetimes) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained);
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::SwiftLiterals));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation and Swift compiler";
#endif
}

TEST(ObjCRuntimeSource, RecompiledStoredStringsPreserveCollectionsAndIdentity) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained);
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::StoredStrings));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation";
#endif
}
