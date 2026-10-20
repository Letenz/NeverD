// A PE32 parent whose two objects unwind into the caller's catch. Build with
// i686-pc-windows-msvc, exceptions and frame pointers at both O0 and O1.
extern "C" __declspec(dllimport) __declspec(noreturn) void __stdcall
ExitProcess(unsigned);

extern "C" {
__declspec(dllexport) volatile unsigned cleanup_only_trace = 0;
__declspec(dllexport) volatile unsigned cleanup_only_enabled = 1;
__declspec(dllexport) __declspec(noinline) void cleanup_only_throw() {
  throw 7;
}
}

struct CleanupOnlyGuard {
  unsigned Tag;
  __declspec(noinline) ~CleanupOnlyGuard();
};

CleanupOnlyGuard::~CleanupOnlyGuard() {
  cleanup_only_trace = cleanup_only_trace * 10 + Tag;
}

extern "C" __declspec(dllexport) __declspec(noinline) int
cleanup_only_parent() {
  if (!cleanup_only_enabled)
    return 0;
  CleanupOnlyGuard Outer{1};
  CleanupOnlyGuard Inner{2};
  cleanup_only_throw();
  return 9;
}

extern "C" __declspec(noreturn) void mainCRTStartup() {
  try {
    cleanup_only_parent();
  } catch (int Value) {
    ExitProcess(Value == 7 && cleanup_only_trace == 21 ? 0 : 1);
  }
  ExitProcess(2);
}
