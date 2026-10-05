//===- android_signals.c - Independent Bionic and kernel signal ABIs -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned long U64;
struct Action {
  int Flags;
  void (*Handler)(int);
  U64 Mask;
  void (*Restorer)(void);
};
struct KernelAction {
  U64 Handler, Flags, Restorer, Mask;
};
_Static_assert(sizeof(struct Action) == 32, "Bionic LP64 action");
_Static_assert(__builtin_offsetof(struct Action, Handler) == 8, "padding");
_Static_assert(sizeof(struct KernelAction) == 32, "kernel LP64 action");
extern int sigaction(int, const struct Action *, struct Action *);
extern int sigaction64(int, const struct Action *, struct Action *);
extern long syscall(long, ...);
extern int *__errno(void);
extern int mprotect(void *, U64, int);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);

static U64 raw(U64 Signal, U64 New, U64 Old, U64 Size) {
  register U64 X0 __asm__("x0") = Signal;
  register U64 X1 __asm__("x1") = New;
  register U64 X2 __asm__("x2") = Old;
  register U64 X3 __asm__("x3") = Size;
  register U64 X8 __asm__("x8") = 134;
  __asm__ volatile("svc #0"
                   : "+r"(X0)
                   : "r"(X1), "r"(X2), "r"(X3), "r"(X8)
                   : "memory");
  return X0;
}

U64 signal_call(U64 Op, U64 Signal, U64 New, U64 Old, U64 Size, int *Error,
                U64 Readonly) {
  if (Readonly && mprotect((void *)Readonly, 4096, 1))
    return 99;
  *__errno() = 73;
  U64 Result;
  switch (Op) {
  case 0:
    Result = (U64)(long)sigaction((int)Signal, (void *)New, (void *)Old);
    break;
  case 1:
    Result = (U64)(long)sigaction64((int)Signal, (void *)New, (void *)Old);
    break;
  case 2:
    Result = raw(Signal, New, Old, Size);
    break;
  default:
    Result = (U64)syscall(134, Signal, New, Old, Size);
    break;
  }
  *Error = *__errno();
  return Result;
}

U64 signal_sequence(volatile U64 *Out) {
  unsigned char *Bytes = (unsigned char *)Out;
  struct Action *Old = (void *)(Bytes + 64), *Next = (void *)(Bytes + 128);
  struct KernelAction *Middle = (void *)(Bytes + 192);
  struct KernelAction *RawNext = (void *)(Bytes + 256);
  struct Action *Last = (void *)(Bytes + 320);
  Next->Flags = (int)0x94000004;
  Next->Handler = (void (*)(int))0x123456789abc0018UL;
  Next->Mask = ~0UL;
  Next->Restorer = (void (*)(void))0xfedcba9876540018UL;
  *__errno() = 73;
  Out[0] = (U64)(long)sigaction(11, Next, Old);
  Out[1] = raw(11, 0, (U64)Middle, 8);
  RawNext->Handler = 1;
  RawNext->Flags = 0x10000001;
  RawNext->Restorer = 0x1122334455667788UL;
  RawNext->Mask = ~0UL;
  // Failed copy-out must not undo the newly installed kernel disposition.
  Out[2] = raw(11, (U64)RawNext, 1, 8);
  Out[3] = (U64)(long)sigaction64(11, 0, Last);
  Out[4] = *__errno();
  return 0;
}

U64 signal_dynamic(U64 Old, U64 Closed) {
  void *Handle = dlopen("libsignals-model.so", 2);
  int (*Query)(int, const struct Action *, struct Action *) =
      dlsym(Handle, "sigaction");
  if (!Handle || !Query)
    return 99;
  if (Closed)
    dlclose(Handle);
  return (U64)(long)Query(11, 0, (void *)Old);
}
