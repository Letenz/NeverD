//===- NeverDCmdExecution.cpp - Persistent public API JSONL transport
//------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Persistent public API JSONL transport.
///

#include "../NeverDCLI.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

namespace neverd::cli {
namespace {
namespace j = llvm::json;
llvm::Expected<std::string> configuration(llvm::StringRef Name) {
  if (Name.empty() || Name.size() > NEVERD_MODULE_PATH_LIMIT ||
      Name.contains('\0') || !j::isUTF8(Name))
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "invalid execution configuration path");
  std::ifstream Input(
      std::filesystem::path(std::u8string(Name.begin(), Name.end())),
      std::ios::binary);
  if (!Input)
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "cannot open explicit execution configuration");
  std::string Text;
  char Buffer[4096];
  while (Input.read(Buffer, sizeof(Buffer)) || Input.gcount()) {
    if (uint64_t(Input.gcount()) > NEVERD_EXECUTION_JSON_LIMIT - Text.size())
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "execution configuration exceeds input limit");
    Text.append(Buffer, size_t(Input.gcount()));
  }
  if (Input.bad() || Text.find('\0') != std::string::npos)
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "execution configuration read failed or contains NUL");
  return Text;
}
struct Metadata {
  j::Value ID = nullptr;
  bool Run = false;
};
// Only transport metadata is inspected here. Pass the original bytes to the
// C API; do not normalize away duplicate keys/Unicode or parse guest inputs.
Metadata metadata(llvm::StringRef Line) {
  Metadata M;
  auto V = j::parse(Line);
  if (!V) {
    llvm::consumeError(V.takeError());
    return M;
  }
  if (const auto *O = V->getAsObject()) {
    if (auto ID = O->getString("id");
        ID && ID->size() <= NEVERD_EXECUTION_NAME_LIMIT && !ID->contains('\0'))
      M.ID = ID->str();
    M.Run = O->getString("op") == "run";
  }
  return M;
}
enum class LineResult { End, Valid, Invalid, IOError };
LineResult line(std::string &Text) {
  Text.clear();
  bool Any = false, Invalid = false;
  char C;
  while (std::cin.get(C)) {
    Any = true;
    if (C == '\n')
      break;
    if (!C || Text.size() == NEVERD_EXECUTION_JSON_LIMIT)
      Invalid = true;
    if (!Invalid)
      Text.push_back(C);
  }
  if (std::cin.bad())
    return LineResult::IOError;
  if (!Any)
    return LineResult::End;
  return Invalid ? LineResult::Invalid : LineResult::Valid;
}
int startupError(const char *Error) {
  llvm::WithColor::error() << (Error ? Error : "execution startup failed")
                           << "\n";
  neverd_free_string(Error);
  return 1;
}
} // namespace
int runExecution() {
  try {
    auto Text = configuration(ExecutionConfiguration.getValue());
    if (!Text) {
      llvm::WithColor::error() << llvm::toString(Text.takeError()) << "\n";
      return 1;
    }
    const char *Error = nullptr;
    const auto Sources = ExecutionModules.empty()
                             ? neverd_module_set_create_v1(nullptr, &Error)
                             : neverd_module_set_from_file(
                                   ExecutionModules.getValue().c_str(), &Error);
    if (!Sources)
      return startupError(Error);
    auto FreeSources =
        llvm::scope_exit([&] { neverd_module_set_destroy(Sources, nullptr); });
    neverd_execution_options_v1 Options{sizeof(Options), Sources,
                                        Text->c_str()};
    const auto Run = neverd_execution_create_v1(&Options, &Error);
    if (!Run)
      return startupError(Error);
    auto FreeRun =
        llvm::scope_exit([&] { neverd_execution_destroy(Run, nullptr); });
    std::mutex Output;
    std::thread Worker;
    std::atomic<bool> Done{true}, HadError{false};
    auto Join = llvm::scope_exit([&] {
      if (Worker.joinable()) {
        neverd_execution_request_pause(Run, nullptr);
        Worker.join();
      }
    });
    auto Emit = [&](llvm::StringRef Report) {
      std::lock_guard Lock(Output);
      llvm::outs() << Report << "\n";
      llvm::outs().flush();
    };
    auto Failure = [&](j::Value ID, llvm::StringRef Message) {
      HadError.store(true);
      Emit(llvm::formatv("{0}", j::Value(j::Object{{"schema_version", 1},
                                                   {"id", std::move(ID)},
                                                   {"ok", false},
                                                   {"error", Message}}))
               .str());
    };
    auto Execute = [&](const std::string &Line, bool InWorker) {
      const char *FailureText = nullptr;
      const char *Report =
          neverd_execution_command_json(Run, Line.c_str(), &FailureText);
      auto Free = llvm::scope_exit([&] {
        neverd_free_string(Report);
        neverd_free_string(FailureText);
      });
      if (InWorker)
        Done.store(true);
      if (!Report) {
        Failure(metadata(Line).ID,
                FailureText ? FailureText : "execution command failed");
        return false;
      }
      Emit(Report);
      auto V = j::parse(Report);
      if (!V) {
        llvm::consumeError(V.takeError());
        return false;
      }
      const auto *O = V->getAsObject();
      const auto *R = O ? O->getObject("result") : nullptr;
      return R && R->getBoolean("quit") == true;
    };
    const auto Initial = neverd_execution_inspect_json(Run, &Error);
    if (!Initial)
      return startupError(Error);
    auto InitialValue = j::parse(Initial);
    neverd_free_string(Initial);
    if (!InitialValue) {
      llvm::WithColor::error()
          << llvm::toString(InitialValue.takeError()) << "\n";
      return 1;
    }
    Emit(llvm::formatv(
             "{0}", j::Value(j::Object{{"schema_version", 1},
                                       {"type", "created"},
                                       {"result", std::move(*InitialValue)}}))
             .str());
    std::cin.tie(nullptr);
    std::string Input;
    while (true) {
      const auto Status = line(Input);
      if (Status == LineResult::End)
        break;
      if (Status == LineResult::IOError) {
        llvm::WithColor::error() << "execution command stream read failed\n";
        return 1;
      }
      if (Worker.joinable() && Done.load())
        Worker.join();
      if (Status == LineResult::Invalid) {
        Failure(nullptr, "command line exceeds input limit or contains NUL");
        continue;
      }
      if (metadata(Input).Run) {
        if (Worker.joinable()) {
          Failure(metadata(Input).ID, "a run command is still pending");
          continue;
        }
        Done.store(false);
        Worker = std::thread([&, Command = Input] {
          try {
            Execute(Command, true);
          } catch (const std::exception &E) {
            Done.store(true);
            Failure(metadata(Command).ID, E.what());
          } catch (...) {
            Done.store(true);
            Failure(nullptr, "execution transport worker failed");
          }
        });
      } else if (Execute(Input, false)) {
        break;
      }
    }
    // Join before observing the final error status or destroying SDK handles.
    if (Worker.joinable()) {
      neverd_execution_request_pause(Run, nullptr);
      Worker.join();
    }
    return HadError.load() ? 2 : 0;
  } catch (const std::exception &E) {
    llvm::WithColor::error()
        << "execution transport failed: " << E.what() << "\n";
    return 1;
  }
}
} // namespace neverd::cli
