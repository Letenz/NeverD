//===- ProcessTests.cpp - The neverd-mcp executable -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Launches the server on this test's own executable, with the real engine,
/// and reads it as a client does.
///
//===----------------------------------------------------------------------===//

#include "common/transport/Json.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
#endif

namespace {
#ifndef _WIN32
using neverd::transport::Json;

void require(bool Condition, const std::string &Message) {
  if (!Condition)
    throw std::runtime_error(Message);
}

class Process {
  int Input = -1, Output = -1, Errors = -1;
  pid_t PID = -1;
  std::string Pending;

public:
  explicit Process(const std::vector<std::string> &Arguments) {
    int In[2], Out[2], Err[2];
    require(::pipe(In) == 0 && ::pipe(Out) == 0 && ::pipe(Err) == 0,
            "pipes for the server");
    posix_spawn_file_actions_t Actions;
    posix_spawn_file_actions_init(&Actions);
    posix_spawn_file_actions_adddup2(&Actions, In[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&Actions, Out[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&Actions, Err[1], STDERR_FILENO);
    for (const int FD : {In[0], In[1], Out[0], Out[1], Err[0], Err[1]})
      posix_spawn_file_actions_addclose(&Actions, FD);
    std::vector<char *> Argv;
    for (const auto &Argument : Arguments)
      Argv.push_back(const_cast<char *>(Argument.c_str()));
    Argv.push_back(nullptr);
    const int Status =
        posix_spawn(&PID, Argv[0], &Actions, nullptr, Argv.data(), environ);
    posix_spawn_file_actions_destroy(&Actions);
    ::close(In[0]);
    ::close(Out[1]);
    ::close(Err[1]);
    Input = In[1];
    Output = Out[0];
    Errors = Err[0];
    require(Status == 0, "the server starts");
  }

  ~Process() {
    for (const int FD : {Input, Output, Errors})
      if (FD >= 0)
        ::close(FD);
    if (PID > 0) {
      ::kill(PID, SIGKILL);
      while (::waitpid(PID, nullptr, 0) < 0 && errno == EINTR) {
      }
    }
  }

  void send(const Json &Message) {
    const auto Line = Message.dump() + "\n";
    require(::write(Input, Line.data(), Line.size()) ==
                static_cast<ssize_t>(Line.size()),
            "a request is written");
  }

  /// The next line the server writes, which must be one JSON message.
  Json receive(std::chrono::seconds Limit = std::chrono::seconds(90)) {
    const auto Deadline = std::chrono::steady_clock::now() + Limit;
    for (;;) {
      if (const auto End = Pending.find('\n'); End != std::string::npos) {
        const auto Line = Pending.substr(0, End);
        Pending.erase(0, End + 1);
        return neverd::transport::parseJson(Line);
      }
      const auto Left = std::chrono::duration_cast<std::chrono::milliseconds>(
                            Deadline - std::chrono::steady_clock::now())
                            .count();
      require(Left > 0, "the server answers in time");
      pollfd Ready{Output, POLLIN, 0};
      if (::poll(&Ready, 1, static_cast<int>(Left)) <= 0)
        continue;
      char Buffer[65536];
      const auto Count = ::read(Output, Buffer, sizeof(Buffer));
      require(Count > 0, "the server keeps its output open");
      Pending.append(Buffer, static_cast<std::size_t>(Count));
    }
  }

  struct Exit {
    int Status;
    std::string Output, Diagnostics;
  };
  /// Closes the input, and returns the exit status and the rest of the
  /// output and of stderr.
  Exit finish() {
    ::close(Input);
    Input = -1;
    std::string Rest, Diagnostics;
    char Buffer[4096];
    for (ssize_t Count; (Count = ::read(Output, Buffer, sizeof(Buffer))) > 0;)
      Rest.append(Buffer, static_cast<std::size_t>(Count));
    for (ssize_t Count; (Count = ::read(Errors, Buffer, sizeof(Buffer))) > 0;)
      Diagnostics.append(Buffer, static_cast<std::size_t>(Count));
    int Status = 0;
    while (::waitpid(PID, &Status, 0) < 0 && errno == EINTR) {
    }
    PID = -1;
    return {WIFEXITED(Status) ? WEXITSTATUS(Status) : -1, Pending + Rest,
            Diagnostics};
  }
};

Json request(int ID, const char *Method, Json Params = Json::object()) {
  return {{"jsonrpc", "2.0"},
          {"id", ID},
          {"method", Method},
          {"params", std::move(Params)}};
}

Json call(Process &Server, int ID, const char *Tool, Json Arguments) {
  Server.send(request(ID, "tools/call",
                      {{"name", Tool}, {"arguments", std::move(Arguments)}}));
  const auto Reply = Server.receive();
  require(Reply["id"] == ID && Reply.contains("result"),
          std::string(Tool) + ": " + Reply.dump());
  return Reply["result"];
}

std::string text(const Json &Result) {
  return Result["content"][0]["text"].get<std::string>();
}

void launchArguments(const std::string &Binary) {
  for (const auto &[Arguments, Expected] :
       std::vector<std::pair<std::vector<std::string>, int>>{
           {{Binary, "--help"}, 0},
           {{Binary}, 2},
           {{Binary, "--worker", "/bin/true", "--file", Binary}, 2},
           {{Binary, "--file", Binary, "--attach", Binary}, 2},
           {{Binary, "--file", "/nonexistent/neverd-input"}, 1}}) {
    Process Server(Arguments);
    const auto Ended = Server.finish();
    require(Ended.Status == Expected, Arguments.back() + " exits " +
                                          std::to_string(Ended.Status) + ": " +
                                          Ended.Diagnostics);
  }
}

void session(const std::string &Binary, const std::string &Input) {
  Process Server({Binary, "--stdio", "--file", Input});
  Server.send(request(1, "initialize",
                      {{"protocolVersion", "2025-11-25"},
                       {"capabilities", Json::object()},
                       {"clientInfo", {{"name", "test"}, {"version", "1"}}}}));
  require(Server.receive()["result"]["protocolVersion"] == "2025-11-25",
          "initialize");
  Server.send({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
  Server.send(request(2, "tools/list"));
  require(Server.receive()["result"]["tools"].size() == 13, "thirteen tools");

  const auto Metadata = neverd::transport::parseJson(
      text(call(Server, 3, "project_metadata", Json::object())));
  require(Metadata.contains("revision") && Metadata.contains("sha256"),
          "metadata: " + Metadata.dump());
  const auto Functions = neverd::transport::parseJson(
      text(call(Server, 4, "list_functions", {{"filter", "main"}})));
  require(Functions["columns"][0] == "address" && !Functions["rows"].empty(),
          "functions: " + Functions.dump());

  const auto Source =
      call(Server, 5, "read_decompilation", {{"address", "main"}});
  const auto Whole = text(Source);
  require(Source["isError"] == false, "decompile main: " + Whole);
  const auto Header =
      neverd::transport::parseJson(Whole.substr(0, Whole.find('\n')));
  require(Header.contains("address") && Header.contains("total_lines"),
          "the code header: " + Header.dump());

  const auto Tree = call(Server, 6, "list_call_tree", {{"function", "main"}});
  require(Tree["isError"] == false, "call tree: " + text(Tree));
  const auto Disassembly =
      call(Server, 7, "read_disassembly", {{"address", "main"}, {"limit", 4}});
  require(Disassembly["isError"] == false &&
              text(Disassembly).find('\n') != std::string::npos,
          "disassembly: " + text(Disassembly));
  // The usage text below is in this executable.
  const auto Found = neverd::transport::parseJson(
      text(call(Server, 10, "search",
                {{"kind", "text"}, {"pattern", "usage: neverd-mcp"}})));
  require(Found["columns"][1] == "context" && !Found["rows"].empty(),
          "a text search: " + Found.dump());
  const auto Unknown = call(Server, 8, "read_decompilation",
                            {{"address", "no_such_function_anywhere"}});
  require(Unknown["isError"] == true, "an unknown name is an error");

  Server.send(
      request(9, "resources/read", {{"uri", "neverd://project/metadata"}}));
  const auto Resource = Server.receive();
  require(Resource["result"]["contents"][0]["mimeType"] == "application/json",
          "the metadata resource: " + Resource.dump());
  const auto Ended = Server.finish();
  require(Ended.Status == 0 && Ended.Output.empty(),
          "the server ends with its input: " + Ended.Diagnostics);
}
#endif
} // namespace

int main(int Count, char **Arguments) {
#ifdef _WIN32
  (void)Count;
  (void)Arguments;
  std::cout << "SKIP: the process test runs on POSIX hosts\n";
  return 77;
#else
  if (Count != 2) {
    std::cerr << "usage: neverd-mcp-process-tests NEVERD_MCP\n";
    return 2;
  }
  std::signal(SIGPIPE, SIG_IGN);
  try {
    // The test's own executable is a native input with a main.
    char Self[4096];
    const auto Length = ::readlink("/proc/self/exe", Self, sizeof(Self) - 1);
    const std::string Input =
        Length > 0 ? std::string(Self, static_cast<std::size_t>(Length))
                   : std::string(Arguments[0]);
    launchArguments(Arguments[1]);
    session(Arguments[1], Input);
  } catch (const std::exception &Failure) {
    std::cout << "FAIL " << Failure.what() << "\n";
    return 1;
  }
  std::cout << "PASS\n";
  return 0;
#endif
}
