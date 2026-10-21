//===- Main.cpp - Run the neverd-mcp tests --------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "Testing.h"

#include <exception>
#include <iostream>

int main() {
  unsigned Failed = 0;
  for (const auto &Each : neverd::binmcp::test::cases()) {
    try {
      Each.Run();
      std::cout << "PASS " << Each.Name << "\n";
    } catch (const std::exception &Failure) {
      ++Failed;
      std::cout << "FAIL " << Each.Name << ": " << Failure.what() << "\n";
    }
  }
  std::cout << neverd::binmcp::test::cases().size() - Failed << " passed, "
            << Failed << " failed\n";
  return Failed ? 1 : 0;
}
