//===- ExecutionCAPIDisabledTests.cpp - Feature-disabled C boundary
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Feature-disabled C boundary.
///

#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPIExecution.h"

#include <cstdlib>
#include <string>

namespace {
// This isolated target compiles the production boundary and allocator without
// linking the SDK or any emulation archive. Its owned diagnostics use malloc.
TEST(ExecutionCAPIDisabled, ExportsDiagnoseDisabledExecutionWithoutCpuSymbols) {
  const char *Error = nullptr;
  auto Check = [&] {
    ASSERT_NE(Error, nullptr);
    EXPECT_NE(std::string(Error).find("disabled"), std::string::npos);
    std::free(const_cast<char *>(Error));
    Error = nullptr;
  };
  neverd_execution_options_v1 O{sizeof(O), 7, "{}"};
  EXPECT_EQ(neverd_execution_create_v1(&O, &Error), 0u);
  Check();
  EXPECT_EQ(neverd_execution_destroy(7, &Error), 0);
  Check();
  EXPECT_EQ(neverd_execution_destroy(0, &Error), 1);
  EXPECT_EQ(Error, nullptr);
  for (auto Report :
       {neverd_execution_state_json, neverd_execution_run_json,
        neverd_execution_inspect_json, neverd_execution_instances_json}) {
    EXPECT_EQ(Report(7, &Error), nullptr);
    Check();
  }
  for (auto Action :
       {neverd_execution_request_pause, neverd_execution_resume}) {
    EXPECT_EQ(Action(7, &Error), 0);
    Check();
  }
  for (auto Input :
       {neverd_execution_declare_binding_json, neverd_execution_supply_json,
        neverd_execution_invoke_json}) {
    EXPECT_EQ(Input(7, "{}", &Error), 0);
    Check();
  }
  uint8_t Byte = 77;
  EXPECT_EQ(neverd_execution_read_memory(7, 0, &Byte, 1, &Error), 0);
  Check();
  EXPECT_EQ(Byte, 77);
  EXPECT_EQ(neverd_execution_write_memory(7, 0, &Byte, 1, &Error), 0);
  Check();
  EXPECT_EQ(neverd_execution_lookup_symbol_json(7, "i", "f", &Error), nullptr);
  Check();
  EXPECT_EQ(neverd_execution_command_json(7, "{}", &Error), nullptr);
  Check();
}
TEST(ExecutionCAPIDisabled, ExactVersionPrefixIsStillRequired) {
  const char *Error = nullptr;
  struct Prefix {
    size_t Size;
  } P{sizeof(P)};
  EXPECT_EQ(neverd_execution_create_v1(
                reinterpret_cast<neverd_execution_options_v1 *>(&P), &Error),
            0u);
  ASSERT_NE(Error, nullptr);
  EXPECT_NE(std::string(Error).find("exact v1"), std::string::npos);
  std::free(const_cast<char *>(Error));
}
} // namespace
