//===- ExportDirectory.h - Verified local artifact export --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verified local artifact export.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "Internal.h"
#include "platform/WindowsFile.h"

#include "neverd/web/Limits.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/SHA256.h"

#include <array>
#include <cerrno>
#include <filesystem>
#include <functional>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace neverd::web {
/// Explicit local disclosure only. A fresh private directory, fixed filenames,
/// exclusive no-follow writes and read-back verification. No archive pathname
/// is ever passed to this writer. A manifest is published only after its
/// read-back verification; failures can leave partial evidence for inspection.
class ExportDirectory {
#ifdef _WIN32
  win::Destination Destination;
  win::Handle Pending;
#else
  struct FD {
    int Value;
    explicit FD(int V) : Value(V) {
      if (V < 0)
        throw Error("export_io_error");
    }
    ~FD() { close(Value); }
    FD(const FD &) = delete;
  };
  int Directory = -1;
#endif
  uint64_t Written = 0, Files = 0;

public:
  explicit ExportDirectory(std::string_view Path) {
#ifdef _WIN32
    Destination = win::newDirectory(Path, win::DirectoryAccess::Traverse);
#else
    if (Path.empty() ||
        Path.size() > ::neverd::limits::web::kExportDirectoryPathSizeLimit ||
        Path.find('\0') != Path.npos)
      throw Error("invalid_path");
    const std::filesystem::path P{Path};
    const auto Name = P.filename().string();
    validateMemberName(Name);
    const auto Parent = P.has_parent_path() ? P.parent_path().string() : ".";
    FD ParentFD(open(Parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (mkdirat(ParentFD.Value, Name.c_str(), 0700) != 0)
      throw Error(errno == EEXIST ? "export_destination_exists"
                                  : "export_io_error");
    Directory = openat(ParentFD.Value, Name.c_str(),
                       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (Directory < 0)
      throw Error("export_io_error");
#endif
  }
  ~ExportDirectory() {
#ifndef _WIN32
    if (Directory >= 0)
      close(Directory);
#endif
  }
  ExportDirectory(const ExportDirectory &) = delete;

  void write(std::string_view Name, uint64_t Size, std::string_view Hash,
             const std::function<std::string(uint64_t, uint64_t)> &Read) {
    validateMemberName(Name);
    if (Size > ::neverd::limits::web::kExportDirectoryMaxOutput - Written ||
        Files >= ::neverd::limits::web::kExportDirectoryWriteFilesLimit)
      throw Error("export_budget_exceeded");
#ifdef _WIN32
    auto F = win::createFile(Destination.Directory.get(), Name);
    for (uint64_t At = 0; At < Size;) {
      const auto N = std::min(BlobTransferBytes, Size - At);
      const auto Bytes = Read(At, N);
      if (Bytes.size() != N)
        throw Error("export_read_error");
      win::writeAt(F.get(), Bytes, At);
      At += N;
    }
    win::flush(F.get());
    llvm::SHA256 Digest;
    std::array<char, BlobTransferBytes> Buffer;
    for (uint64_t At = 0; At < Size;) {
      const auto N = win::readAt(F.get(), Buffer.data(),
                                 std::min(BlobTransferBytes, Size - At), At);
      if (!N)
        throw Error("export_io_error");
      Digest.update(llvm::StringRef(Buffer.data(), size_t(N)));
      At += N;
    }
    if (win::inspect(F.get()).size() != Size ||
        llvm::toHex(Digest.final(), true) != Hash)
      throw Error("export_verification_failed");
    if (Name == "manifest.pending")
      Pending = std::move(F);
#else
    FD F(openat(Directory, std::string(Name).c_str(),
                O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    for (uint64_t At = 0; At < Size;) {
      const auto N = std::min(BlobTransferBytes, Size - At);
      const auto Bytes = Read(At, N);
      if (Bytes.size() != N)
        throw Error("export_read_error");
      size_t Done = 0;
      while (Done < Bytes.size()) {
        const auto W =
            ::write(F.Value, Bytes.data() + Done, Bytes.size() - Done);
        if (W < 0 && errno == EINTR)
          continue;
        if (W <= 0)
          throw Error("export_io_error");
        Done += size_t(W);
      }
      At += N;
    }
    llvm::SHA256 Digest;
    std::array<char, BlobTransferBytes> Buffer;
    for (uint64_t At = 0; At < Size;) {
      const auto N = std::min(BlobTransferBytes, Size - At);
      const auto R = pread(F.Value, Buffer.data(), N, off_t(At));
      if (R < 0 && errno == EINTR)
        continue;
      if (R <= 0)
        throw Error("export_io_error");
      Digest.update(llvm::StringRef(Buffer.data(), size_t(R)));
      At += uint64_t(R);
    }
    struct stat Info{};
    if (fstat(F.Value, &Info) != 0 || Info.st_size < 0 ||
        uint64_t(Info.st_size) != Size ||
        llvm::toHex(Digest.final(), true) != Hash)
      throw Error("export_verification_failed");
#endif
    Written += Size;
    ++Files;
  }
  void write(std::string_view Name, const Blob &Bytes, std::string_view Hash) {
    write(Name, Bytes.size(), Hash,
          [&](uint64_t At, uint64_t N) { return Bytes.read(At, N); });
  }
  void write(std::string_view Name, std::string_view Text) {
    write(Name, Text.size(), sha256(Text), [&](uint64_t At, uint64_t N) {
      return std::string(Text.substr(At, N));
    });
  }
  void complete(std::string_view Manifest) {
    write("manifest.pending", Manifest);
#ifdef _WIN32
    win::rename(Pending.get(), Destination.Directory.get(), "manifest.json");
    Pending = {};
#else
    // Atomic, no-clobber publication within the already-open private directory.
    if (linkat(Directory, "manifest.pending", Directory, "manifest.json", 0) !=
            0 ||
        unlinkat(Directory, "manifest.pending", 0) != 0)
      throw Error("export_io_error");
#endif
  }
  uint64_t bytes() const { return Written; }
  uint64_t files() const { return Files; }
};
} // namespace neverd::web
