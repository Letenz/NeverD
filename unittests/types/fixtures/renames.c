// Types whose names the user changes in the local types.  The PDB types
// is_on()'s and sleep_device()'s parameter, so its record has members
// before the decompiler asks for them, and both read a member as an
// enumeration whose enumerators the code names.  Regenerate renames.exe
// and renames.pdb with:
//   clang --driver-mode=cl --target=x86_64-pc-windows-msvc /Zi /O1 /GS- /c \
//     renames.c /Forenames.obj /clang:-fstandalone-debug
//   lld-link /DEBUG /ENTRY:main /NODEFAULTLIB /SUBSYSTEM:CONSOLE \
//     /OUT:renames.exe /PDB:renames.pdb renames.obj

typedef enum _POWER_STATE { PowerOff, PowerOn, PowerSleep } POWER_STATE;

typedef struct _DEVICE_STATE {
  unsigned short Type;
  unsigned short Size;
  POWER_STATE State;
} DEVICE_STATE;

__declspec(dllexport) int is_on(DEVICE_STATE *Device) {
  return Device->State == PowerOn ? Device->Size : 0;
}

__declspec(dllexport) void sleep_device(DEVICE_STATE *Device) {
  Device->State = PowerSleep;
}

int main(void) { return 0; }
