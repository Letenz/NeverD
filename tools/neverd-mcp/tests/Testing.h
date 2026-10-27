//===- Testing.h - neverd-mcp test registry ---------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Cases register themselves; tests/Main.cpp runs them all.
///
//===----------------------------------------------------------------------===//

#pragma once

#include <stdexcept>
#include <string>
#include <vector>

namespace neverd::binmcp::test {
struct Case {
  const char *Name;
  void (*Run)();
};

inline std::vector<Case> &cases() {
  static std::vector<Case> All;
  return All;
}

struct Registration {
  Registration(const char *Name, void (*Run)()) {
    cases().push_back({Name, Run});
  }
};

inline void require(bool Condition, const std::string &Message) {
  if (!Condition)
    throw std::runtime_error(Message);
}
} // namespace neverd::binmcp::test

#define MCP_TEST(Name)                                                         \
  static void Name();                                                          \
  static const ::neverd::binmcp::test::Registration Name##Case(#Name, Name);   \
  static void Name()
