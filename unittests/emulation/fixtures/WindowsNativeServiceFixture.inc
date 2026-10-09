// Independently authored native-service argument and continuation oracle.
typedef U32 (*NativeThreadQuery)(void *, U32, void *, U32, U32 *);
#if defined(_M_X64) || defined(__x86_64__)
typedef struct {
  U64 BeforeSP, AfterSP, ReturnPC, SavedFlags, BeforeFlags, AfterFlags, NextPC;
} NativeObservation;
// The number comes from the current provider's prologue. No Windows version's
// number is built into this fixture. Poison RCX after placing argument 0 in
// R10.
__attribute__((naked, noinline)) static U32
rawThreadQuery(void *Thread, U32 Class, void *Data, U32 Size, U32 *Returned,
               NativeObservation *Observation, U32 Number) {
  __asm__("movq 0x30(%rsp), %r11\n\t"
          "movq %rsp, 0(%r11)\n\t"
          "pushfq\n\t"
          "popq 32(%r11)\n\t"
          "movq %rcx, %r10\n\t"
          "movabsq $0x1122334455667788, %rcx\n\t"
          "movl 0x38(%rsp), %eax\n\t"
          "syscall\n\t"
          "1:\n\t"
          "movq 0x30(%rsp), %r10\n\t"
          "movq %rsp, 8(%r10)\n\t"
          "movq %rcx, 16(%r10)\n\t"
          "movq %r11, 24(%r10)\n\t"
          "pushfq\n\t"
          "popq 40(%r10)\n\t"
          "leaq 1b(%rip), %r11\n\t"
          "movq %r11, 48(%r10)\n\t"
          "retq\n\t");
}
__attribute__((naked, noinline)) static U32
interiorThreadQuery(void *Thread, U32 Class, void *Data, U32 Size,
                    U32 *Returned, const void *Interior, U32 Number) {
  __asm__("movq %rcx, %r10\n\t"
          "movabsq $0x1122334455667788, %rcx\n\t"
          "movl 0x38(%rsp), %eax\n\t"
          "jmpq *0x30(%rsp)\n\t");
}
#endif
static void nativeServices(void *Native, U32 Mode) {
  void *CurrentThread = (void *)~1ull;
  const char *Names[] = {"NtQueryInformationThread",
                         "ZwQueryInformationThread"};
  for (U32 I = 0; I != 2; ++I) {
    const U8 *Address = lookup(Native, Names[I]);
    struct {
      U32 Before;
      U8 Hidden[4];
      U32 After;
    } Data = {0xabcdef01, {0xa5, 0x5a, 0x6b, 0x7c}, 0x10fedcba};
    struct {
      U32 Before, Length, After;
    } Returned = {0x13579bdf, 0xeeeeeeee, 0x2468ace0};
    U32 Status;
    if (Mode == NativeServicesMode) {
      NativeThreadQuery Query = (NativeThreadQuery)Address;
      Status = Query(CurrentThread, 0x11, Data.Hidden, 1, &Returned.Length);
      require(Status == 0 && Data.Hidden[0] == 0 && Returned.Length == 1, 91);
      Data.Hidden[0] = 0xa5;
      Returned.Length = 0xeeeeeeee;
    }
#if defined(_M_X64) || defined(__x86_64__)
    require(Address[0] == 0x4c && Address[1] == 0x8b && Address[2] == 0xd1 &&
                Address[3] == 0xb8,
            92);
    U32 Number = word(Address, 4);
    if (Mode == NativeUnknownMode)
      Number = 0xffffffff;
    if (Mode == NativeUnimplementedMode) {
      const U8 *Other = lookup(Native, "NtAddAtom");
      require(Other[3] == 0xb8, 93);
      Number = word(Other, 4);
    }
    NativeObservation Observation = {0};
    Status = rawThreadQuery(CurrentThread, 0x11, Data.Hidden, 1,
                            &Returned.Length, &Observation, Number);
    require(Status == 0 && Data.Hidden[0] == 0 && Returned.Length == 1, 94);
    require(Observation.BeforeSP == Observation.AfterSP &&
                Observation.ReturnPC == Observation.NextPC,
            95);
    const U64 StatusFlags = 0x8d5;
    require((Observation.SavedFlags & StatusFlags) ==
                    (Observation.BeforeFlags & StatusFlags) &&
                (Observation.AfterFlags & StatusFlags) ==
                    (Observation.BeforeFlags & StatusFlags),
            96);
    Data.Hidden[0] = 0xa5;
    Returned.Length = 0xeeeeeeee;
    Status = interiorThreadQuery(CurrentThread, 0x11, Data.Hidden, 1,
                                 &Returned.Length, Address + 8, Number);
    require(Status == 0 && Data.Hidden[0] == 0 && Returned.Length == 1, 98);
#endif
    require(Data.Before == 0xabcdef01 && Data.After == 0x10fedcba &&
                Data.Hidden[1] == 0x5a && Data.Hidden[2] == 0x6b &&
                Data.Hidden[3] == 0x7c && Returned.Before == 0x13579bdf &&
                Returned.After == 0x2468ace0,
            97);
  }
}
