//===- PkgFixture.h - Inert pkg envelope fixtures -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independently built byte layouts without a producer or target dependency.
///
//===----------------------------------------------------------------------===//

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neverd::web::pkg_test {
inline std::string quote(std::string_view Text) {
  constexpr char Hex[] = "0123456789abcdef";
  std::string Result = "\"";
  for (const unsigned char C : Text) {
    if (C < 32) {
      Result += "\\u00";
      Result += Hex[C >> 4];
      Result += Hex[C & 15];
    } else {
      if (C == '"' || C == '\\')
        Result += '\\';
      Result += char(C);
    }
  }
  return Result + '"';
}
inline std::string stat(bool Directory, uint64_t Size = 0) {
  return "{\"mode\":33188,\"size\":" + std::to_string(Size) +
         ",\"isFileValue\":" + (Directory ? "false" : "true") +
         ",\"isDirectoryValue\":" + (Directory ? "true" : "false") +
         ",\"isSocketValue\":false,\"isSymbolicLinkValue\":false}";
}
inline std::string prelude(std::string_view VFS, std::string_view Entry,
                           std::string_view Links = "{}",
                           std::string_view Dictionary = "{}",
                           std::string_view Compression = "0") {
  return "(function(process, require, console, EXECPATH_FD, PAYLOAD_POSITION, "
         "PAYLOAD_SIZE) { return (function (REQUIRE_COMMON, "
         "VIRTUAL_FILESYSTEM, "
         "DEFAULT_ENTRYPOINT, SYMLINKS, DICT, DOCOMPRESS) {\n"
         "/* CANARY_PRELUDE_BODY */\n})(function (exports) {\n"
         "/* CANARY_COMMON_BODY */\n},\n" +
         std::string(VFS) + "\n,\n" + quote(Entry) + "\n,\n" +
         std::string(Links) + "\n,\n" + std::string(Dictionary) + "\n,\n" +
         std::string(Compression) + "\n);\n})";
}
inline std::string locator(const std::array<uint64_t, 4> &Values,
                           bool CRLF = false) {
  constexpr std::array<std::string_view, 4> Names{
      "PAYLOAD_POSITION", "PAYLOAD_SIZE", "PRELUDE_POSITION", "PRELUDE_SIZE"};
  constexpr std::array<size_t, 4> Widths{22, 18, 22, 18};
  std::string Text;
  for (size_t I = 0; I < 4; ++I) {
    if (I)
      Text += CRLF ? "\r\n" : "\n";
    const auto Number = std::to_string(Values[I]);
    Text += "    var " + std::string(Names[I]) + " = '" + Number +
            std::string(Widths[I] - Number.size(), ' ') + "' | 0;";
  }
  return Text;
}
inline std::string image(std::string_view Payload, std::string_view Prelude,
                         bool CRLF = false, size_t PrefixSize = 1024,
                         size_t LocatorAt = 16, std::string_view Trailer = {}) {
  const auto Locator = locator(
      {PrefixSize, Payload.size(), PrefixSize + Payload.size(), Prelude.size()},
      CRLF);
  if (LocatorAt > PrefixSize || Locator.size() > PrefixSize - LocatorAt)
    throw std::invalid_argument("fixture prefix is too short");
  std::string Result(PrefixSize, 'x');
  Result.replace(LocatorAt, Locator.size(), Locator);
  return Result + std::string(Payload) + std::string(Prelude) +
         std::string(Trailer);
}

struct Builder {
  std::string VFS = "{", Payload;
  void add(std::string_view Path,
           const std::vector<std::pair<unsigned, std::string>> &Stores) {
    if (VFS.size() > 1)
      VFS += ',';
    VFS += quote(Path) + ":{";
    bool First = true;
    for (const auto &[Kind, Bytes] : Stores) {
      if (!First)
        VFS += ',';
      First = false;
      VFS += quote(std::to_string(Kind)) + ":[" +
             std::to_string(Payload.size()) + "," +
             std::to_string(Bytes.size()) + "]";
      Payload += Bytes;
    }
    VFS += '}';
  }
  void file(std::string_view Path, std::string_view Bytes,
            std::optional<std::string> Cache = std::nullopt) {
    std::vector<std::pair<unsigned, std::string>> Stores;
    if (Cache)
      Stores.emplace_back(0, *Cache);
    Stores.emplace_back(1, Bytes);
    Stores.emplace_back(3, stat(false, Bytes.size()));
    add(Path, Stores);
  }
  void directory(std::string_view Path) {
    add(Path, {{2, "[]"}, {3, stat(true)}});
  }
  std::string code(std::string_view Entry,
                   std::string_view Links = "{}") const {
    return prelude(VFS + '}', Entry, Links);
  }
  std::string build(std::string_view Entry, bool CRLF = false,
                    std::string_view Links = "{}",
                    std::string_view Trailer = {}) const {
    return image(Payload, code(Entry, Links), CRLF, 1024, 16, Trailer);
  }
};
inline std::string
bundle(const std::vector<std::pair<std::string, std::string>> &Files) {
  if (Files.empty())
    throw std::invalid_argument("fixture needs a declared entry");
  Builder B;
  for (const auto &[Name, Bytes] : Files)
    B.file("/snapshot/" + Name, Bytes);
  return B.build("/snapshot/" + Files.front().first);
}
} // namespace neverd::web::pkg_test
