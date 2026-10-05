//===- LinuxFiles.cpp - Explicit read-only Linux memory files ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxFiles.h"

#include "llvm/ADT/SmallVector.h"

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
  return llvm::Error::success();
}

bool LinuxFiles::isOutput(uint32_t FD) const {
  auto I = Descriptors.find(FD);
  if (I == Descriptors.end())
    return false;
  const auto *S = std::get_if<Stream>(&I->second);
  return S && (*S == Stream::Output || *S == Stream::Error);
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::open(uint64_t Address, uint32_t Flags, ProcessResult &Result) {
  const uint32_t LargeFile = CPU.architecture() == GuestArchitecture::AArch64
                                 ? OpenLargeFileARM64
                                 : OpenLargeFileX64;
  if (Flags & ~(OpenCloseOnExec | LargeFile))
    return unsupported(Result, FileOpenFlags);

  // Import only through the first NUL, including at a readable page's last
  // byte. No host pathname conversion or filesystem lookup occurs.
  std::string Path;
  for (uint64_t I = 0; I < FilePathLimit; ++I) {
    if (Address >= Layout.UserLimit || I >= Layout.UserLimit - Address)
      return std::optional<uint64_t>(uint64_t(0) - BadAddress);
    auto Access = CPU.canAccess(Address + I, 1, Read | UserAccessible);
    if (!Access)
      return Access.takeError();
    if (!*Access)
      return std::optional<uint64_t>(uint64_t(0) - BadAddress);
    auto Byte = CPU.readInteger(Address + I, 1);
    if (!Byte)
      return Byte.takeError();
    if (!*Byte)
      break;
    Path.push_back(*Byte);
  }
  if (Path.size() == FilePathLimit)
    return std::optional<uint64_t>(uint64_t(0) - NameTooLong);
  if (Path.empty())
    return std::optional<uint64_t>(uint64_t(0) - NoEntry);
  if (!canonicalPath(Path))
    return unsupported(Result, FilePathForm);

  uint32_t FD = 0;
  while (FD < Options->DescriptorLimit && Descriptors.contains(FD))
    ++FD;
  if (FD == Options->DescriptorLimit)
    return std::optional<uint64_t>(uint64_t(0) - TooManyFiles);
  auto File = Options->Files.find(Path);
  if (File == Options->Files.end()) {
    // Catalogue prefixes denote directories; a known file used as a path
    // component is ENOTDIR. Opening a directory itself remains unmodeled.
    const std::string Prefix = Path + '/';
    auto Next = Options->Files.lower_bound(Prefix);
    if (Next != Options->Files.end() &&
        llvm::StringRef(Next->first).starts_with(Prefix))
      return unsupported(Result, FileDirectory);
    for (size_t I = Path.find('/', 1); I != std::string::npos;
         I = Path.find('/', I + 1))
      if (Options->Files.contains(Path.substr(0, I)))
        return std::optional<uint64_t>(uint64_t(0) - NotDirectory);
    return std::optional<uint64_t>(uint64_t(0) - NoEntry);
  }
  Descriptors.emplace(FD, OpenFile{File->second});
  return std::optional<uint64_t>(FD);
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
