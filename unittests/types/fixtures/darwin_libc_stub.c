// The routines linux_libc_calls.c imports from libSystem, which only need
// to exist for the link: NeverD reads the program, nothing runs it.  A macOS
// x64 program's stat is stat$INODE64.  With the headers
// scripts/generate_darwin_types.py --install-headers <directory> installs,
// and the options it prints for each target (OPTIONS):
//   clang --target=x86_64-apple-macos15.0 -dynamiclib -nostdlib \
//     -fuse-ld=lld -Wl,-install_name,/usr/lib/libSystem.B.dylib \
//     -o libSystem.dylib darwin_libc_stub.c
//   clang OPTIONS -O1 -fno-stack-protector -fuse-ld=lld -nostdlib \
//     -Wl,-e,_main -o darwin_libc_calls_x64 linux_libc_calls.c \
//     libSystem.dylib
// and for darwin_libc_calls_arm64 and darwin_libc_calls_ios_arm64 the same
// with macos-arm64's and ios-arm64's.
int stat_any(const char *Path, void *Info) __asm("_stat");
int stat_any(const char *Path, void *Info) { return 0; }
int stat_inode64(const char *Path, void *Info) __asm("_stat$INODE64");
int stat_inode64(const char *Path, void *Info) { return 0; }
long read(int File, void *Buffer, unsigned long Count) { return 0; }
long strtol(const char *Text, char **End, int Base) { return 0; }
// darwin_variadic_calls.c's.
int fcntl(int File, int Command, ...) { return 0; }
int ioctl(int File, unsigned long Request, ...) { return 0; }
int openat(int Directory, const char *Path, int Flags, ...) { return 0; }
void syslog_extsn(int Priority, const char *Format,
                  ...) __asm("_syslog$DARWIN_EXTSN");
void syslog_extsn(int Priority, const char *Format, ...) {}
int shm_open(const char *Name, int Flags, ...) { return 0; }
int sendfile(int File, int Socket, long long Offset, long long *Length,
             void *Headers, int Flags) {
  return 0;
}
int pthread_setname_np(const char *Name) { return 0; }
