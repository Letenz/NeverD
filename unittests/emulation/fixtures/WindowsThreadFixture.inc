//===- WindowsThreadFixture.inc - Native thread information checks -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
__declspec(dllimport) long ZwQueryInformationThread(HANDLE Thread, DWORD Class,
                                                    void *Buffer, DWORD Length,
                                                    DWORD *Returned);
__declspec(dllimport) long ZwSetInformationThread(HANDLE Thread, DWORD Class,
                                                  const void *Buffer,
                                                  DWORD Length);
__declspec(dllimport) long ZwQueryInformationProcess(HANDLE Process,
                                                     DWORD Class, void *Buffer,
                                                     DWORD Length,
                                                     DWORD *Returned);

// The information class and length are ULONGs, even in a 64-bit register.
__attribute__((naked)) static long
queryThreadLowWords(HANDLE Thread, DWORD Class, void *Buffer, DWORD Length,
                    DWORD *Returned) {
#if defined(__x86_64__) || defined(_M_X64)
  __asm__("movabsq $0xffff000000000000, %r10\n"
          "orq %r10, %rdx\norq %r10, %r9\n"
          "jmpq *__imp_ZwQueryInformationThread(%rip)");
#else
  __asm__("orr x1, x1, #0xffff000000000000\n"
          "orr x3, x3, #0xffff000000000000\n"
          "adrp x16, __imp_ZwQueryInformationThread\n"
          "ldr x16, [x16, :lo12:__imp_ZwQueryInformationThread]\nbr x16");
#endif
}
__attribute__((naked)) static long setThreadLowWords(HANDLE Thread, DWORD Class,
                                                     const void *Buffer,
                                                     DWORD Length) {
#if defined(__x86_64__) || defined(_M_X64)
  __asm__("movabsq $0xffff000000000000, %r10\n"
          "orq %r10, %rdx\norq %r10, %r9\n"
          "jmpq *__imp_ZwSetInformationThread(%rip)");
#else
  __asm__("orr x1, x1, #0xffff000000000000\n"
          "orr x3, x3, #0xffff000000000000\n"
          "adrp x16, __imp_ZwSetInformationThread\n"
          "ldr x16, [x16, :lo12:__imp_ZwSetInformationThread]\nbr x16");
#endif
}

static void threadScenario(char Mode) {
  if (Mode == ThreadUnknownInformation[0]) {
    const ULONG_PTR Mask = 1;
    ZwSetInformationThread(GetCurrentThread(), 0x7fffffff, &Mask, sizeof(Mask));
    require(0);
    return;
  }
  if (Mode != ThreadInformation[0])
    return;
  _Alignas(4) unsigned char Bytes[16] = {0x55, 0xa5, 0x66};
  DWORD Returned = 0xabcdef01;
  const HANDLE Thread = GetCurrentThread();
  SetLastError(LastErrorSeed);
  ULONG_PTR Basic[6] = {0};
  require(ZwQueryInformationThread(Thread, 0, Basic, sizeof(Basic),
                                   &Returned) == 0);
  const ULONG_PTR OriginalMask = Basic[4];
  require(OriginalMask != 0);
  const ULONG_PTR OneProcessor = OriginalMask & (~OriginalMask + 1);
  require(setThreadLowWords(Thread, 4, &OneProcessor, sizeof(OneProcessor)) ==
          0);
  require(ZwQueryInformationThread(Thread, 0, Basic, sizeof(Basic),
                                   &Returned) == 0 &&
          Basic[4] == OneProcessor);
  require(ZwSetInformationThread(Thread, 4, &OriginalMask,
                                 sizeof(OriginalMask)) == 0);
  ULONG_PTR ProcessBasic[6] = {0};
  require(ZwQueryInformationProcess(GetCurrentProcess(), 0, ProcessBasic,
                                    sizeof(ProcessBasic), &Returned) == 0);
  const ULONG_PTR ProcessMask = ProcessBasic[2];
  require((OriginalMask & ProcessMask) == OriginalMask);
  // Native affinity intersects the request with the process's processors.
  // Unavailable bits must not turn an otherwise valid request into a stop.
  const ULONG_PTR WithUnavailable = OneProcessor | ~ProcessMask;
  require(setThreadLowWords(Thread, 4, &WithUnavailable,
                            sizeof(WithUnavailable)) == 0);
  require(ZwQueryInformationThread(Thread, 0, Basic, sizeof(Basic),
                                   &Returned) == 0 &&
          Basic[4] == OneProcessor);
  const ULONG_PTR Empty = 0;
  require((DWORD)setThreadLowWords(Thread, 4, &Empty, sizeof(Empty)) ==
          0xc000000du);
  const ULONG_PTR Unavailable = ~ProcessMask;
  require((DWORD)setThreadLowWords(Thread, 4, &Unavailable,
                                   sizeof(Unavailable)) == 0xc000000du);
  require(ZwQueryInformationThread(Thread, 0, Basic, sizeof(Basic),
                                   &Returned) == 0 &&
          Basic[4] == OneProcessor);
  require(ZwSetInformationThread(Thread, 4, &OriginalMask,
                                 sizeof(OriginalMask)) == 0);
  require(queryThreadLowWords(Thread, 0x11, Bytes + 1, 1, &Returned) == 0);
  require(Bytes[0] == 0x55 && Bytes[1] == 0 && Bytes[2] == 0x66 &&
          Returned == 1);
  for (DWORD Length = 1; Length <= 8; ++Length)
    for (DWORD Offset = 0; Offset != 4; ++Offset)
      require((DWORD)setThreadLowWords(Thread, 0x11, Bytes + Offset, Length) ==
              (Offset ? 0x80000002u : 0xc0000004u));
  for (DWORD Length = 2; Length <= 8; ++Length)
    for (DWORD Offset = 0; Offset != 4; ++Offset) {
      Returned = 0xabcdef01;
      require((DWORD)queryThreadLowWords(Thread, 0x11, Bytes + Offset, Length,
                                         &Returned) ==
              (Length >= 4 && Offset ? 0x80000002u : 0xc0000004u));
      require(Returned == 0xabcdef01);
    }
  require(ZwQueryInformationThread(Thread, 0x11, Bytes + 1, 1, 0) == 0 &&
          Bytes[1] == 0);
  // A zero-length set does not dereference the unused information pointer.
  require(setThreadLowWords(Thread, 0x11, (const void *)1, 0) == 0);
  require(ZwSetInformationThread(Thread, 0x11, 0, 0) == 0);
  Bytes[1] = 0xa5;
  require((DWORD)ZwQueryInformationThread(Thread, 0x11, Bytes + 1, 0,
                                          &Returned) == 0xc0000004u);
  require((DWORD)ZwQueryInformationThread(Thread, 0x11, Bytes + 1, 2,
                                          &Returned) == 0xc0000004u);
  require(Bytes[0] == 0x55 && Bytes[1] == 0xa5 && Bytes[2] == 0x66);
  Returned = 0xabcdef01;
  require(ZwQueryInformationThread(Thread, 0x11, Bytes + 1, 1, &Returned) == 0);
  require(Bytes[0] == 0x55 && Bytes[1] == 1 && Bytes[2] == 0x66 &&
          Returned == 1 && GetLastError() == LastErrorSeed);
}
