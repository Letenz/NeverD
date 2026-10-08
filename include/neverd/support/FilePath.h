//===- FilePath.h - UTF-8 filesystem path boundaries ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#pragma once

#include <filesystem>
#include <string>

namespace neverd {

/// LLVM filenames, C API strings and JSON use UTF-8. Keep paths native while
/// using std::filesystem/streams, and encode only at these string boundaries.
/// path::string() uses the system code page on Windows and can lose Unicode.
inline std::string pathToUTF8(const std::filesystem::path &Path) {
  const auto UTF8 = Path.u8string();
  return std::string(UTF8.begin(), UTF8.end());
}

} // namespace neverd
