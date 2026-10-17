//===- LinuxNetwork.cpp - Offline stream contracts -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validated virtual connection attempts and shared stream state.

#include "LinuxNetwork.h"

#include "LinuxFiles.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation::linux_model {
uint16_t networkAddressFamily(llvm::ArrayRef<uint8_t> Address) {
  if (Address.size() < 2)
    return 0;
  const auto Family = llvm::support::endian::read16le(Address.data());
  if (Family == 2 && Address.size() == 16 &&
      llvm::all_of(Address.drop_front(8), [](uint8_t V) { return V == 0; }))
    return Family;
  if (Family == 10 && Address.size() == 28)
    return Family;
  return 0;
}
llvm::Error validateNetworkOptions(const LinuxNetworkOptions &Options) {
  if (Options.Connections.size() > 256)
    return failure("offline network has too many connection attempts");
  uint64_t Bytes = 0;
  for (const auto &C : Options.Connections) {
    if (!networkAddressFamily(C.Peer) ||
        (C.ConnectErrno != 0 && C.ConnectErrno != 111 &&
         C.ConnectErrno != 110 && C.ConnectErrno != 101 &&
         C.ConnectErrno != 113 && C.ConnectErrno != 13) ||
        !C.MaxSend || C.MaxSend > 65536 || !C.MaxReceive ||
        C.MaxReceive > 65536 ||
        (C.ConnectErrno &&
         (!C.ExpectedSend.empty() || !C.Receive.empty() || C.ReceiveEOF)))
      return failure("invalid offline TCP connection contract");
    Bytes += C.ExpectedSend.size() + C.Receive.size();
    if (Bytes > 1024 * 1024)
      return failure("offline network stream byte limit exceeded");
  }
  return llvm::Error::success();
}

