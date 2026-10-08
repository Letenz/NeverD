//===- unpack_library.c - Original DLL, dependency and native host --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned int U32;
typedef unsigned long long U64;
typedef unsigned char U8;
#define NEVERD_LIBRARY_VALUE(Name, Value) enum { Name = Value };
#define NEVERD_LIBRARY_TEXT(Name, Text) static const char Name[] = Text;
#include "UnpackLibraryCases.def"
#undef NEVERD_LIBRARY_TEXT
#undef NEVERD_LIBRARY_VALUE
__declspec(dllimport) void *LoadLibraryA(const char *);
__declspec(dllimport) int FreeLibrary(void *);
__declspec(dllimport) void *GetProcAddress(void *, const char *);
__declspec(dllimport) U32 GetLastError(void);
__declspec(dllimport) void *GetModuleHandleA(const char *);
__declspec(dllimport) U32 GetModuleFileNameA(void *, char *, U32);
__declspec(dllimport) U32 GetCurrentProcessId(void);
__declspec(dllimport) void *GetStdHandle(U32);
__declspec(dllimport) int WriteFile(void *, const void *, U32, U32 *, void *);
__declspec(dllimport) __declspec(noreturn) void ExitProcess(U32);
static void check(int OK) {
  if (!OK)
    ExitProcess(FailureStatus);
}
static void output(const char *Text, U32 Size) {
  U32 Written = 0;
  check(WriteFile(GetStdHandle(StdoutSelector), Text, Size, &Written, 0));
  check(Written == Size);
}

#if defined(NEVERD_LIBRARY_DEPENDENCY)
static U32 Attached;
__declspec(dllexport) U32 Dependency(void) {
  return Attached ? DependencyValue : 0;
}
int dllEntry(void *Base, U32 Reason, void *Reserved) {
  check(Base != 0 && (Reason != 1 || Reserved == 0));
  Attached = Reason == 1;
  return 1;
}
#elif defined(NEVERD_LIBRARY_HOST)
typedef U32 (*QueryFunction)(void);
void hostEntry(void) {
  char Path[1024];
  U32 Size = GetModuleFileNameA(0, Path, sizeof(Path));
  check(Size && Size + sizeof(InputFile) < sizeof(Path));
  while (Size && Path[Size - 1] != '\\' && Path[Size - 1] != '/')
    --Size;
  for (U32 I = 0; I < sizeof(InputFile); ++I)
    Path[Size + I] = InputFile[I];
  void *Module = LoadLibraryA(Path);
  if (!Module)
    ExitProcess(GetLastError());
  QueryFunction Query = (QueryFunction)GetProcAddress(Module, "Query");
  check(Query && Query() == ExportValue);
  check(GetProcAddress(Module, "Alias") == (void *)Query);
  check(GetProcAddress(Module, (const char *)(U64)QueryOrdinal) ==
        (void *)Query);
  check(GetProcAddress(Module, (const char *)(U64)AliasOrdinal) ==
        (void *)Query);
  check(GetProcAddress(Module, (const char *)(U64)HiddenOrdinal) ==
        (void *)Query);
  U32 *Data = (U32 *)GetProcAddress(Module, "DataValue");
  check(Data && *Data == ExportValue);
  QueryFunction Forward = (QueryFunction)GetProcAddress(Module, "Forwarded");
  check(Forward && Forward() == GetCurrentProcessId());
  output(QueryText, sizeof(QueryText) - 1);
  check(FreeLibrary(Module));
  ExitProcess(0);
}
#else
__declspec(dllimport) U32 Dependency(void);
extern U8 __ImageBase;
struct PackRecord {
  U32 Mode, Key, Bytes, Fail;
  U8 Code[Capacity];
};
#pragma section(".pack", read, write)
__declspec(allocate(".pack")) volatile struct PackRecord Pack = {0};
__declspec(thread) U32 ThreadValue = InitialTLS;
__declspec(allocate(".tls")) char TLSStart;
__declspec(allocate(".tls$ZZZ")) char TLSEnd;
U32 _tls_index;
static U32 Initializers, Attached;
U32 DataValue;
#define BODY __attribute__((section(".body$m"), noinline))
__attribute__((section(".body$a"),
               noinline)) int dllEntry(void *Base, U32 Reason, void *Reserved);
