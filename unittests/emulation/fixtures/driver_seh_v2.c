//===- driver_seh_v2.c - Independent V2 helper exception oracle -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

typedef unsigned int U32;
unsigned long _exception_code(void);
extern void RaiseFromV2(U32 Mode);
extern void RaiseFromTrailingV2(void);
int DriverEntry(void *Driver, void *RegistryPath);
static void *volatile EntryAddress = (void *)DriverEntry;

int DriverEntry(void *Driver, void *RegistryPath) {
  (void)Driver;
  (void)RegistryPath;
  if (EntryAddress != (void *)DriverEntry)
    return (int)0xc000000dU;
  volatile U32 Completed = 0;
  for (U32 Mode = 0; Mode != 3; ++Mode) {
    __try {
      if (Mode == 2)
        RaiseFromTrailingV2();
      else
        RaiseFromV2(Mode);
      return (int)0xc000000dU;
    } __except (1) {
      if (_exception_code() != (Mode ? 0x80000002U : 0xc0000005U))
        return (int)0xc000000dU;
      ++Completed;
    }
  }
  return Completed == 3 ? 0 : (int)0xc000000dU;
}
