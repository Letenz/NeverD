//===- Backend.cpp - Where the MCP server's queries run -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Worker framing, and the client side of a workbench's shared session.
///
//===----------------------------------------------------------------------===//

#include "Backend.h"

#include "ResourceLimits.h"

#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <variant>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace neverd::binmcp {
namespace {
constexpr std::size_t MaxFrame = transport::MaxJsonBytes;

/// A string member, or empty when the member is absent or not a string.
std::string text(const Json &Object, const char *Key) {
  const auto It = Object.find(Key);
  return It != Object.end() && It->is_string() ? It->get<std::string>()
                                               : std::string();
}
} // namespace

std::string encodeFrame(const Json &Message) {
  const auto Body = transport::serializeJson(Message, MaxFrame);
  const auto Size = static_cast<std::uint32_t>(Body.size());
  std::string Frame{static_cast<char>(Size >> 24),
                    static_cast<char>(Size >> 16), static_cast<char>(Size >> 8),
                    static_cast<char>(Size)};
  return Frame + Body;
}

std::uint32_t frameLength(const unsigned char Header[4]) {
  const std::uint32_t Size = (std::uint32_t(Header[0]) << 24) |
                             (std::uint32_t(Header[1]) << 16) |
                             (std::uint32_t(Header[2]) << 8) | Header[3];
  if (Size == 0 || Size > MaxFrame)
    throw std::runtime_error("Invalid worker frame size");
  return Size;
}

//===----------------------------------------------------------------------===//
// A workbench's shared session
//===----------------------------------------------------------------------===//

/// The connection and a reader that queues each frame, or the failure that
/// ended it, so that every wait for one is bounded.
struct AttachBackend::Channel {
#ifdef _WIN32
  HANDLE Pipe = INVALID_HANDLE_VALUE;
  /// Set when the session closes. Cancelling the pipe's I/O then would miss
  /// a read the reader starts just after, which would wait on the workbench.
  HANDLE Stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ~Channel() {
    if (Stop)
      CloseHandle(Stop);
  }
#else
  int Socket = -1;
#endif
  std::chrono::seconds Timeout{
      ::neverd::limits::mcp::kDefaultAttachTimeoutSeconds};
  std::mutex Lock;
  std::condition_variable Arrived;
  std::deque<std::variant<Json, std::string>> Messages;
  /// Why the session can take no further request.
  std::optional<std::string> Ended;
  bool Closed = false;
  std::thread Reader;

#ifdef _WIN32
  // The pipe is open for overlapped I/O: synchronous I/O on one handle is
  // serialized, so a pending read would hold every write back.
  bool transfer(bool Write, void *Data, std::size_t Size, std::size_t &Done) {
    // Once the session closes no transfer starts, and one that waits ends.
    if (WaitForSingleObject(Stop, 0) == WAIT_OBJECT_0)
      return false;
    OVERLAPPED Pending{};
    Pending.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!Pending.hEvent)
      return false;
    const auto Count = static_cast<DWORD>(Size);
    const BOOL Started = Write ? WriteFile(Pipe, Data, Count, nullptr, &Pending)
                               : ReadFile(Pipe, Data, Count, nullptr, &Pending);
    const bool Waiting = !Started && GetLastError() == ERROR_IO_PENDING;
    const HANDLE Events[] = {Pending.hEvent, Stop};
    if (Waiting &&
        WaitForMultipleObjects(2, Events, FALSE, INFINITE) != WAIT_OBJECT_0)
      CancelIoEx(Pipe, &Pending);
    DWORD Moved = 0;
    const bool Finished = (Started || Waiting) &&
                          GetOverlappedResult(Pipe, &Pending, &Moved, TRUE);
    CloseHandle(Pending.hEvent);
    Done = Moved;
    return Finished && Moved > 0;
  }
