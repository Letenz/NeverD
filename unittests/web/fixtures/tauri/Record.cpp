//===- Record.cpp - Development-only Tauri container selection ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Export an explicitly selected native member through NeverD's archive owner.
/// No stored pathname becomes a host path and no input executable is launched.
///
//===----------------------------------------------------------------------===//

#include "ExportDirectory.h"
#include "Internal.h"

#include "neverd/web/PackageArchive.h"

#include "llvm/Support/raw_ostream.h"

#include <charconv>

int main(int Argc, char **Argv) {
  using namespace neverd::web;
  if (Argc != 5)
    return 2;
  uint64_t Index = 0;
  const std::string_view Text(Argv[3]);
  const auto Parsed =
      std::from_chars(Text.data(), Text.data() + Text.size(), Index);
  if (Parsed.ec != std::errc{} || Parsed.ptr != Text.data() + Text.size())
    return 2;
  try {
    const auto S = capture(Argv[1], Limits{});
    if (S.Artifacts.size() != 1 || S.Artifacts[0].Directory)
      throw Error("invalid_fixture_input");
    const auto A = extractPackageArchive(S.Artifacts[0], Argv[2]);
    if (Index >= A.Members.size() || !A.Members[Index].available())
      throw Error("invalid_fixture_member");
    const auto &M = A.Members[Index];
    ExportDirectory Output(Argv[4]);
    Output.write("original.bin", M.Content, M.BlobHash);
    const auto Manifest =
        json(llvm::json::Object{{"schema_version", 1},
                                {"purpose", "development_fixture_selection"},
                                {"archive_profile", A.Profile},
                                {"archive_id", A.ID},
                                {"original_sha256", A.BlobHash},
                                {"expanded_sha256", A.ExpandedHash},
                                {"member_id", M.ID},
                                {"member_index", Index},
                                {"expanded_offset", std::to_string(M.Offset)},
                                {"member_bytes", std::to_string(M.Size)},
                                {"member_sha256", M.BlobHash},
                                {"executes_input", false}});
    Output.complete(Manifest);
    llvm::outs() << Manifest << '\n';
    return 0;
  } catch (const Error &E) {
    llvm::errs() << E.what() << '\n';
    return 1;
  }
}
