//===- ExecutionReportTests.cpp - CPU query semantics --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionReport.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd::emulation {
namespace {
namespace field = execution_report;
#define NEVERD_CONFIGURATION_TEXT(Name, Text) constexpr char Name[] = Text;
#include "ExecutionConfigurationCases.def"
#undef NEVERD_CONFIGURATION_TEXT

TEST(ExecutionReport, RejectsMalformedAndUnsupportedConfigurationBeforeProbe) {
#define NEVERD_CONFIGURATION_JSON_INVALID(Name, Text)                          \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    auto Config = executionConfigurationFromJSON(Text);                        \
    ASSERT_FALSE(bool(Config));                                                \
    auto Error = Config.takeError();                                           \
    EXPECT_FALSE(Error.isA<BackendUnavailableError>());                        \
    llvm::consumeError(std::move(Error));                                      \
  }
#include "ExecutionConfigurationCases.def"
#undef NEVERD_CONFIGURATION_JSON_INVALID
  auto Large =
      executionConfigurationFromJSON(std::string(field::JSONLimit + 1, ' '));
  ASSERT_FALSE(bool(Large));
  EXPECT_EQ(llvm::toString(Large.takeError()), field::ConfigurationTooLarge);
}

TEST(ExecutionReport, UserProfilesReportIsolationAndServiceRequests) {
  for (const auto *Input : {UserX64Configuration, UserARMConfiguration}) {
    const auto Config = llvm::cantFail(executionConfigurationFromJSON(Input));
    const auto Resolved = llvm::cantFail(resolveExecutionConfiguration(Config));
    EXPECT_EQ(Resolved.Capabilities.Privilege, ExecutionPrivilege::User);
    EXPECT_TRUE(Resolved.Capabilities.supports(
        ExecutionFeature::UserSupervisorIsolation));
    EXPECT_TRUE(Resolved.Capabilities.supports(ExecutionFeature::ServiceTraps));
    auto Report = llvm::cantFail(
        llvm::json::parse(llvm::cantFail(executionCapabilitiesJSON(Config))));
    const auto *Caps = Report.getAsObject()->getObject(field::Capabilities);
    ASSERT_NE(Caps, nullptr);
    EXPECT_EQ(Caps->getString(field::Privilege),
              executionPrivilegeName(ExecutionPrivilege::User));
    EXPECT_TRUE(Report.getAsObject()->get(field::Host)->getAsNull());
  }
}

TEST(ExecutionReport, DefaultQueryDoesNotClaimLiveAvailability) {
  auto Config =
      llvm::cantFail(executionConfigurationFromJSON(field::EmptyConfiguration));
  auto Text = llvm::cantFail(executionCapabilitiesJSON(Config));
  auto Report = llvm::cantFail(llvm::json::parse(Text));
  auto *Root = Report.getAsObject();
  ASSERT_NE(Root, nullptr);
  EXPECT_EQ(Root->getInteger(field::SchemaVersion), field::Version);
  ASSERT_NE(Root->get(field::Host), nullptr);
  EXPECT_TRUE(Root->get(field::Host)->getAsNull());
  auto *Requested = Root->getObject(field::RequestedConfiguration);
  auto *Resolved = Root->getObject(field::Configuration);
  ASSERT_NE(Requested, nullptr);
  ASSERT_NE(Resolved, nullptr);
  EXPECT_EQ(Requested->getString(field::Backend), execution::Auto);
  EXPECT_FALSE(Requested->get(field::PageSize));
  EXPECT_EQ(Resolved->getString(field::Backend), execution::Unicorn);
  EXPECT_EQ(Resolved->getString(field::Contract), execution::Software);
  EXPECT_EQ(Resolved->getString(field::Privilege),
            executionPrivilegeName(ExecutionPrivilege::Flat));
  EXPECT_EQ(Resolved->getInteger(field::PageSize),
            llvm::cantFail(resolveExecutionConfiguration(Config))
                .Capabilities.PageSize);
  auto *Build = Root->getObject(field::Build);
  ASSERT_NE(Build, nullptr);
  EXPECT_EQ(
      Build->getString(field::Availability),
      backendAvailabilityName(llvm::cantFail(queryExecutionBackendBuild(
                                                 ExecutionBackendKind::Unicorn,
                                                 Config.Architecture))
                                  .Availability));
}

