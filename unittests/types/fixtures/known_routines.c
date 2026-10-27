// A driver whose exports are named as the Windows kernel's routines, which
// NeverD's windows-kernel-x64 library declares the prototypes of.  Some are
// exported under a second name too, as a linker that folds identical
// functions exports them; KeClearEvent (VOID) and KeResetEvent (LONG),
// folded as the kernel folds them, return differently.  Regenerate
// known_routines.sys with:
//   clang-cl --target=x86_64-pc-windows-msvc /O1 /GS- /c known_routines.c \
//     /Foknown_routines.obj
//   lld-link /ENTRY:DriverEntry /NODEFAULTLIB /SUBSYSTEM:NATIVE /DLL \
//     /EXPORT:PsGetThreadProcess=IoThreadToProcess \
//     /EXPORT:AnUnknownRoutine=ExFreePoolWithTag \
//     /EXPORT:KeClearEvent=KeResetEvent \
//     /OUT:known_routines.sys known_routines.obj

__declspec(dllexport) void *IoThreadToProcess(void *Thread) {
  return *(void **)((char *)Thread + 0x220);
}

static unsigned long long Pool[64];
static unsigned long Tags[64];

__declspec(dllexport) void *ExAllocatePoolWithTag(int PoolType,
                                                  unsigned long long Bytes,
                                                  unsigned long Tag) {
  if (PoolType < 0 || PoolType >= 64 || Bytes > sizeof(Pool[0]))
    return 0;
  Tags[PoolType] = Tag;
  return &Pool[PoolType];
}

__declspec(dllexport) void ExFreePoolWithTag(void *P, unsigned long Tag) {
  unsigned long long Index = (unsigned long long *)P - Pool;
  if (Index < 64 && Tags[Index] == Tag)
    Tags[Index] = 0;
}

__declspec(dllexport) long KeResetEvent(void *Event) {
  long Previous = *(volatile long *)((char *)Event + 4);
  *(volatile long *)((char *)Event + 4) = 0;
  return Previous;
}

// Its prototype's priority increment and wait flag are never read.
__declspec(dllexport) long KeSetEvent(void *Event, long Increment,
                                      unsigned char Wait) {
  long Previous = *(volatile long *)((char *)Event + 4);
  *(volatile long *)((char *)Event + 4) = 1;
  return Previous;
}

long DriverEntry(void *Driver, void *Path) {
  (void)Driver;
  (void)Path;
  return 0;
}
