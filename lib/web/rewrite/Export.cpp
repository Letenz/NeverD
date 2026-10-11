//===- Export.cpp - Exclusive rewrite files and ownership-aware rollback ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Preserve caller data during local disclosure and failed transactions.
///
//===----------------------------------------------------------------------===//

#include "Export.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Error.h"
#include "neverd/web/SourceRewrite.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <filesystem>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace neverd::web::rewrite {

struct Export::Impl {
  static constexpr std::array Names{"original.js", "candidate.js",
                                    "receipt.json"};
  unsigned Next = 0;
  bool Committed = false;
#ifndef _WIN32
  struct FD {
    int Value = -1;
    FD() = default;
    FD(const FD &) = delete;
    FD &operator=(const FD &) = delete;
    ~FD() {
      if (Value >= 0)
        close(Value);
    }
  } Parent, Directory;
  std::string Name;
  struct stat DirectoryInfo{};
  std::array<struct stat, 3> Files{};
  std::array<bool, 3> Created{};
  // Keep each inode alive until commit/rollback. Stat numbers alone could
  // identify a foreign replacement after unlink and inode-number reuse.
  std::array<FD, 3> Handles;

  static bool same(const struct stat &A, const struct stat &B) {
    return A.st_dev == B.st_dev && A.st_ino == B.st_ino &&
           (A.st_mode & S_IFMT) == (B.st_mode & S_IFMT);
  }
  bool at(int Dir, const char *Entry, const struct stat &Expected) const {
    struct stat Current{};
    return fstatat(Dir, Entry, &Current, AT_SYMLINK_NOFOLLOW) == 0 &&
           same(Current, Expected);
  }
  void requireLocation() const {
    if (!at(Parent.Value, Name.c_str(), DirectoryInfo))
      throw Error("source_rewrite_export_directory_changed");
  }
  ~Impl() {
    if (Committed || Directory.Value < 0)
      return;
    for (unsigned I = 0; I < Names.size(); ++I)
      if (Created[I] && at(Directory.Value, Names[I], Files[I]))
        unlinkat(Directory.Value, Names[I], 0);
    // Never traverse the caller's path during cleanup. A moved original
    // directory can remain empty; a replacement at that name is untouched.
    if (at(Parent.Value, Name.c_str(), DirectoryInfo))
      unlinkat(Parent.Value, Name.c_str(), AT_REMOVEDIR);
  }
#endif
};

Export::Export(std::string_view Path) : State(std::make_unique<Impl>()) {
#ifdef _WIN32
  throw Error("export_writer_unavailable");
#else
  if (Path.empty() || Path.size() > 32768 || Path.find('\0') != Path.npos)
    throw Error("invalid_path");
  const std::filesystem::path P{Path};
  State->Name = P.filename().string();
  if (State->Name.empty() || State->Name == "." || State->Name == "..")
    throw Error("invalid_path");
  const auto Parent = P.has_parent_path() ? P.parent_path().string() : ".";
  State->Parent.Value =
      open(Parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (State->Parent.Value < 0)
    throw Error("export_io_error");
  if (mkdirat(State->Parent.Value, State->Name.c_str(), 0700) != 0)
    throw Error(errno == EEXIST ? "export_destination_exists"
                                : "export_io_error");
  State->Directory.Value =
      openat(State->Parent.Value, State->Name.c_str(),
             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (State->Directory.Value < 0 ||
      fstat(State->Directory.Value, &State->DirectoryInfo) != 0)
    throw Error("export_io_error");
  State->requireLocation();
#endif
}

Export::~Export() = default;

void Export::write(ExportFile File, std::string_view Bytes) {
#ifdef _WIN32
  throw Error("export_writer_unavailable");
#else
  const auto Index = static_cast<unsigned>(File);
  if (State->Committed || Index != State->Next || Index >= Impl::Names.size())
    throw Error("source_rewrite_export_invalid_order");
  const auto Budget = File == ExportFile::Receipt ? 4 * MaxSourceRewriteBytes
                                                  : MaxSourceRewriteBytes;
  if (Bytes.size() > Budget)
    throw Error("source_rewrite_export_byte_budget_exceeded");
  State->requireLocation();
  auto &F = State->Handles[Index];
  F.Value = openat(State->Directory.Value, Impl::Names[Index],
                   O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (F.Value < 0)
    throw Error("export_io_error");
  if (fstat(F.Value, &State->Files[Index]) != 0)
    throw Error("export_io_error");
  State->Created[Index] = true;
  for (size_t At = 0; At < Bytes.size();) {
    const auto Count = std::min<size_t>(65536, Bytes.size() - At);
    const auto N = ::write(F.Value, Bytes.data() + At, Count);
    if (N < 0 && errno == EINTR)
      continue;
    if (N <= 0)
      throw Error("export_io_error");
    At += size_t(N);
  }
  llvm::SHA256 Digest;
  std::array<char, 65536> Buffer;
  for (uint64_t At = 0; At < Bytes.size();) {
    const auto N =
        pread(F.Value, Buffer.data(),
              std::min<uint64_t>(Buffer.size(), Bytes.size() - At), off_t(At));
    if (N < 0 && errno == EINTR)
      continue;
    if (N <= 0)
      throw Error("export_io_error");
    Digest.update(llvm::StringRef(Buffer.data(), size_t(N)));
    At += uint64_t(N);
  }
  struct stat Info{};
  if (fstat(F.Value, &Info) != 0 || !S_ISREG(Info.st_mode) ||
      Info.st_size < 0 || uint64_t(Info.st_size) != Bytes.size() ||
      llvm::toHex(Digest.final(), true) != sha256(Bytes) ||
      !State->at(State->Directory.Value, Impl::Names[Index],
                 State->Files[Index]))
    throw Error("export_verification_failed");
  ++State->Next;
#endif
}

void Export::commit() {
#ifdef _WIN32
  throw Error("export_writer_unavailable");
#else
  if (State->Committed || State->Next != Impl::Names.size())
    throw Error("source_rewrite_export_incomplete");
  State->requireLocation();
  for (unsigned I = 0; I < Impl::Names.size(); ++I)
    if (!State->at(State->Directory.Value, Impl::Names[I], State->Files[I]))
      throw Error("source_rewrite_export_file_changed");
  State->Committed = true;
#endif
}

} // namespace neverd::web::rewrite
