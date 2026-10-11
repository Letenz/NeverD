//===- Export.h - Transactional local rewrite export --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Fixed exclusive files with descriptor-based rollback of owned entries.
///
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

namespace neverd::web::rewrite {

enum class ExportFile { Original, Candidate, Receipt };

/// A new local directory is the transaction boundary. Destruction before
/// commit removes only entries still matching files created by this object.
/// Observed directory/file replacement fails; unrelated entries survive.
class Export {
  struct Impl;
  std::unique_ptr<Impl> State;

public:
  explicit Export(std::string_view Path);
  ~Export();
  Export(const Export &) = delete;
  Export &operator=(const Export &) = delete;
  void write(ExportFile File, std::string_view Bytes);
  void commit();
};

} // namespace neverd::web::rewrite