static BODY void initialize(void) { ThreadValue += CallbackIncrement; }
typedef void (*TLSCallback)(void *, U32, void *);
static TLSCallback Callbacks[2];
static BODY void initializedTLS(void *Base, U32 Reason, void *Reserved) {
  check(Base == &__ImageBase && Reserved == 0);
  check(Dependency() == DependencyValue);
  if (Reason == 1) {
    ++Initializers;
    initialize();
  }
}
BODY U32 Query(void);
static volatile U64 SelfQuery[] = {(U64)Query, 0};
BODY U32 Query(void) {
  check(Attached == 1 && Initializers == 1 && DataValue == ExportValue);
  check(ThreadValue == InitialTLS + CallbackIncrement + EntryIncrement);
  check(SelfQuery[0] == (U64)Query && SelfQuery[1] == 0);
  return DataValue;
}
#if defined(__x86_64__)
// Keep the nested CALL near the outer continuation so discovery watches both
// starts. The inner stop must not discard the outer helper's state.
__attribute__((naked, section(".body$m"), noinline, used)) static U32
processIDTail(void) {
  __asm__("lea 16(%rsp), %rsp\n\t"
          "jmp *__imp_GetCurrentProcessId(%rip)");
}
__attribute__((naked, section(".body$m"), noinline, used)) static U32
processIDProxy(void) {
  __asm__("push %rax\n\t"
          "mov 8(%rsp), %rax\n\t"
          "lea 1(%rax), %rax\n\t"
          "mov %rax, 8(%rsp)\n\t"
          "pop %rax\n\t"
          "push %rcx\n\t"
          "call processIDTail\n\t"
          ".byte 0xcc");
}
__attribute__((naked, section(".body$m"), noinline)) static U32
processID(void) {
  __asm__("sub $40, %rsp\n\t"
          "call processIDProxy\n\t"
          ".byte 0xee\n\t"
          "add $40, %rsp\n\t"
          "ret");
}
__attribute__((naked, noinline, used)) static U32 dependencyProxy(void) {
  __asm__("push %rax\n\t"
          "mov 8(%rsp), %rax\n\t"
          "lea 1(%rax), %rax\n\t"
          "mov %rax, 8(%rsp)\n\t"
          "pop %rax\n\t"
          "jmp *__imp_Dependency(%rip)");
}
__attribute__((naked, section(".body$m"), noinline)) static U32
dependencyValue(void) {
  __asm__("sub $40, %rsp\n\t"
          "call dependencyProxy\n\t"
          ".byte 0xee\n\t"
          "add $40, %rsp\n\t"
          "ret");
}
#else
static BODY U32 processID(void) { return GetCurrentProcessId(); }
static BODY U32 dependencyValue(void) { return Dependency(); }
#endif
static BODY void openSelf(void *Base) {
  typedef U32 (*OpenFile)(void **, U32, const void *, void *, U32, U32);
  typedef U32 (*CloseFile)(void *);
  struct {
    unsigned short Length, MaximumLength;
    unsigned short *Buffer;
  } Name;
  struct {
    U32 Length;
    void *Root;
    const void *Name;
    U32 Flags;
    void *Security, *Quality;
  } Attributes;
  struct {
    U64 Status, Information;
  } Status;
  char Path[1024];
  unsigned short Wide[1028];
  U32 Size = GetModuleFileNameA(Base, Path, sizeof(Path));
  check(Size && Size < sizeof(Path));
  Wide[0] = '\\';
  Wide[1] = '?';
  Wide[2] = '?';
  Wide[3] = '\\';
  for (U32 I = 0; I <= Size; ++I)
    Wide[I + 4] = (U8)Path[I];
  Name.Length = (unsigned short)((Size + 4) * sizeof(Wide[0]));
  Name.MaximumLength = Name.Length + sizeof(Wide[0]);
  Name.Buffer = Wide;
  Attributes.Length = sizeof(Attributes);
  Attributes.Root = 0;
  Attributes.Name = &Name;
  Attributes.Flags = 0x40;
  Attributes.Security = Attributes.Quality = 0;
  void *Native = GetModuleHandleA("ntdll.dll");
  OpenFile Open = (OpenFile)GetProcAddress(Native, "ZwOpenFile");
  CloseFile Close = (CloseFile)GetProcAddress(Native, "ZwClose");
  check(Open && Close);
  void *File = 0;
  Status.Status = Status.Information = 0;
  check(Open(&File, 0x100001, &Attributes, &Status, 7, 0x60) == 0);
  check(File && Status.Status == 0 && Status.Information == 1);
  check(Close(File) == 0);
}
int dllEntry(void *Base, U32 Reason, void *Reserved) {
  check(Base == &__ImageBase && Reserved == 0);
  check(GetModuleHandleA(0) != Base && GetModuleHandleA(InputFile) == Base);
  check(dependencyValue() == DependencyValue);
  if (Reason == 1) {
    check(Initializers == 1 && ThreadValue == InitialTLS + CallbackIncrement);
    ++Attached;
    ThreadValue += EntryIncrement;
    DataValue = ExportValue;
    check(processID() != 0);
    openSelf(Base);
    output(AttachText, sizeof(AttachText) - 1);
    return !Pack.Fail;
  }
  check(Reason == 0 && Attached == 1);
  output(DetachText, sizeof(DetachText) - 1);
  return 1;
}
__attribute__((noinline)) static void decode(void) {
  volatile U8 *To = (volatile U8 *)(void *)dllEntry;
  for (U32 I = 0; I < Pack.Bytes; ++I)
    To[I] = Pack.Code[I] ^ Pack.Key;
}
__attribute__((noinline)) static int wrappedEntry(void *Base, U32 Reason,
                                                  void *Reserved) {
  int Result = dllEntry(Base, Reason, Reserved);
  check(Result == 1);
  return Result;
}
__attribute__((noinline)) int loader(void *Base, U32 Reason, void *Reserved) {
  if (Reason == 1) {
    decode();
    if (Pack.Mode == PrivateTLSMode) {
      Callbacks[0] = initializedTLS;
      initializedTLS(Base, Reason, Reserved);
    }
  }
  if (Pack.Mode == WrappedEntryMode)
    return wrappedEntry(Base, Reason, Reserved);
  return dllEntry(Base, Reason, Reserved);
}
static void tls(void *Base, U32 Reason, void *Reserved) {
  check(Base == &__ImageBase && Reserved == 0);
  check(Dependency() == DependencyValue);
  if (Reason == 1) {
    if (Pack.Mode == PrivateTLSMode)
      return;
    if (Pack.Mode == TLSMode)
      decode();
    if (Pack.Mode == LoaderMode || Pack.Mode == WrappedEntryMode) {
      ++Initializers;
      ThreadValue += CallbackIncrement;
    } else
      initializedTLS(Base, Reason, Reserved);
  }
}
static TLSCallback Callbacks[2] = {tls, 0};
__declspec(allocate(".rdata")) const struct {
  const void *Start, *End;
  U32 *Index;
  const TLSCallback *Callbacks;
  U32 ZeroFill, Characteristics;
} _tls_used = {&TLSStart, &TLSEnd, &_tls_index, Callbacks, 0, 0};
#endif
