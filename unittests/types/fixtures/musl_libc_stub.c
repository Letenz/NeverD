// The routines linux_libc_calls.c imports from musl, which only need to
// exist for the link: NeverD reads the program, nothing runs it.  A 32-bit
// program's stat is musl's __stat_time64, whose struct stat holds 64-bit
// times.  With M the headers musl 1.2.5 installs for the architecture
// (make ARCH=x86_64 install-headers, in a build directory of its own):
//   clang --target=x86_64-linux-musl -nostdinc \
//     -isystem $(clang -print-resource-dir)/include -isystem $M/include \
//     -shared -nostdlib -fuse-ld=lld -Wl,-soname,libc.so -o libc.so \
//     musl_libc_stub.c
//   clang --target=x86_64-linux-musl -nostdinc \
//     -isystem $(clang -print-resource-dir)/include -isystem $M/include \
//     -O1 -fno-stack-protector -fuse-ld=lld -nostdlib \
//     -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 -Wl,-e,main \
//     -o musl_libc_calls_x64 linux_libc_calls.c libc.so
// and for musl_libc_calls_x86 the same with --target=i386-linux-musl, ARCH=i386
// and -Wl,--dynamic-linker=/lib/ld-musl-i386.so.1.
int stat(const char *Path, void *Info) { return 0; }
int __stat_time64(const char *Path, void *Info) { return 0; }
long read(int File, void *Buffer, unsigned long Count) { return 0; }
long strtol(const char *Text, char **End, int Base) { return 0; }
