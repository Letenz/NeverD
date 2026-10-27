//===- NeverDSymbolTransportCurl.cpp - Symbol downloads over libcurl ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// fetchURL on Linux and macOS, through the system's libcurl, loaded when a
/// download is asked for so that the command line needs no libcurl to run.
///
//===----------------------------------------------------------------------===//

#include "NeverDSymbolDownload.h"

#include "neverd/support/FilePath.h"

#include <cstdint>
#include <cstdio>
#include <dlfcn.h>
#include <fstream>
#include <type_traits>

namespace neverd::cli {
namespace {

#define NEVERD_CURL_VALUE(Name, Value) constexpr long Name = Value;
#define NEVERD_SYMBOL_LIMIT(Name, Value) constexpr long long Name = Value;
#include "../common/symbols/SymbolDownloadABI.def"

constexpr const char *CurlLibraries[] = {
#define NEVERD_CURL_LIBRARY(Name) Name,
#include "../common/symbols/SymbolDownloadABI.def"
};

/// libcurl's handle and file offset types, as its headers declare them.
using CurlHandle = void;
using CurlOffset = int64_t;

/// The status a server answers with the file.
constexpr long HttpOk = 200;

/// The libcurl functions fetchURL calls.
struct Curl {
  int (*GlobalInit)(long) = nullptr;
  CurlHandle *(*Init)() = nullptr;
  int (*SetOption)(CurlHandle *, int, ...) = nullptr;
  int (*Perform)(CurlHandle *) = nullptr;
  int (*GetInfo)(CurlHandle *, int, ...) = nullptr;
  void (*Cleanup)(CurlHandle *) = nullptr;
  const char *(*Describe)(int) = nullptr;

  explicit operator bool() const {
    return Init && SetOption && Perform && GetInfo && Cleanup && Describe;
  }
};

const Curl &curl() {
  static const Curl Loaded = [] {
    Curl Functions;
    void *Library = nullptr;
    for (const char *Name : CurlLibraries)
      if ((Library = dlopen(Name, RTLD_NOW | RTLD_LOCAL)))
        break;
    if (!Library)
      return Functions;
    const auto Resolve = [&](auto &Function, const char *Symbol) {
      Function = reinterpret_cast<std::remove_reference_t<decltype(Function)>>(
          dlsym(Library, Symbol));
    };
    Resolve(Functions.GlobalInit, "curl_global_init");
    Resolve(Functions.Init, "curl_easy_init");
    Resolve(Functions.SetOption, "curl_easy_setopt");
    Resolve(Functions.Perform, "curl_easy_perform");
    Resolve(Functions.GetInfo, "curl_easy_getinfo");
    Resolve(Functions.Cleanup, "curl_easy_cleanup");
    Resolve(Functions.Describe, "curl_easy_strerror");
    if (Functions.GlobalInit)
      Functions.GlobalInit(GlobalDefault);
    return Functions;
  }();
  return Loaded;
}

/// Where the body goes, and whether every byte got there.
struct Sink {
  std::ofstream Out;
  const TransferProgress *Progress;
  const Curl *Functions;
  CurlHandle *Handle = nullptr;
};

size_t write(char *Data, size_t Size, size_t Count, void *User) {
  auto *Target = static_cast<Sink *>(User);
  Target->Out.write(Data, static_cast<std::streamsize>(Size * Count));
  return Target->Out ? Size * Count : 0;
}

int progress(void *User, CurlOffset Total, CurlOffset Received, CurlOffset,
             CurlOffset) {
  const auto &Target = *static_cast<Sink *>(User);
  // Only the file counts, not the page that says why it is missing.
  long Status = 0;
  Target.Functions->GetInfo(Target.Handle, InfoResponseCode, &Status);
  if (*Target.Progress && Received > 0 && Status == HttpOk)
    (*Target.Progress)(static_cast<uint64_t>(Received),
                       Total > 0 ? static_cast<uint64_t>(Total) : 0);
  return 0;
}

} // namespace

TransferResult fetchURL(const std::string &URL,
                        const std::filesystem::path &File,
                        const TransferProgress &Progress) {
  const Curl &Functions = curl();
  if (!Functions)
    return {0, "libcurl is not installed"};
  Sink Target{std::ofstream(File, std::ios::binary | std::ios::trunc),
              &Progress, &Functions};
  if (!Target.Out)
    return {0, "cannot write " + pathToUTF8(File)};
  CurlHandle *Handle = Functions.Init();
  if (!Handle)
    return {0, "libcurl could not start a transfer"};
  Target.Handle = Handle;
  char Error[ErrorBufferSize] = {};
  // Each option takes the exact type its number's range names.
  Functions.SetOption(Handle, OptURL, URL.c_str());
  Functions.SetOption(Handle, OptErrorBuffer, Error);
  Functions.SetOption(Handle, OptUserAgent, "NeverD-Symbols/1.0");
  Functions.SetOption(Handle, OptFollowLocation, 1L);
  Functions.SetOption(Handle, OptMaxRedirects, static_cast<long>(MaxRedirects));
  Functions.SetOption(Handle, OptProtocols, "http,https");
  Functions.SetOption(Handle, OptRedirectProtocols, "http,https");
  Functions.SetOption(Handle, OptConnectTimeout,
                      static_cast<long>(ConnectSeconds));
  Functions.SetOption(Handle, OptLowSpeedLimit, 1L);
  Functions.SetOption(Handle, OptLowSpeedTime, static_cast<long>(IdleSeconds));
  Functions.SetOption(Handle, OptTimeout, static_cast<long>(TransferSeconds));
  Functions.SetOption(Handle, OptMaxFileSize,
                      static_cast<CurlOffset>(MaxBytes));
  Functions.SetOption(Handle, OptWriteFunction, &write);
  Functions.SetOption(Handle, OptWriteData, &Target);
  Functions.SetOption(Handle, OptNoProgress, 0L);
  Functions.SetOption(Handle, OptXferInfoFunction, &progress);
  Functions.SetOption(Handle, OptXferInfoData, &Target);
  const int Code = Functions.Perform(Handle);
  long Status = 0;
  Functions.GetInfo(Handle, InfoResponseCode, &Status);
  Functions.Cleanup(Handle);
  Target.Out.close();
  // A transfer that broke after the status arrived kept no whole file.
  if (Code != 0)
    return {0, Error[0] ? Error : Functions.Describe(Code)};
  if (!Target.Out)
    return {0, "cannot write " + pathToUTF8(File)};
  return {Status, {}};
}

} // namespace neverd::cli
