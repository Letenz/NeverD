//===- AttachTests.cpp - A workbench's shared session ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Worker framing, and the client of a shared session against a stand-in
/// workbench on a local socket.
///
//===----------------------------------------------------------------------===//

#include "Backend.h"
#include "Testing.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

using namespace neverd::binmcp;
using neverd::binmcp::test::require;
namespace transport = neverd::transport;

MCP_TEST(FramesAreBoundedAndRoundTrip) {
  const Json Message{{"address", "0xffffffffffffffff"},
                     {"text", "\xE4\xB8\xAD\xE6\x96\x87\nsecond line"}};
  const auto Frame = encodeFrame(Message);
  const auto *Header = reinterpret_cast<const unsigned char *>(Frame.data());
  require(frameLength(Header) == Frame.size() - 4,
          "the header counts the body");
  require(transport::parseJson(Frame.substr(4)) == Message, "the body");
  for (const std::uint32_t Size : {0u, 8u * 1024 * 1024 + 1}) {
    const unsigned char Bad[4] = {static_cast<unsigned char>(Size >> 24),
                                  static_cast<unsigned char>(Size >> 16),
                                  static_cast<unsigned char>(Size >> 8),
                                  static_cast<unsigned char>(Size)};
    bool Refused = false;
    try {
      (void)frameLength(Bad);
    } catch (const std::runtime_error &) {
      Refused = true;
    }
    require(Refused, "a frame of " + std::to_string(Size) + " bytes");
  }
}

namespace {
#ifdef _WIN32
/// The workbench's end of a shared session.
using Connection = HANDLE;

bool readAll(Connection Pipe, void *Into, std::size_t Size) {
  auto *Bytes = static_cast<char *>(Into);
  while (Size) {
    DWORD Count = 0;
    if (!ReadFile(Pipe, Bytes, static_cast<DWORD>(Size), &Count, nullptr) ||
        !Count)
      return false;
    Bytes += Count;
    Size -= Count;
  }
  return true;
}

bool writeAll(Connection Pipe, const void *From, std::size_t Size) {
  DWORD Count = 0;
  return WriteFile(Pipe, From, static_cast<DWORD>(Size), &Count, nullptr) &&
         Count == Size;
}
#else
using Connection = int;

bool readAll(Connection Socket, void *Into, std::size_t Size) {
  return ::recv(Socket, Into, Size, MSG_WAITALL) == static_cast<ssize_t>(Size);
}

bool writeAll(Connection Socket, const void *From, std::size_t Size) {
  return ::write(Socket, From, Size) == static_cast<ssize_t>(Size);
}
#endif

void sendFrame(Connection Peer, const Json &Message) {
  const auto Bytes = encodeFrame(Message);
  require(writeAll(Peer, Bytes.data(), Bytes.size()),
          "the workbench writes a frame");
}

Json receiveFrame(Connection Peer) {
  unsigned char Header[4];
  require(readAll(Peer, Header, 4), "a frame header");
  std::string Body(frameLength(Header), '\0');
  require(readAll(Peer, Body.data(), Body.size()), "a frame body");
  return transport::parseJson(Body);
}

/// A workbench sharing its session, on a local socket or a named pipe: it
/// serves one connection with \p Serve and writes the credential file.
class Workbench {
  std::filesystem::path Root;
  std::atomic<bool> Accepted = false;
  std::thread Peer;
#ifdef _WIN32
  std::wstring Pipe;
  HANDLE Listener = INVALID_HANDLE_VALUE;
#else
  int Listener = -1;
#endif

public:
  std::filesystem::path Credentials;

  explicit Workbench(std::function<void(Connection)> Serve,
                     unsigned Mode = 0600) {
    static std::atomic<unsigned> Serial = 0;
#ifdef _WIN32
    const auto Process = std::to_string(GetCurrentProcessId());
#else
    const auto Process = std::to_string(::getpid());
#endif
    const std::string Name =
        "neverd-mcp-attach-" + Process + "-" + std::to_string(++Serial);
    Root = std::filesystem::temp_directory_path() / Name;
    std::filesystem::create_directories(Root);
#ifdef _WIN32
    (void)Mode;
    // The client adds the pipe namespace to a bare name.
    const std::string Endpoint = Name;
    Pipe = L"\\\\.\\pipe\\" + std::wstring(Name.begin(), Name.end());
    Listener = CreateNamedPipeW(Pipe.c_str(), PIPE_ACCESS_DUPLEX,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                1, 1 << 16, 1 << 16, 0, nullptr);
    require(Listener != INVALID_HANDLE_VALUE, "the workbench listens");
#else
    const auto Endpoint = (Root / "s").string();
    sockaddr_un Address{};
    Address.sun_family = AF_UNIX;
    require(Endpoint.size() < sizeof(Address.sun_path), "a short socket path");
    Endpoint.copy(Address.sun_path, Endpoint.size());
    Listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    require(Listener >= 0 &&
                ::bind(Listener, reinterpret_cast<sockaddr *>(&Address),
                       sizeof(Address)) == 0 &&
                ::listen(Listener, 1) == 0,
            "the workbench listens");
#endif
    Credentials = Root / "credentials.json";
    std::ofstream(Credentials)
        << Json{{"endpoint", Endpoint}, {"token", "secret"}}.dump();
#ifndef _WIN32
    ::chmod(Credentials.c_str(), Mode);
#endif
    Peer = std::thread([this, Serve = std::move(Serve)] {
#ifdef _WIN32
      const bool Connected = ConnectNamedPipe(Listener, nullptr) ||
                             GetLastError() == ERROR_PIPE_CONNECTED;
      Accepted = true;
      if (!Connected)
        return;
      try {
        Serve(Listener);
      } catch (const std::exception &) {
        // The client went away; its own checks report what it saw.
      }
      DisconnectNamedPipe(Listener);
#else
      const int Connection = ::accept(Listener, nullptr, nullptr);
      Accepted = true;
      if (Connection < 0)
        return;
      try {
        Serve(Connection);
      } catch (const std::exception &) {
        // The client went away; its own checks report what it saw.
      }
      ::close(Connection);
#endif
    });
  }

