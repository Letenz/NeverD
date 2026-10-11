//===- Internal.h - Private installation trace contracts ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Share command admission, bounded accounting and metadata publication.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/PackageInstall.h"

#include "llvm/Support/JSON.h"

namespace neverd::web::package_install {

struct Command {
  std::string Status = "unsupported_command_syntax";
  std::string_view Path;
  uint64_t Start = 0, End = 0;
};

void charge(uint64_t &Steps, uint64_t Count = 1);
Command command(std::string_view Text, uint64_t &Steps);
llvm::json::Object summary(const PackageInstall &Analysis, uint64_t Revision);
llvm::json::Object record(const PackageInstallScript &Record);
llvm::json::Object record(const PackageInstallSource &Record);
llvm::json::Object record(const PackageInstallRequest &Record);
llvm::json::Object record(const PackageInstallAssociation &Record);

} // namespace neverd::web::package_install
