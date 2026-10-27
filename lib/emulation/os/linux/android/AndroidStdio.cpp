//===- AndroidStdio.cpp - Workload-owned Bionic input streams ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Opaque FILE lifetime, buffering and positioning over the shared Linux FD
/// owner. API 28 behavior is documented in docs/android-native-emulation.md.
#include "AndroidInternal.h"
#include "AndroidThreads.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::android_model {
namespace {
constexpr uint64_t End = UINT32_MAX;
bool failed(uint64_t R) { return R >= uint64_t(0) - 4095; }
} // namespace

BionicResult Bionic::streamError(uint32_t Error, uint64_t ReturnValue) {
  if (auto E = setErrno(Error))
    return std::move(E);
  return value(ReturnValue);
}

llvm::Expected<std::optional<uint64_t>>
Bionic::streamService(linux_model::ServiceKind Kind, uint32_t FD, uint64_t A1,
                      uint64_t A2) {
  if (auto E = access(0, 0, 0))
    return std::move(E);
  ProcessServiceEvent Event{0, 0, {FD, A1, A2, 0, 0, 0}, std::nullopt};
  auto R = kernelService(Kind, Event);
  if (!R)
    return R.takeError();
  if (*R && failed(**R))
    if (auto E = setErrno(uint64_t(0) - **R))
      return std::move(E);
  return *R;
}

llvm::Expected<Bionic::InputStream *> Bionic::inputStream(uint64_t Handle) {
  if (auto E = access(0, 0, 0))
    return std::move(E);
  auto I = InputStreams.find(Handle);
  if (I == InputStreams.end())
    return failure("invalid or retired Bionic FILE handle");
  if (I->second.Reader && *I->second.Reader != threadID())
    return failure("concurrent access to a suspended Bionic FILE is unmodeled");
  if (auto E = opaqueHandle(Handle))
    return std::move(E);
  return &I->second;
}

llvm::Error Bionic::opaqueHandle(uint64_t Handle, llvm::StringRef Kind) {
  // An opaque token deliberately exposes no readable guest layout. FILE
  // allocations have a separate second page for their fallback byte buffer.
  auto Mapped = CPU.canAccess(Handle, PageSize, UserAccessible);
  if (!Mapped)
    return Mapped.takeError();
  if (!*Mapped)
    return failure(llvm::Twine("unmapped Bionic ") + Kind + " handle");
  for (unsigned Permission : {Read, Write, Execute}) {
    auto Changed = CPU.canAccess(Handle, 1, Permission | UserAccessible);
    if (!Changed)
      return Changed.takeError();
    if (*Changed)
      return failure(llvm::Twine("Bionic ") + Kind +
                     " storage must remain opaque");
  }
  return llvm::Error::success();
}

llvm::Expected<std::optional<uint64_t>>
Bionic::streamBufferSize(uint32_t FD, std::optional<uint64_t> SuppliedSize) {
  auto Status = Kernel.descriptorStatus(FD);
  if (!Status)
    return std::optional<uint64_t>();
  if (const auto *Error = std::get_if<uint32_t>(&*Status)) {
    if (auto E = setErrno(*Error))
      return std::move(E);
    // A known fstat failure selects BUFSIZ. Unknown metadata is not a failure.
    return std::optional<uint64_t>(1024);
  }
  const auto BlockSize =
      std::get<linux_model::LinuxFiles::DescriptorStatus>(*Status).BlockSize;
  // A supplied nonempty buffer does not use the filesystem size. The size
  // comparison only controls Bionic's seek optimization, which this model
  // deliberately does not perform. Still preserve known descriptor errors.
  if (!BlockSize && SuppliedSize)
    return SuppliedSize;
  if (!BlockSize) {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = "stdio buffering requires explicit file metadata";
    return std::optional<uint64_t>();
  }
  return std::optional<uint64_t>(*BlockSize ? *BlockSize : 1024);
}

llvm::Expected<bool> Bionic::prepareStreamBuffer(InputStream &Stream) {
  if (Stream.Buffer)
    return true;
  auto Size = streamBufferSize(Stream.FD);
  if (!Size)
    return Size.takeError();
  if (!*Size)
    return false;
  auto Buffer = allocate(**Size);
  if (!Buffer)
    return Buffer.takeError();
  Stream.Buffer = *Buffer ? *Buffer : Stream.Token + PageSize;
  Stream.Capacity = *Buffer ? **Size : 1;
  Stream.OwnBuffer = *Buffer != 0;
  if (!*Buffer)
    Stream.BufferMode = 2;
  return true;
}

