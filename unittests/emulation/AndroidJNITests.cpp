//===- AndroidJNITests.cpp - JNI values, lifetime and thread boundaries ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original guest callers across relocation packing and CPU transports.
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessReport.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/MemoryBuffer.h"

#include <tuple>

namespace neverd::emulation {
namespace {
class AndroidJNI : public testing::TestWithParam<
                       std::tuple<const char *, ExecutionBackendKind>> {
protected:
  std::filesystem::path Path;
  ProcessOptions Options;
  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Android fixture compiler unavailable";
#else
    Path = std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
           (std::string(std::get<0>(GetParam())) + ".so");
    Options.Backend = std::get<1>(GetParam());
    ExecutionConfiguration C;
    C.Backend = Options.Backend;
    C.Architecture = GuestArchitecture::AArch64;
    C.Contract = ExecutionContract::CheckedUserAArch64;
    auto P = probeExecutionBackend(C);
    ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
    if (P->Availability != BackendAvailability::Available)
      GTEST_SKIP() << P->Reason;
    Options.Android.emplace();
    Options.Android->Initialize = false;
    Options.Android->JNI = true;
    Options.Android->ThreadLimit = 2;
    Options.InstructionQuantum = 83;
    Options.Limits.Instructions = 200000;
    Options.Limits.Events = 5000;
    Options.Limits.TimeoutMicroseconds = 30000000;
#endif
  }
  ProcessResult run(const char *Entry) {
    Options.Android->EntrySymbol = Entry;
    return llvm::cantFail(
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options));
  }
};
TEST_P(AndroidJNI, VMThreadAttachmentAndVersions) {
  auto R = run("jni_vm_lifecycle");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}
