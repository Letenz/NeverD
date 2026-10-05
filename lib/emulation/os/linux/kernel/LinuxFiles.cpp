//===- LinuxFiles.cpp - Explicit read-only Linux memory files ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxFiles.h"

#include "LinuxTime.h"
#include "LinuxUserMemory.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <cassert>

namespace neverd::emulation::linux_model {
namespace {
bool canonicalPath(llvm::StringRef Path) {
  if (!Path.consume_front("/") || Path.empty() || Path.contains('\0'))
    return false;
  llvm::SmallVector<llvm::StringRef> Components;
  Path.split(Components, '/');
  return llvm::all_of(Components, [](llvm::StringRef Part) {
    return !Part.empty() && Part != "." && Part != ".." &&
           Part.size() <= FileNameLimit;
  });
}

std::optional<uint64_t> unsupported(ProcessResult &Result, const char *Reason) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = Reason;
  return std::nullopt;
}
} // namespace

llvm::Error validateFileOptions(const LinuxFileOptions &Options) {
  if (Options.DescriptorLimit < 3 ||
      Options.DescriptorLimit > FileDescriptorLimit ||
      Options.Files.size() > FileCountLimit)
    return failure(FileOptionsLimit);
  uint64_t Total = 0;
  for (const auto &[Path, Bytes] : Options.Files) {
    if (Path.size() >= FilePathLimit || !canonicalPath(Path))
      return failure(FileOptionPath);
    // A file cannot also be an ancestor directory of another file.
    for (size_t I = Path.find('/', 1); I != std::string::npos;
         I = Path.find('/', I + 1))
      if (Options.Files.contains(Path.substr(0, I)))
        return failure(FileOptionPath);
    const uint64_t Cost = Path.size() + 1;
    if (Cost > FileByteLimit - Total ||
        Bytes.size() > FileByteLimit - Total - Cost)
      return failure(FileOptionsLimit);
    Total += Cost + Bytes.size();
  }
  for (const auto &[Path, Metadata] : Options.Metadata) {
    if (!Options.Files.contains(Path))
      return failure(FileMetadataPath);
    if ((Metadata.Mode & ~FilePermissionMask) != FileRegularMode ||
        Metadata.Size > MaxSignedIOSize || Metadata.Blocks > MaxSignedIOSize ||
        Metadata.BlockSize > INT32_MAX ||
        !isNormalizedTimespec(Metadata.AccessTime) ||
        !isNormalizedTimespec(Metadata.ModificationTime) ||
        !isNormalizedTimespec(Metadata.ChangeTime))
      return failure(FileMetadataOption);
  }
  return llvm::Error::success();
}

bool LinuxFiles::isOutput(uint32_t FD) const {
  auto I = Descriptors.find(FD);
  if (I == Descriptors.end())
    return false;
  const auto *S = std::get_if<Stream>(&I->second);
  return S && (*S == Stream::Output || *S == Stream::Error);
}

llvm::Expected<LinuxFiles::Pathname> LinuxFiles::readPath(uint64_t Address) {
  // Import only through the first NUL, including at a readable page's last
  // byte. No host pathname conversion or filesystem lookup occurs.
  std::string Path;
  for (uint64_t I = 0; I < FilePathLimit; ++I) {
    if (Address >= Layout.UserLimit || I >= Layout.UserLimit - Address)
      return uint32_t(BadAddress);
    auto Access = CPU.canAccess(Address + I, 1, Read | UserAccessible);
    if (!Access)
      return Access.takeError();
    if (!*Access)
      return uint32_t(BadAddress);
    auto Byte = CPU.readInteger(Address + I, 1);
    if (!Byte)
      return Byte.takeError();
    if (!*Byte)
      break;
    Path.push_back(*Byte);
  }
  if (Path.size() == FilePathLimit)
    return uint32_t(NameTooLong);
  if (Path.empty())
    return uint32_t(NoEntry);
  return Path;
}