BionicResult Bionic::openStream(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  auto First = byte(A[1]);
  if (!First)
    return First.takeError();
  if (*First != 'r' && *First != 'w' && *First != 'a')
    return streamError(linux_model::InvalidArgument, 0);
  auto Mode = string(A[1]);
  if (!Mode)
    return Mode.takeError();
  const bool Update = Mode->find('+') != std::string::npos;
  const uint32_t AccessMode = Update ? 2 : *First == 'r' ? 0 : 1;
  uint32_t Flags = AccessMode;
  if (*First != 'r')
    Flags |= linux_model::OpenCreate;
  if (*First == 'w')
    Flags |= linux_model::OpenTruncate;
  if (*First == 'a')
    Flags |= linux_model::OpenAppend;
  if (*First != 'r' && Mode->find('x') != std::string::npos)
    Flags |= linux_model::OpenExclusive;
  if (Mode->find('e') != std::string::npos)
    Flags |= linux_model::OpenCloseOnExec;
  // API 28 ignores other suffix letters; 'x' only affects creation modes.
  const bool Adopt = Call.Name == symbol::Fdopen;
  uint32_t FD;
  if (Adopt) {
    if (Mode->find('e') != std::string::npos) {
      Result.Stop = ProcessStopReason::UnsupportedService;
      Result.Diagnostic = "fdopen close-on-exec mutation is unmodeled";
      return std::optional<BionicValue>();
    }
    FD = A[0];
    auto Status = Kernel.descriptorStatus(FD);
    if (!Status)
      return std::optional<BionicValue>();
    if (const auto *Error = std::get_if<uint32_t>(&*Status))
      return streamError(*Error, 0);
    const auto &Descriptor =
        std::get<linux_model::LinuxFiles::DescriptorStatus>(*Status);
    if (Descriptor.AccessMode != 2 && Descriptor.AccessMode != AccessMode)
      return streamError(linux_model::InvalidArgument, 0);
    if (*First == 'a' && !Descriptor.Append) {
      Result.Stop = ProcessStopReason::UnsupportedService;
      Result.Diagnostic = "fdopen append flag mutation is unmodeled";
      return std::optional<BionicValue>();
    }
  } else {
    ProcessServiceEvent Event{0, 0, {A[0], Flags, 0666, 0, 0, 0}, std::nullopt};
    auto Opened = kernelService(linux_model::ServiceKind::Open, Event);
    if (!Opened)
      return Opened.takeError();
    if (!*Opened)
      return std::optional<BionicValue>();
    if (failed(**Opened))
      return streamError(uint64_t(0) - **Opened, 0);
    FD = **Opened;
  }
  auto Token = allocate(2 * PageSize);
  if (!Token)
    return Token.takeError();
  if (!*Token) {
    if (!Adopt) {
      auto Closed = streamService(linux_model::ServiceKind::Close, FD);
      if (!Closed)
        return Closed.takeError();
      if (!*Closed || **Closed)
        return failure("could not unwind failed fopen");
    }
    return value(0);
  }
  ProcessServiceEvent Protect{
      0, 0, {*Token, PageSize, 0, 0, 0, 0}, std::nullopt};
  auto Protected = kernelService(linux_model::ServiceKind::Mprotect, Protect);
  if (!Protected)
    return Protected.takeError();
  if (!*Protected || **Protected)
    return failure("could not protect opaque FILE storage");
  auto &Stream =
      InputStreams.emplace(*Token, InputStream{FD, *Token}).first->second;
  Stream.CanRead = AccessMode != 1;
  Stream.CanWrite = AccessMode != 0;
  Stream.Writing = AccessMode == 1;
  if (!Adopt && *First == 'a') {
    auto Positioned = streamService(linux_model::ServiceKind::Lseek, FD, 0, 2);
    if (!Positioned)
      return Positioned.takeError();
    if (!*Positioned)
      return std::optional<BionicValue>();
  }
  return value(*Token);
}

