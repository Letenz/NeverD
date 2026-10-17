//===- DarwinCallbackTestData.h - Original callback workload ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Expected externally visible constructor and TLS lifetime behavior.
#ifndef NEVERD_UNITTESTS_EMULATION_DARWINCALLBACKTESTDATA_H
#define NEVERD_UNITTESTS_EMULATION_DARWINCALLBACKTESTDATA_H
#include "neverd/emulation/ProcessSession.h"
namespace neverd::emulation::darwin_test {
struct CallbackCase {
  const char *Mode, *Output;
  unsigned Status;
};
constexpr CallbackCase CallbackCases[] = {
    {"return", "LPEabFM2N1", 37}, {"exit", "LPEabFM2N1", 37},
    {"raw", "LPEabFM", 38},       {"ctor-exit", "LPEab2N1", 37},
    {"ctor-raw", "LPEab", 38},    {"stack-exit", "LPEabFMS2N1", 37},
    {"dtor-raw", "LPEabFM2", 39}};
inline void callbackOptions(ProcessOptions &Options,
                            const std::filesystem::path &Directory,
                            const char *Mode) {
  Options.Arguments = {"different/argv-zero", Mode};
  Options.Environment = {"NEVERD_CALLBACK_SCOPE=one"};
  Options.InstructionQuantum = 1024;
  Options.Darwin = DarwinProcessOptions{
      {{"@loader_path/libmiddle.dylib", Directory / "libmiddle.dylib"},
       {"@loader_path/libleaf.dylib", Directory / "libleaf.dylib"}}};
}
} // namespace neverd::emulation::darwin_test
#endif
