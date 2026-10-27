//===- AndroidResolver.cpp - API 28 offline address resolution -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Offline resolver ownership and API 28 address conversion.

#include "AndroidResolver.h"

#include "../kernel/LinuxNetwork.h"
#include "AndroidInternal.h"

#include "neverd/emulation/Limits.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

#include <set>
#include <tuple>

namespace neverd::emulation::android_model {
namespace {
constexpr unsigned AddressInfoSize = 48;
constexpr uint32_t SupportedFlags = 0x40f; // API 28 AI_MASK
auto key(const AndroidResolverQuery &Q) {
  return std::tuple(Q.Node, Q.Service, Q.Flags, Q.Family, Q.SocketType,
                    Q.Protocol);
}
bool parseIPv4(llvm::StringRef Text, std::vector<uint8_t> &Bytes) {
  llvm::SmallVector<llvm::StringRef, 4> Parts;
  Text.split(Parts, '.', -1, true);
  if (Parts.size() != 4)
    return false;
  for (auto Part : Parts) {
    if (Part.empty())
      return false;
    unsigned Value = 0;
    for (char C : Part) {
      if (C < '0' || C > '9' || (Value = Value * 10 + (C - '0')) > 255)
        return false;
    }
    Bytes.push_back(static_cast<uint8_t>(Value));
  }
  return true;
}
bool parseIPv6(llvm::StringRef Text, std::vector<uint8_t> &Bytes) {
  const auto Gap = Text.find("::");
  if (Gap != llvm::StringRef::npos &&
      Text.find("::", Gap + 2) != llvm::StringRef::npos)
    return false;
  auto Part = [&](llvm::StringRef S, bool Last, std::vector<uint8_t> &Out) {
    if (S.empty())
      return true;
    llvm::SmallVector<llvm::StringRef, 8> Words;
    S.split(Words, ':', -1, true);
    for (size_t I = 0; I < Words.size(); ++I) {
      const auto W = Words[I];
      if (Last && I + 1 == Words.size() && W.contains('.')) {
        if (!parseIPv4(W, Out))
          return false;
      } else {
        unsigned V = 0;
        if (W.empty() || W.size() > 4 || !llvm::all_of(W, llvm::isHexDigit) ||
            W.getAsInteger(16, V))
          return false;
        Out.push_back(static_cast<uint8_t>(V >> 8));
        Out.push_back(static_cast<uint8_t>(V));
      }
      if (Out.size() > 16)
        return false;
    }
    return true;
  };
  if (Gap == llvm::StringRef::npos)
    return Part(Text, true, Bytes) && Bytes.size() == 16;
  std::vector<uint8_t> Right;
  if (!Part(Text.take_front(Gap), false, Bytes) ||
      !Part(Text.drop_front(Gap + 2), true, Right) ||
      Bytes.size() + Right.size() >= 16)
    return false;
  Bytes.resize(16 - Right.size(), 0);
  Bytes.insert(Bytes.end(), Right.begin(), Right.end());
  return true;
}
} // namespace
llvm::Error
validateResolverQueries(const std::vector<AndroidResolverQuery> &Queries) {
  if (Queries.size() > ::neverd::limits::emulation::
                           kAndroidResolverValidateResolverQueriesSizeLimit)
    return failure("too many resolver observations");
  std::set<decltype(key(AndroidResolverQuery{}))> Keys;
  uint64_t Bytes = 0;
  auto Text = [&](const std::optional<std::string> &S) {
    if (!S)
      return true;
    Bytes += S->size() + 1;
    return S->size() <=
               ::neverd::limits::emulation::
                   kAndroidResolverValidateResolverQueriesTextTextSSizeLimit &&
           S->find('\0') == std::string::npos;
  };
  for (const auto &Q : Queries) {
    if (!Text(Q.Node) || !Text(Q.Service) || (!Q.Node && !Q.Service) ||
        !Keys.insert(key(Q)).second || (Q.Flags & ~SupportedFlags) ||
        (Q.Family != 0 && Q.Family != 2 && Q.Family != 10) ||
        (Q.SocketType != 0 && Q.SocketType != 1 && Q.SocketType != 2) ||
        (Q.Protocol != 0 && Q.Protocol != 6 && Q.Protocol != 17) ||
        Q.Error > 14 || Q.SystemErrno > 4095 ||
        (Q.Error == 11 ? !Q.SystemErrno : Q.SystemErrno != 0) ||
        (Q.Error ? !Q.Addresses.empty() : Q.Addresses.empty()) ||
        Q.Addresses.size() >
            ::neverd::limits::emulation::
                kAndroidResolverValidateResolverQueriesQAddressesSizeLimit)
      return failure("invalid resolver query contract");
    for (size_t I = 0; I < Q.Addresses.size(); ++I) {
      const auto &A = Q.Addresses[I];
      const auto Family = linux_model::networkAddressFamily(A.Address);
      if (!Family || (Q.Family && Q.Family != Family) ||
          (A.SocketType != 1 && A.SocketType != 2) ||
          A.Protocol != (A.SocketType == 1 ? 6u : 17u) ||
          (Q.SocketType && Q.SocketType != A.SocketType) ||
          (Q.Protocol && Q.Protocol != A.Protocol) ||
          (A.CanonicalName && (!(Q.Flags & 2) || I != 0)) ||
          !Text(A.CanonicalName))
        return failure("resolver result contradicts query or sockaddr ABI");
      Bytes += AddressInfoSize + A.Address.size();
    }
    if (Bytes > ::neverd::limits::emulation::
                    kAndroidResolverValidateResolverQueriesBytesLimit)
      return failure("resolver observation byte limit exceeded");
  }
  return llvm::Error::success();
}

BionicResult Bionic::addressText(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  const uint32_t Family = A[0], Capacity = A[3];
  const bool Parse = Call.Name == symbol::InetPton;
  auto Error = [&](uint32_t Number) -> BionicResult {
    if (auto E = setErrno(Number))
      return std::move(E);
    return value(Parse ? UINT64_MAX : 0);
  };
  if (Family != 2 && Family != 10)
    return Error(97); // EAFNOSUPPORT, before either pointer is read.
  if (Parse) {
    auto Text = string(A[1]);
    if (!Text)
      return Text.takeError();
    std::vector<uint8_t> Bytes;
    if (!(Family == 2 ? parseIPv4(*Text, Bytes) : parseIPv6(*Text, Bytes)))
      return value(0);
    if (auto E = access(A[2], Bytes.size(), Write))
      return std::move(E);
    if (auto E = CPU.write(A[2], Bytes))
      return std::move(E);
    return value(1);
  }
  std::vector<uint8_t> Bytes(Family == 2 ? 4 : 16);
  if (auto E = access(A[1], Bytes.size(), Read))
    return std::move(E);
  if (auto E = CPU.read(A[1], Bytes))
    return std::move(E);
  auto Decimal = [&](unsigned Offset) {
    std::string Text;
    for (unsigned I = 0; I != 4; ++I) {
      if (I)
        Text += '.';
      Text += std::to_string(Bytes[Offset + I]);
    }
    return Text;
  };
  std::string Text;
  if (Family == 2) {
    Text = Decimal(0);
  } else {
    uint16_t Words[8];
    unsigned BestStart = 8, BestLength = 1;
    for (unsigned I = 0; I != 8; ++I)
      Words[I] = llvm::support::endian::read16be(Bytes.data() + I * 2);
    for (unsigned I = 0; I < 8;) {
      const unsigned Start = I;
      while (I < 8 && Words[I] == 0)
        ++I;
      if (I - Start > BestLength) {
        BestStart = Start;
        BestLength = I - Start;
      }
      if (I == Start)
        ++I;
    }
    // Bionic prints compatible and mapped IPv4 suffixes as dotted decimal.
    const bool Dotted =
        BestStart == 0 &&
        (BestLength == 6 || (BestLength == 5 && Words[5] == 0xffff));
    auto Part = [&](unsigned Begin, unsigned End) {
      std::string S;
      for (unsigned I = Begin; I < End; ++I) {
        if (I != Begin)
          S += ':';
        if (Dotted && I == 6) {
          S += Decimal(12);
          break;
        }
        S += llvm::utohexstr(Words[I], true);
      }
      return S;
    };
    Text = BestStart == 8
               ? Part(0, 8)
               : Part(0, BestStart) + "::" + Part(BestStart + BestLength, 8);
  }
  if (Text.size() >= Capacity)
    return Error(28); // ENOSPC leaves the destination untouched.
  Text.push_back('\0');
  if (auto E = access(A[2], Text.size(), Write))
    return std::move(E);
  if (auto E = CPU.write(
          A[2], llvm::ArrayRef(reinterpret_cast<const uint8_t *>(Text.data()),
                               Text.size())))
    return std::move(E);
  return value(A[2]);
}

BionicResult Bionic::resolveAddress(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  // API 28 returns this error before examining hints or touching *res.
  if (!A[0] && !A[1])
    return value(8);
  uint8_t RootBytes[8]{};
  auto ErrorResult = [&](uint64_t Code) -> BionicResult {
    if (auto E = access(A[3], 8, Write))
      return std::move(E);
    if (auto E = CPU.write(A[3], RootBytes))
      return std::move(E);
    return value(Code);
  };
  AndroidResolverQuery Query;
  if (A[2]) {
    if (auto E = access(A[2], AddressInfoSize, Read))
      return std::move(E);
    uint8_t Hints[AddressInfoSize];
    if (auto E = CPU.read(A[2], Hints))
      return std::move(E);
    using llvm::support::endian::read32le;
    using llvm::support::endian::read64le;
    Query.Flags = read32le(Hints);
    Query.Family = read32le(Hints + 4);
    Query.SocketType = read32le(Hints + 8);
    Query.Protocol = read32le(Hints + 12);
    if (read32le(Hints + 16) || read64le(Hints + 24) || read64le(Hints + 32) ||
        read64le(Hints + 40))
      return ErrorResult(12); // EAI_BADHINTS
    if (Query.Flags & ~SupportedFlags)
      return ErrorResult(3); // EAI_BADFLAGS
    if (Query.Family != 0 && Query.Family != 2 && Query.Family != 10)
      return ErrorResult(5); // EAI_FAMILY
  }
  for (unsigned I = 0; I != 2; ++I) {
    if (!A[I])
      continue;
    auto Text = string(A[I]);
    if (!Text)
      return Text.takeError();
    (I ? Query.Service : Query.Node) = std::move(*Text);
  }
  const AndroidResolverQuery *Observation = nullptr;
  if (Options.Android->ResolverQueries)
    for (const auto &Q : *Options.Android->ResolverQueries)
      if (key(Q) == key(Query)) {
        Observation = &Q;
        break;
      }
  if (!Observation) {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = "getaddrinfo query has no exact offline observation";
    return std::optional<BionicValue>();
  }
  if (Observation->Error) {
    if (Observation->Error == 11)
      if (auto E = setErrno(Observation->SystemErrno))
        return std::move(E);
    return ErrorResult(Observation->Error);
  }
  if (auto E = access(A[3], 8, Write))
    return std::move(E);
  // Each node owns its sockaddr and optional string in one model allocation.
  // Publishing the list is transactional; allocation failure releases all
  // nodes.
  std::vector<uint64_t> Nodes;
  auto Rollback = [&]() -> llvm::Error {
    for (auto P : Nodes)
      if (auto E = release(P))
        return E;
    return llvm::Error::success();
  };
  for (const auto &Address : Observation->Addresses) {
    const uint64_t Size =
        AddressInfoSize + Address.Address.size() +
        (Address.CanonicalName ? Address.CanonicalName->size() + 1 : 0);
    auto P = allocate(Size);
    if (!P) {
      auto Error = P.takeError();
      return llvm::joinErrors(std::move(Error), Rollback());
    }
    if (!*P) {
      if (auto E = Rollback())
        return std::move(E);
      return ErrorResult(6); // EAI_MEMORY
    }
    Nodes.push_back(*P);
  }
  for (size_t I = 0; I < Nodes.size(); ++I) {
    const auto &Address = Observation->Addresses[I];
    std::vector<uint8_t> Bytes(
        AddressInfoSize + Address.Address.size() +
            (Address.CanonicalName ? Address.CanonicalName->size() + 1 : 0),
        0);
    using llvm::support::endian::write32le;
    using llvm::support::endian::write64le;
    write32le(Bytes.data(), Query.Flags);
    write32le(Bytes.data() + 4,
              linux_model::networkAddressFamily(Address.Address));
    write32le(Bytes.data() + 8, Address.SocketType);
    write32le(Bytes.data() + 12, Address.Protocol);
    write32le(Bytes.data() + 16, Address.Address.size());
    write64le(Bytes.data() + 32, Nodes[I] + AddressInfoSize);
    const uint64_t Next = I + 1 < Nodes.size() ? Nodes[I + 1] : 0;
    write64le(Bytes.data() + 40, Next);
    std::copy(Address.Address.begin(), Address.Address.end(),
              Bytes.begin() + AddressInfoSize);
    if (Address.CanonicalName) {
      const auto Offset = AddressInfoSize + Address.Address.size();
      write64le(Bytes.data() + 24, Nodes[I] + Offset);
      std::copy(Address.CanonicalName->begin(), Address.CanonicalName->end(),
                Bytes.begin() + Offset);
    }
    if (auto E = CPU.write(Nodes[I], Bytes))
      return llvm::joinErrors(std::move(E), Rollback());
  }
  llvm::support::endian::write64le(RootBytes, Nodes.front());
  if (auto E = CPU.write(A[3], RootBytes))
    return llvm::joinErrors(std::move(E), Rollback());
  for (size_t I = 0; I < Nodes.size(); ++I)
    ResolverNodes.emplace(Nodes[I], I + 1 < Nodes.size() ? Nodes[I + 1] : 0);
  return value(0);
}

BionicResult Bionic::freeAddresses(const NativeCallEvent &Call) {
  std::vector<uint64_t> Nodes;
  for (uint64_t P = Call.Arguments[0]; P;) {
    auto I = ResolverNodes.find(P);
    if (I == ResolverNodes.end() || !Allocations.contains(P))
      return failure("freeaddrinfo requires a live resolver-owned list");
    if (auto E = access(P, AddressInfoSize, Read))
      return std::move(E);
    uint8_t Link[8];
    if (auto E = CPU.read(P + 40, Link))
      return std::move(E);
    if (llvm::support::endian::read64le(Link) != I->second)
      return failure("modified addrinfo links have no owned release contract");
    Nodes.push_back(P);
    P = I->second;
  }
  for (auto P : Nodes) {
    if (auto E = release(P))
      return std::move(E);
    ResolverNodes.erase(P);
  }
  return value(0);
}
} // namespace neverd::emulation::android_model