TEST_P(AndroidJNI,
       LoaderFailuresRetainExceptionsAcrossDetachWithoutClosingNamespaces) {
  using Snapshot = mobile::dalvik::AndroidApplicationSnapshot;
  using Loader = Snapshot::ClassLookupFailure::Loader;
  Options.Android->Application = Snapshot{Snapshot::Phase::Attached};
  Options.Android->Application->class_lookup_failures = {
      {Loader::Boot, "fixture.Optional", "declared missing"}};
  auto R = run("jni_loader_failure");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
  for (unsigned Mode : {1u, 2u}) {
    Options.Android->Arguments = {Mode};
    auto Unknown = run("jni_loader_failure");
    EXPECT_EQ(Unknown.Stop, Mode == 1 ? ProcessStopReason::UnsupportedService
                                      : ProcessStopReason::RuntimeFailure)
        << Unknown.Diagnostic;
    if (Mode == 2)
      EXPECT_NE(Unknown.Diagnostic.find("unmodeled class: fixture/Optional"),
                std::string::npos);
  }
  Options.Android->Arguments.clear();
  Options.Android->Application->class_lookup_failures.clear();
  auto Unknown = run("jni_loader_failure");
  EXPECT_EQ(Unknown.Stop, ProcessStopReason::UnsupportedService);
}
TEST_P(AndroidJNI, InstrumentationFieldAndGetterShareLifetimeAndWrites) {
  using Snapshot = mobile::dalvik::AndroidApplicationSnapshot;
  for (unsigned Mode : {1u, 2u, 3u}) {
    Options.Android->Application = Snapshot{Snapshot::Phase::Attached};
    if (Mode != 3)
      Options.Android->Application->instrumentation =
          Mode == 1 ? Snapshot::Instrumentation::Uninitialized
                    : Snapshot::Instrumentation::Platform;
    Options.Android->Arguments = {Mode};
    auto R = run("jni_instrumentation");
    EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
  }
  Options.Android->Application = Snapshot{Snapshot::Phase::Attached};
  Options.Android->Arguments = {0};
  auto Unknown = run("jni_instrumentation");
  EXPECT_EQ(Unknown.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Unknown.Diagnostic.find("instrumentation state"),
            std::string::npos);
}
TEST_P(AndroidJNI, ApplicationSnapshotHasThreeDistinctLifecyclePhases) {
  using Phase = mobile::dalvik::AndroidApplicationSnapshot::Phase;
  for (auto P : {Phase::Unattached, Phase::Attached, Phase::Application}) {
    Options.Android->Application =
        mobile::dalvik::AndroidApplicationSnapshot{P, {}};
    Options.Android->Arguments = {uint64_t(P)};
    auto R = run("jni_application_snapshot");
    EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
  }
}
TEST_P(AndroidJNI, JavaStateObservationPreservesCompleteExecution) {
  using Phase = mobile::dalvik::AndroidApplicationSnapshot::Phase;
  Options.Android->TraceLimit = 50000;
  for (auto P :
       {std::optional<Phase>{}, std::optional{Phase::Unattached},
        std::optional{Phase::Attached}, std::optional{Phase::Application}}) {
    Options.Android->Application =
        P ? std::optional{mobile::dalvik::AndroidApplicationSnapshot{*P, {}}}
          : std::nullopt;
    Options.Android->Arguments = {uint64_t(P.value_or(Phase::Unattached))};
    Options.Android->ObserveJavaState = false;
    auto Baseline = run("jni_application_snapshot");
    ASSERT_FALSE(Baseline.JavaState);
    Options.Android->ObserveJavaState = true;
    auto Observed = run("jni_application_snapshot");
    ASSERT_TRUE(Observed.JavaState);
    const auto &State = *Observed.JavaState;
    EXPECT_EQ(State.framework.declared_phase, P);
    EXPECT_FALSE(State.classes.empty());
    if (P == Phase::Application) {
      EXPECT_NE(State.framework.application, 0u);
      EXPECT_NE(State.framework.application_loader, 0u);
      EXPECT_NE(State.framework.boot_loader,
                State.framework.application_loader);
    } else {
      EXPECT_EQ(State.framework.application, 0u);
      EXPECT_EQ(State.framework.application_loader, 0u);
    }
    EXPECT_EQ(Observed.Stop, P ? ProcessStopReason::Returned
                               : ProcessStopReason::UnsupportedService);
    // Covers counters, references/return bits, threads, calls, trace, memory,
    // diagnostics and existing observations, not just the final result code.
    Observed.JavaState.reset();
    EXPECT_EQ(processResultJSON(Observed), processResultJSON(Baseline));
  }
  Options.Android->JNI = false;
  Options.Android->Application.reset();
  auto Bad =
      emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  ASSERT_FALSE(bool(Bad));
  EXPECT_NE(llvm::toString(Bad.takeError()).find("observation requires JNI"),
            std::string::npos);
}
TEST_P(AndroidJNI,
       JavaStateSeparatesNativeRegistrationFromClassInitialization) {
#ifndef NEVERD_ANDROID_DEX_FIXTURE_DIR
  GTEST_SKIP() << "Python DEX fixture generator unavailable";
#else
  auto File = llvm::MemoryBuffer::getFile(
      (std::filesystem::path(NEVERD_ANDROID_DEX_FIXTURE_DIR) /
       "registration.dex")
          .string());
  ASSERT_TRUE(bool(File));
  auto Bytes = (*File)->getBuffer();
  Options.Android->DexImages.emplace_back(Bytes.begin(), Bytes.end());
  Options.Android->TraceLimit = 50000;
  for (unsigned Mode : {0u, 5u}) {
    SCOPED_TRACE(Mode);
    Options.Android->Arguments = {Mode};
    Options.Android->ObserveJavaState = false;
    auto Before = run("jni_native_registration");
    Options.Android->ObserveJavaState = true;
    auto After = run("jni_native_registration");
    ASSERT_EQ(After.Stop, ProcessStopReason::Returned) << After.Diagnostic;
    ASSERT_TRUE(After.JavaState);
    EXPECT_FALSE(After.JavaState->terminal);
    EXPECT_FALSE(After.JNIRegistrations.empty());
    unsigned Supplied = 0, Materialized = 0;
    for (const auto &C : After.JavaState->classes) {
      if (!C.supplied)
        continue;
      ++Supplied;
      Materialized += C.class_object != 0;
      EXPECT_EQ(C.source_id, "supplied-dex-0");
      using Init = mobile::dalvik::JavaClassState::Initialization;
      EXPECT_EQ(C.initialization,
                Mode == 5 && C.descriptor == "Lregistration/Deferred;"
                    ? Init::Erroneous
                    : Init::NotStarted);
      EXPECT_FALSE(C.projected_loader); // No application snapshot was supplied.
    }
    EXPECT_EQ(Supplied, 3u);
    EXPECT_EQ(Materialized, 3u);
    After.JavaState.reset();
    EXPECT_EQ(processResultJSON(After), processResultJSON(Before));
  }
#endif
}
TEST_P(AndroidJNI, ApplicationUnknownStateAndPackageDoNotBecomeNullOrDefaults) {
  auto Unknown = run("jni_application_snapshot");
  EXPECT_EQ(Unknown.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Unknown.Diagnostic.find("lifecycle snapshot"), std::string::npos);
  Options.Android->Application = mobile::dalvik::AndroidApplicationSnapshot{
      mobile::dalvik::AndroidApplicationSnapshot::Phase::Application, {}};
  auto Package = run("jni_application_package");
  EXPECT_EQ(Package.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Package.Diagnostic.find("package name"), std::string::npos);
  Options.Android->Application->package_name = "test.app";
  auto Known = run("jni_application_package");
  EXPECT_EQ(Known.Stop, ProcessStopReason::Returned) << Known.Diagnostic;
  EXPECT_EQ(Known.ReturnValue, 0u);
}
TEST_P(AndroidJNI, ApplicationLoaderIsIndependentOfApplicationCreation) {
#ifndef NEVERD_ANDROID_DEX_FIXTURE_DIR
  GTEST_SKIP() << "Python DEX fixture generator unavailable";
#else
  auto File = llvm::MemoryBuffer::getFile(
      (std::filesystem::path(NEVERD_ANDROID_DEX_FIXTURE_DIR) /
       "registration.dex")
          .string());
  ASSERT_TRUE(bool(File));
  auto Bytes = (*File)->getBuffer();
  Options.Android->DexImages.emplace_back(Bytes.begin(), Bytes.end());
  using Snapshot = mobile::dalvik::AndroidApplicationSnapshot;
  Options.Android->Application = Snapshot{Snapshot::Phase::Attached};
  Options.Android->ObserveJavaState = true;
  for (auto Established :
       {std::optional<bool>{}, std::optional{false}, std::optional{true}}) {
    Options.Android->Application->application_loader_established = Established;
    auto R = run("jni_loader_before_application");
    ASSERT_TRUE(R.JavaState);
    EXPECT_EQ(R.JavaState->framework.application_loader_established,
              Established);
    EXPECT_EQ(R.JavaState->framework.application, 0u);
    EXPECT_EQ(R.Stop, Established == true
                          ? ProcessStopReason::Returned
                          : ProcessStopReason::UnsupportedService)
        << R.Diagnostic;
    if (Established == true) {
      EXPECT_EQ(R.ReturnValue, 0u);
      EXPECT_NE(R.JavaState->framework.application_loader, 0u);
      EXPECT_NE(R.JavaState->framework.boot_loader, 0u);
    }
  }
#endif
}
TEST_P(AndroidJNI, ApplicationInputRequiresJNIAndValidPhaseMetadata) {
  Options.Android->EntrySymbol = "jni_application_snapshot";
  Options.Android->Application.emplace();
  Options.Android->JNI = false;
  auto Rejected =
      emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  EXPECT_FALSE(bool(Rejected));
  EXPECT_NE(llvm::toString(Rejected.takeError()).find("requires JNI"),
            std::string::npos);
  Options.Android->JNI = true;
  Options.Android->Application->package_name = "test.app";
  auto Invalid =
      emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  EXPECT_FALSE(bool(Invalid));
  EXPECT_NE(llvm::toString(Invalid.takeError()).find("package snapshot"),
            std::string::npos);
}
TEST_P(AndroidJNI,
       ApplicationFilesUseDeclaredDirectoryAndRetainedFileIdentity) {
  Options.Android->Application = mobile::dalvik::AndroidApplicationSnapshot{
      mobile::dalvik::AndroidApplicationSnapshot::Phase::Application,
      {},
      "/fixture/files"};
  Options.LinuxFiles.emplace();
  Options.LinuxFiles->Directories = {"/fixture/files"};
  auto R = run("jni_application_files");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
  Options.LinuxFiles->Directories.clear();
  auto Invalid =
      emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  EXPECT_FALSE(bool(Invalid));
  EXPECT_NE(
      llvm::toString(Invalid.takeError()).find("declared Linux directory"),
      std::string::npos);
  Options.Android->Application->files_directory.reset();
  auto Unknown = run("jni_application_files");
  EXPECT_EQ(Unknown.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Unknown.Diagnostic.find("files directory is not supplied"),
            std::string::npos);
}
TEST_P(AndroidJNI, BuildVersionFieldsUseTheNativePropertySnapshot) {
  Options.Android->Properties = {{"ro.build.version.release", "9.2"},
                                 {"ro.build.version.incremental", "9.2"},
                                 {"ro.build.version.sdk", "0x1c"}};
  Options.Android->Arguments = {28, 1};
  auto R = run("jni_build_version");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}
