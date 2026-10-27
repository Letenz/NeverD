// A Windows program's own DWARF, which MinGW's compilers write into its
// image: NeverD reads its types under MinGW's record ABI, Microsoft's records
// with the x87's long double, or under Microsoft's where the DWARF's long
// double is a double, as clang-cl's is.  Regenerate the images with:
//   clang --target=x86_64-w64-mingw32 -g -O0 -nostdlib -fuse-ld=lld \
//     -Wl,--entry=start -Wl,--subsystem,console mingw_types.c \
//     -o mingw_types.x64.exe
//   clang --target=i686-w64-mingw32 -g -O0 -nostdlib -fuse-ld=lld \
//     -Wl,--entry=_start -Wl,--subsystem,console mingw_types.c \
//     -o mingw_types.x86.exe
//   clang --target=x86_64-pc-windows-msvc -gdwarf -O0 -nostdlib \
//     -fuse-ld=lld -Wl,/entry:start -Wl,/subsystem:console \
//     -Wl,/debug:dwarf mingw_types.c -o mingw_types.msvc.exe

struct sample {
  char c;
  long double ld;
  double d;
  long long q;
  char e;
};

struct sample g_sample;

int start(void) { return g_sample.c; }