bool LinuxFiles::isSocket(uint32_t FD) const {
  auto I = Descriptors.find(FD);
  return I != Descriptors.end() &&
         std::holds_alternative<OpenSocket>(*I->second);
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::network(ServiceKind Kind, const ProcessServiceEvent &Event,
                    const std::optional<LinuxNetworkOptions> &Network,
                    ProcessResult &Result) {
  using Reply = std::optional<uint64_t>;
  const auto &A = Event.Arguments;
  if (!Options || !Network)
    return unsupported(Result,
                       "socket services require linux_files and linux_network");
  if (Kind == ServiceKind::Socket) {
    const uint32_t Family = A[0], Type = A[1], Protocol = A[2];
    if ((Family != 2 && Family != 10) ||
        (Type & ~uint32_t(OpenCloseOnExec | OpenNonBlock)) != 1 ||
        (Protocol != 0 && Protocol != 6))
      return unsupported(Result,
                         "only offline IPv4/IPv6 TCP sockets are modeled");
    const auto FD = nextDescriptor();
    if (FD == Options->DescriptorLimit)
      return Reply(uint64_t(0) - TooManyFiles);
    Descriptors.emplace(FD,
                        std::make_shared<Descriptor>(OpenSocket{Family, Type}));
    opened(FD, Type);
    return Reply(FD);
  }
  const auto I = Descriptors.find(uint32_t(A[0]));
  if (I == Descriptors.end())
    return Reply(uint64_t(0) - BadDescriptor);
  auto *Socket = std::get_if<OpenSocket>(I->second.get());
  if (!Socket)
    return Reply(uint64_t(0) - 88); // ENOTSOCK
  if (Kind == ServiceKind::SetSocketOption ||
      Kind == ServiceKind::GetSocketOption) {
    const uint32_t Level = A[1], Name = A[2];
    if (Level != 1 || (Name != 20 && Name != 21))
      return unsupported(Result, "unmodeled TCP socket option");
    auto &Seconds =
        Name == 20 ? Socket->ReceiveTimeoutSeconds : Socket->SendTimeoutSeconds;
    auto Accessible = [&](uint64_t Address, uint64_t Size,
                          unsigned Permissions) -> llvm::Expected<bool> {
      if (Address > Layout.UserLimit || Size > Layout.UserLimit - Address)
        return false;
      return CPU.canAccess(Address, Size, Permissions | UserAccessible);
    };
    uint8_t Timeval[16]{};
    if (Kind == ServiceKind::SetSocketOption) {
      if (int32_t(A[4]) < 16)
        return Reply(uint64_t(0) - InvalidArgument);
      auto Readable = Accessible(A[3], 16, Read);
      if (!Readable)
        return Readable.takeError();
      if (!*Readable)
        return Reply(uint64_t(0) - BadAddress);
      if (auto E = CPU.read(A[3], Timeval))
        return std::move(E);
      const int64_t Sec = llvm::support::endian::read64le(Timeval);
      const int64_t Usec = llvm::support::endian::read64le(Timeval + 8);
      if (Usec < 0 || Usec >= 1000000)
        return Reply(uint64_t(0) - 33); // EDOM
      // Whole seconds avoid inventing the guest kernel's HZ and rounding.
      // Keep negative seconds distinct from zero (immediate vs infinite).
      if (Usec || Sec > 86400)
        return unsupported(Result,
                           "TCP timeout requires whole seconds up to one day");
      Seconds = Sec;
      return Reply(0);
    }
    auto LengthReadable = Accessible(A[4], 4, Read | Write);
    if (!LengthReadable)
      return LengthReadable.takeError();
    if (!*LengthReadable)
      return Reply(uint64_t(0) - BadAddress);
    uint8_t LengthBytes[4];
    if (auto E = CPU.read(A[4], LengthBytes))
      return std::move(E);
    const int32_t Length = llvm::support::endian::read32le(LengthBytes);
    if (Length < 0)
      return Reply(uint64_t(0) - InvalidArgument);
    const uint32_t Count = std::min<uint32_t>(Length, 16);
    if (Count) {
      auto Writable = Accessible(A[3], Count, Write);
      if (!Writable)
        return Writable.takeError();
      if (!*Writable)
        return Reply(uint64_t(0) - BadAddress);
      llvm::support::endian::write64le(Timeval, std::max<int64_t>(Seconds, 0));
      if (auto E = CPU.write(A[3], llvm::ArrayRef(Timeval).take_front(Count)))
        return std::move(E);
    }
    llvm::support::endian::write32le(LengthBytes, Count);
    if (auto E = CPU.write(A[4], LengthBytes))
      return std::move(E);
    return Reply(0);
  }
  if (Kind == ServiceKind::Connect) {
    if (Socket->Connection)
      return Reply(uint64_t(0) - 106); // EISCONN
    if (Socket->Failed)
      return unsupported(Result,
                         "a failed TCP socket must be closed before reuse");
    if (Socket->Flags & OpenNonBlock)
      return unsupported(Result,
                         "asynchronous TCP connection completion is unmodeled");
    const uint32_t Size = A[2];
    if (Size < 2 || Size > 128)
      return Reply(uint64_t(0) - InvalidArgument);
    if (A[1] > Layout.UserLimit || Size > Layout.UserLimit - A[1])
      return Reply(uint64_t(0) - BadAddress);
    auto Accessible = CPU.canAccess(A[1], Size, Read | UserAccessible);
    if (!Accessible)
      return Accessible.takeError();
    if (!*Accessible)
      return Reply(uint64_t(0) - BadAddress);
    std::vector<uint8_t> Peer(Size);
    if (auto E = CPU.read(A[1], Peer))
      return std::move(E);
    if (networkAddressFamily(Peer) != Socket->Family)
      return unsupported(Result,
                         "TCP peer requires a canonical matching sockaddr");
    if (NextConnection == Network->Connections.size() ||
        Network->Connections[NextConnection].Peer != Peer)
      return unsupported(Result,
                         "TCP peer has no matching offline connection attempt");
    const auto &C = Network->Connections[NextConnection++];
    if (C.ConnectErrno) {
      Socket->Failed = true;
      return Reply(uint64_t(0) - C.ConnectErrno);
    }
    Socket->Connection = &C;
    return Reply(0);
  }
  if ((Kind == ServiceKind::SendTo || Kind == ServiceKind::ReceiveFrom) &&
      (A[4] || A[5]))
    return unsupported(
        Result, "TCP explicit peer output or send destination is unmodeled");
  const uint32_t Flags = A[3];
  // MSG_NOSIGNAL changes only an EPIPE signal; this subset never fabricates
  // that failure. MSG_DONTWAIT cannot invent missing receive readiness.
  if (Flags & ~uint32_t(0x4000 | 0x40))
    return unsupported(Result, "unmodeled TCP transfer flags");
  return transferSocket(*Socket, Kind == ServiceKind::SendTo, A[1], A[2],
                        Result);
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::transferSocket(OpenSocket &Socket, bool Send, uint64_t Address,
                           uint64_t Size, ProcessResult &Result) {
  using Reply = std::optional<uint64_t>;
  if (!Socket.Connection)
    return unsupported(Result,
                       "TCP transfer requires a completed offline connection");
  if (Size > MaxSignedIOSize)
    return Reply(uint64_t(0) - InvalidArgument);
  if (!Size)
    return Reply(0);
  const auto &C = *Socket.Connection;
  const auto &Bytes = Send ? C.ExpectedSend : C.Receive;
  auto &Position = Send ? Socket.Sent : Socket.Received;
  if (Position == Bytes.size()) {
    if (!Send && C.ReceiveEOF)
      return Reply(0);
    return unsupported(Result,
                       Send ? "outgoing TCP stream exceeds its offline contract"
                            : "TCP receive readiness has no offline input");
  }
  const auto Count = std::min<uint64_t>(Size, Send ? C.MaxSend : C.MaxReceive);
  if (Send && Count > Bytes.size() - Position)
    return unsupported(Result,
                       "outgoing TCP stream exceeds its offline contract");
  const auto Transfer = std::min<uint64_t>(Count, Bytes.size() - Position);
  if (Address > Layout.UserLimit || Transfer > Layout.UserLimit - Address)
    return Reply(uint64_t(0) - BadAddress);
  auto Accessible =
      CPU.canAccess(Address, Transfer, (Send ? Read : Write) | UserAccessible);
  if (!Accessible)
    return Accessible.takeError();
  // Cross-page partial socket transfers are not covered by this atomic
  // offline contract. Do not consume a prefix or claim an errno ordering.
  if (!*Accessible)
    return unsupported(Result,
                       "TCP transfer requires a completely accessible buffer");
  const auto Expected = llvm::ArrayRef(Bytes).slice(Position, Transfer);
  if (Send) {
    std::vector<uint8_t> Actual(Transfer);
    if (auto E = CPU.read(Address, Actual))
      return std::move(E);
    if (llvm::ArrayRef(Actual) != Expected)
      return unsupported(Result,
                         "outgoing TCP bytes differ from the offline contract");
  } else if (auto E = CPU.write(Address, Expected))
    return std::move(E);
  Position += Transfer;
  return Reply(Transfer);
}
} // namespace neverd::emulation::linux_model
