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

extern int kill(int, int);
extern int getpid(void);
extern int gettid(void);
extern int pthread_create(U64 *, const void *, void *(*)(void *), void *);
extern int pthread_join(U64, void **);
extern int pthread_once(int *, void (*)(void));
extern int usleep(unsigned);

static U64 raw_kill(U64 PID, U64 Signal) {
  register U64 X0 __asm__("x0") = PID;
  register U64 X1 __asm__("x1") = Signal;
  register U64 X8 __asm__("x8") = 129;
  __asm__ volatile("svc #0" : "+r"(X0) : "r"(X1), "r"(X8) : "memory");
  return X0;
}

U64 signal_send(U64 Op, U64 PID, U64 Signal, volatile U64 *Out) {
  *__errno() = 73;
  Out[0] = 0x11223344;
  U64 Result;
  if (Op == 0)
    Result = (U64)(long)kill((int)PID, (int)Signal);
  else if (Op == 1)
    Result = (U64)syscall(129, PID, Signal);
  else if (Op == 2)
    Result = raw_kill(PID, Signal);
  else {
    void *Handle = dlopen("libsignals-model.so", 2);
    int (*Send)(int, int) = dlsym(Handle, "kill");
    if (!Handle || !Send)
      return 99;
    if (Op == 4)
      dlclose(Handle);
    Result = (U64)(long)Send((int)PID, (int)Signal);
  }
  Out[1] = *__errno();
  Out[2] = Result;
  return Result;
}

static volatile U64 *SignalOutput;
static U64 SignalMode, SignalOp;
static void *signal_worker(void *Arg) {
  (void)Arg;
  __atomic_store_n(SignalOutput + 3, (U64)gettid(), __ATOMIC_RELEASE);
  if (SignalMode == 0) {
    while (!__atomic_load_n(SignalOutput + 4, __ATOMIC_ACQUIRE))
      gettid();
  } else if (SignalMode == 1 || SignalMode == 3) {
    if (SignalMode == 3)
      usleep(1);
    signal_send(SignalOp, (U64)getpid(), 9, SignalOutput);
  }
  SignalOutput[5] = 0x5566;
  return (void *)0x7788;
}

U64 signal_threads(U64 Mode, U64 Op, volatile U64 *Out) {
  U64 Thread;
  SignalOutput = Out;
  SignalMode = Mode;
  SignalOp = Op;
  if (pthread_create(&Thread, 0, signal_worker, 0))
    return 91;
  if (Mode == 3)
    return 42;
  if (Mode == 0) {
    while (!__atomic_load_n(Out + 3, __ATOMIC_ACQUIRE))
      gettid();
  } else {
    void *Value = 0;
    if (pthread_join(Thread, &Value))
      return 92;
    Out[6] = (U64)Value;
  }
  signal_send(Op, (U64)getpid(), 9, Out);
  Out[7] = 0x9999;
  return 93;
}

static int SignalOnce;
static void signal_initializer(void) {
  signal_send(0, (U64)getpid(), 9, SignalOutput);
  SignalOutput[3] = 1;
}
U64 signal_callback(volatile U64 *Out) {
  SignalOutput = Out;
  pthread_once(&SignalOnce, signal_initializer);
  Out[4] = 1;
  return 94;
}

extern int sigprocmask(int, const U64 *, U64 *);
extern int sigprocmask64(int, const U64 *, U64 *);
extern int sigpending(U64 *);
extern int sigpending64(U64 *);
extern void (*signal(int, void (*)(int)))(int);
extern int sigemptyset(U64 *);
extern int sigfillset(U64 *);
extern int sigaddset(U64 *, int);
extern int sigdelset(U64 *, int);
extern int sigismember(const U64 *, int);

static U64 raw_signal(U64 N, U64 A, U64 B, U64 C, U64 D) {
  register U64 X0 __asm__("x0") = A;
  register U64 X1 __asm__("x1") = B;
  register U64 X2 __asm__("x2") = C;
  register U64 X3 __asm__("x3") = D;
  register U64 X8 __asm__("x8") = N;
  __asm__ volatile("svc #0"
                   : "+r"(X0)
                   : "r"(X1), "r"(X2), "r"(X3), "r"(X8)
                   : "memory");
  return X0;
}

U64 signal_mask_call(U64 Op, U64 How, U64 New, U64 Old, U64 Size, int *Error) {
  *__errno() = 73;
  U64 R = Op == 0 ? (U64)(long)sigprocmask((int)How, (void *)New, (void *)Old)
          : Op == 1
              ? (U64)(long)sigprocmask64((int)How, (void *)New, (void *)Old)
          : Op == 2 ? raw_signal(135, How, New, Old, Size)
                    : (U64)syscall(135, How, New, Old, Size);
  *Error = *__errno();
  return R;
}

