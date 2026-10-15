//===- AndroidDirectories.cpp - Opaque Bionic directory streams ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// API 28 directory buffering and cookies over the shared Linux FD owner.
#include "AndroidInternal.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::android_model {
namespace {
constexpr uint64_t DirectoryBufferSize = 15 * 280;
bool failed(uint64_t R) { return R >= uint64_t(0) - 4095; }
} // namespace
BionicResult Bionic::openDirectory(const NativeCallEvent &Call) {
  uint32_t FD;
  const bool Adopt = Call.Name == symbol::Fdopendir;
  if (Adopt) {
    FD = Call.Arguments[0];
    auto S = Kernel.descriptorStatus(FD);
    if (!S)
      return std::optional<BionicValue>();
    if (auto *Error = std::get_if<uint32_t>(&*S))
      return streamError(*Error, 0);
    if (!std::get<linux_model::LinuxFiles::DescriptorStatus>(*S).Directory)
      return streamError(linux_model::NotDirectory, 0);
  } else {
    ProcessServiceEvent Event{
        0,
        0,
        {Call.Arguments[0],
         linux_model::OpenDirectoryARM64 | linux_model::OpenCloseOnExec, 0, 0,
         0, 0},
        std::nullopt};
    auto Open = kernelService(linux_model::ServiceKind::Open, Event);
    if (!Open)
      return Open.takeError();
    if (!*Open)
      return std::optional<BionicValue>();
    if (failed(**Open))
      return streamError(uint64_t(0) - **Open, 0);
    FD = **Open;
  }
  auto Token = allocate(3 * PageSize);
  if (!Token)
    return Token.takeError();
  if (!*Token) {
    // API 28 opendir does not close its new FD when DIR allocation fails.
    return value(0);
  }
  ProcessServiceEvent Protect{
      0, 0, {*Token, PageSize, 0, 0, 0, 0}, std::nullopt};
  auto P = kernelService(linux_model::ServiceKind::Mprotect, Protect);
  if (!P)
    return P.takeError();
  if (!*P || **P)
    return failure("could not protect opaque DIR storage");
  DirectoryStreams.emplace(*Token, DirectoryStream{FD, *Token});
  return value(*Token);
}

BionicResult Bionic::directoryCall(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  const auto &Name = Call.Name;
  if (!A[0] && Name == symbol::Closedir)
    return streamError(linux_model::InvalidArgument, UINT32_MAX);
  auto I = DirectoryStreams.find(A[0]);
  if (I == DirectoryStreams.end())
    return failure("invalid or retired Bionic DIR handle");
  if (auto E = opaqueHandle(A[0]))
    return std::move(E);
  auto &D = I->second;
  if (Name == symbol::Dirfd)
    return value(D.FD);
  if (Name == symbol::Telldir)
    return value(D.Position);
  if (Name == symbol::Closedir) {
    const auto FD = D.FD;
    if (auto E = release(D.Token))
      return std::move(E);
    DirectoryStreams.erase(I);
    auto R = streamService(linux_model::ServiceKind::Close, FD);
    if (!R)
      return R.takeError();
    if (!*R)
      return std::optional<BionicValue>();
    return value(failed(**R) ? UINT32_MAX : 0);
  }
  if (Name == symbol::Rewinddir || Name == symbol::Seekdir) {
    auto R = streamService(linux_model::ServiceKind::Lseek, D.FD,
                           Name == symbol::Rewinddir ? 0 : A[1], 0);
    if (!R)
      return R.takeError();
    if (!*R)
      return std::optional<BionicValue>();
    if (Name == symbol::Rewinddir || !failed(**R)) {
      D.Begin = D.End = 0;
      D.Position = Name == symbol::Rewinddir ? 0 : **R;
    }
    return value(0);
  }
  if (D.Begin == D.End) {
    auto R = streamService(linux_model::ServiceKind::Getdents64, D.FD,
                           D.Token + PageSize, DirectoryBufferSize);
    if (!R)
      return R.takeError();
    if (!*R)
      return std::optional<BionicValue>();
    if (!**R || failed(**R))
      return value(0);
    D.Begin = 0;
    D.End = **R;
  }
  const auto Entry = D.Token + PageSize + D.Begin;
  if (auto E = access(Entry, 19, Read))
    return std::move(E);
  uint8_t Header[19];
  if (auto E = CPU.read(Entry, Header))
    return std::move(E);
  const auto Length = llvm::support::endian::read16le(Header + 16);
  if (Length < 24 || Length % 8 || Length > D.End - D.Begin)
    return failure("modified Bionic directory record length");
  D.Position = llvm::support::endian::read64le(Header + 8);
  D.Begin += Length;
  return value(Entry);
}
} // namespace neverd::emulation::android_model
