//===- Stdio.h - MCP newline transport --------------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Standard MCP stdio: one JSON-RPC message per line in each direction.
///
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <functional>
#include <string_view>

namespace neverd::binmcp {
class Backend;
class Server;

/// The longest message a client may send.
inline constexpr std::size_t MaxLine = 1024 * 1024;
/// Requests that may wait at once.
inline constexpr std::size_t MaxPending = 16;

/// Reads at most \p Size bytes into \p Into; 0 at the end of the input.
using Source = std::function<std::size_t(char *Into, std::size_t Size)>;
/// Writes one line, its newline included; false once the output has closed.
using Sink = std::function<bool(std::string_view Line)>;

/// Serves until the input ends.  A reader takes each message, and
/// cancellations of waiting ones, while this thread answers them in order;
/// between requests it runs the backend's idle analysis.  Returns 0, or 1 when
/// the output closed first.
int serve(Server &Service, Backend &Session, Source Read, Sink Write);

} // namespace neverd::binmcp
