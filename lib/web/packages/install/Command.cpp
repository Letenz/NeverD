//===- Command.cpp - Literal Node script command admission --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recognize a complete inert token subset without interpreting a shell.
///
//===----------------------------------------------------------------------===//

#include "Internal.h"

#include "neverd/web/Error.h"

namespace neverd::web::package_install {
void charge(uint64_t &Steps, uint64_t Count) {
  if (Count > MaxPackageInstallSteps - Steps)
    throw Error("package_install_work_budget_exceeded");
  Steps += Count;
}

Command command(std::string_view Text, uint64_t &Steps) {
  charge(Steps, Text.size() + 1);
  // A positive ASCII word alphabet excludes every shell expansion, separator,
  // quote, escape, glob and assignment, including platform-specific syntax.
  for (const unsigned char C : Text)
    if (!((C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') ||
          (C >= '0' && C <= '9') || C == '_' || C == '-' || C == '.' ||
          C == '/' || C == ' ' || C == '\t'))
      return {};
  uint64_t Cursor = 0;
  auto Word = [&]() {
    while (Cursor < Text.size() &&
           (Text[Cursor] == ' ' || Text[Cursor] == '\t'))
      ++Cursor;
    const auto Start = Cursor;
    while (Cursor < Text.size() && Text[Cursor] != ' ' && Text[Cursor] != '\t')
      ++Cursor;
    return Text.substr(Start, Cursor - Start);
  };
  Command C;
  if (Word() != "node") {
    C.Status = "unsupported_command";
    return C;
  }
  auto Path = Word();
  if (Path == "--")
    Path = Word();
  if (Path.empty()) {
    C.Status = "missing_script_argument";
    return C;
  }
  if (Path.front() == '-' || Path == "inspect") {
    C.Status = "node_option_or_subcommand_boundary";
    return C;
  }
  // Remaining words are admitted by the complete scan but not interpreted or
  // retained. Their presence never proves that Node or this file is executed.
  C.Status = "literal_node_script_candidate";
  C.Path = Path;
  C.End = Cursor;
  C.Start = Cursor - Path.size();
  return C;
}
} // namespace neverd::web::package_install