BionicResult Bionic::streamCall(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  const auto &Name = Call.Name;
  const bool Flush = Name == symbol::Fflush || Name == symbol::FflushUnlocked;
  if (Flush && !A[0]) {
    bool Failed = false;
    for (auto &[Token, Stream] : InputStreams) {
      auto R = flushStream(Stream);
      if (!R)
        return R.takeError();
      if (!*R)
        return std::optional<BionicValue>();
      Failed |= !**R;
    }
    return value(Failed ? End : 0);
  }
  auto Found = inputStream(A[0]);
  if (!Found)
    return Found.takeError();
  auto &Stream = **Found;
  if (Name == symbol::Fileno || Name == symbol::FilenoUnlocked)
    return value(Stream.FD);
  if (Name == symbol::Feof || Name == symbol::FeofUnlocked)
    return value(Stream.EndOfFile);
  if (Name == symbol::Ferror || Name == symbol::FerrorUnlocked)
    return value(Stream.Error);
  if (Name == symbol::Clearerr || Name == symbol::ClearerrUnlocked) {
    Stream.Error = Stream.EndOfFile = false;
    return value(0);
  }
  if (Flush) {
    if (!Stream.CanWrite)
      return streamError(linux_model::BadDescriptor, End);
    auto R = flushStream(Stream);
    if (!R)
      return R.takeError();
    if (!*R)
      return std::optional<BionicValue>();
    return value(**R ? 0 : End);
  }
  if (Name == symbol::Fclose) {
    auto Flushed = flushStream(Stream);
    if (!Flushed)
      return Flushed.takeError();
    if (!*Flushed)
      return std::optional<BionicValue>();
    auto Closed = streamService(linux_model::ServiceKind::Close, Stream.FD);
    if (!Closed)
      return Closed.takeError();
    if (!*Closed)
      return std::optional<BionicValue>();
    if (Stream.OwnBuffer)
      if (auto E = release(Stream.Buffer))
        return std::move(E);
    if (auto E = release(Stream.Token))
      return std::move(E);
    InputStreams.erase(A[0]);
    return value(failed(**Closed) || !**Flushed ? End : 0);
  }
  const bool GetPosition = Name == symbol::Fgetpos || Name == symbol::Fgetpos64;
  if (Name == symbol::Ftell || Name == symbol::Ftello ||
      Name == symbol::Ftello64 || GetPosition) {
    auto Flushed =
        flushStream(Stream); // API 28 ftell ignores flush's guest failure.
    if (!Flushed)
      return Flushed.takeError();
    if (!*Flushed)
      return std::optional<BionicValue>();
    auto Offset =
        streamService(linux_model::ServiceKind::Lseek, Stream.FD, 0, 1);
    if (!Offset)
      return Offset.takeError();
    if (!*Offset)
      return std::optional<BionicValue>();
    uint64_t Position =
        failed(**Offset) ? UINT64_MAX : **Offset - (Stream.End - Stream.Begin);
    if (!GetPosition)
      return value(Position);
    if (auto E = access(A[1], 8, Write))
      return std::move(E);
    uint8_t Bytes[8];
    llvm::support::endian::write64le(Bytes, Position);
    if (auto E = CPU.write(A[1], Bytes))
      return std::move(E);
    return value(Position == UINT64_MAX ? End : 0);
  }
  const bool Rewind = Name == symbol::Rewind;
  uint64_t Offset = Rewind ? 0 : A[1];
  uint32_t Whence = Rewind ? 0 : uint32_t(A[2]);
  if (Name == symbol::Fsetpos || Name == symbol::Fsetpos64) {
    if (auto E = access(A[1], 8, Read))
      return std::move(E);
    uint8_t Bytes[8];
    if (auto E = CPU.read(A[1], Bytes))
      return std::move(E);
    Offset = llvm::support::endian::read64le(Bytes);
    Whence = 0;
  }
  if (Whence > 2)
    return streamError(linux_model::InvalidArgument, End);
  if (Whence == 1) {
    auto Flushed = flushStream(Stream);
    if (!Flushed)
      return Flushed.takeError();
    if (!*Flushed)
      return std::optional<BionicValue>();
    auto Current =
        streamService(linux_model::ServiceKind::Lseek, Stream.FD, 0, 1);
    if (!Current)
      return Current.takeError();
    if (!*Current)
      return std::optional<BionicValue>();
    if (failed(**Current))
      return value(End);
    const int64_t Logical =
        static_cast<int64_t>(**Current - (Stream.End - Stream.Begin));
    const int64_t Delta = static_cast<int64_t>(Offset);
    if ((Delta > 0 && Logical > INT64_MAX - Delta) ||
        (Delta < 0 && Logical < INT64_MIN - Delta))
      return failure("signed overflow in Bionic relative stream seek");
    Offset = static_cast<uint64_t>(Logical + Delta);
    Whence = 0;
  }
  auto Ready = prepareStreamBuffer(Stream);
  if (!Ready)
    return Ready.takeError();
  if (!*Ready)
    return std::optional<BionicValue>();
  auto Flushed = flushStream(Stream);
  if (!Flushed)
    return Flushed.takeError();
  if (!*Flushed)
    return std::optional<BionicValue>();
  if (!**Flushed) {
    if (Rewind)
      Stream.Error = Stream.EndOfFile = false;
    return value(Rewind ? 0 : End);
  }
  auto Moved =
      streamService(linux_model::ServiceKind::Lseek, Stream.FD, Offset, Whence);
  if (!Moved)
    return Moved.takeError();
  if (!*Moved)
    return std::optional<BionicValue>();
  if (!failed(**Moved)) {
    Stream.Begin = Stream.End = 0;
    Stream.EndOfFile = false;
  }
  if (Rewind)
    Stream.Error = Stream.EndOfFile = false;
  return value(Rewind || !failed(**Moved) ? 0 : End);
}

