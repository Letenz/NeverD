//===- ExecutionSDKDisabledTests.cpp - Disabled execution in the real SDK
//---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Disabled execution in the real SDK.
///

#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"

#include <string>

extern "C" int neverd_test_module_c_header(void);

namespace {
TEST(ExecutionSDKDisabled, EveryExecutionExportKeepsItsDisabledDiagnostic) {
  const char *Error = nullptr;
  auto Check = [&] {
    ASSERT_NE(Error, nullptr);
    EXPECT_NE(std::string(Error).find("disabled"), std::string::npos);
    neverd_free_string(Error);
    Error = nullptr;
  };
  auto Sources = neverd_module_set_create_v1(nullptr, &Error);
  ASSERT_NE(Sources, 0u);
  ASSERT_EQ(Error, nullptr);
  neverd_execution_options_v1 Options{sizeof(Options), Sources, "{}"};
  EXPECT_EQ(neverd_execution_create_v1(&Options, &Error), 0u);
  Check();
  EXPECT_EQ(neverd_execution_destroy(Sources, &Error), 0);
  Check();
  EXPECT_EQ(neverd_execution_destroy(0, &Error), 1);
  EXPECT_EQ(Error, nullptr);
  for (auto Report :
       {neverd_execution_state_json, neverd_execution_run_json,
        neverd_execution_inspect_json, neverd_execution_instances_json}) {
    EXPECT_EQ(Report(Sources, &Error), nullptr);
    Check();
  }
  for (auto Action :
       {neverd_execution_request_pause, neverd_execution_resume}) {
    EXPECT_EQ(Action(Sources, &Error), 0);
    Check();
  }
  for (auto Input :
       {neverd_execution_declare_binding_json, neverd_execution_supply_json,
        neverd_execution_invoke_json}) {
    EXPECT_EQ(Input(Sources, "{}", &Error), 0);
    Check();
  }
  uint8_t Byte = 77;
  EXPECT_EQ(neverd_execution_read_memory(Sources, 0, &Byte, 1, &Error), 0);
  Check();
  EXPECT_EQ(Byte, 77);
  EXPECT_EQ(neverd_execution_write_memory(Sources, 0, &Byte, 1, &Error), 0);
  Check();
  EXPECT_EQ(neverd_execution_lookup_symbol_json(Sources, "i", "f", &Error),
            nullptr);
  Check();
  EXPECT_EQ(neverd_execution_command_json(Sources, "{}", &Error), nullptr);
  Check();

  // Failed execution operations must not consume the independent source.
  const char *Report = neverd_module_set_inspect_json(Sources, &Error);
  ASSERT_NE(Report, nullptr);
  EXPECT_EQ(Error, nullptr);
  neverd_free_string(Report);
  EXPECT_EQ(neverd_module_set_prepare_eager(Sources, &Error), 1);
  EXPECT_EQ(Error, nullptr);
  EXPECT_EQ(neverd_module_set_destroy(Sources, &Error), 1);
  EXPECT_EQ(Error, nullptr);
}

TEST(ExecutionSDKDisabled, ExactVersionValidationPrecedesDisabledExecution) {
  const char *Error = nullptr;
  struct Prefix {
    size_t Size;
  } P{sizeof(P)};
  EXPECT_EQ(neverd_execution_create_v1(
                reinterpret_cast<neverd_execution_options_v1 *>(&P), &Error),
            0u);
  ASSERT_NE(Error, nullptr);
  EXPECT_NE(std::string(Error).find("exact v1"), std::string::npos);
  neverd_free_string(Error);
}

TEST(ExecutionSDKDisabled, PureCModuleConsumerRemainsAvailable) {
  EXPECT_EQ(neverd_test_module_c_header(), 0);
}
} // namespace