TEST_P(AndroidJNI, BuildVersionMissingAndEmptyPropertiesKeepPlatformDefaults) {
  Options.Android->Properties = {{"ro.build.version.release", ""}};
  Options.Android->Arguments = {0, 0};
  auto R = run("jni_build_version");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}
TEST_P(AndroidJNI, BuildVersionRejectsWrongTypesWritesAndUnknownFields) {
  for (const char *Entry :
       {"jni_build_version_wrong_accessor", "jni_build_version_write",
        "jni_build_version_unknown_field"}) {
    auto R = run(Entry);
    EXPECT_EQ(R.Stop, std::string_view(Entry).ends_with("unknown_field")
                          ? ProcessStopReason::UnsupportedService
                          : ProcessStopReason::RuntimeFailure)
        << Entry << R.Diagnostic;
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_FALSE(R.NativeCalls.back().Result.has_value());
  }
}
TEST_P(AndroidJNI, DexClassCapacityIsSeparateAndAggregate) {
#ifndef NEVERD_ANDROID_DEX_FIXTURE_DIR
  GTEST_SKIP() << "Python DEX fixture generator unavailable";
#else
  Options.MemoryLimit = 128 * 1024 * 1024;
  auto image = [](const char *Name) {
    auto File = llvm::MemoryBuffer::getFile(
        (std::filesystem::path(NEVERD_ANDROID_DEX_FIXTURE_DIR) / Name)
            .string());
    if (!File) {
      ADD_FAILURE() << File.getError().message();
      return std::vector<uint8_t>{};
    }
    auto Bytes = (*File)->getBuffer();
    return std::vector<uint8_t>(Bytes.begin(), Bytes.end());
  };
  auto Small = image("classes-17-0.dex");
  auto Half = image("classes-2048-0.dex");
  auto OtherHalf = image("classes-2048-1.dex");
  auto Full = image("classes-4096-0.dex");
  auto invoke = [&](std::vector<std::vector<uint8_t>> Images, const char *Last,
                    const char *Error = nullptr) {
    Options.Android->DexImages = std::move(Images);
    Options.Android->EntrySymbol = "jni_dex_class_pair";
    NativeMemoryRegion Region;
    Region.Address = 0x20000000;
    Region.Size = 4096;
    Region.Bytes.resize(128);
    const std::string First = "bench/d000/C000000";
    std::copy(First.begin(), First.end(), Region.Bytes.begin());
    const std::string LastName = Last;
    std::copy(LastName.begin(), LastName.end(), Region.Bytes.begin() + 64);
    Options.Android->Memory = {std::move(Region)};
    Options.Android->Arguments = {0x20000000, 0x20000040};
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    if (Error) {
      ASSERT_FALSE(bool(R));
      EXPECT_NE(llvm::toString(R.takeError()).find(Error), std::string::npos);
      return;
    }
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::Returned) << R->Diagnostic;
    EXPECT_EQ(R->ReturnValue, 0u);
  };
  invoke({Small}, "bench/d000/C000016");
  invoke({Full}, "other/d000/C004095");
  invoke({Half, OtherHalf}, "other/d001/C002047");
  invoke({image("classes-4097-0.dex")}, "", "class inventory");
  invoke({Half, image("classes-2049-1.dex")}, "", "class inventory");
  invoke({Full, OtherHalf}, "", "class inventory");
  invoke({Small, Small}, "", "duplicate supplied DEX class");
  invoke(std::vector<std::vector<uint8_t>>(17, Small), "", "at most 16");
  Small.back() ^= 1;
  invoke({Small}, "", "Invalid DEX");
