//===- Main.cpp - The neverd-mcp executable -------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Launch arguments, the protocol's own output descriptor, and the backend.
///
//===----------------------------------------------------------------------===//

#include "Backend.h"
#include "ResourceLimits.h"
#include "Server.h"
#include "StdioTransport.h"

#include <cstdio>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
using namespace neverd::binmcp;

constexpr const char *Usage =
    "Usage: neverd-mcp [--stdio] (--file INPUT | --attach CREDENTIAL_FILE)\n"
    "\n"
    "NeverD's read-only MCP server (MCP 2025-11-25, newline JSON-RPC on\n"
    "stdio).\n"
    "  --file INPUT        analyze INPUT in this process, read-only\n"
    "  --attach FILE       query the workbench session shared through FILE\n";

std::filesystem::path pathOf(const std::string &Text) {
  return std::filesystem::path(std::u8string(Text.begin(), Text.end()));
}

/// Reserves the protocol's output before the engine loads anything that may
/// print: C stdio and C++ streams then write to stderr.
int protocolOutput() {
  std::fflush(stdout);
#ifdef _WIN32
  const int Output = _dup(_fileno(stdout));
  if (Output < 0 || _dup2(_fileno(stderr), _fileno(stdout)) != 0)
    throw std::runtime_error("Cannot isolate protocol output");
  _setmode(Output, _O_BINARY);
  _setmode(_fileno(stdin), _O_BINARY);
#else
  const int Output = ::dup(STDOUT_FILENO);
  if (Output < 0 || ::dup2(STDERR_FILENO, STDOUT_FILENO) < 0)
    throw std::runtime_error("Cannot isolate protocol output");
  (void)::fcntl(Output, F_SETFD, FD_CLOEXEC);
#endif
  return Output;
}

std::size_t readInput(char *Into, std::size_t Size) {
  for (;;) {
#ifdef _WIN32
    const int Count = _read(0, Into, static_cast<unsigned>(Size));
#else
    const auto Count = ::read(STDIN_FILENO, Into, Size);
    if (Count < 0 && errno == EINTR)
      continue;
#endif
    return Count > 0 ? static_cast<std::size_t>(Count) : 0;
  }
}

bool writeOutput(int Output, std::string_view Line) {
  while (!Line.empty()) {
#ifdef _WIN32
    const int Count =
        _write(Output, Line.data(), static_cast<unsigned>(Line.size()));
#else
    const auto Count = ::write(Output, Line.data(), Line.size());
    if (Count < 0 && errno == EINTR)
      continue;
#endif
    if (Count <= 0)
      return false;
    Line.remove_prefix(static_cast<std::size_t>(Count));
  }
  return true;
}

int run(const std::vector<std::string> &Arguments) {
  std::optional<std::string> File, Attach;
  for (std::size_t I = 1; I < Arguments.size(); ++I) {
    const auto &Argument = Arguments[I];
    if (Argument == "--stdio")
      continue;
    if (Argument == "--help" || Argument == "-h") {
      std::cout << Usage;
      return 0;
    }
    if (Argument == "--version") {
      std::cout << "neverd-mcp 0.2.0 (MCP " << ProtocolVersion << ")\n";
      return 0;
    }
    if (Argument == "--worker" || Argument.rfind("--worker=", 0) == 0) {
      std::cerr << "neverd-mcp: --worker is no longer used: neverd-mcp runs "
                   "the engine itself; pass only --file INPUT\n";
      return 2;
    }
    std::optional<std::string> *Slot = nullptr;
    std::string Value;
    for (auto [Name, Into] :
         {std::pair{"--file", &File}, std::pair{"--attach", &Attach}}) {
      const std::string Option = Name;
      if (Argument == Option && I + 1 < Arguments.size()) {
        Slot = Into;
        Value = Arguments[++I];
      } else if (Argument.rfind(Option + "=", 0) == 0) {
        Slot = Into;
        Value = Argument.substr(Option.size() + 1);
      }
    }
    if (!Slot || *Slot || Value.empty()) {
      std::cerr << "neverd-mcp: invalid argument " << Argument << "\n" << Usage;
      return 2;
    }
    *Slot = std::move(Value);
  }
  if (File.has_value() == Attach.has_value()) {
    std::cerr << "neverd-mcp: use either --file INPUT or --attach "
                 "CREDENTIAL_FILE\n"
              << Usage;
    return 2;
  }
  try {
    const int Output = protocolOutput();
    std::unique_ptr<Backend> Session;
    if (File)
      Session = std::make_unique<EngineBackend>(pathOf(*File));
    else
      Session = std::make_unique<AttachBackend>(pathOf(*Attach));
    Server Service(*Session);
    return serve(Service, *Session, readInput, [Output](std::string_view Line) {
      return writeOutput(Output, Line);
    });
  } catch (const std::exception &Failure) {
    std::cerr << "neverd-mcp: " << Failure.what() << "\n";
    return 1;
  }
}
} // namespace

#ifdef _WIN32
int wmain(int Count, wchar_t **Arguments) {
  try {
    std::vector<std::string> UTF8;
    for (int I = 0; I < Count; ++I) {
      const auto Size =
          WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Arguments[I], -1,
                              nullptr, 0, nullptr, nullptr);
      if (Size <= 0 || Size > ::neverd::limits::mcp::kMainSizeLimit)
        throw std::invalid_argument("Invalid launch argument");
      std::string Text(Size, '\0');
      if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Arguments[I], -1,
                              Text.data(), Size, nullptr, nullptr) != Size)
        throw std::invalid_argument("Invalid launch argument");
      Text.pop_back();
      UTF8.push_back(std::move(Text));
    }
    return run(UTF8);
#else
int main(int Count, char **Arguments) {
  std::signal(SIGPIPE, SIG_IGN);
  try {
    return run({Arguments, Arguments + Count});
#endif
  } catch (const std::exception &Failure) {
    std::cerr << "neverd-mcp: " << Failure.what() << "\n";
    return 1;
  }
}