  ~Workbench() {
    if (!Accepted) {
      // Wake the accept that no client made.
#ifdef _WIN32
      const HANDLE Wake =
          CreateFileW(Pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                      OPEN_EXISTING, 0, nullptr);
      if (Wake != INVALID_HANDLE_VALUE)
        CloseHandle(Wake);
#else
      sockaddr_un Address{};
      Address.sun_family = AF_UNIX;
      const auto Endpoint = (Root / "s").string();
      Endpoint.copy(Address.sun_path, Endpoint.size());
      const int Wake = ::socket(AF_UNIX, SOCK_STREAM, 0);
      (void)::connect(Wake, reinterpret_cast<sockaddr *>(&Address),
                      sizeof(Address));
      ::close(Wake);
#endif
    }
    Peer.join();
#ifdef _WIN32
    CloseHandle(Listener);
#else
    ::close(Listener);
#endif
    std::error_code Ignored;
    std::filesystem::remove_all(Root, Ignored);
  }
};

/// The handshake a workbench answers an authenticated client with.
void greet(Connection Socket) {
  const auto Auth = receiveFrame(Socket);
  require(Auth["operation"] == "authenticate" &&
              Auth["payload"]["token"] == "secret",
          "the client authenticates with the token");
  sendFrame(Socket, {{"protocol_major", 1}, {"type", "hello"}});
}

/// Waits for the client to close its side.
void drain(Connection Socket) {
  char Byte;
  while (readAll(Socket, &Byte, 1)) {
  }
}

std::string failure(const std::function<void()> &Run) {
  try {
    Run();
  } catch (const std::runtime_error &Error) {
    return Error.what();
  }
  return {};
}
} // namespace

MCP_TEST(IdleEventsAndLoadProgressDoNotReplaceTheResult) {
  std::promise<Json> Request;
  auto Seen = Request.get_future();
  Workbench Peer([&](Connection Socket) {
    greet(Socket);
    Request.set_value(receiveFrame(Socket));
    for (int I = 0; I < 100; ++I)
      sendFrame(Socket, {{"type", "heartbeat"}, {"revision", "1"}});
    sendFrame(Socket, {{"type", "revision"}, {"revision", "2"}});
    for (int Done = 0; Done < 2; ++Done)
      sendFrame(Socket, {{"type", "response"},
                         {"request_id", "1"},
                         {"status", "progress"},
                         {"payload", {{"phase", "image"}, {"done", Done}}}});
    sendFrame(Socket, {{"type", "response"},
                       {"request_id", "0"},
                       {"status", "ok"},
                       {"payload", {{"stale", true}}}});
    sendFrame(Socket, {{"type", "response"},
                       {"request_id", "1"},
                       {"status", "ok"},
                       {"payload", {{"opened", true}}}});
    drain(Socket);
  });
  AttachBackend Session(Peer.Credentials);
  require(Session.attached(), "a shared session is attached");
  const auto Result = Session.query("metadata", Json::object(), "7");
  require(Result["status"] == "ok" &&
              Result["payload"] == Json{{"opened", true}},
          "the result: " + Result.dump());
  const auto Sent = Seen.get();
  require(Sent["operation"] == "metadata" && Sent["request_id"] == "1" &&
              Sent["expected_revision"] == "7" && Sent["protocol_major"] == 1,
          "the request: " + Sent.dump());
}

#ifndef _WIN32
MCP_TEST(ACredentialFileOthersCanReadIsRefused) {
  Workbench Peer([](Connection) {}, 0644);
  require(failure([&] { AttachBackend Session(Peer.Credentials); }) ==
              "Session credential file must be private to its owner",
          "a readable credential file is refused");
}
#endif

MCP_TEST(AnIncompatibleHandshakeIsRefused) {
  Workbench Peer([](Connection Socket) {
    (void)receiveFrame(Socket);
    sendFrame(Socket, {{"protocol_major", 2}, {"type", "hello"}});
    drain(Socket);
  });
  require(failure([&] { AttachBackend Session(Peer.Credentials); }) ==
              "Incompatible NeverD worker handshake",
          "another protocol is refused");
}

MCP_TEST(AFatalMessageOrATimeoutEndsTheSession) {
  Workbench Fatal([](Connection Socket) {
    greet(Socket);
    (void)receiveFrame(Socket);
    sendFrame(Socket, {{"type", "fatal"}, {"error", {{"code", "crashed"}}}});
    drain(Socket);
  });
  AttachBackend Session(Fatal.Credentials);
  const auto First = failure([&] { Session.query("metadata", {}, {}); });
  require(First.rfind("NeverD session failed: ", 0) == 0, First);
  require(failure([&] { Session.query("metadata", {}, {}); }) == First,
          "the session stays ended");
  Workbench Silent([](Connection Socket) {
    greet(Socket);
    drain(Socket);
  });
  AttachBackend Slow(Silent.Credentials, std::chrono::seconds(1));
  require(failure([&] { Slow.query("metadata", {}, {}); }) ==
              "NeverD session request timed out",
          "an unanswered request times out");
  require(failure([&] { Slow.query("metadata", {}, {}); }) ==
              "NeverD session request timed out",
          "a timed-out session takes no further request");
}
