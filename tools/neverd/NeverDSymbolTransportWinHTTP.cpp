//===- NeverDSymbolTransportWinHTTP.cpp - Symbol downloads over WinHTTP ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// fetchURL on Windows, through WinHTTP, the system's HTTP client: it follows
/// redirects and uses the proxy the system names.
///
//===----------------------------------------------------------------------===//

#include "NeverDSymbolDownload.h"

#include "neverd/support/FilePath.h"

#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>
#include <winhttp.h>

namespace neverd::cli {
namespace {

#define NEVERD_SYMBOL_LIMIT(Name, Value) constexpr long long Name = Value;
#include "../common/symbols/SymbolDownloadABI.def"

/// The status a server answers with the file.
constexpr DWORD HttpOk = 200;

struct HandleCloser {
  void operator()(void *Handle) const { WinHttpCloseHandle(Handle); }
};
using Handle = std::unique_ptr<void, HandleCloser>;

std::wstring wide(const std::string &Text) {
  const int Size = MultiByteToWideChar(
      CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), nullptr, 0);
  std::wstring Out(Size, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()),
                      Out.data(), Size);
  return Out;
}

std::string lastError(const char *What) {
  return std::string(What) + " failed (error " +
         std::to_string(GetLastError()) + ")";
}

} // namespace

TransferResult fetchURL(const std::string &URL,
                        const std::filesystem::path &File,
                        const TransferProgress &Progress) {
  std::wstring Address = wide(URL);
  URL_COMPONENTS Parts = {};
  Parts.dwStructSize = sizeof(Parts);
  Parts.dwSchemeLength = static_cast<DWORD>(-1);
  Parts.dwHostNameLength = static_cast<DWORD>(-1);
  Parts.dwUrlPathLength = static_cast<DWORD>(-1);
  Parts.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(Address.c_str(), 0, 0, &Parts))
    return {0, lastError("WinHttpCrackUrl")};
  const std::wstring Host(Parts.lpszHostName, Parts.dwHostNameLength);
  const std::wstring Path =
      std::wstring(Parts.lpszUrlPath, Parts.dwUrlPathLength) +
      std::wstring(Parts.lpszExtraInfo ? Parts.lpszExtraInfo : L"",
                   Parts.lpszExtraInfo ? Parts.dwExtraInfoLength : 0);

  Handle Session;
#ifdef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
  // The system's proxy settings, as Windows 8.1 and later resolve them.
  Session.reset(WinHttpOpen(L"NeverD-Symbols/1.0",
                            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
#endif
  if (!Session)
    Session.reset(
        WinHttpOpen(L"NeverD-Symbols/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  if (!Session)
    return {0, lastError("WinHttpOpen")};
  const int Connect = static_cast<int>(ConnectSeconds * 1000);
  const int Idle = static_cast<int>(IdleSeconds * 1000);
  WinHttpSetTimeouts(Session.get(), Connect, Connect, Idle, Idle);
  DWORD Redirects = static_cast<DWORD>(MaxRedirects);
  WinHttpSetOption(Session.get(), WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS,
                   &Redirects, sizeof(Redirects));
  Handle Connection(
      WinHttpConnect(Session.get(), Host.c_str(), Parts.nPort, 0));
  if (!Connection)
    return {0, lastError("WinHttpConnect")};
  Handle Request(WinHttpOpenRequest(
      Connection.get(), L"GET", Path.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES,
      Parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
  if (!Request)
    return {0, lastError("WinHttpOpenRequest")};
  if (!WinHttpSendRequest(Request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(Request.get(), nullptr))
    return {0, lastError("The request")};

  DWORD Status = 0;
  DWORD Size = sizeof(Status);
  if (!WinHttpQueryHeaders(
          Request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX, &Status, &Size,
          WINHTTP_NO_HEADER_INDEX))
    return {0, lastError("WinHttpQueryHeaders")};
  if (Status != HttpOk)
    return {static_cast<long>(Status), {}};
  DWORD Length = 0;
  Size = sizeof(Length);
  const uint64_t Total =
      WinHttpQueryHeaders(
          Request.get(),
          WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX, &Length, &Size, WINHTTP_NO_HEADER_INDEX)
          ? Length
          : 0;

  std::ofstream Out(File, std::ios::binary | std::ios::trunc);
  if (!Out)
    return {0, "cannot write " + pathToUTF8(File)};
  const auto Started = GetTickCount64();
  std::vector<char> Buffer;
  uint64_t Received = 0;
  while (true) {
    DWORD Available = 0;
    if (!WinHttpQueryDataAvailable(Request.get(), &Available))
      return {0, lastError("Reading the response")};
    if (Available == 0)
      break;
    Buffer.resize(Available);
    DWORD Read = 0;
    if (!WinHttpReadData(Request.get(), Buffer.data(), Available, &Read))
      return {0, lastError("Reading the response")};
    Received += Read;
    if (Received > static_cast<uint64_t>(MaxBytes))
      return {0, "the file exceeds the download limit"};
    if (GetTickCount64() - Started >
        static_cast<uint64_t>(TransferSeconds) * 1000)
      return {0, "the download timed out"};
    Out.write(Buffer.data(), Read);
    if (!Out)
      return {0, "cannot write " + pathToUTF8(File)};
    if (Progress)
      Progress(Received, Total);
  }
  Out.close();
  if (!Out)
    return {0, "cannot write " + pathToUTF8(File)};
  return {static_cast<long>(Status), {}};
}

} // namespace neverd::cli
