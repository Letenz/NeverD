//===- AndroidStdioRead.cpp - Buffered and direct guest input ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Read API 28 byte streams through one kernel-owned descriptor cursor.
#include "AndroidInternal.h"

#include "neverd/emulation/CPU.h"

#include <algorithm>

namespace neverd::emulation::android_model {
llvm::Expected<std::optional<uint64_t>>
Bionic::streamRead(InputStream &Stream, uint64_t Output, uint64_t Size) {
  auto Ready = prepareStreamRead(Stream);
  if (!Ready)
    return Ready.takeError();
  if (!*Ready)
    return std::optional<uint64_t>();
  if (!**Ready)
    return std::optional<uint64_t>(0);
  auto Read =
      streamService(linux_model::ServiceKind::Read, Stream.FD, Output, Size);
  if (!Read)
    return Read.takeError();
  if (!*Read)
    return std::optional<uint64_t>();
  if (**Read >= uint64_t(0) - 4095) {
    Stream.Error = true;
    return std::optional<uint64_t>(0);
  }
  if (!**Read)
    Stream.EndOfFile = true;
  return *Read;
}

llvm::Expected<bool> Bionic::refillStream(InputStream &Stream) {
  auto Readable = prepareStreamRead(Stream);
  if (!Readable)
    return Readable.takeError();
  if (!*Readable)
    return false;
  if (!**Readable) {
    Stream.Begin = Stream.End = 0;
    return true;
  }
  auto Ready = prepareStreamBuffer(Stream);
  if (!Ready)
    return Ready.takeError();
  if (!*Ready)
    return false;
  auto Count = streamRead(Stream, Stream.Buffer, Stream.Capacity);
  if (!Count)
    return Count.takeError();
  if (!*Count)
    return false;
  Stream.Begin = 0;
  Stream.End = **Count;
  return true;
}

llvm::Error Bionic::copyStream(InputStream &Stream, uint64_t Output,
                               uint64_t Size) {
  if (auto E = access(Stream.Buffer + Stream.Begin, Size, Read))
    return E;
  if (auto E = access(Output, Size, Write))
    return E;
  if (Size && (Output <= Stream.Buffer + Stream.Begin
                   ? Stream.Buffer + Stream.Begin - Output < Size
                   : Output - Stream.Buffer - Stream.Begin < Size))
    return failure("overlapping stdio buffer and read destination");
  std::vector<uint8_t> Bytes(Size);
  if (auto E = CPU.read(Stream.Buffer + Stream.Begin, Bytes))
    return E;
  if (auto E = CPU.write(Output, Bytes))
    return E;
  Stream.Begin += Size;
  return llvm::Error::success();
}

BionicResult Bionic::readStream(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  const bool Block =
      Call.Name == symbol::Fread || Call.Name == symbol::FreadUnlocked;
  const bool Line =
      Call.Name == symbol::Fgets || Call.Name == symbol::FgetsUnlocked;
  auto Found = inputStream(Block ? A[3] : Line ? A[2] : A[0]);
  if (!Found)
    return Found.takeError();
  auto &Stream = **Found;
  if (!Block && !Line) {
    if (Stream.Begin == Stream.End) {
      auto Ready = refillStream(Stream);
      if (!Ready)
        return Ready.takeError();
      if (!*Ready)
        return std::optional<BionicValue>();
    }
    if (Stream.Begin == Stream.End)
      return value(UINT32_MAX);
    auto Character = byte(Stream.Buffer + Stream.Begin);
    if (!Character)
      return Character.takeError();
    ++Stream.Begin;
    return value(*Character);
  }
  uint64_t Requested;
  if (Block) {
    if (A[1] && A[2] > UINT64_MAX / A[1]) {
      Stream.Error = true;
      return streamError(linux_model::ValueOverflow, 0);
    }
    Requested = A[1] * A[2];
    if (!Requested)
      return value(0);
    // The API 28 file read callback takes an int count. Reject excessive
    // transfers instead of accidentally modeling host-sized read semantics.
    if (Requested > INT32_MAX)
      return failure("stdio read exceeds the supported signed-int extent");
    auto Ready = prepareStreamBuffer(Stream);
    if (!Ready)
      return Ready.takeError();
    if (!*Ready)
      return std::optional<BionicValue>();
  } else {
    if (int32_t(A[1]) <= 0)
      return streamError(linux_model::InvalidArgument, 0);
    Requested = uint32_t(A[1]) - 1;
  }
  uint64_t Done = 0;
  bool Direct = false;
  while (Done < Requested) {
    if (auto E = access(0, 0, 0))
      return std::move(E);
    if (A[0] > UINT64_MAX - Done)
      return failure("stdio destination address overflow");
    const uint64_t Output = A[0] + Done;
    uint64_t Available = Stream.End - Stream.Begin;
    if (!Available) {
      Direct |= Block && Requested - Done > Stream.Capacity;
      if (Direct) {
        auto Count = streamRead(Stream, Output, Requested - Done);
        if (!Count)
          return Count.takeError();
        if (!*Count)
          return std::optional<BionicValue>();
        if (!**Count)
          break;
        Done += **Count;
        continue;
      }
      auto Ready = refillStream(Stream);
      if (!Ready)
        return Ready.takeError();
      if (!*Ready)
        return std::optional<BionicValue>();
      Available = Stream.End - Stream.Begin;
      if (!Available)
        break;
    }
    uint64_t Count = std::min(Available, Requested - Done);
    bool Newline = false;
    if (Line) {
      // Bounded guest reads preserve the first newline without probing beyond
      // it, including buffers that end at a guest mapping boundary.
      for (uint64_t I = 0; I < Count; ++I) {
        auto Character = byte(Stream.Buffer + Stream.Begin + I);
        if (!Character)
          return Character.takeError();
        if (*Character == '\n') {
          Count = I + 1;
          Newline = true;
          break;
        }
      }
    }
    if (auto E = copyStream(Stream, Output, Count))
      return std::move(E);
    Done += Count;
    if (Newline)
      break;
  }
  if (Block)
    return value(Done / A[1]);
  if (!Done && Requested)
    return value(0);
  if (A[0] > UINT64_MAX - Done)
    return failure("stdio destination address overflow");
  if (auto E = access(A[0] + Done, 1, Write))
    return std::move(E);
  const uint8_t Nul = 0;
  if (auto E = CPU.write(A[0] + Done, llvm::ArrayRef<uint8_t>(&Nul, 1)))
    return std::move(E);
  return value(A[0]);
}
} // namespace neverd::emulation::android_model
