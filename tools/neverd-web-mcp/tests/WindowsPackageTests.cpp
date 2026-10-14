//===- WindowsPackageTests.cpp - Relocated Windows product tests --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Actual installed C ABI, CLI, worker and MCP without development paths.
///
//===----------------------------------------------------------------------===//

#define NOMINMAX
#include "../../../unittests/web/AsarEnvelopeFixture.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>
#include <windows.h>

namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;
void check(bool OK, const char *Why) {
  if (!OK)
    throw std::runtime_error(Why);
}
struct Handle {
  HANDLE Value = INVALID_HANDLE_VALUE;
  Handle() = default;
  explicit Handle(HANDLE V) : Value(V) {}
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
  Handle(Handle &&H) noexcept
      : Value(std::exchange(H.Value, INVALID_HANDLE_VALUE)) {}
  Handle &operator=(Handle &&H) noexcept {
    if (this != &H) {
      reset();
      Value = std::exchange(H.Value, INVALID_HANDLE_VALUE);
    }
    return *this;
  }
  ~Handle() { reset(); }
  void reset() {
    if (Value != INVALID_HANDLE_VALUE)
      CloseHandle(Value);
    Value = INVALID_HANDLE_VALUE;
  }
};
std::string utf8(const fs::path &P) {
  const auto &W = P.native();
  const int N =
      WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, W.data(),
                          int(W.size()), nullptr, 0, nullptr, nullptr);
  check(N > 0, "Invalid fixture path");
  std::string S(N, '\0');
  check(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, W.data(),
                            int(W.size()), S.data(), N, nullptr, nullptr) == N,
        "Invalid UTF-8");
  return S;
}
void write(const fs::path &P, std::string_view B) {
  std::ofstream F(P, std::ios::binary);
  F.write(B.data(), B.size());
  check(F.good(), "Fixture write failed");
}
std::string read(const fs::path &P) {
  std::ifstream F(P, std::ios::binary);
  return {std::istreambuf_iterator<char>(F), {}};
}
struct Fixture {
  fs::path Root;
  explicit Fixture(const fs::path &Parent)
      : Root(Parent /
             (L"installed-test-" + std::to_wstring(GetCurrentProcessId()) +
              L"-" + std::to_wstring(GetTickCount64()))) {
    check(fs::create_directory(Root), "Fixture exists");
  }
  ~Fixture() {
    std::error_code EC;
    fs::remove_all(Root, EC);
  }
};

