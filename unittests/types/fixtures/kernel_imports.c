// A driver calling routines the Windows kernel exports, which NeverD's
// windows-kernel-x64 library declares the prototypes of: their calls pass
// what the prototypes take, the registers a call clobbered before them
// none.  Regenerate kernel_imports.sys with:
//   printf 'LIBRARY ntoskrnl.exe\nEXPORTS\nIoThreadToProcess\n%s\n%s\n' \
//     ExAllocatePoolWithTag ExFreePoolWithTag > ntoskrnl.def
//   llvm-dlltool -m i386:x86-64 -d ntoskrnl.def -l ntoskrnl.lib
//   clang-cl --target=x86_64-pc-windows-msvc /O1 /GS- /c kernel_imports.c \
//     /Fokernel_imports.obj
//   lld-link /ENTRY:DriverEntry /NODEFAULTLIB /SUBSYSTEM:NATIVE /DLL \
//     /OUT:kernel_imports.sys kernel_imports.obj ntoskrnl.lib

__declspec(dllimport) void *IoThreadToProcess(void *Thread);
__declspec(dllimport) void *ExAllocatePoolWithTag(int PoolType,
                                                  unsigned long long Bytes,
                                                  unsigned long Tag);
__declspec(dllimport) void ExFreePoolWithTag(void *P, unsigned long Tag);

__declspec(dllexport) void *Owner(void *Thread) {
  return IoThreadToProcess(Thread);
}

__declspec(dllexport) long Probe(void *Thread, unsigned long long Bytes) {
  void *P = ExAllocatePoolWithTag(0, Bytes, 'borP');
  if (!P)
    return -1;
  // The allocation clobbered every argument register the call reads.
  void *Process = IoThreadToProcess(Thread);
  ExFreePoolWithTag(P, 'borP');
  return Process != 0;
}

long DriverEntry(void *Driver, void *Path) {
  (void)Path;
  return Probe(Driver, 64);
}
