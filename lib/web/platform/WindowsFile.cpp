//===- WindowsFile.cpp - Local NTFS ownership and confinement ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Handle-relative traversal, protected current-user ACLs and bounded I/O.
///
//===----------------------------------------------------------------------===//

#include "WindowsFile.h"

#include "neverd/web/Limits.h"

#ifdef _WIN32
#include "../Internal.h"

#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <cstring>
#include <filesystem>
#include <sddl.h>
#include <utility>
#include <winternl.h>

namespace neverd::web::win {
Handle::~Handle() {
  if (*this)
    CloseHandle(Value);
}
Handle::Handle(Handle &&Other) noexcept
    : Value(std::exchange(Other.Value, INVALID_HANDLE_VALUE)) {}
Handle &Handle::operator=(Handle &&Other) noexcept {
  if (this != &Other) {
    if (*this)
      CloseHandle(Value);
    Value = std::exchange(Other.Value, INVALID_HANDLE_VALUE);
  }
  return *this;
}

std::wstring wide(std::string_view Text) {
  if (Text.empty() || Text.size() > 32767 || Text.find('\0') != Text.npos)
    throw Error("invalid_path");
  const int N = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(),
                                    int(Text.size()), nullptr, 0);
  if (!N)
    throw Error("invalid_path");
  std::wstring Result(N, L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(),
                          int(Text.size()), Result.data(), N) != N)
    throw Error("invalid_path");
  return Result;
}
std::string utf8(std::wstring_view Text) {
  if (Text.empty() || Text.size() > 32767)
    throw Error("unsafe_member_name");
  const int N =
      WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Text.data(),
                          int(Text.size()), nullptr, 0, nullptr, nullptr);
  if (!N)
    throw Error("unsafe_member_name");
  std::string Result(N, '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Text.data(),
                          int(Text.size()), Result.data(), N, nullptr,
                          nullptr) != N)
    throw Error("unsafe_member_name");
  return Result;
}

namespace {
class Security {
  PSECURITY_DESCRIPTOR Descriptor = nullptr;

public:
  Security() {
    HANDLE Raw;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Raw))
      throw Error("private_storage_unavailable");
    Handle Token(Raw);
    DWORD N = 0;
    GetTokenInformation(Token.get(), TokenUser, nullptr, 0, &N);
    if (!N || N > ::neverd::limits::web::kMaxWindowsPathBufferUnits)
      throw Error("private_storage_unavailable");
    std::vector<unsigned char> Buffer(N);
    if (!GetTokenInformation(Token.get(), TokenUser, Buffer.data(), N, &N))
      throw Error("private_storage_unavailable");
    LPWSTR SID = nullptr;
    if (!ConvertSidToStringSidW(
            reinterpret_cast<TOKEN_USER *>(Buffer.data())->User.Sid, &SID))
      throw Error("private_storage_unavailable");
    const std::wstring SDDL = L"D:P(A;;FA;;;" + std::wstring(SID) + L")";
    LocalFree(SID);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            SDDL.c_str(), SDDL_REVISION_1, &Descriptor, nullptr))
      throw Error("private_storage_unavailable");
  }
  ~Security() { LocalFree(Descriptor); }
  PSECURITY_DESCRIPTOR get() const { return Descriptor; }
};

