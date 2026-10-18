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
#include "neverd/emulation/ProcessSession.h"

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
TEST_P(AndroidJNI, ModifiedUTF8UTF16AndRangeException) {
  auto R = run("jni_string_values");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
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