#endif

  bool readExact(unsigned char *Into, std::size_t Size) {
    std::size_t Done = 0;
    while (Done < Size) {
#ifdef _WIN32
      std::size_t Count = 0;
      if (!transfer(false, Into + Done, Size - Done, Count))
        return false;
#else
      const auto Count = ::read(Socket, Into + Done, Size - Done);
      if (Count < 0 && errno == EINTR)
        continue;
      if (Count <= 0)
        return false;
#endif
      Done += static_cast<std::size_t>(Count);
    }
    return true;
  }

  void write(const std::string &Bytes) {
    std::size_t Done = 0;
    while (Done < Bytes.size()) {
#ifdef _WIN32
      std::size_t Count = 0;
      if (!transfer(true, const_cast<char *>(Bytes.data()) + Done,
                    Bytes.size() - Done, Count))
        throw std::runtime_error("NeverD session closed");
#else
#ifdef MSG_NOSIGNAL
      constexpr int Flags = MSG_NOSIGNAL;
#else
      constexpr int Flags = 0;
#endif
      const auto Count =
          ::send(Socket, Bytes.data() + Done, Bytes.size() - Done, Flags);
      if (Count < 0 && errno == EINTR)
        continue;
      if (Count <= 0)
        throw std::runtime_error("NeverD session closed");
#endif
      Done += static_cast<std::size_t>(Count);
    }
  }

  void read() {
    std::string Reason = "NeverD session closed";
    try {
      for (;;) {
        unsigned char Header[4];
        if (!readExact(Header, 4))
          break;
        std::string Body(frameLength(Header), '\0');
        if (!readExact(reinterpret_cast<unsigned char *>(Body.data()),
                       Body.size()))
          break;
        auto Message = transport::parseJson(Body, MaxFrame);
        if (!Message.is_object())
          throw std::runtime_error("Worker frame must be an object");
        const auto Type = text(Message, "type");
        // Idle liveness events must not fill the queue.
        if (Type == "heartbeat" || Type == "progress" || Type == "revision")
          continue;
        std::lock_guard Guard(Lock);
        if (Messages.size() >= ::neverd::limits::mcp::kBackendMaxQueued)
          throw std::runtime_error("NeverD session sent more than was asked");
        Messages.emplace_back(std::move(Message));
        Arrived.notify_all();
      }
    } catch (const std::exception &Failure) {
      Reason = Failure.what();
    }
    std::lock_guard Guard(Lock);
    Messages.emplace_back(std::move(Reason));
    Arrived.notify_all();
  }

  void close() {
    {
      std::lock_guard Guard(Lock);
      if (Closed)
        return;
      Closed = true;
      if (!Ended)
        Ended = "NeverD session has ended";
    }
#ifdef _WIN32
    SetEvent(Stop);
#else
    if (Socket >= 0)
      ::shutdown(Socket, SHUT_RDWR);
#endif
    if (Reader.joinable())
      Reader.join();
#ifdef _WIN32
    if (Pipe != INVALID_HANDLE_VALUE)
      CloseHandle(Pipe);
#else
    if (Socket >= 0)
      ::close(Socket);
#endif
  }
};