Handle openAt(HANDLE Parent, std::wstring_view Name, ACCESS_MASK Access,
              ULONG Disposition, ULONG Options, ULONG Share,
              PSECURITY_DESCRIPTOR Security = nullptr) {
  if (Name.size() > 32767)
    throw Error("invalid_path");
  UNICODE_STRING String{};
  String.Buffer = const_cast<wchar_t *>(Name.data());
  String.Length = String.MaximumLength = USHORT(Name.size() * sizeof(wchar_t));
  OBJECT_ATTRIBUTES Attributes{};
  Attributes.Length = sizeof(Attributes);
  Attributes.RootDirectory = Parent;
  Attributes.ObjectName = &String;
  Attributes.SecurityDescriptor = Security;
  // Exact enumeration spelling is authoritative, including case-sensitive
  // NTFS directories. No inherited handle or DOS short-name substitution.
  IO_STATUS_BLOCK Status{};
  HANDLE Raw = INVALID_HANDLE_VALUE;
  using Create = decltype(&NtCreateFile);
  static const auto Function = reinterpret_cast<Create>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
  if (!Function)
    throw Error("windows_file_api_unavailable");
  const auto Code =
      Function(&Raw, Access | SYNCHRONIZE, &Attributes, &Status, nullptr,
               FILE_ATTRIBUTE_NORMAL, Share, Disposition,
               Options | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT,
               nullptr, 0);
  if (Code < 0) {
    if (ULONG(Code) == 0xc0000035)
      throw Error("export_destination_exists");
    throw Error(Disposition == FILE_CREATE ? "export_io_error"
                                           : "input_unavailable");
  }
  return Handle(Raw);
}

void seek(HANDLE File, uint64_t Offset) {
  LARGE_INTEGER Position;
  Position.QuadPart = LONGLONG(Offset);
  if (Offset > INT64_MAX ||
      !SetFilePointerEx(File, Position, nullptr, FILE_BEGIN))
    throw Error("file_io_error");
}
} // namespace

uint64_t Info::size() const {
  return (uint64_t(Identity.nFileSizeHigh) << 32) | Identity.nFileSizeLow;
}
uint64_t Info::id() const {
  return (uint64_t(Identity.nFileIndexHigh) << 32) | Identity.nFileIndexLow;
}
bool Info::directory() const {
  return Identity.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
}
bool Info::same(const Info &Other) const {
  return id() == Other.id() &&
         Identity.dwVolumeSerialNumber == Other.Identity.dwVolumeSerialNumber &&
         Identity.dwFileAttributes == Other.Identity.dwFileAttributes &&
         Identity.nNumberOfLinks == Other.Identity.nNumberOfLinks &&
         size() == Other.size() &&
         Times.LastWriteTime.QuadPart == Other.Times.LastWriteTime.QuadPart &&
         Times.ChangeTime.QuadPart == Other.Times.ChangeTime.QuadPart;
}
bool Entry::matches(const Info &Opened) const {
  if (ID != Opened.id() || Attributes != Opened.Identity.dwFileAttributes)
    return false;
  // NTFS directory entries carry cached metadata. Creating a descendant can
  // leave the enumerated directory times stale even after every writer closes.
  // Bind its identity here; Reader compares authoritative handle revisions
  // before and after visiting the directory. File revisions remain checked.
  return Opened.directory() ||
         (Size == Opened.size() &&
          WriteTime.QuadPart == Opened.Times.LastWriteTime.QuadPart &&
          ChangeTime.QuadPart == Opened.Times.ChangeTime.QuadPart);
}

Info inspect(HANDLE File, bool AdmitStreams) {
  Info Result;
  if (GetFileType(File) != FILE_TYPE_DISK ||
      !GetFileInformationByHandle(File, &Result.Identity) ||
      !GetFileInformationByHandleEx(File, FileBasicInfo, &Result.Times,
                                    sizeof(Result.Times)))
    throw Error("input_unavailable");
  if (Result.Identity.dwFileAttributes &
      (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE |
       FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN |
       FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS))
    throw Error("unsupported_file_type");
  if (!Result.directory() && Result.Identity.nNumberOfLinks != 1)
    throw Error("hard_link_not_admitted");
  if (!AdmitStreams) {
    alignas(8) std::array<unsigned char, 65536> Buffer{};
    if (!GetFileInformationByHandleEx(File, FileStreamInfo, Buffer.data(),
                                      DWORD(Buffer.size()))) {
      if (Result.directory() && GetLastError() == ERROR_HANDLE_EOF)
        return Result;
      throw Error("input_streams_unavailable");
    }
    const auto *Stream =
        reinterpret_cast<const FILE_STREAM_INFO *>(Buffer.data());
    // NTFS directories can have no stream. Files have exactly the default
    // unnamed stream. Reject ADS on directories too rather than omitting bytes.
    if (Stream->StreamNameLength || Stream->NextEntryOffset) {
      const std::wstring_view Name(Stream->StreamName,
                                   Stream->StreamNameLength / sizeof(wchar_t));
      if (Stream->NextEntryOffset || Name != L"::$DATA")
        throw Error("alternate_stream_not_admitted");
    }
  }
  return Result;
}

