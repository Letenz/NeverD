//===- WailsFixture.h - C++ stored embed.FS fixtures -------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Authored inert ELF64 tables and declared Go build information. No producer
/// compiler, Wails runtime or external fixture script is required.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SHA256.h"

#include <string>
#include <vector>

namespace neverd::web::wails_test {
inline constexpr uint64_t VM = 0x400000, BuildOffset = 0x3000;
struct Entry {
  std::string Path, Contents;
};
inline void put(std::string &B, uint64_t At, uint64_t V, unsigned Width = 8) {
  for (unsigned I = 0; I < Width; ++I)
    B.at(At + I) = char(V >> (8 * I));
}
inline std::string prefix(std::string_view Text) {
  llvm::SHA256 Hash;
  if (Text.size() > 1024)
    Hash.update(llvm::StringRef("\1", 1));
  Hash.update(llvm::StringRef(Text));
  const auto Bytes = Hash.final();
  std::string Result{reinterpret_cast<const char *>(Bytes.data()), 16};
  if (Text.size() <= 1024)
    Result[0] = char(uint8_t(Result[0]) ^ 0xff);
  return Result;
}
inline void string(std::string &Out, const std::string &Text) {
  auto Size = uint64_t(Text.size());
  do {
    const auto Byte = Size & 127;
    Size >>= 7;
    Out.push_back(char(Byte | (Size ? 128 : 0)));
  } while (Size);
  Out += Text;
}
inline std::string moduleInfo() {
  return "path\tPRIVATE_BUILD_CANARY\n"
         "dep\tgithub.com/wailsapp/wails/v2\tv2.11.0\th1:fixture\n"
         "build\tGOARCH=amd64\n"
         "build\tGOOS=linux\n";
}
inline void buildInfo(std::string &B, std::string Go = "go1.25.7",
                      std::string Mod = moduleInfo()) {
  std::string Info(32, '\0');
  Info.replace(0, 14, "\xff Go buildinf:", 14);
  Info[14] = 8;
  Info[15] = 2;
  string(Info, Go);
  Mod.insert(0,
             "\x30\x77\xaf\x0c\x92\x74\x08\x02\x41\xe1\xc1\x07\xe6\xd6\x18\xe6",
             16);
  Mod.append("\xf9\x32\x43\x31\x86\x18\x20\x72\x00\x82\x42\x10\x41\x16\xd8\xf2",
             16);
  string(Info, Mod);
  B.replace(BuildOffset, 0x1000, std::string(0x1000, '\0'));
  B.replace(BuildOffset, Info.size(), Info);
}
inline std::vector<Entry> entries() {
  return {{"PRIVATE_CANARY/", ""},
          {"PRIVATE_CANARY/app.js", "export const answer=42;"},
          {"PRIVATE_CANARY/index.html",
           "<script type=module src='./app.js'></script>"}};
}
inline std::string elf(std::vector<std::vector<Entry>> Tables = {entries()}) {
  std::string B(0x5000, '\0');
  B.replace(0, 7, "\177ELF\2\1\1", 7);
  put(B, 16, 2, 2);
  put(B, 18, 62, 2);
  put(B, 20, 1, 4);
  put(B, 24, 0x600000);
  put(B, 32, 64);
  put(B, 52, 64, 2);
  put(B, 54, 56, 2);
  put(B, 56, 3, 2);
  for (uint64_t I = 0; I < 3; ++I) {
    const auto At = 64 + I * 56;
    put(B, At, 1, 4);
    put(B, At + 4, I == 0 ? 4 : I == 1 ? 6 : 5, 4);
    put(B, At + 8, I == 0 ? 0 : I == 1 ? 0x3000 : 0x4000);
    put(B, At + 16, I == 0 ? VM : I == 1 ? 0x500000 : 0x600000);
    put(B, At + 32, I == 0 ? 0x3000 : 0x1000);
    put(B, At + 40, I == 0 ? 0x3000 : 0x1000);
    put(B, At + 48, 0x1000);
  }
  B[0x4000] = char(0xc3);
  buildInfo(B);
  uint64_t Names = 0x1000, Data = 0x1400;
  for (uint64_t T = 0; T < Tables.size(); ++T) {
    const auto At = 0x2000 + T * 0x400;
    put(B, At, VM + At + 24);
    put(B, At + 8, Tables[T].size());
    put(B, At + 16, Tables[T].size());
    for (uint64_t I = 0; I < Tables[T].size(); ++I) {
      const auto &E = Tables[T][I];
      const auto Row = At + 24 + I * 48;
      B.replace(Names, E.Path.size(), E.Path);
      put(B, Row, VM + Names);
      put(B, Row + 8, E.Path.size());
      Names += E.Path.size();
      if (E.Path.ends_with('/'))
        continue;
      B.replace(Data, E.Contents.size(), E.Contents);
      put(B, Row + 16, VM + Data);
      put(B, Row + 24, E.Contents.size());
      B.replace(Row + 32, 16, prefix(E.Contents));
      Data += E.Contents.size();
    }
  }
  return B;
}
} // namespace neverd::web::wails_test