U64 signal_mask_sequence(U64 Op, volatile U64 *Out) {
  U64 Mask = ~0UL, Removal = (1UL << 9) | (0xfUL << 31);
  int Error = 0;
  if (signal_mask_call(Op, 2, (U64)&Mask, (U64)&Mask, 8, &Error))
    return 91;
  Out[0] = Mask;
  if (signal_mask_call(Op, 99, 0, (U64)&Mask, 8, &Error))
    return 92;
  Out[1] = Mask;
  if (signal_mask_call(Op, 1, (U64)&Removal, 0, 8, &Error))
    return 93;
  if (signal_mask_call(Op, 99, 0, (U64)&Mask, 8, &Error))
    return 94;
  Out[2] = Mask;
  Out[3] = (unsigned)Error;
  return 0;
}

struct SignalMachine {
  U64 Fault, Regs[31], SP, PC, PState;
  unsigned char Reserved[4096] __attribute__((aligned(16)));
};
struct SignalContext {
  U64 Flags, Link;
  struct {
    U64 Pointer;
    int Flags;
    U64 Size;
  } Stack;
  U64 Mask;
  unsigned char Unused[120];
  struct SignalMachine Machine;
};
_Static_assert(__builtin_offsetof(struct SignalContext, Machine) == 176,
               "AArch64 signal context");
_Static_assert(sizeof(struct SignalContext) == 4560, "AArch64 ucontext size");
static volatile U64 *DeliveryOut;
static U64 DeliveryMode, DeliveryDepth;

static void delivery_handler(int Number, void *Info, void *Context) {
  struct SignalContext *UC = Context;
  unsigned *SI = Info;
  U64 Mask = 0, Pending = 0;
  ++DeliveryOut[0];
  ++DeliveryDepth;
  if (DeliveryDepth > DeliveryOut[9])
    DeliveryOut[9] = DeliveryDepth;
  DeliveryOut[1] = (unsigned)Number;
  sigprocmask64(0, 0, &Mask);
  sigpending64(&Pending);
  DeliveryOut[2] = Mask;
  DeliveryOut[3] = Pending;
  DeliveryOut[6] = UC->Mask;
  DeliveryOut[7] = ((U64)SI[4] << 32) | SI[5];
  DeliveryOut[12] = SI[0] == 10 && SI[1] == 0 && SI[2] == 0 &&
                    UC->Machine.PC != 0 && !(UC->Machine.PC & 3) &&
                    UC->Machine.SP > (U64)UC && UC->Stack.Flags == 2;
  DeliveryOut[15] = (U64)gettid();
  if (DeliveryMode == 2 && DeliveryOut[0] < 3)
    kill(getpid(), 10);
  if (DeliveryMode == 3 && DeliveryOut[0] == 1) {
    kill(getpid(), 10);
    kill(getpid(), 10);
    sigpending64(&Pending);
    DeliveryOut[13] = Pending;
  }
  if (DeliveryMode == 5) {
    UC->Machine.Regs[0] = 0x1234;
    UC->Mask = 1UL << 12;
  } else if (DeliveryMode == 8) {
    UC->Machine.Reserved[0] = 0;
  } else if (DeliveryMode == 9) {
    UC->Machine.PState = 5;
  } else if (DeliveryMode == 10) {
    UC->Stack.Flags = 0;
  } else if (DeliveryMode == 11) {
    UC->Mask = (1UL << 8) | (1UL << 18);
  } else if (DeliveryMode == 14) {
    UC->Machine.PC += 4;
  }
  __asm__ volatile("movi v0.16b, #0x66\n"
                   "mov x9, #0\n"
                   "msr fpcr, x9\n"
                   "msr fpsr, x9\n"
                   "msr nzcv, x9\n"
                   :
                   :
                   : "v0", "x9", "cc", "memory");
  --DeliveryDepth;
}

__attribute__((naked)) static void delivery_restorer(void) {
  __asm__ volatile("mov x8, #139\nsvc #0");
}

static int install_delivery(U64 Mode, volatile U64 *Out) {
  DeliveryOut = Out;
  DeliveryMode = Mode;
  DeliveryDepth = 0;
  struct Action A = {4, (void (*)(int))delivery_handler, 1UL << 11, 0};
  if (Mode == 2)
    A.Flags |= 0x40000000;
  if (Mode == 4)
    A.Flags |= (int)0x80000000;
  if (Mode == 7) {
    A.Flags |= 0x04000000;
    A.Restorer = delivery_restorer;
  }
  return sigaction(10, &A, 0);
}