TEST(ExecutionReport, RoundTripPreservesRequirementsAndContractFacts) {
  auto Config =
      llvm::cantFail(executionConfigurationFromJSON(ARMConfiguration));
  auto Resolved = llvm::cantFail(resolveExecutionConfiguration(Config));
  auto Report = llvm::cantFail(
      llvm::json::parse(llvm::cantFail(executionCapabilitiesJSON(Config))));
  auto *Root = Report.getAsObject();
  ASSERT_NE(Root, nullptr);
  auto *Normalized = Root->getObject(field::Configuration);
  auto *Caps = Root->getObject(field::Capabilities);
  ASSERT_NE(Normalized, nullptr);
  ASSERT_NE(Caps, nullptr);
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  OS << llvm::json::Value(llvm::json::Object(*Normalized));
  auto RoundTrip = llvm::cantFail(executionConfigurationFromJSON(Text));
  EXPECT_EQ(RoundTrip.RequiredFeatures, Config.RequiredFeatures);
  EXPECT_EQ(RoundTrip.Privilege, Resolved.Configuration.Privilege);
  EXPECT_EQ(RoundTrip.Architecture, GuestArchitecture::AArch64);
  EXPECT_EQ(Caps->getString(field::AddressModel),
            executionAddressModelName(ExecutionAddressModel::Split));
  EXPECT_EQ(Caps->getBoolean(field::InstructionAllowlist), true);
  auto *Instructions = Caps->getArray(field::InstructionFamilies);
  ASSERT_NE(Instructions, nullptr);
  EXPECT_EQ(Instructions->size(),
            Resolved.Capabilities.InstructionFamilies.size());
  auto *Features = Caps->getArray(field::Features);
  ASSERT_NE(Features, nullptr);
  bool SIMD = false, FloatingPoint = false;
  for (const auto &Feature : *Features) {
    SIMD |=
        Feature.getAsString() == executionFeatureName(ExecutionFeature::SIMD);
    FloatingPoint |= Feature.getAsString() ==
                     executionFeatureName(ExecutionFeature::FloatingPoint);
    EXPECT_TRUE(Feature.getAsString().has_value());
  }
  EXPECT_TRUE(SIMD);
  EXPECT_TRUE(FloatingPoint);
}

TEST(ExecutionReport, ExplicitProbeReportsUnavailableWithoutChangingBackend) {
  ExecutionConfiguration Config;
  Config.Contract = ExecutionContract::CheckedX64;
#ifdef _WIN32
  Config.Backend = ExecutionBackendKind::KVM;
#else
  Config.Backend = ExecutionBackendKind::WHP;
#endif
  auto Report = llvm::cantFail(llvm::json::parse(
      llvm::cantFail(executionCapabilitiesJSON(Config, true))));
  auto *Root = Report.getAsObject();
  ASSERT_NE(Root, nullptr);
  auto *Host = Root->getObject(field::Host);
  auto *Normalized = Root->getObject(field::Configuration);
  ASSERT_NE(Host, nullptr);
  ASSERT_NE(Normalized, nullptr);
  EXPECT_EQ(Host->getString(field::Availability),
            backendAvailabilityName(BackendAvailability::HostPlatformMismatch));
  EXPECT_EQ(Host->getString(field::Scope), field::InitializationScope);
  EXPECT_EQ(Normalized->getString(field::Backend),
            executionBackendName(Config.Backend));
}
} // namespace
} // namespace neverd::emulation