#endif
}
TEST_P(AndroidJNI, ExplicitClassLookupFailuresPreserveExceptionsAndUnknowns) {
  for (const std::string Type :
       {"java/lang/ClassNotFoundException", "java/lang/NoClassDefFoundError"}) {
    Options.Android->JNIClassLookupFailures = {{"fixture/Absent", Type}};
    NativeMemoryRegion Region;
    Region.Address = 0x20000000;
    Region.Size = 4096;
    Region.Bytes.assign(Type.begin(), Type.end());
    Region.Bytes.push_back(0);
    Options.Android->Memory = {Region};
    Options.Android->Arguments = {Region.Address};
    auto R = run("jni_explicit_lookup_failure");
    EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
  }
  Options.Android->JNIClassLookupFailures.clear();
  auto Unknown = run("jni_explicit_lookup_failure");
  EXPECT_EQ(Unknown.Stop, ProcessStopReason::RuntimeFailure);
  EXPECT_NE(Unknown.Diagnostic.find("unmodeled class: fixture/Absent"),
            std::string::npos);
}
TEST_P(AndroidJNI, RejectsContradictoryClassLoaderDeclarations) {
  for (const std::string Name : std::vector<std::string>{
           "", "java/lang/String", "android/os/Build$VERSION", "bad.name",
           "[Lfixture/Absent;", "/bad", "bad/", "bad//Name",
           std::string("bad\0Name", 8)}) {
    Options.Android->JNIClassLookupFailures = {
        {Name, "java/lang/ClassNotFoundException"}};
    Options.Android->EntrySymbol = "jni_vm_lifecycle";
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    ASSERT_FALSE(bool(R)) << Name;
    llvm::consumeError(R.takeError());
  }
  Options.Android->JNIClassLookupFailures = {
      {"fixture/Absent", "java/lang/Error"}};
  auto R = emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  ASSERT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  Options.Android->JNIClassLookupFailures = {
      {"fixture/Absent", "java/lang/ClassNotFoundException"}};
  Options.Android->JNI = false;
  R = emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("require JNI"),
            std::string::npos);
}
TEST_P(AndroidJNI,
       NativeRegistrationsRetainPartialRebindingsAndDeclaringOwners) {
#ifndef NEVERD_ANDROID_DEX_FIXTURE_DIR
  GTEST_SKIP() << "Python DEX fixture generator unavailable";
#else
  auto File = llvm::MemoryBuffer::getFile(
      (std::filesystem::path(NEVERD_ANDROID_DEX_FIXTURE_DIR) /
       "registration.dex")
          .string());
  ASSERT_TRUE(bool(File));
  auto Bytes = (*File)->getBuffer();
  Options.Android->DexImages.emplace_back(Bytes.begin(), Bytes.end());
  for (unsigned Mode = 0; Mode != 12; ++Mode) {
    SCOPED_TRACE(Mode);
    Options.Android->Arguments = {Mode};
    auto R = run("jni_native_registration");
    EXPECT_EQ(R.Stop, Mode < 6 ? ProcessStopReason::Returned
                               : ProcessStopReason::RuntimeFailure)
        << R.Diagnostic;
    if (Mode < 6)
      EXPECT_EQ(R.ReturnValue, 0u);
    std::map<std::string, uint64_t> Bindings;
    for (const auto &B : R.JNIRegistrations)
      ASSERT_TRUE(
          Bindings.emplace(B.Class + "." + B.Name + B.Signature, B.Address)
              .second);
    std::map<std::string, uint64_t> Expected = {
        {"registration/Child.child()V", 0x12340008},
        {"registration/Deferred.after()V", 0x1234000c},
        {"registration/Parent.alpha(I)I", 0x12340000},
        {"registration/Parent.beta(J)J", 0x12340004}};
    if (Mode == 1)
      Expected.erase("registration/Child.child()V");
    if (Mode == 2) {
      Expected.erase("registration/Parent.alpha(I)I");
      Expected.erase("registration/Parent.beta(J)J");
    }
    if (Mode == 3 || Mode == 4)
      Expected["registration/Parent.alpha(I)I"] =
          Mode == 3 ? 0x12340010 : 0x12340014;
    if (Mode == 9 || Mode == 10)
      Expected["registration/Parent.alpha(I)I"] = 0x1234001c;
    EXPECT_EQ(Bindings, Expected);
  }
  Options.Android->JNIClassLookupFailures = {
      {"registration/Child", "java/lang/ClassNotFoundException"}};
  auto R = emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("shadows a known class"),
            std::string::npos);
