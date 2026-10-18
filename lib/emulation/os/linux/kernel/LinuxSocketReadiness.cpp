//===- LinuxSocketReadiness.cpp - Explicit virtual socket arrival state ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Non-consuming readiness from the same descriptions used by I/O.

#include "LinuxFiles.h"
#include "LinuxTime.h"

namespace neverd::emulation::linux_model {
LinuxFiles::Readiness LinuxFiles::socketReadiness(const OpenSocket &Socket,
                                                  unsigned Wanted) const {
  const bool Datagram = (Socket.Flags & 0xf) == 2;
  Readiness R;
  auto Unknown = [&]() {
    R.Unknown = "socket select readiness has no offline observation";
    return R;
  };
  if ((Datagram ? !Socket.Datagram : !Socket.Connection) || (Wanted & 4))
    return Unknown();
  if (Wanted & 2) {
    const auto Size = Datagram ? Socket.Datagram->ExpectedSend.size()
                               : Socket.Connection->ExpectedSend.size();
    if (Socket.Sent >= Size)
      return Unknown();
    R.Available |= 2;
  }
  if (!(Wanted & 1))
    return R;
  auto At = [&](std::optional<uint64_t> Time) {
    if (Time && (!Clock || !Clock->advancesOnIdle())) {
      R.Unknown = "timed network input requires virtual idle time";
    } else if (Time && Clock->elapsed() < *Time) {
      R.Wake = *Time;
    } else {
      R.Available |= 1;
    }
    return R;
  };
  const auto &Events = Datagram ? Socket.Datagram->ReceiveEvents
                                : Socket.Connection->ReceiveEvents;
  if (Socket.ReceiveEvent < Events.size()) {
    const auto &E = Events[Socket.ReceiveEvent];
    // A send prerequisite or completion error alone is no poll observation.
    if (Socket.Sent < E.AfterSend || E.Error)
      return Unknown();
    return At(E.ReadyAtNS); // Includes an explicitly queued empty datagram.
  }
  if (!Datagram) {
    if (Socket.Received < Socket.Connection->Receive.size())
      return At({});
    if (Socket.Connection->ReceiveEOF)
      return At(Socket.Connection->ReceiveEOFAtNS);
  }
  auto Quiet = Datagram ? Socket.Datagram->ReceiveQuietUntilNS
                        : Socket.Connection->ReceiveQuietUntilNS;
  if (Quiet) {
    if (!Clock || !Clock->advancesOnIdle()) {
      R.Unknown = "timed network input requires virtual idle time";
      return R;
    }
    if (Clock->elapsed() <= *Quiet && *Quiet < uint64_t(INT64_MAX)) {
      R.Wake = *Quiet + 1; // First unknown nanosecond, not invented data.
      return R;
    }
  }
  return Unknown();
}

std::optional<std::optional<uint64_t>>
LinuxFiles::receiveBeforeReady(const OpenSocket &Socket, uint32_t Flags,
                               ProcessResult &Result) const {
  const auto R = socketReadiness(Socket, 1);
  // Ordinary untimed completion/error admission keeps its existing owner.
  // A finite future arrival or quiet boundary is positive evidence of absence.
  if (R.Unknown || (R.Available & 1))
    return std::nullopt;
  if ((Flags & 0x40) || (Socket.Flags & OpenNonBlock))
    return std::optional<uint64_t>(uint64_t(0) - 11);
  return std::optional<std::optional<uint64_t>>(
      std::in_place,
      unsupported(
          Result,
          "blocking receive before readiness requires a wait contract"));
}
} // namespace neverd::emulation::linux_model
