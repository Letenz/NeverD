//===- AllocatorProbe.cpp - Shared-library allocator fixture
//---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared-library half of the process allocator ownership regression.
///
//===----------------------------------------------------------------------===//

#include <cstdlib>
#include <string>

static const std::string StartupString(1024, 's');

extern "C" {
void *allocatorProbeMalloc(size_t Size) { return std::malloc(Size); }
void allocatorProbeFree(void *Ptr) { std::free(Ptr); }
char *allocatorProbeNew(size_t Size) { return new char[Size]; }
void allocatorProbeDelete(char *Ptr) { delete[] Ptr; }
const char *allocatorProbeStartup() { return StartupString.data(); }
}