class Process {
  Handle In, Out, Child, Log;
  std::string Pending;

public:
  Process(const fs::path &Binary, std::wstring Options,
          const fs::path &Errors) {
    SECURITY_ATTRIBUTES Security{sizeof(Security), nullptr, TRUE};
    HANDLE A, B, C, D;
    check(CreatePipe(&A, &B, &Security, 0), "Input pipe failed");
    Handle ChildIn(A);
    In = Handle(B);
    check(CreatePipe(&C, &D, &Security, 0), "Output pipe failed");
    Out = Handle(C);
    Handle ChildOut(D);
    Log = Handle(CreateFileW(Errors.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                             &Security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                             nullptr));
    check(Log.Value != INVALID_HANDLE_VALUE, "Stderr failed");
    check(SetHandleInformation(In.Value, HANDLE_FLAG_INHERIT, 0) &&
              SetHandleInformation(Out.Value, HANDLE_FLAG_INHERIT, 0),
          "Inheritance failed");
    STARTUPINFOEXW Startup{};
    Startup.StartupInfo.cb = sizeof(Startup);
    Startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    Startup.StartupInfo.hStdInput = ChildIn.Value;
    Startup.StartupInfo.hStdOutput = ChildOut.Value;
    Startup.StartupInfo.hStdError = Log.Value;
    SIZE_T N = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &N);
    std::vector<unsigned char> Storage(N);
    Startup.lpAttributeList =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(Storage.data());
    check(InitializeProcThreadAttributeList(Startup.lpAttributeList, 1, 0, &N),
          "Attribute list failed");
    std::array<HANDLE, 3> Allowed{ChildIn.Value, ChildOut.Value, Log.Value};
    const bool Attributes = UpdateProcThreadAttribute(
        Startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        Allowed.data(), sizeof(Allowed), nullptr, nullptr);
    std::wstring Command = L"\"" + Binary.native() + L"\" " + Options;
    PROCESS_INFORMATION P{};
    const bool Created =
        Attributes &&
        CreateProcessW(Binary.c_str(), Command.data(), nullptr, nullptr, TRUE,
                       EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr,
                       Binary.parent_path().c_str(), &Startup.StartupInfo, &P);
    DeleteProcThreadAttributeList(Startup.lpAttributeList);
    check(Created, "Installed process launch failed");
    Child = Handle(P.hProcess);
    CloseHandle(P.hThread);
  }
  ~Process() {
    if (WaitForSingleObject(Child.Value, 0) == WAIT_TIMEOUT) {
      TerminateProcess(Child.Value, 1);
      WaitForSingleObject(Child.Value, 5000);
    }
  }
  void send(std::string_view B) {
    DWORD N;
    check(B.size() < 4096 &&
              WriteFile(In.Value, B.data(), DWORD(B.size()), &N, nullptr) &&
              N == B.size(),
          "Send failed");
  }
  void more() {
    const auto Deadline = GetTickCount64() + 30000;
    while (GetTickCount64() < Deadline) {
      DWORD Available;
      check(PeekNamedPipe(Out.Value, nullptr, 0, nullptr, &Available, nullptr),
            "Incomplete installed response");
      if (Available) {
        std::array<char, 65536> B{};
        DWORD N;
        check(ReadFile(Out.Value, B.data(),
                       std::min<DWORD>(Available, DWORD(B.size())), &N,
                       nullptr) &&
                  N,
              "Read failed");
        Pending.append(B.data(), N);
        check(Pending.size() <= 8 * 1024 * 1024, "Response too large");
        return;
      }
      Sleep(5);
    }
    throw std::runtime_error("Installed response timed out");
  }
  Json line() {
    while (Pending.find('\n') == Pending.npos)
      more();
    const auto N = Pending.find('\n');
    auto V = Json::parse(Pending.substr(0, N));
    Pending.erase(0, N + 1);
    return V;
  }
  Json rpc(const char *Method, Json Params, unsigned ID) {
    send(Json{{"jsonrpc", "2.0"},
              {"id", ID},
              {"method", Method},
              {"params", std::move(Params)}}
             .dump() +
         "\n");
    auto V = line();
    check(V.at("id") == ID && !V.contains("error"), "Installed MCP failed");
    return V.at("result");
  }
  Json worker(const char *Operation, Json Payload, unsigned Sequence) {
    const auto ID = std::to_string(Sequence);
    const auto B = Json{{"protocol_major", 1},
                        {"request_id", ID},
                        {"operation", Operation},
                        {"payload", std::move(Payload)}}
                       .dump();
    std::string Header(4, '\0');
    for (unsigned I = 0; I < 4; ++I)
      Header[I] = char(B.size() >> (24 - 8 * I));
    send(Header + B);
    for (;;) {
      while (Pending.size() < 4)
        more();
      uint32_t N = 0;
      for (unsigned I = 0; I < 4; ++I)
        N = (N << 8) | static_cast<unsigned char>(Pending[I]);
      check(N && N <= 8 * 1024 * 1024, "Worker frame invalid");
      while (Pending.size() < N + 4)
        more();
      auto V = Json::parse(Pending.substr(4, N));
      Pending.erase(0, N + 4);
      if (V.value("type", "") == "heartbeat")
        continue;
      check(V.value("request_id", "") == ID && V.at("status") == "ok",
            "Installed worker failed");
      return V.at("payload");
    }
  }
  std::string finish() {
    In.reset();
    std::string Result = std::exchange(Pending, {});
    const auto Deadline = GetTickCount64() + 30000;
    for (;;) {
      DWORD Available;
      if (!PeekNamedPipe(Out.Value, nullptr, 0, nullptr, &Available, nullptr)) {
        check(GetLastError() == ERROR_BROKEN_PIPE, "Pipe failed");
        break;
      }
      if (Available) {
        more();
        Result += std::exchange(Pending, {});
      } else {
        check(GetTickCount64() < Deadline, "Process exit timed out");
        Sleep(5);
      }
    }
    check(WaitForSingleObject(Child.Value, 30000) == WAIT_OBJECT_0,
          "Process still running");
    DWORD Code;
    check(GetExitCodeProcess(Child.Value, &Code) && Code == 0,
          "Installed process exited with failure");
    return Result;
  }
};
struct API {
  HMODULE DLL;
#define ENTRY(Name) decltype(&::Name) Name;
  ENTRY(neverd_free_string)
  ENTRY(neverd_web_session_create)
  ENTRY(neverd_web_session_destroy) ENTRY(neverd_web_capabilities_json)
      ENTRY(neverd_web_import_preview_json) ENTRY(neverd_web_import_commit_json)
          ENTRY(neverd_web_artifacts_json) ENTRY(neverd_web_source_analyze_json)
              ENTRY(neverd_web_source_readable_export_json)
                  ENTRY(neverd_web_asar_extract_json)
#undef ENTRY
                      explicit API(const fs::path &Bin) {
    DLL = LoadLibraryExW((Bin / L"neverd.dll").c_str(), nullptr,
                         LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                             LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    check(DLL != nullptr, "Installed DLL/runtime missing");
#define LOAD(Name)                                                             \
  Name = reinterpret_cast<decltype(Name)>(GetProcAddress(DLL, #Name));         \
  check(Name != nullptr, "Installed C ABI missing");
    LOAD(neverd_free_string)
    LOAD(neverd_web_session_create)
    LOAD(neverd_web_session_destroy) LOAD(neverd_web_capabilities_json)
        LOAD(neverd_web_import_preview_json) LOAD(neverd_web_import_commit_json)
            LOAD(neverd_web_artifacts_json) LOAD(neverd_web_source_analyze_json)
                LOAD(neverd_web_source_readable_export_json)
                    LOAD(neverd_web_asar_extract_json)
#undef LOAD
  }
  ~API() { FreeLibrary(DLL); }
  Json take(const char *Text) {
    check(Text != nullptr, "Missing C ABI result");
    std::unique_ptr<const char, decltype(neverd_free_string)> Owned(
        Text, neverd_free_string);
    auto V = Json::parse(Text);
    check(V.value("status", "") == "ok", "Installed analysis failed");
    return V;
  }
};
} // namespace

int wmain(int Count, wchar_t **Args) {
  try {
    check(Count == 3, "Usage: package-tests <installed-bin> <fixture-parent>");
    const auto Bin = fs::canonical(Args[1]);
    Fixture F(fs::canonical(Args[2]));
    std::array<wchar_t, 32768> System{};
    check(GetSystemDirectoryW(System.data(), DWORD(System.size())),
          "System directory missing");
    check(SetEnvironmentVariableW(L"PATH", System.data()) &&
              SetEnvironmentVariableW(L"TEMP", F.Root.c_str()) &&
              SetEnvironmentVariableW(L"TMP", F.Root.c_str()) &&
              SetEnvironmentVariableW(L"NEVERD_SIGNATURE_CACHE", L"off"),
          "Clean environment failed");
    API A(Bin);
    const auto Caps = A.take(A.neverd_web_capabilities_json());
    check(Caps.at("input_reader") == "windows-ntfs-handle-v1",
          "Windows owner absent");
    const std::string Source = "const SECRET_INSTALLED=1+2;";
    const auto Input = F.Root / L"input.js";
    write(Input, Source);
    const auto Path = utf8(Input);
    auto S = A.neverd_web_session_create();
    check(S != nullptr, "Web session missing");
    std::unique_ptr<void, decltype(A.neverd_web_session_destroy)> Session(
        S, A.neverd_web_session_destroy);
    auto P = A.take(A.neverd_web_import_preview_json(S, Path.data(),
                                                     Path.size(), nullptr, 0));
    const std::string Token = P.at("preview_token");
    auto C =
        A.take(A.neverd_web_import_commit_json(S, Token.data(), Token.size()));
    const std::string R = C.at("revision");
    const auto Items =
        A.take(A.neverd_web_artifacts_json(S, R.data(), R.size(), 0, 10));
    const std::string ID = Items.at("items").at(0).at("artifact_id");
    A.take(A.neverd_web_source_analyze_json(S, R.data(), R.size(), ID.data(),
                                            ID.size(), "script", 6));
    fs::remove(Input);
    const auto Output = utf8(F.Root / L"export");
    const auto E = A.take(A.neverd_web_source_readable_export_json(
        S, R.data(), R.size(), ID.data(), ID.size(), "script", 6, Output.data(),
        Output.size()));
    check(E.at("readable_status") == "verified_same_parser_tree" &&
              read(F.Root / L"export" / L"original.bin") == Source,
          "Installed immutable export failed");
    write(Input, Source);
    const auto Archive = F.Root / L"app.asar";
    write(Archive,
          neverd::web::test::asarBytes(
              R"({"files":{"main.js":{"size":3,"offset":"0"}}})", "1+2"));
    const auto AP = utf8(Archive);
    P = A.take(
        A.neverd_web_import_preview_json(S, AP.data(), AP.size(), nullptr, 0));
    const std::string T = P.at("preview_token");
    C = A.take(A.neverd_web_import_commit_json(S, T.data(), T.size()));
    const std::string AR = C.at("revision");
    const auto Rows =
        A.take(A.neverd_web_artifacts_json(S, AR.data(), AR.size(), 0, 10));
    const std::string AI = Rows.at("items").at(0).at("artifact_id");
    A.take(A.neverd_web_asar_extract_json(S, AR.data(), AR.size(), AI.data(),
                                          AI.size(), nullptr, 0));
    for (const auto *Name : {L"icuuc77.dll", L"icudt77.dll"}) {
      auto Module = GetModuleHandleW(Name);
      check(Module != nullptr, "ICU/Unicode data missing");
      std::array<wchar_t, 32768> Loaded{};
      check(GetModuleFileNameW(Module, Loaded.data(), DWORD(Loaded.size())) &&
                fs::equivalent(Loaded.data(), Bin / Name),
            "ICU loaded outside package");
    }
    Process CLI(Bin / L"neverd.exe", L"web inspect \"" + Input.native() + L"\"",
                F.Root / L"cli.log");
    check(Json::parse(CLI.finish()).value("status", "") == "ok",
          "Installed CLI failed");
    Process Worker(Bin / L"neverd-worker.exe", L"", F.Root / L"worker.log");
    check(Worker.worker("web_capabilities", Json::object(), 1)
                  .at("input_reader") == Caps.at("input_reader"),
          "Worker capability mismatch");
    P = Worker.worker("web_import_preview",
                      {{"schema_version", 1}, {"path", Path}}, 2);
    C = Worker.worker(
        "web_import_commit",
        {{"schema_version", 1}, {"preview_token", P.at("preview_token")}}, 3);
    check(C.at("status") == "ok", "Worker capture failed");
    Worker.worker("shutdown", Json::object(), 4);
    Worker.finish();
    Process MCP(Bin / L"neverd-web-mcp.exe",
                L"--input \"" + Input.native() + L"\"", F.Root / L"mcp.log");
    MCP.rpc("initialize",
            {{"protocolVersion", "2025-06-18"},
             {"capabilities", Json::object()},
             {"clientInfo",
              {{"name", "installed-qualification"}, {"version", "1"}}}},
            1);
    MCP.send(
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n");
    const auto Tool = MCP.rpc("tools/call",
                              {{"name", "neverd_web_import_preview"},
                               {"arguments", {{"input_index", 0}}}},
                              2);
    check(!Tool.value("isError", false) &&
              Tool.dump().find("preview_token") != std::string::npos,
          "MCP capture failed");
    MCP.finish();
    for (const auto *Name : {L"cli.log", L"worker.log", L"mcp.log"})
      check(read(F.Root / Name).find("SECRET_INSTALLED") == std::string::npos,
            "Source leaked to stderr");
    std::cout << Json{{"schema_version", 1},
                      {"status", "ok"},
                      {"input_reader", Caps.at("input_reader")},
                      {"artifact_storage", Caps.at("artifact_storage")},
                      {"checks",
                       {"public_c_abi", "immutable_source_export",
                        "asar_icu_77_1", "cli", "worker", "mcp"}}}
                     .dump()
              << '\n';
    return 0;
  } catch (const std::exception &E) {
    std::cerr << E.what() << '\n';
    return 1;
  }
}