LinuxFiles::PathKind LinuxFiles::lookupPath(const std::string &Path) const {
  if (Path == "/")
    return PathKind::Directory;
  if (Options->Files.contains(Path))
    return PathKind::File;
  // Catalogue prefixes denote directories; a known file used as a path
  // component is ENOTDIR.
  const std::string Prefix = Path + '/';
  auto Next = Options->Files.lower_bound(Prefix);
  if (Next != Options->Files.end() &&
      llvm::StringRef(Next->first).starts_with(Prefix))
    return PathKind::Directory;
  for (size_t I = Path.find('/', 1); I != std::string::npos;
       I = Path.find('/', I + 1))
    if (Options->Files.contains(Path.substr(0, I)))
      return PathKind::NotDirectory;
  return PathKind::Missing;
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::access(uint64_t Address, uint32_t Mode, ProcessResult &Result) {
  // Linux validates the int mode before importing the pathname. Credentials,
  // ACLs and mount policy are not implied by a file's metadata observation.
  if (Mode & ~uint32_t(FileAccessMask))
    return std::optional<uint64_t>(uint64_t(0) - InvalidArgument);
  auto Imported = readPath(Address);
  if (!Imported)
    return Imported.takeError();
  if (const auto *Error = std::get_if<uint32_t>(&*Imported))
    return std::optional<uint64_t>(uint64_t(0) - *Error);
  const auto &Path = std::get<std::string>(*Imported);
  if (Path != "/" && !canonicalPath(Path))
    return unsupported(Result, FilePathForm);
  switch (lookupPath(Path)) {
  case PathKind::Missing:
    return std::optional<uint64_t>(uint64_t(0) - NoEntry);
  case PathKind::NotDirectory:
    return std::optional<uint64_t>(uint64_t(0) - NotDirectory);
  case PathKind::File:
  case PathKind::Directory:
    if (Mode)
      return unsupported(Result, FileAccessMode);
    return std::optional<uint64_t>(0);
  }
  llvm_unreachable("unknown Linux catalogue entry kind");
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::open(uint64_t Address, uint32_t Flags, ProcessResult &Result) {
  const uint32_t LargeFile = CPU.architecture() == GuestArchitecture::AArch64
                                 ? OpenLargeFileARM64
                                 : OpenLargeFileX64;
  if (Flags & ~(OpenCloseOnExec | LargeFile))
    return unsupported(Result, FileOpenFlags);
  auto Imported = readPath(Address);
  if (!Imported)
    return Imported.takeError();
  if (const auto *Error = std::get_if<uint32_t>(&*Imported))
    return std::optional<uint64_t>(uint64_t(0) - *Error);
  const auto &Path = std::get<std::string>(*Imported);
  if (!canonicalPath(Path))
    return unsupported(Result, FilePathForm);

  uint32_t FD = 0;
  while (FD < Options->DescriptorLimit && Descriptors.contains(FD))
    ++FD;
  if (FD == Options->DescriptorLimit)
    return std::optional<uint64_t>(uint64_t(0) - TooManyFiles);
  switch (lookupPath(Path)) {
  case PathKind::Missing:
    return std::optional<uint64_t>(uint64_t(0) - NoEntry);
  case PathKind::NotDirectory:
    return std::optional<uint64_t>(uint64_t(0) - NotDirectory);
  case PathKind::Directory:
    return unsupported(Result, FileDirectory);
  case PathKind::File:
    break;
  }
  auto File = Options->Files.find(Path);
  auto Metadata = Options->Metadata.find(Path);
  Descriptors.emplace(FD,
                      OpenFile{File->second, Metadata == Options->Metadata.end()
                                                 ? nullptr
                                                 : &Metadata->second});
  return std::optional<uint64_t>(FD);
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::status(const OpenFile &File, uint64_t Address,
                   ProcessResult &Result) {
  const auto &Metadata = *File.Metadata;
  const bool X64 = CPU.architecture() == GuestArchitecture::X64;
  llvm::SmallVector<uint8_t, FileStatusSizeX64> Bytes(
      X64 ? FileStatusSizeX64 : FileStatusSizeARM64, 0);
  auto Put = [&](unsigned Offset, unsigned Width, uint64_t Value) {
    if (Width == 4)
      llvm::support::endian::write32le(Bytes.data() + Offset, Value);
    else
      llvm::support::endian::write64le(Bytes.data() + Offset, Value);
  };
#define NEVERD_LINUX_FILE_STATUS(Member, ARMOffset, ARMWidth, X64Offset,       \
                                 X64Width)                                     \
  Put(X64 ? X64Offset : ARMOffset, X64 ? X64Width : ARMWidth,                  \
      static_cast<uint64_t>(Metadata.Member));
#include "LinuxFileStatus.def"
#undef NEVERD_LINUX_FILE_STATUS
  auto Stored = writeUserMemory(CPU, Layout, Address, Bytes);
  if (!Stored)
    return Stored.takeError();
  switch (*Stored) {
  case UserWriteResult::Stored:
    return std::optional<uint64_t>(0);
  case UserWriteResult::BadAddress:
    return std::optional<uint64_t>(uint64_t(0) - BadAddress);
  case UserWriteResult::MixedAccess:
    return unsupported(Result, FileStatusPartialOutput);
  }
  llvm_unreachable("unknown Linux user-copy outcome");
}

llvm::Expected<uint64_t> LinuxFiles::read(OpenFile &File, uint64_t Address,
                                          uint64_t Size) {
  // access_ok checks the original extent, before count clamping or EOF.
  if (Address > Layout.UserLimit || Size > Layout.UserLimit - Address)
    return uint64_t(0) - BadAddress;
  // Every cursor is nonnegative. Check the original signed file extent before
  // either transfer clamping or EOF can reduce the request.
  if (Size > MaxSignedIOSize - File.Offset)
    return uint64_t(0) - InvalidArgument;
  const uint64_t Available =
      File.Bytes.size() - std::min<uint64_t>(File.Offset, File.Bytes.size());
  Size = std::min({Size, Available, MaxReadWriteSize & ~(Layout.PageSize - 1)});
  uint64_t Copied = 0;
  while (Copied < Size) {
    const uint64_t Start = Address + Copied;
    const uint64_t Count =
        std::min(Size - Copied, Layout.PageSize - Start % Layout.PageSize);
    auto Access = CPU.canAccess(Start, Count, Write | UserAccessible);
    if (!Access)
      return Access.takeError();
    if (!*Access)
      return Copied ? Copied : uint64_t(0) - BadAddress;
    if (auto E = CPU.write(Start, File.Bytes.slice(File.Offset, Count)))
      return std::move(E);
    Copied += Count;
    File.Offset += Count;
  }
  return Copied;
}

uint64_t LinuxFiles::seek(OpenFile &File, uint64_t Offset, uint32_t Whence) {
  uint64_t Base;
  switch (Whence) {
  case SeekSet:
    Base = 0;
    break;
  case SeekCurrent:
    Base = File.Offset;
    break;
  case SeekEnd:
    Base = File.Bytes.size();
    break;
  default:
    llvm_unreachable("seek mode must be admitted by the service owner");
  }
  // Unsigned arithmetic preserves the syscall's signed offset bits without
  // invoking host signed overflow. Every admitted cursor is <= INT64_MAX.
  const uint64_t Position = Base + Offset;
  if (Position > MaxSignedIOSize ||
      (Offset <= MaxSignedIOSize && Position < Base))
    return uint64_t(0) - InvalidArgument;
  File.Offset = Position;
  return Position;
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::handle(ServiceKind Kind, const ProcessServiceEvent &Event,
                   ProcessResult &Result) {
  if (!Options)
    return unsupported(Result, FileInputsMissing);
  const auto &[A0, A1, A2, A3, A4, A5] = Event.Arguments;
  if (Kind == ServiceKind::Access || Kind == ServiceKind::FaccessAt)
    // Absolute names ignore dirfd. Raw faccessat has no flags argument.
    return Kind == ServiceKind::Access ? access(A0, A1, Result)
                                       : access(A1, A2, Result);
  if (Kind == ServiceKind::Open || Kind == ServiceKind::OpenAt)
    // Absolute names ignore dirfd. No mode is consumed without O_CREAT.
    return Kind == ServiceKind::Open ? open(A0, A1, Result)
                                     : open(A1, A2, Result);
  const uint32_t FD = A0;
  auto I = Descriptors.find(FD);
  if (I == Descriptors.end())
    return std::optional<uint64_t>(uint64_t(0) - BadDescriptor);
  if (Kind == ServiceKind::Close) {
    Descriptors.erase(I);
    return std::optional<uint64_t>(0);
  }
  auto *File = std::get_if<OpenFile>(&I->second);
  if (Kind == ServiceKind::Fstat) {
    if (!File || !File->Metadata)
      return unsupported(Result, FileStatusMissing);
    return status(*File, A1, Result);
  }
  if (Kind == ServiceKind::Read) {
    if (!File) {
      if (std::get<Stream>(I->second) == Stream::Input)
        return unsupported(Result, FileInputStream);
      return std::optional<uint64_t>(uint64_t(0) - BadDescriptor);
    }
    auto Value = read(*File, A1, A2);
    if (!Value)
      return Value.takeError();
    return std::optional<uint64_t>(*Value);
  }
  assert(Kind == ServiceKind::Lseek);
  const uint32_t Whence = A2;
  if (Whence > SeekHole)
    return std::optional<uint64_t>(uint64_t(0) - InvalidArgument);
  if (!File) {
    if (std::get<Stream>(I->second) == Stream::Input)
      return unsupported(Result, FileInputStream);
    return std::optional<uint64_t>(uint64_t(0) - IllegalSeek);
  }
  if (Whence > SeekEnd)
    return unsupported(Result, FileSeekMode);
  return std::optional<uint64_t>(seek(*File, A1, Whence));
}
} // namespace neverd::emulation::linux_model
