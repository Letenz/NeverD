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
    // Flushed before the case runs: a crash then ends the log with its name
    // instead of losing the buffered results of every case before it.
    std::cout << "RUN  " << Each.Name << std::endl;
    try {
      Each.Run();
      std::cout << "PASS " << Each.Name << std::endl;
    } catch (const std::exception &Failure) {
      ++Failed;
      std::cout << "FAIL " << Each.Name << ": " << Failure.what() << std::endl;
    }
  }
  std::cout << neverd::binmcp::test::cases().size() - Failed << " passed, "
            << Failed << " failed\n";
  return Failed ? 1 : 0;
}