#endif
}
TEST_P(AndroidJNI, ModifiedUTF8UTF16AndRangeException) {
  auto R = run("jni_string_values");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}
TEST_P(AndroidJNI,
       RegisteredNativeCallsExecuteGuestBodiesAndRetireOnlyTheirLocals) {
#ifndef NEVERD_ANDROID_DEX_FIXTURE_DIR
  GTEST_SKIP() << "Python DEX fixture generator unavailable";
#else
  auto File = llvm::MemoryBuffer::getFile(
      (std::filesystem::path(NEVERD_ANDROID_DEX_FIXTURE_DIR) /
       "native-calls.dex")
          .string());
  ASSERT_TRUE(bool(File));
  auto Bytes = (*File)->getBuffer();
  Options.Android->DexImages.emplace_back(Bytes.begin(), Bytes.end());
  Options.Android->ObserveJavaState = true;
  Options.LinuxTime.emplace().AdvanceOnIdle = true;
  Options.LinuxTime->Clocks = {{1, {0, 0}}};
  for (unsigned Mode : {0u, 18u, 21u, 22u, 23u, 24u}) {
    SCOPED_TRACE(Mode);
    Options.Android->Arguments = {Mode};
    auto R = run("jni_native_calls");
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
    ASSERT_TRUE(R.JavaState);
    for (const auto &C : R.JavaState->classes)
      if (C.descriptor == "Lnativecall/Base;")
        EXPECT_EQ(C.initialization,
                  mobile::dalvik::JavaClassState::Initialization::Initialized);
      else if (C.descriptor == "Lnativecall/Bad;")
        EXPECT_EQ(
            C.initialization,
            Mode == 18
                ? mobile::dalvik::JavaClassState::Initialization::Erroneous
                : mobile::dalvik::JavaClassState::Initialization::NotStarted);
    EXPECT_FALSE(R.JavaState->terminal);
  }
#endif
}
TEST_P(AndroidJNI, RegisteredNativeCallsRejectUnknownBindingsAndInvalidABIs) {
#ifndef NEVERD_ANDROID_DEX_FIXTURE_DIR
  GTEST_SKIP() << "Python DEX fixture generator unavailable";
#else
  auto File = llvm::MemoryBuffer::getFile(
      (std::filesystem::path(NEVERD_ANDROID_DEX_FIXTURE_DIR) /
       "native-calls.dex")
          .string());
  ASSERT_TRUE(bool(File));
  auto Bytes = (*File)->getBuffer();
  Options.Android->DexImages.emplace_back(Bytes.begin(), Bytes.end());
  for (unsigned Mode = 1; Mode <= 17; ++Mode) {
    SCOPED_TRACE(Mode);
    Options.Android->Arguments = {Mode};
    auto R = run("jni_native_calls");
    auto Expected =
        Mode == 1 || Mode == 2 || (Mode >= 10 && Mode <= 12) || Mode == 15
            ? ProcessStopReason::UnsupportedService
        : Mode == 16 ? ProcessStopReason::InstructionLimit
                     : ProcessStopReason::RuntimeFailure;
    EXPECT_EQ(R.Stop, Expected) << R.Diagnostic;
    EXPECT_FALSE(R.ReturnValue);
    EXPECT_FALSE(R.Diagnostic.empty());
    if (Mode == 1 || Mode == 2 || Mode == 15) {
      ASSERT_FALSE(R.NativeCalls.empty());
      EXPECT_EQ(R.NativeCalls.back().Name, "JNIEnv.129");
    }
  }
#endif
}
TEST_P(AndroidJNI, ARTStringEdgesPreserveSupplementaryCharactersAndNulls) {
  auto R = run("jni_art_string_edges");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}
