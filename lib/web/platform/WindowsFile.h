//===- WindowsFile.h - Confined Windows file handles --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Local NTFS handles, private creation and identity-preserving operations.
///
//===----------------------------------------------------------------------===//

#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace neverd::web::win {
class Handle {
  HANDLE Value = INVALID_HANDLE_VALUE;

public:
  Handle() = default;
  explicit Handle(HANDLE Value) : Value(Value) {}
  ~Handle();
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
  Handle(Handle &&Other) noexcept;
  Handle &operator=(Handle &&Other) noexcept;
  HANDLE get() const { return Value; }
  explicit operator bool() const { return Value != INVALID_HANDLE_VALUE; }
};

struct Info {
  BY_HANDLE_FILE_INFORMATION Identity{};
  FILE_BASIC_INFO Times{};
  uint64_t size() const;
  uint64_t id() const;
  bool directory() const;
  bool same(const Info &Other) const;
};
struct Entry {
  std::string Name;
  uint64_t ID, Size;
  DWORD Attributes;
  LARGE_INTEGER WriteTime, ChangeTime;
  bool matches(const Info &Opened) const;
};

std::wstring wide(std::string_view Text);
std::string utf8(std::wstring_view Text);
Info inspect(HANDLE File, bool AdmitStreams = false);
/// Walk from a local drive root, opening every component without reparsing.
Handle openRoot(std::string_view Path, bool Directory = false);
Handle openChild(HANDLE Parent, std::string_view Name);
std::vector<Entry> entries(HANDLE Directory, uint64_t MaxEntries);
Handle createDirectory(HANDLE Parent, std::string_view Name);
Handle createFile(HANDLE Parent, std::string_view Name, bool Temporary = false);
struct Destination {
  Handle Parent, Directory;
};
Destination newDirectory(std::string_view Path);
Handle privateSpool();
/// Duplicate with reduced access and close every writable handle at sealing.
Handle readOnly(HANDLE File);
/// The caller serializes operations sharing a synchronous file pointer.
uint64_t readAt(HANDLE File, void *Bytes, uint64_t Size, uint64_t Offset);
void writeAt(HANDLE File, std::string_view Bytes, uint64_t Offset);
void flush(HANDLE File);
void remove(HANDLE File);
void rename(HANDLE File, HANDLE Parent, std::string_view Name);
} // namespace neverd::web::win
#endif