U64 signal_delivery(U64 Op, U64 Mode, volatile U64 *Out) {
  if (install_delivery(Mode, Out))
    return 91;
  U64 Mask = 1UL << 9, Pending = 0;
  if (Mode == 1 || Mode == 6) {
    if (sigprocmask64(0, &Mask, 0))
      return 92;
    kill(getpid(), 10);
    kill(getpid(), 10);
    sigpending(&Pending);
    Out[10] = Pending;
    Out[11] = Out[0];
    if (Mode == 6) {
      struct Action Ignore = {0, (void (*)(int))1, 0, 0};
      if (sigaction(10, &Ignore, 0))
        return 93;
      sigpending64(&Pending);
      Out[13] = Pending;
    }
    Out[8] = (U64)(long)sigprocmask64(1, &Mask, 0);
  } else {
    if (Op == 0)
      Out[8] = (U64)(long)kill(getpid(), 10);
    else if (Op == 1)
      Out[8] = (U64)syscall(129, getpid(), 10);
    else if (Op == 2)
      Out[8] = raw_kill((U64)getpid(), 10);
    else {
      void *H = dlopen("libsignals-model.so", 2);
      int (*F)(int, int) = dlsym(H, "kill");
      if (!F)
        return 94;
      Out[8] = (U64)(long)F(getpid(), 10);
    }
    if (Mode == 4)
      kill(getpid(), 10);
  }
  Mask = 0;
  sigprocmask64(0, 0, &Mask);
  Out[5] = Mask;
  Out[14] = 0x55;
  return 0;
}

static void ordinary_handler(int Number) { DeliveryOut[0] += Number; }
U64 signal_convenience(volatile U64 *Out) {
  DeliveryOut = Out;
  U64 Mask;
  *__errno() = 73;
  Out[1] = sigemptyset(&Mask);
  Out[2] = Mask;
  Out[3] = sigaddset(&Mask, 64);
  Out[4] = Mask;
  Out[5] = sigismember(&Mask, 64);
  Out[6] = sigdelset(&Mask, 64);
  Out[7] = Mask;
  Out[8] = sigfillset(&Mask);
  Out[9] = Mask;
  Out[10] = (U64)(long)sigaddset(&Mask, 65);
  Out[11] = *__errno();
  Out[12] = (U64)(long)sigemptyset(0);
  Out[13] = *__errno();
  if (signal(10, ordinary_handler) != 0)
    return 91;
  if (kill(getpid(), 10))
    return 92;
  return 0;
}

static void *delivery_worker(void *Arg) {
  (void)Arg;
  U64 Mask = 0;
  sigprocmask64(0, 0, &Mask);
  DeliveryOut[16] = Mask;
  while (!__atomic_load_n(DeliveryOut + 20, __ATOMIC_ACQUIRE))
    gettid();
  Mask = 0;
  sigprocmask64(2, &Mask, 0);
  return (void *)77;
}
U64 signal_thread_masks(volatile U64 *Out) {
  if (install_delivery(0, Out))
    return 91;
  U64 Mask = 1UL << 9, Thread;
  sigprocmask64(0, &Mask, 0);
  if (pthread_create(&Thread, 0, delivery_worker, 0))
    return 92;
  kill(getpid(), 10);
  kill(getpid(), 10);
  __atomic_store_n(Out + 20, 1, __ATOMIC_RELEASE);
  void *Value;
  if (pthread_join(Thread, &Value))
    return 93;
  Out[17] = (U64)Value;
  sigprocmask64(0, 0, &Mask);
  Out[18] = Mask;
  return 0;
}

U64 signal_registers(U64 Mode, volatile U64 *Out) {
  if (install_delivery(Mode, Out))
    return 91;
  void *Sample = (void *)((unsigned char *)Out + 256);
  __asm__ volatile("mov x0, #1000\nmov x1, #10\nmov x8, #129\n"
                   "mov x9, #0x987\nmov x10, #0x654\nmovi v0.16b, #0x3c\n"
                   "mov x11, #0x400000\nmsr fpcr, x11\n"
                   "mov x11, #0x8000000\nmsr fpsr, x11\n"
                   "mov x11, #0xa0000000\nmsr nzcv, x11\n"
                   "svc #0\n"
                   "add x9, x9, #1\n"
                   "stp x9, x10, [%0]\nstr q0, [%0, #16]\n"
                   "mrs x9, fpcr\nstr x9, [%0, #32]\n"
                   "mrs x9, fpsr\nstr x9, [%0, #40]\n"
                   "mrs x9, nzcv\nstr x9, [%0, #48]\n"
                   "msr fpcr, xzr\nmsr fpsr, xzr\n"
                   :
                   : "r"(Sample)
                   : "x0", "x1", "x8", "x9", "x10", "x11", "v0", "cc",
                     "memory");
  return 0;
}
