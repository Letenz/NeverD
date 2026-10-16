//===- DarwinLibSystemFileTestData.h - Imported file workload ----*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Explicit file inputs and independently stated C call expectations.
#ifndef NEVERD_TESTS_DARWINLIBSYSTEMFILETESTDATA_H
#define NEVERD_TESTS_DARWINLIBSYSTEMFILETESTDATA_H

#include "neverd/emulation/DarwinFileOptions.h"

namespace neverd::emulation::darwin_test {
inline DarwinFileOptions libSystemFileOptions() {
  DarwinFileOptions O;
  O.Files["/data"] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};
  O.WritableFiles.insert("/data");
  O.Directories.insert("/");
  O.MutableDirectories.insert("/");
  O.InitialUmask = 0027;
  return O;
}
inline constexpr char LibSystemFilesJSON[] = R"({
  "files":[{"path":"/data","bytes_hex":"30313233343536373839","writable":true}],
  "directories":[{"path":"/","mutable":true}],"umask":23})";
struct LibSystemFileCall {
  const char *Name;
  unsigned Count;
};
inline constexpr LibSystemFileCall LibSystemFileCalls[] = {
    {"___error", 0}, {"_open", 2},  {"_open", 2},   {"_open", 2},
    {"_read", 3},    {"_dup", 1},   {"_read", 3},   {"_lseek", 3},
    {"_pread", 4},   {"_lseek", 3}, {"_pwrite", 4}, {"_lseek", 3},
    {"_lseek", 3},   {"_read", 3},  {"_dup2", 2},   {"_read", 3},
    {"_lseek", 3},   {"_dup2", 2},  {"_dup2", 2},   {"_lseek", 3},
    {"_lseek", 3},   {"_lseek", 3}, {"_pread", 4},  {"_lseek", 3},
    {"_lseek", 3},   {"_lseek", 3}, {"_pwrite", 4}, {"_pread", 4},
    {"_close", 1},   {"_close", 1}, {"_close", 1},  {"_open", 3},
    {"_write", 3},   {"_close", 1}, {"_open", 2},   {"_read", 3},
    {"_read", 3},    {"_close", 1}, {"_open", 3},   {"_write", 3}};
} // namespace neverd::emulation::darwin_test
#endif