AttachBackend::AttachBackend(const std::filesystem::path &Credentials,
                             std::chrono::seconds Timeout)
    : Session(std::make_unique<Channel>()) {
  Session->Timeout = Timeout;
#ifndef _WIN32
  struct stat Status{};
  if (::stat(Credentials.c_str(), &Status) != 0)
    throw std::runtime_error("Cannot read the session credential file");
  if (Status.st_mode & 077)
    throw std::runtime_error(
        "Session credential file must be private to its owner");
#endif
  std::ifstream File(Credentials, std::ios::binary);
  if (!File)
    throw std::runtime_error("Cannot read the session credential file");
  const std::string Text((std::istreambuf_iterator<char>(File)),
                         std::istreambuf_iterator<char>());
  Json Saved;
  try {
    Saved = transport::parseJson(Text, 64 * 1024);
  } catch (const std::exception &) {
  }
  if (!Saved.is_object() || !Saved.contains("endpoint") ||
      !Saved["endpoint"].is_string() || !Saved.contains("token") ||
      !Saved["token"].is_string())
    throw std::runtime_error("Malformed session credential file");
  const auto Endpoint = Saved["endpoint"].get<std::string>();
#ifdef _WIN32
  if (!Session->Stop)
    throw std::runtime_error("Cannot connect to the shared session");
  const std::string Name = Endpoint.rfind("\\\\.\\pipe\\", 0) == 0
                               ? Endpoint
                               : "\\\\.\\pipe\\" + Endpoint;
  const int Wide = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                       Name.c_str(), -1, nullptr, 0);
  std::wstring WideName(Wide > 0 ? Wide : 1, L'\0');
  if (Wide <= 0 ||
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Name.c_str(), -1,
                          WideName.data(), Wide) != Wide)
    throw std::runtime_error("Malformed session endpoint");
  Session->Pipe =
      CreateFileW(WideName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                  OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
  if (Session->Pipe == INVALID_HANDLE_VALUE)
    throw std::runtime_error("Cannot connect to the shared session");
#else
  sockaddr_un Address{};
  Address.sun_family = AF_UNIX;
  if (Endpoint.empty() || Endpoint.size() >= sizeof(Address.sun_path))
    throw std::runtime_error("Malformed session endpoint");
  std::memcpy(Address.sun_path, Endpoint.data(), Endpoint.size());
  Session->Socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (Session->Socket < 0 ||
      ::connect(Session->Socket, reinterpret_cast<sockaddr *>(&Address),
                sizeof(Address)) != 0) {
    if (Session->Socket >= 0)
      ::close(Session->Socket);
    Session->Socket = -1;
    throw std::runtime_error("Cannot connect to the shared session");
  }
#endif
  Session->Reader = std::thread([Channel = Session.get()] { Channel->read(); });
  try {
    Session->write(encodeFrame({{"protocol_major", 1},
                                {"request_id", "auth"},
                                {"operation", "authenticate"},
                                {"payload", {{"token", Saved["token"]}}}}));
    const auto Hello = next();
    if (Hello.value("protocol_major", Json()) != 1 ||
        text(Hello, "type") != "hello")
      throw std::runtime_error("Incompatible NeverD worker handshake");
  } catch (...) {
    Session->close();
    throw;
  }
}

AttachBackend::~AttachBackend() { Session->close(); }

Json AttachBackend::next() {
  std::unique_lock Guard(Session->Lock);
  if (!Session->Arrived.wait_for(Guard, Session->Timeout,
                                 [&] { return !Session->Messages.empty(); })) {
    // A late answer must not stand in for a later request's.
    Session->Ended = "NeverD session request timed out";
    throw std::runtime_error(*Session->Ended);
  }
  auto Message = std::move(Session->Messages.front());
  Session->Messages.pop_front();
  if (auto *Failure = std::get_if<std::string>(&Message)) {
    Session->Ended = *Failure;
    throw std::runtime_error(*Failure);
  }
  return std::get<Json>(std::move(Message));
}

Json AttachBackend::query(const std::string &Operation, const Json &Payload,
                          const std::optional<std::string> &ExpectedRevision) {
  {
    std::lock_guard Guard(Session->Lock);
    if (Session->Ended)
      throw std::runtime_error(*Session->Ended);
  }
  const auto ID = std::to_string(++Serial);
  Json Request{{"protocol_major", 1},
               {"request_id", ID},
               {"operation", Operation},
               {"payload", Payload}};
  if (ExpectedRevision)
    Request["expected_revision"] = *ExpectedRevision;
  Session->write(encodeFrame(Request));
  for (;;) {
    auto Message = next();
    const auto Type = text(Message, "type");
    if (Type == "fatal") {
      std::lock_guard Guard(Session->Lock);
      Session->Ended =
          "NeverD session failed: " + Message.value("error", Message).dump();
      throw std::runtime_error(*Session->Ended);
    }
    // A load reports its progress as responses to the request; they are not
    // its result.
    if (Type == "response" && Message.value("request_id", Json()) == ID &&
        text(Message, "status") != "progress")
      return Message;
  }
}

} // namespace neverd::binmcp