Handle openRoot(std::string_view Path, bool Directory) {
  auto P = wide(Path);
  std::replace(P.begin(), P.end(), L'/', L'\\');
  while (P.size() > 3 && P.back() == L'\\')
    P.pop_back();
  if (P.starts_with(L"\\\\") || P.ends_with(L"\\.") || P.ends_with(L"\\..") ||
      (P.size() >= 2 && P[1] == L':' && (P.size() < 3 || P[2] != L'\\')))
    throw Error("invalid_path");
  std::array<wchar_t, 32768> Full{};
  const DWORD N =
      GetFullPathNameW(P.c_str(), DWORD(Full.size()), Full.data(), nullptr);
  if (!N || N >= Full.size() || Full[1] != L':' || Full[2] != L'\\' ||
      !((Full[0] >= L'A' && Full[0] <= L'Z') ||
        (Full[0] >= L'a' && Full[0] <= L'z')))
    throw Error("invalid_path");
  std::wstring Root(Full.data(), 3);
  if (GetDriveTypeW(Root.c_str()) != DRIVE_FIXED)
    throw Error("unsupported_file_system");
  const std::wstring Native = L"\\??\\" + Root;
  auto Current =
      openAt(nullptr, Native, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
             FILE_OPEN, FILE_DIRECTORY_FILE,
             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
  std::array<wchar_t, 32> FS{};
  if (!GetVolumeInformationByHandleW(Current.get(), nullptr, 0, nullptr,
                                     nullptr, nullptr, FS.data(),
                                     DWORD(FS.size())) ||
      std::wstring_view(FS.data()) != L"NTFS")
    throw Error("unsupported_file_system");
  std::wstring_view Remaining(Full.data() + 3, N - 3);
  while (!Remaining.empty()) {
    const auto Split = Remaining.find(L'\\');
    const auto Part = Remaining.substr(0, Split);
    validateMemberName(utf8(Part));
    const bool Last = Split == Remaining.npos;
    Current =
        openAt(Current.get(), Part, FILE_READ_DATA | FILE_READ_ATTRIBUTES,
               FILE_OPEN, !Last || Directory ? FILE_DIRECTORY_FILE : 0,
               Last ? FILE_SHARE_READ : FILE_SHARE_READ | FILE_SHARE_WRITE);
    const auto Info = inspect(Current.get());
    if ((!Last || Directory) && !Info.directory())
      throw Error("unsupported_file_type");
    if (Last)
      break;
    Remaining.remove_prefix(Split + 1);
  }
  return Current;
}
Handle openChild(HANDLE Parent, std::string_view Name) {
  validateMemberName(Name);
  auto Result =
      openAt(Parent, wide(Name), FILE_READ_DATA | FILE_READ_ATTRIBUTES,
             FILE_OPEN, 0, FILE_SHARE_READ);
  inspect(Result.get());
  return Result;
}

std::vector<Entry> entries(HANDLE Directory, uint64_t MaxEntries) {
  std::vector<Entry> Result;
  alignas(8) std::array<unsigned char, 65536> Buffer{};
  auto Class = FileIdBothDirectoryRestartInfo;
  while (true) {
    if (!GetFileInformationByHandleEx(Directory, Class, Buffer.data(),
                                      DWORD(Buffer.size()))) {
      if (GetLastError() == ERROR_NO_MORE_FILES)
        break;
      throw Error("input_unavailable");
    }
    Class = FileIdBothDirectoryInfo;
    size_t At = 0;
    while (true) {
      constexpr size_t Header = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
      if (At > Buffer.size() - Header)
        throw Error("input_changed");
      const auto *Item =
          reinterpret_cast<const FILE_ID_BOTH_DIR_INFO *>(Buffer.data() + At);
      if (Item->FileNameLength % sizeof(wchar_t) ||
          Item->FileNameLength > Buffer.size() - At - Header)
        throw Error("input_changed");
      const std::wstring_view Name(Item->FileName,
                                   Item->FileNameLength / sizeof(wchar_t));
      if (Name != L"." && Name != L"..") {
        if (Result.size() >= MaxEntries || Item->EndOfFile.QuadPart < 0)
          throw Error("budget_exceeded");
        Result.push_back({utf8(Name), uint64_t(Item->FileId.QuadPart),
                          uint64_t(Item->EndOfFile.QuadPart),
                          Item->FileAttributes, Item->LastWriteTime,
                          Item->ChangeTime});
      }
      if (!Item->NextEntryOffset)
        break;
      if (Item->NextEntryOffset < Header + Item->FileNameLength ||
          Item->NextEntryOffset % 8 ||
          Item->NextEntryOffset > Buffer.size() - At)
        throw Error("input_changed");
      At += Item->NextEntryOffset;
    }
  }
  return Result;
}

Handle createDirectory(HANDLE Parent, std::string_view Name,
                       DirectoryAccess Access) {
  validateMemberName(Name);
  Security ACL;
  // The kernel's relative rename opens the target directory for
  // FILE_WRITE_DATA. Use traversal access compatible with that open. Only the
  // rollback owner needs directory removal authority.
  const ACCESS_MASK Extra =
      Access == DirectoryAccess::Remove ? FILE_LIST_DIRECTORY | DELETE : 0;
  auto Result = openAt(
      Parent, wide(Name),
      FILE_TRAVERSE | FILE_READ_ATTRIBUTES | READ_CONTROL | Extra, FILE_CREATE,
      FILE_DIRECTORY_FILE, FILE_SHARE_READ | FILE_SHARE_WRITE, ACL.get());
  inspect(Result.get());
  return Result;
}
Handle createFile(HANDLE Parent, std::string_view Name, bool Temporary) {
  validateMemberName(Name);
  Security ACL;
  auto Result = openAt(
      Parent, wide(Name),
      FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | DELETE |
          READ_CONTROL,
      FILE_CREATE,
      FILE_NON_DIRECTORY_FILE | (Temporary ? FILE_DELETE_ON_CLOSE : 0),
      Temporary ? FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE : 0,
      ACL.get());
  inspect(Result.get());
  return Result;
}
Destination newDirectory(std::string_view Path, DirectoryAccess Access) {
  const std::filesystem::path P{wide(Path)};
  const auto Name = utf8(P.filename().native());
  validateMemberName(Name);
  const auto Parent =
      P.has_parent_path() ? utf8(P.parent_path().native()) : ".";
  Destination Result;
  Result.Parent = openRoot(Parent, true);
  Result.Directory = createDirectory(Result.Parent.get(), Name, Access);
  return Result;
}
Handle privateSpool() {
  std::array<wchar_t, 32768> Temp{};
  const DWORD N = GetTempPathW(DWORD(Temp.size()), Temp.data());
  if (!N || N >= Temp.size())
    throw Error("blob_storage_unavailable");
  auto Parent = openRoot(utf8(std::wstring_view(Temp.data(), N)), true);
  std::array<unsigned char, 16> Random{};
  if (BCryptGenRandom(nullptr, Random.data(), DWORD(Random.size()),
                      BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
    throw Error("blob_storage_unavailable");
  constexpr char Hex[] = "0123456789abcdef";
  std::string Name = "neverd-web-spool-";
  for (auto Byte : Random) {
    Name += Hex[Byte >> 4];
    Name += Hex[Byte & 15];
  }
  auto Result = createFile(Parent.get(), Name + ".tmp", true);
  // ReOpenFile creates a second file object without resolving a pathname.
  // Closing the original POSIX-delete file object removes the empty name
  // before any captured bytes are copied. A mere DuplicateHandle would keep
  // that same file object alive and delay its cleanup until sealing.
  Handle Writer(
      ReOpenFile(Result.get(),
                 FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES |
                     READ_CONTROL | SYNCHRONIZE,
                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                 FILE_FLAG_OPEN_REPARSE_POINT));
  if (!Writer)
    throw Error("blob_storage_unavailable");
  FILE_DISPOSITION_INFO_EX Disposition{FILE_DISPOSITION_FLAG_DELETE |
                                       FILE_DISPOSITION_FLAG_POSIX_SEMANTICS};
  if (!SetFileInformationByHandle(Result.get(), FileDispositionInfoEx,
                                  &Disposition, sizeof(Disposition)))
    throw Error("blob_storage_unavailable");
  Result = {};
  BY_HANDLE_FILE_INFORMATION Info{};
  if (!GetFileInformationByHandle(Writer.get(), &Info) || Info.nNumberOfLinks)
    throw Error("blob_storage_unavailable");
  return Writer;
}
Handle readOnly(HANDLE File) {
  HANDLE Raw;
  if (!DuplicateHandle(GetCurrentProcess(), File, GetCurrentProcess(), &Raw,
                       FILE_READ_DATA | FILE_READ_ATTRIBUTES | READ_CONTROL |
                           SYNCHRONIZE,
                       FALSE, 0))
    throw Error("blob_storage_unavailable");
  return Handle(Raw);
}
uint64_t readAt(HANDLE File, void *Bytes, uint64_t Size, uint64_t Offset) {
  seek(File, Offset);
  DWORD Read = 0;
  if (Size > BlobTransferBytes ||
      !ReadFile(File, Bytes, DWORD(Size), &Read, nullptr))
    throw Error("file_io_error");
  return Read;
}
void writeAt(HANDLE File, std::string_view Bytes, uint64_t Offset) {
  seek(File, Offset);
  size_t At = 0;
  while (At < Bytes.size()) {
    DWORD Written = 0;
    if (!WriteFile(
            File, Bytes.data() + At,
            DWORD(std::min<uint64_t>(BlobTransferBytes, Bytes.size() - At)),
            &Written, nullptr) ||
        !Written)
      throw Error("file_io_error");
    At += Written;
  }
}
void flush(HANDLE File) {
  if (!FlushFileBuffers(File))
    throw Error("file_io_error");
}
void remove(HANDLE File) {
  FILE_DISPOSITION_INFO Info{TRUE};
  if (!SetFileInformationByHandle(File, FileDispositionInfo, &Info,
                                  sizeof(Info)))
    throw Error("export_io_error");
}
void rename(HANDLE File, HANDLE Parent, std::string_view Name) {
  validateMemberName(Name);
  const auto Wide = wide(Name);
  std::vector<unsigned char> Buffer(
      sizeof(FILE_RENAME_INFO) + (Wide.size() + 1) * sizeof(wchar_t), 0);
  auto *Info = reinterpret_cast<FILE_RENAME_INFO *>(Buffer.data());
  Info->ReplaceIfExists = FALSE;
  Info->RootDirectory = Parent;
  Info->FileNameLength = DWORD(Wide.size() * sizeof(wchar_t));
  std::memcpy(Info->FileName, Wide.data(), Info->FileNameLength);
  // The Win32 adapter rejects a non-null RootDirectory on the qualified host.
  // The native operation admits that retained directory without resolving a
  // process-relative pathname. FileRenameInformation is WDK information class
  // 10. The SDK's placeholder enum cannot represent it; use the native ABI's
  // 32-bit information-class argument without an out-of-range C++ enum cast.
  using SetInformation =
      NTSTATUS(NTAPI *)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
  static const auto Function = reinterpret_cast<SetInformation>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile"));
  constexpr ULONG RenameInformation = 10;
  IO_STATUS_BLOCK Status{};
  if (!Function || Function(File, &Status, Info, ULONG(Buffer.size()),
                            RenameInformation) < 0)
    throw Error("export_io_error");
}
} // namespace neverd::web::win
#endif
