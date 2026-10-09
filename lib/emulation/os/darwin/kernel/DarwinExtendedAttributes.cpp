//===- DarwinExtendedAttributes.cpp - Explicit xattr read observations ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Original bounded model of the LP64 ABI linked in docs/darwin-emulation.md.
// File objects own observations; no host filesystem or authorization is read.
#include "DarwinFiles.h"
#include "DarwinUserMemory.h"

#include <algorithm>

namespace neverd::emulation::darwin_model {
using namespace value;
namespace {
std::optional<ServiceResult> returned(uint64_t Value, bool Error = false) {
  return ServiceResult{Value, Error};
}
std::optional<ServiceResult> unsupported(ProcessResult &Result,
                                         const char *Reason) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = Reason;
  return std::nullopt;
}
} // namespace

llvm::Expected<DarwinFiles::Pathname>
DarwinFiles::readExtendedAttributeName(uint64_t Address) {
  std::string Name;
  for (uint64_t I = 0; I <= darwin_file_limits::ExtendedAttributeName; ++I) {
    if (Address >= UserLimit || I >= UserLimit - Address)
      return uint32_t(BadAddress);
    auto Access = Memory.canAccess(Address + I, 1, Read | UserAccessible);
    if (!Access)
      return Access.takeError();
    if (!*Access)
      return uint32_t(BadAddress);
    auto Byte = Memory.readInteger(Address + I, 1);
    if (!Byte)
      return Byte.takeError();
    if (!*Byte)
      return Name;
    Name.push_back(*Byte);
  }
  return uint32_t(NameTooLong);
}

const std::vector<DarwinExtendedAttribute> *
DarwinFiles::extendedAttributes(const Description &File) const {
  if (File.File)
    return File.File->ExtendedAttributes;
  if (File.Directory)
    return File.Directory->ExtendedAttributes;
  if (File.Link)
    return File.Link->ExtendedAttributes;
  return nullptr;
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::extendedAttributeRead(ServiceKind Service,
                                   const ProcessServiceEvent &Event,
                                   ProcessResult &Result) {
  const auto &A = Event.Arguments;
  const bool Held =
      Service == ServiceKind::FgetXattr || Service == ServiceKind::FlistXattr;
  const bool List =
      Service == ServiceKind::ListXattr || Service == ServiceKind::FlistXattr;
  const uint32_t Flags = A[List ? 3 : 5];
  if (Flags & (XattrNoSecurity | XattrNoDefault |
               (Held ? XattrNoFollow | XattrNoFollowAny : 0)))
    return returned(InvalidArgument, true);
  std::optional<Description> ResolvedFile;
  const Description *File;
  if (Held) {
    const auto Found = Descriptors.find(uint32_t(A[0]));
    if (Found == Descriptors.end())
      return returned(BadDescriptor, true);
    File = Found->second.Open.get();
  } else {
    auto Resolved =
        resolvePath(A[0], AtCurrentDirectory, LookupMode::Existing,
                    {!(Flags & XattrNoFollow), bool(Flags & XattrNoFollowAny)});
    if (!Resolved)
      return Resolved.takeError();
    if (auto *Error = std::get_if<uint32_t>(&*Resolved))
      return returned(*Error, true);
    if (auto *Reason = std::get_if<const char *>(&*Resolved))
      return unsupported(Result, *Reason);
    ResolvedFile = std::move(std::get<Description>(*Resolved));
    File = &*ResolvedFile;
  }
  if (File->Type != Kind::File && File->Type != Kind::Directory &&
      File->Type != Kind::SymbolicLink)
    return unsupported(Result, diagnostic::ExtendedAttributeKind);
  std::optional<std::string> Name;
  if (!List) {
    auto Imported = readExtendedAttributeName(A[1]);
    if (!Imported)
      return Imported.takeError();
    if (auto *Error = std::get_if<uint32_t>(&*Imported))
      return returned(*Error, true);
    Name = std::move(std::get<std::string>(*Imported));
  }
  return extendedAttributeResult(
      *File, Name ? std::optional<llvm::StringRef>(*Name) : std::nullopt,
      A[List ? 1 : 2], A[List ? 2 : 3], List ? 0 : uint32_t(A[4]), Flags, Held,
      Result);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::extendedAttributeResult(const Description &File,
                                     std::optional<llvm::StringRef> Name,
                                     uint64_t Address, uint64_t Size,
                                     uint32_t Position, uint32_t Flags,
                                     bool Held, ProcessResult &Result) {
  if (Name && !validExtendedAttributeName(*Name))
    return returned(InvalidArgument, true);
  if (Name && !ordinaryExtendedAttributeName(*Name))
    return unsupported(Result, diagnostic::ExtendedAttributeSpecial);
  if (Flags &
      ~uint32_t(XattrNoFollow | XattrCreate | XattrReplace | XattrNoFollowAny))
    return unsupported(Result, diagnostic::ExtendedAttributeFlags);
  // Path get retains two exact historical size-query sentinels. FD get
  // instead clamps large lengths and skips a non-NULL zero-size uio.
  const bool LegacyQuery =
      Name && !Held && (Size == UINT32_MAX || Size == UINT64_MAX);
  const bool Copy = Address && !LegacyQuery && (!Held || !Name || Size);
  if (Name && Copy && Position)
    return returned(InvalidArgument, true);
  const auto *Attributes = extendedAttributes(File);
  if (!Attributes)
    return unsupported(Result, diagnostic::ExtendedAttributeUnknown);
  llvm::ArrayRef<uint8_t> Bytes;
  std::vector<uint8_t> Names;
  uint64_t Total = 0;
  bool RangeError = false;
  if (Name) {
    const auto Found = llvm::find_if(*Attributes, [&](const auto &Attribute) {
      return Attribute.Name == *Name;
    });
    if (Found == Attributes->end())
      return returned(NoAttribute, true);
    Total = Found->Bytes.size();
    if (!Copy)
      return returned(Total);
    Size = std::min<uint64_t>(Size, INT32_MAX);
    if (Size < Total)
      return returned(ResultTooLarge, true);
    Bytes = Found->Bytes;
  } else {
    for (const auto &Attribute : *Attributes)
      Total += Attribute.Name.size() + 1;
    if (!Address || !Size)
      return returned(Total);
    if (Size > INT64_MAX) {
      if (!Total)
        return unsupported(Result, diagnostic::ExtendedAttributeEmptyNegative);
      return returned(ResultTooLarge, true);
    }
    // The observed provider publishes complete names before ERANGE. Never
    // split a UTF-8 name or derive the list order from a map of names.
    for (const auto &Attribute : *Attributes) {
      if (Attribute.Name.size() + 1 > Size - Names.size()) {
        RangeError = true;
        break;
      }
      Names.insert(Names.end(), Attribute.Name.begin(), Attribute.Name.end());
      Names.push_back(0);
    }
    Bytes = Names;
  }
  if (!Bytes.empty()) {
    auto Prefix = userMemoryPrefix(Memory, Address, Bytes.size(), Write);
    if (!Prefix)
      return Prefix.takeError();
    if (*Prefix != Bytes.size())
      return unsupported(Result, diagnostic::ExtendedAttributeOutput);
    if (auto Error = Memory.write(Address, Bytes))
      return std::move(Error);
  }
  return returned(RangeError ? ResultTooLarge : Total, RangeError);
}
} // namespace neverd::emulation::darwin_model
