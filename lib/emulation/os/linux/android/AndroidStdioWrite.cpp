//===- AndroidStdioWrite.cpp - Bionic buffered output and transitions ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Workload-only output over Linux descriptors. Buffer copies, direct writes
/// and flush failures retain distinct guest-memory and errno semantics.
#include "AndroidInternal.h"

#include <algorithm>

namespace neverd::emulation::android_model {
llvm::Expected<std::optional<bool>> Bionic::flushStream(InputStream &Stream) {
  using Reply = std::optional<bool>;
  if (Stream.Reader && *Stream.Reader != threadID())
    return failure("concurrent access to a suspended Bionic FILE is unmodeled");
  if (auto E = opaqueHandle(Stream.Token))
    return std::move(E);
  if (!Stream.Writing || !Stream.Pending)
    return Reply(true);
  const auto Size = Stream.Pending;
  Stream.Pending =
      0; // API 28 discards the pending count before a write attempt.
  uint64_t Done = 0;
  while (Done < Size) {
    auto R = streamService(linux_model::ServiceKind::Write, Stream.FD,
                           Stream.Buffer + Done, Size - Done);
    if (!R)
      return R.takeError();
    if (!*R)
      return Reply();
    if (!**R || **R >= uint64_t(0) - 4095) {
      Stream.Error = true;
      return Reply(false);
    }
    Done += **R;
  }
  return Reply(true);
}

llvm::Expected<std::optional<bool>>
Bionic::prepareStreamRead(InputStream &Stream) {
  using Reply = std::optional<bool>;
  if (!Stream.CanRead) {
    Stream.Error = true;
    if (auto E = setErrno(linux_model::BadDescriptor))
      return std::move(E);
    return Reply(false);
  }
  if (Stream.Writing) {
    auto Flushed = flushStream(Stream);
    if (!Flushed || !*Flushed || !**Flushed)
      return Flushed;
    Stream.Writing = false;
  }
  return Reply(true);
}

BionicResult Bionic::writeStream(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  const bool Text =
      Call.Name == symbol::Fputs || Call.Name == symbol::FputsUnlocked;
  auto Found = inputStream(Text ? A[1] : A[3]);
  if (!Found)
    return Found.takeError();
  auto &Stream = **Found;
  uint64_t Size;
  if (Text) {
    auto S = string(A[0]);
    if (!S)
      return S.takeError();
    Size = S->size();
  } else {
    if (A[1] && A[2] > UINT64_MAX / A[1]) {
      Stream.Error = true;
      return streamError(linux_model::ValueOverflow, 0);
    }
    Size = A[1] * A[2];
  }
  if (!Size)
    return value(0);
  if (Size > INT32_MAX)
    return failure("stdio write exceeds signed-int extent");
  if (!Stream.CanWrite)
    return streamError(linux_model::BadDescriptor, Text ? UINT32_MAX : 0);
  if (!Stream.Writing) {
    // The API 28 update transition discards read-ahead at the current FD
    // offset. Callers needing the logical read position must seek before
    // changing mode.
    Stream.Begin = Stream.End = 0;
    Stream.EndOfFile = false;
    Stream.Writing = true;
  }
  auto Ready = prepareStreamBuffer(Stream);
  if (!Ready)
    return Ready.takeError();
  if (!*Ready)
    return std::optional<BionicValue>();
  uint64_t Done = 0;
  while (Done < Size) {
    if (auto E = access(0, 0, 0))
      return std::move(E);
    if (A[0] > UINT64_MAX - Done)
      return failure("stdio source address overflow");
    const auto Source = A[0] + Done;
    uint64_t Span = Size - Done;
    bool Newline = false;
    if (Stream.BufferMode == 1) {
      for (uint64_t N = 0; N < Span; ++N) {
        auto C = byte(Source + N);
        if (!C)
          return C.takeError();
        if (*C == '\n') {
          Span = N + 1;
          Newline = true;
          break;
        }
      }
    }
    uint64_t Count;
    if (Stream.BufferMode == 2 ||
        (!Stream.Pending && Span >= Stream.Capacity)) {
      Count = std::min(Span, Stream.BufferMode == 2 ? uint64_t(1024)
                                                    : Stream.Capacity);
      auto R = streamService(linux_model::ServiceKind::Write, Stream.FD, Source,
                             Count);
      if (!R)
        return R.takeError();
      if (!*R)
        return std::optional<BionicValue>();
      if (!**R || **R >= uint64_t(0) - 4095) {
        Stream.Error = true;
        break;
      }
      Count = **R;
    } else {
      Count = std::min(Span, Stream.Capacity - Stream.Pending);
      const auto Target = Stream.Buffer + Stream.Pending;
      if (auto E = access(Source, Count, Read))
        return std::move(E);
      if (auto E = access(Target, Count, Write))
        return std::move(E);
      if (Count && (Source <= Target ? Target - Source < Count
                                     : Source - Target < Count))
        return failure("overlapping stdio buffer and write source");
      std::vector<uint8_t> Bytes(Count);
      if (auto E = CPU.read(Source, Bytes))
        return std::move(E);
      if (auto E = CPU.write(Target, Bytes))
        return std::move(E);
      Stream.Pending += Count;
      if (Span > Count) {
        auto Flushed = flushStream(Stream);
        if (!Flushed)
          return Flushed.takeError();
        if (!*Flushed)
          return std::optional<BionicValue>();
        if (!**Flushed)
          break;
      }
    }
    if (Newline && Count == Span) {
      auto Flushed = flushStream(Stream);
      if (!Flushed)
        return Flushed.takeError();
      if (!*Flushed)
        return std::optional<BionicValue>();
      if (!**Flushed)
        break;
    }
    Done += Count;
  }
  return value(Text ? (Done == Size ? 0 : UINT32_MAX) : Done / A[1]);
}
} // namespace neverd::emulation::android_model