TEST_P(AndroidJNI, ArrayConstructionAbortsWithoutRecoverableException) {
  for (const char *Entry : {"jni_negative_array", "jni_negative_object_array",
                            "jni_bad_array_initializer"}) {
    auto R = run(Entry);
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure)
        << Entry << R.Diagnostic;
    EXPECT_NE(R.Diagnostic.find("JNI abort:"), std::string::npos)
        << Entry << R.Diagnostic;
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_FALSE(R.NativeCalls.back().Result.has_value());
  }
}
TEST_P(AndroidJNI, ArrayValuesCommitAbortAndBounds) {
  auto R = run("jni_arrays");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}
TEST_P(AndroidJNI, LocalFramesAndGlobalReferencesSurviveDetach) {
  auto R = run("jni_references");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}
TEST_P(AndroidJNI, EveryPrimitiveWidthPreservesCompleteValueBits) {
  auto R = run("jni_primitive_bits");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}
TEST_P(AndroidJNI, CooperativeThreadsUseDistinctEnvironments) {
  auto R = run("jni_threads");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
  ASSERT_EQ(R.NativeThreads.size(), 2u);
  EXPECT_TRUE(R.NativeThreads[1].Finished);
}
TEST_P(AndroidJNI, InvalidIdentitiesAndBorrowsFailClearly) {
  for (const auto *Entry :
       {"jni_stale_env", "jni_stale_ref", "jni_wrong_thread",
        "jni_invalid_utf8", "jni_double_release", "jni_frame_underflow",
        "jni_detach_borrow", "jni_wrong_borrow_object",
        "jni_wrong_reference_kind", "jni_pending_illegal", "jni_table_replaced",
        "jni_named_env_spoof", "jni_named_vm_spoof"}) {
    auto R = run(Entry);
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure)
        << Entry << R.Diagnostic;
    EXPECT_NE(R.Diagnostic.find("JNI"), std::string::npos) << R.Diagnostic;
  }
}
TEST_P(AndroidJNI, PendingExceptionAllowsCleanupWithoutLosingValues) {
  auto R = run("jni_pending_cleanup");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}