BionicResult Bionic::bufferStream(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  auto Found = inputStream(A[0]);
  if (!Found)
    return Found.takeError();
  auto &Stream = **Found;
  uint64_t Buffer = A[1], Size = A[3];
  uint32_t Mode = A[2];
  if (Call.Name == symbol::Setbuf || Call.Name == symbol::Setbuffer) {
    Mode = Buffer ? 0 : 2;
    Size =
        Call.Name == symbol::Setbuf ? 1024 : uint64_t(int64_t(int32_t(A[2])));
  } else if (Call.Name == symbol::Setlinebuf) {
    Buffer = Size = 0;
    Mode = 1;
  }
  if (Mode != 2 && (Mode > 2 || int32_t(Size) < 0))
    return value(End); // API 28 leaves errno unchanged for invalid arguments.
  // Sizes outside the buffer's signed-int ABI are not silently truncated.
  if (Mode != 2 && Size > INT32_MAX)
    return failure("stdio buffer exceeds the supported signed-int extent");
  auto Flushed = flushStream(Stream);
  if (!Flushed)
    return Flushed.takeError();
  if (!*Flushed)
    return std::optional<BionicValue>();
  std::optional<uint64_t> DefaultSize;
  if (Mode != 2) {
    auto Queried = streamBufferSize(
        Stream.FD,
        Buffer && Size ? std::optional<uint64_t>(Size) : std::nullopt);
    if (!Queried)
      return Queried.takeError();
    if (!*Queried)
      return std::optional<BionicValue>();
    DefaultSize = **Queried;
  }
  if (Stream.OwnBuffer)
    if (auto E = release(Stream.Buffer))
      return std::move(E);
  Stream.BufferMode = Mode;
  Stream.OwnBuffer = Stream.EndOfFile = false;
  Stream.Begin = Stream.End = 0;
  Stream.Buffer = Stream.Token + PageSize;
  Stream.Capacity = 1;
  if (Mode == 2)
    return value(0);
  if (!Size) {
    Size = *DefaultSize;
    Buffer = 0;
  }
  bool AllocationFailed = false;
  if (!Buffer) {
    auto Allocated = allocate(Size);
    if (!Allocated)
      return Allocated.takeError();
    Buffer = *Allocated;
    AllocationFailed = !Buffer;
    if (!Buffer && Size != *DefaultSize) {
      Size = *DefaultSize;
      Allocated = allocate(Size);
      if (!Allocated)
        return Allocated.takeError();
      Buffer = *Allocated;
    }
    Stream.OwnBuffer = Buffer != 0;
  }
  if (Buffer) {
    Stream.Buffer = Buffer;
    Stream.Capacity = Size;
  }
  return value(AllocationFailed ? End : 0);
}
} // namespace neverd::emulation::android_model
