//===- AllocatorTests.cpp - Process allocator compatibility
//----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise standard allocation APIs across executable and library boundaries.
/// Calling mi_malloc directly would not prove that ordinary allocation uses it.
///
//===----------------------------------------------------------------------===//

#ifdef NEVERD_TEST_MIMALLOC
#include <mimalloc.h>
#endif
#ifdef NEVERD_TEST_SDK
#include "neverd/sdk/NeverDCAPI.h"
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <source_location>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <malloc.h>
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

extern "C" {
void *allocatorProbeMalloc(size_t Size);
void allocatorProbeFree(void *Ptr);
char *allocatorProbeNew(size_t Size);
void allocatorProbeDelete(char *Ptr);
const char *allocatorProbeStartup();
}

static void require(bool Condition, const char *Message) {
  if (!Condition) {
    std::fprintf(stderr, "%s\n", Message);
    std::abort();
  }
}

#if defined(_WIN32) && defined(NEVERD_TEST_MIMALLOC)
// Resolve checks dynamically so the test itself cannot keep the mimalloc
// import alive. Production tools that only call the SDK need /INCLUDE too.
static bool (*MimallocOwns)(const void *) = nullptr;

static bool expectMimalloc() {
  return GetEnvironmentVariableA("NEVERD_TEST_EXPECT_SYSTEM_ALLOCATOR", nullptr,
                                 0) == 0;
}
#endif

static void checkAllocation(
    const void *Ptr,
    std::source_location Location = std::source_location::current()) {
  require(Ptr != nullptr, "allocation failed");
#ifdef NEVERD_TEST_MIMALLOC
#ifdef _WIN32
  const bool ExpectMimalloc = expectMimalloc();
  const bool IsMimalloc = MimallocOwns(Ptr);
#else
  constexpr bool ExpectMimalloc = true;
  const bool IsMimalloc = mi_is_in_heap_region(Ptr);
#endif
  if (IsMimalloc != ExpectMimalloc) {
    std::fprintf(stderr, "allocation at %s:%u used the wrong allocator\n",
                 Location.file_name(), Location.line());
    std::abort();
  }
#endif
}

