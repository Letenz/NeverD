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

#include "../platform/WindowsFile.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Error.h"
#include "neverd/web/Limits.h"
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
#ifdef _WIN32
  win::Destination Destination;
  std::array<win::Handle, 3> Handles;
  ~Impl() {
    if (Committed || !Destination.Directory)
      return;
    for (auto &File : Handles) {
      if (File) {
        try {
          win::remove(File.get());
        } catch (const Error &) {
        }
        File = {};
      }
    }
    // Removal acts on the created directory object. Foreign entries make it
    // nonempty and survive; no caller pathname is traversed during rollback.
    try {
      win::remove(Destination.Directory.get());
    } catch (const Error &) {
    }
  }
#else
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
  State->Destination = win::newDirectory(Path, win::DirectoryAccess::Remove);
#else
  if (Path.empty() ||
      Path.size() > ::neverd::limits::web::kExportExportPathSizeLimit ||
      Path.find('\0') != Path.npos)
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
  const auto Index = static_cast<unsigned>(File);
  if (State->Committed || Index != State->Next || Index >= Impl::Names.size())
    throw Error("source_rewrite_export_invalid_order");
  const auto Budget = File == ExportFile::Receipt ? 4 * MaxSourceRewriteBytes
                                                  : MaxSourceRewriteBytes;
  if (Bytes.size() > Budget)
    throw Error("source_rewrite_export_byte_budget_exceeded");
#ifdef _WIN32
  auto &F = State->Handles[Index];
  F = win::createFile(State->Destination.Directory.get(), Impl::Names[Index]);
  win::writeAt(F.get(), Bytes, 0);
  win::flush(F.get());
  llvm::SHA256 Digest;
  std::array<char, 65536> Buffer;
  for (uint64_t At = 0; At < Bytes.size();) {
    const auto N =
        win::readAt(F.get(), Buffer.data(),
                    std::min<uint64_t>(Buffer.size(), Bytes.size() - At), At);
    if (!N)
      throw Error("export_io_error");
    Digest.update(llvm::StringRef(Buffer.data(), size_t(N)));
    At += N;
  }
  if (win::inspect(F.get()).size() != Bytes.size() ||
      llvm::toHex(Digest.final(), true) != sha256(Bytes))
    throw Error("export_verification_failed");
#else
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
    const auto Count =
        std::min<size_t>(::neverd::limits::web::kExportWriteCountMinArgument1,
                         Bytes.size() - At);
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
#endif
  ++State->Next;
}

void Export::commit() {
  if (State->Committed || State->Next != Impl::Names.size())
    throw Error("source_rewrite_export_incomplete");
#ifndef _WIN32
  State->requireLocation();
  for (unsigned I = 0; I < Impl::Names.size(); ++I)
    if (!State->at(State->Directory.Value, Impl::Names[I], State->Files[I]))
      throw Error("source_rewrite_export_file_changed");
#endif
  State->Committed = true;
}

} // namespace neverd::web::rewrite
