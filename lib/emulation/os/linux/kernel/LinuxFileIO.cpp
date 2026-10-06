//===- LinuxFileIO.cpp - Descriptor ownership and byte transfers ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxFiles.h"

#include <algorithm>

namespace neverd::emulation::linux_model {
llvm::Expected<std::optional<uint64_t>> LinuxFiles::close(uint32_t FD) {
  auto I = Descriptors.find(FD);
  if (I == Descriptors.end())
    return std::optional<uint64_t>(uint64_t(0) - BadDescriptor);
  Descriptors.erase(I);
  return std::optional<uint64_t>(0);
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::readDescriptor(uint32_t FD, uint64_t Address, uint64_t Size,
                           ProcessResult &Result) {
  auto I = Descriptors.find(FD);
  if (I == Descriptors.end())
    return std::optional<uint64_t>(uint64_t(0) - BadDescriptor);
  auto *File = std::get_if<OpenFile>(&I->second);
  if (!File) {
    if (std::get<Stream>(I->second) == Stream::Input)
      return unsupported(Result, FileInputStream);
    return std::optional<uint64_t>(uint64_t(0) - BadDescriptor);
  }
  auto Value = read(*File, Address, Size);
  if (!Value)
    return Value.takeError();
  return std::optional<uint64_t>(*Value);
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::seekDescriptor(uint32_t FD, uint64_t Offset, uint32_t Whence,
                           ProcessResult &Result) {
  auto I = Descriptors.find(FD);
  if (I == Descriptors.end())
    return std::optional<uint64_t>(uint64_t(0) - BadDescriptor);
  if (Whence > SeekHole)
    return std::optional<uint64_t>(uint64_t(0) - InvalidArgument);
  auto *File = std::get_if<OpenFile>(&I->second);
  if (!File) {
    if (std::get<Stream>(I->second) == Stream::Input)
      return unsupported(Result, FileInputStream);
    return std::optional<uint64_t>(uint64_t(0) - IllegalSeek);
  }
  if (Whence > SeekEnd)
    return unsupported(Result, FileSeekMode);
  return std::optional<uint64_t>(seek(*File, Offset, Whence));
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

} // namespace neverd::emulation::linux_model