int main(int Argc, char **Argv) {
#ifdef _WIN32
#ifdef NEVERD_TEST_MIMALLOC
  HMODULE Mimalloc = GetModuleHandleA(NEVERD_TEST_MIMALLOC_DLL);
  require(Mimalloc != nullptr, "mimalloc import was discarded");
  MimallocOwns = reinterpret_cast<bool (*)(const void *)>(
      GetProcAddress(Mimalloc, "mi_is_in_heap_region"));
  auto IsRedirected = reinterpret_cast<bool (*)()>(
      GetProcAddress(Mimalloc, "mi_is_redirected"));
  require(MimallocOwns && IsRedirected, "mimalloc exports missing");
  require(IsRedirected() == expectMimalloc(),
          "unexpected CRT redirection state");
  // The mimalloc import must precede dependent DLL initialization on Windows.
  checkAllocation(allocatorProbeStartup());
#endif
#endif
  // dyld initializes dependencies before the executable's allocator on macOS.
  // This early system allocation must remain valid and be freed at shutdown.
  require(allocatorProbeStartup()[1023] == 's', "startup allocation lost data");
  auto *Bytes = static_cast<unsigned char *>(std::calloc(64, 1));
  checkAllocation(Bytes);
  for (size_t I = 0; I < 64; ++I)
    require(Bytes[I] == 0, "calloc did not zero memory");
  std::memset(Bytes, 0x5a, 64);
  Bytes = static_cast<unsigned char *>(std::realloc(Bytes, 4096));
  checkAllocation(Bytes);
  for (size_t I = 0; I < 64; ++I)
    require(Bytes[I] == 0x5a, "realloc lost bytes");
  allocatorProbeFree(Bytes);

  void *FromLibrary = allocatorProbeMalloc(1024);
  checkAllocation(FromLibrary);
  std::free(FromLibrary);
  void *FromExecutable = std::malloc(1024);
  checkAllocation(FromExecutable);
  allocatorProbeFree(FromExecutable);

  char *Array = allocatorProbeNew(1024);
  checkAllocation(Array);
  delete[] Array;
  Array = new char[1024];
  checkAllocation(Array);
  allocatorProbeDelete(Array);
  std::vector<int> Values(1024, 17);
  checkAllocation(Values.data());
  require(Values.back() == 17, "C++ container lost data");

  void *Aligned = ::operator new(4096, std::align_val_t(256));
  checkAllocation(Aligned);
  require(reinterpret_cast<uintptr_t>(Aligned) % 256 == 0,
          "aligned new returned an unaligned pointer");
  ::operator delete(Aligned, std::align_val_t(256));

#ifdef _WIN32
  Aligned = _aligned_malloc(4096, 256);
  checkAllocation(Aligned);
  require(reinterpret_cast<uintptr_t>(Aligned) % 256 == 0,
          "CRT aligned allocation returned an unaligned pointer");
  _aligned_free(Aligned);

  // Load after CRT redirection is established, as real native plugins are.
  require(Argc == 2, "expected the allocator plugin DLL path");
  HMODULE Plugin = LoadLibraryA(Argv[1]);
  require(Plugin != nullptr, "allocator plugin failed to load");
  auto PluginMalloc = reinterpret_cast<void *(*)(size_t)>(
      GetProcAddress(Plugin, "allocatorProbeMalloc"));
  auto PluginFree = reinterpret_cast<void (*)(void *)>(
      GetProcAddress(Plugin, "allocatorProbeFree"));
  auto PluginStartup = reinterpret_cast<const char *(*)()>(
      GetProcAddress(Plugin, "allocatorProbeStartup"));
  require(PluginMalloc && PluginFree && PluginStartup,
          "allocator plugin exports missing");
  checkAllocation(PluginStartup());
  void *FromPlugin = PluginMalloc(2048);
  checkAllocation(FromPlugin);
  std::free(FromPlugin);
  FromExecutable = std::malloc(2048);
  checkAllocation(FromExecutable);
  PluginFree(FromExecutable);
  require(FreeLibrary(Plugin), "allocator plugin failed to unload");
#else
  (void)Argc;
  (void)Argv;
#endif

  // Exercise first allocation on a new thread and free after that thread exits.
  void *Transferred = nullptr;
  std::thread Producer([&] {
    Transferred = allocatorProbeMalloc(8192);
    checkAllocation(Transferred);
    std::memset(Transferred, 0x35, 8192);
  });
  Producer.join();
  std::thread Consumer([&] {
    require(static_cast<unsigned char *>(Transferred)[8191] == 0x35,
            "cross-thread allocation lost data");
    allocatorProbeFree(Transferred);
  });
  Consumer.join();

#ifndef _WIN32
  // SDK failure-path tests fork after initializing the engine. The process
  // allocator must survive the platform's malloc-zone fork callbacks.
  const pid_t Child = fork();
  require(Child >= 0, "fork failed");
  if (Child == 0) {
    void *ChildMemory = allocatorProbeMalloc(1024);
    checkAllocation(ChildMemory);
    allocatorProbeFree(ChildMemory);
    _exit(0);
  }
  int Status = 0;
  require(waitpid(Child, &Status, 0) == Child, "waitpid failed");
  require(WIFEXITED(Status) && WEXITSTATUS(Status) == 0,
          "allocator failed in fork child");
#endif

#ifdef NEVERD_TEST_SDK
  // Exercise libneverd itself, not only the small probe shared library.
  neverd_session_t Session = neverd_session_create();
  checkAllocation(Session);
  const char *Version = neverd_version();
  checkAllocation(Version);
  require(std::strlen(Version) > 0, "SDK version is empty");
  neverd_free_string(Version);
  neverd_session_destroy(Session);
#endif
  return 0;
}
