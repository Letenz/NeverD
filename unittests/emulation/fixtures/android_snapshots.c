//===- android_snapshots.c - Guest-created mapping observations ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned long U64;
extern void *mmap(void *, U64, int, int, int, long);
extern int munmap(void *, U64);
extern int mprotect(void *, U64, int);
extern void unsupported_snapshot_service(void);

U64 snapshot_lifetime(unsigned char *Hint, U64 Mode) {
  unsigned char *Bytes = mmap(Hint, 4096, 3, 0x22, -1, 0);
  if (Bytes != Hint)
    return 99;
  for (unsigned I = 0; I != 32; ++I)
    Bytes[13 + I] = (unsigned char)(I * 7 + 3);
  switch (Mode) {
  case 1:
    return munmap(Bytes, 4096);
  case 2:
    return mprotect(Bytes, 4096, 0);
  case 3:
    unsupported_snapshot_service();
    break;
  case 4:
    __asm__ volatile(".inst 0");
    break;
  case 5:
    for (;;)
      __asm__ volatile("" ::: "memory");
  }
  return (U64)Bytes;
}
