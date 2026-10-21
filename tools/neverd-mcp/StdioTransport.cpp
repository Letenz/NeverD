//===- StdioTransport.cpp - MCP newline transport -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A reader thread, one executor, cancellation and the pending bound.
///
//===----------------------------------------------------------------------===//

#include "StdioTransport.h"

#include "ResourceLimits.h"
#include "Server.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace neverd::binmcp {
namespace {
/// How long requests must pause before idle analysis starts, so that a
/// client's burst of reads does not queue behind it.
constexpr auto IdleGracePeriod = std::chrono::milliseconds(
    ::neverd::limits::mcp::kStdioIdleGracePeriodMilliseconds);

Json failure(const Json &ID, int Code, std::string Message) {
  return {{"jsonrpc", "2.0"},
          {"id", ID},
          {"error", {{"code", Code}, {"message", std::move(Message)}}}};
}

/// What the reader and the executor share.  The reader owns a reference, so
/// it may outlive serve() while it waits on input that never comes.
struct Channel {
  Source Read;
  Sink Write;
  std::mutex Lock;
  std::condition_variable Changed;
  struct Job {
    Json Request, ID;
    std::string Slot;
    std::shared_ptr<std::atomic<bool>> Cancelled;
  };
  std::deque<Job> Queue;
  /// Each waiting or running request by its id, or by a serial for one
  /// without an id.
  std::map<std::string, std::shared_ptr<std::atomic<bool>>> Pending;
  std::uint64_t Anonymous = 0;
  bool InputEnded = false;
  std::mutex OutputLock;
  bool OutputClosed = false;

  void emit(const Json &Message) {
    std::lock_guard Guard(OutputLock);
    if (!OutputClosed &&
        !Write(Message.dump(-1, ' ', false, Json::error_handler_t::replace) +
               '\n'))
      OutputClosed = true;
  }

  bool outputClosed() {
    std::lock_guard Guard(OutputLock);
    return OutputClosed;
  }

  void take(std::string_view Line) {
    if (!Line.empty() && Line.back() == '\r')
      Line.remove_suffix(1);
    Json Request;
    try {
      Request = transport::parseJson(Line, MaxLine);
    } catch (const std::exception &) {
      emit(failure(nullptr, -32700, "Invalid JSON"));
      return;
    }
    if (Request.is_object() &&
        member(Request, "method") == "notifications/cancelled") {
      const auto &ID = member(member(Request, "params"), "requestId");
      if (ID.is_string() || ID.is_number_integer()) {
        std::lock_guard Guard(Lock);
        if (const auto It = Pending.find(ID.dump()); It != Pending.end())
          It->second->store(true);
      }
      return;
    }
    const Json ID = member(Request, "id");
    if (!ID.is_null() && !ID.is_string() && !ID.is_number_integer()) {
      emit(failure(nullptr, -32600, "Invalid request id"));
      return;
    }
    auto Cancelled = std::make_shared<std::atomic<bool>>(false);
    bool Overloaded;
    {
      std::lock_guard Guard(Lock);
      const auto Slot =
          ID.is_null() ? "#" + std::to_string(++Anonymous) : ID.dump();
      Overloaded = Pending.size() >= MaxPending || Pending.count(Slot);
      if (!Overloaded) {
        Pending.emplace(Slot, Cancelled);
        Queue.push_back({std::move(Request), ID, Slot, Cancelled});
      }
    }
    if (Overloaded) {
      if (!ID.is_null())
        emit(failure(ID, -32000, "Request queue full or duplicate id"));
      return;
    }
    Changed.notify_all();
  }

  void read() {
    std::string Line;
    std::vector<char> Chunk(64 * 1024);
    bool Whole = true;
    while (Whole) {
      const auto Count = Read(Chunk.data(), Chunk.size());
      if (Count == 0)
        break;
      std::string_view Bytes(Chunk.data(), Count);
      while (Whole) {
        const auto End = Bytes.find('\n');
        Line.append(Bytes.substr(0, End));
        if (Line.size() >= MaxLine) {
          // The rest of the stream cannot be framed.
          emit(failure(nullptr, -32600, "MCP message exceeds 1 MiB"));
          Whole = false;
          Line.clear();
          break;
        }
        if (End == std::string_view::npos)
          break;
        take(Line);
        Line.clear();
        Bytes.remove_prefix(End + 1);
      }
    }
    if (!Line.empty())
      take(Line);
    {
      std::lock_guard Guard(Lock);
      InputEnded = true;
    }
    Changed.notify_all();
  }

  void respond(Server &Service, const Job &Next) {
    if (!Next.Cancelled->load()) {
      Json Response;
      try {
        Response = {{"jsonrpc", "2.0"},
                    {"id", Next.ID},
                    {"result", Service.handle(Next.Request)}};
      } catch (const RpcError &Error) {
        Response = failure(Next.ID, Error.Code, Error.Message);
      } catch (const std::exception &Error) {
        Response = failure(Next.ID, -32603, Error.what());
      }
      // A notification has no response, unless it was no request at all.
      const bool Invalid = member(member(Response, "error"), "code") == -32600;
      if (!Next.Cancelled->load() &&
          (Invalid || !Next.Request.is_object() || Next.Request.contains("id")))
        emit(Response);
    }
    std::lock_guard Guard(Lock);
    Pending.erase(Next.Slot);
  }
};
} // namespace

int serve(Server &Service, Backend &Session, Source Read, Sink Write) {
  auto Shared = std::make_shared<Channel>();
  Shared->Read = std::move(Read);
  Shared->Write = std::move(Write);
  std::thread Reader([Shared] { Shared->read(); });
  auto &State = *Shared;
  while (!State.outputClosed()) {
    Channel::Job Next;
    {
      std::unique_lock Guard(State.Lock);
      const auto Waiting = [&] {
        return State.InputEnded || !State.Queue.empty();
      };
      if (!Waiting() && Session.hasIdleWork())
        State.Changed.wait_for(Guard, IdleGracePeriod, Waiting);
      while (!Waiting() && Session.hasIdleWork()) {
        Guard.unlock();
        const bool Advanced = Session.idleStep();
        Guard.lock();
        // A step that could not advance is retried shortly, not at once.
        if (!Advanced)
          State.Changed.wait_for(
              Guard,
              std::chrono::milliseconds(
                  ::neverd::limits::mcp::kStdioServeMilliseconds),
              Waiting);
      }
      State.Changed.wait(Guard, Waiting);
      if (State.Queue.empty())
        break;
      Next = std::move(State.Queue.front());
      State.Queue.pop_front();
    }
    State.respond(Service, Next);
  }
  bool InputEnded;
  {
    std::lock_guard Guard(State.Lock);
    InputEnded = State.InputEnded;
  }
  // The reader may wait on input that never comes once the output closed;
  // the process ends with it.
  if (InputEnded)
    Reader.join();
  else
    Reader.detach();
  return State.outputClosed() ? 1 : 0;
}

} // namespace neverd::binmcp
