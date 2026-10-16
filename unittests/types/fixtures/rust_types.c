// A function named as Rust names one, in a program whose PDB types its
// parameter as a record of anonymous members, as an MSVC Rust program's PDB
// does, and which carries Go's build-info header too, so that it reads as
// Rust and as Go.  Regenerate rust_types.exe and rust_types.pdb with:
//   clang --driver-mode=cl --target=x86_64-pc-windows-msvc /Zi /O1 /GS- /c \
//     rust_types.c /Forust_types.obj /clang:-fstandalone-debug
//   lld-link /DEBUG /ENTRY:main /NODEFAULTLIB /SUBSYSTEM:CONSOLE \
//     /EXPORT:_RNvCs8vGhbR5OvgK_13rust_eh_probe4main \
//     /OUT:rust_types.exe /PDB:rust_types.pdb rust_types.obj

typedef unsigned char UCHAR;
typedef unsigned short USHORT;
typedef unsigned long ULONG;

typedef struct _DPC {
  union {
    ULONG TargetInfoAsUlong;
    struct {
      UCHAR Type;
      UCHAR Importance;
      USHORT Number;
    };
  };
  void *Context;
} DPC;

__declspec(noinline) ULONG
_RNvCs8vGhbR5OvgK_13rust_eh_probe4main(struct _DPC *Dpc) {
  return Dpc->TargetInfoAsUlong + Dpc->Number;
}

// The header the Go linker writes before its build information.
const char GoBuildInfo[] = "\xff Go buildinf:";

int main(void) {
  DPC Dpc = {0};
  return (int)_RNvCs8vGhbR5OvgK_13rust_eh_probe4main(&Dpc) +
         *(const volatile char *)GoBuildInfo;
}