TEST_P(AndroidJNI, InvalidBorrowCopyoutCannotSucceed) {
  auto R = run("jni_invalid_copyout");
  EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
}
TEST_P(AndroidJNI, JavaMethodExecutionIsNotInvented) {
  auto R = run("jni_unknown_method");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
  EXPECT_NE(R.Diagnostic.find("JNIEnv.33"), std::string::npos);
}
TEST_P(AndroidJNI, IntrinsicJavaMethodsExecuteArgumentsAndPreserveIdentity) {
  auto R = run("jni_method_values");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}
TEST_P(AndroidJNI, StringInternIdentitySurvivesReferenceAndThreadTeardown) {
  auto R = run("jni_string_intern_identity");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}
TEST_P(AndroidJNI, MethodExceptionsPreserveArrayAndExceptionPrecedence) {
  auto R = run("jni_method_exceptions");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}
TEST_P(AndroidJNI, InvalidMethodInvocationCannotSupplyAResult) {
  for (const char *Entry :
       {"jni_method_wrong_return", "jni_method_wrong_receiver",
        "jni_method_forged_id", "jni_method_unreadable_arguments"}) {
    auto R = run(Entry);
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure)
        << Entry << R.Diagnostic;
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_FALSE(R.NativeCalls.back().Result.has_value());
  }
}
TEST_P(AndroidJNI, EnvironmentIsExplicitlyEnabled) {
  Options.Android->JNI = false;
  auto R = run("jni_vm_lifecycle");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
}
INSTANTIATE_TEST_SUITE_P(
    Backends, AndroidJNI,
    testing::Combine(testing::Values("jni-O0-none", "jni-O2-none",
                                     "jni-O0-android", "jni-O2-android",
                                     "jni-O0-relr", "jni-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::HVF,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP)));
} // namespace
} // namespace neverd::emulation
