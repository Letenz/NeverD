//===- RepFaultProbe.cpp - Temporary native restart observations --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include <atomic>
#include <cerrno>
#include <cpuid.h>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>
#include <vector>
#define NEVERD_STRING_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_STRING_BYTES(Name, ...) const uint8_t Name[] = {__VA_ARGS__};
#include "X64StringTransferCases.def"
#undef NEVERD_STRING_VALUE
#undef NEVERD_STRING_BYTES
#define NEVERD_COMPARISON_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_COMPARISON_CASE(Name, Size, Source, ...)                        \
  const uint8_t Name[] = {__VA_ARGS__};
#include "X64StringComparisonCases.def"
#undef NEVERD_COMPARISON_VALUE
#undef NEVERD_COMPARISON_CASE
#define NEVERD_REP_PROBE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_REP_PROBE_TEXT(Name, Value) constexpr char Name[] = Value;
#include "RepFaultProbe.def"
#undef NEVERD_REP_PROBE_TEXT
#undef NEVERD_REP_PROBE_VALUE
struct Operation {
  const char *Name;
  unsigned Size;
  bool Source;
  const uint8_t *Bytes;
  unsigned Length;
};
const Operation Operations[] = {
#define NEVERD_COMPARISON_CASE(Name, Size, Source, ...)                        \
  {#Name, Size, Source, Name, sizeof(Name)},
#include "X64StringComparisonCases.def"
#undef NEVERD_COMPARISON_CASE
};
struct State {
  uint64_t AX, CX, SI, DI, Flags;
};
struct Result {
  uint64_t Flags, CX, SI, DI, PC, Address, TrapFlags, TrapCX, TrapPC;
  int Signal, Traps;
};
Result *Record;
uint64_t FaultPC;
void handler(int Signal, siginfo_t *Info, void *Context) {
  auto &R = static_cast<ucontext_t *>(Context)->uc_mcontext.gregs;
  if (Signal == SIGTRAP) {
    ++Record->Traps;
    Record->TrapFlags = R[REG_EFL];
    Record->TrapCX = R[REG_RCX];
    Record->TrapPC = R[REG_RIP];
    if (R[REG_RIP] != static_cast<greg_t>(FaultPC) ||
        R[REG_RCX] != RepeatCount - 1)
      _exit(UnexpectedTrace);
    R[REG_EFL] &= ~TrapFlag;
    return;
  }
  Record->Flags = R[REG_EFL];
  Record->CX = R[REG_RCX];
  Record->SI = R[REG_RSI];
  Record->DI = R[REG_RDI];
  Record->PC = R[REG_RIP];
  Record->Address = reinterpret_cast<uintptr_t>(Info->si_addr);
  Record->Signal = Signal;
  _exit(FaultExit);
}
void run(const uint8_t *Bytes, unsigned Length, State &S) {
  std::vector<uint8_t> Code(std::begin(OraclePrefix), std::end(OraclePrefix));
  Code.insert(Code.end(), std::begin(SysVArgument), std::end(SysVArgument));
  Code.insert(Code.end(), std::begin(OracleLoad), std::end(OracleLoad));
  const auto Offset = Code.size();
  Code.insert(Code.end(), Bytes, Bytes + Length);
  Code.insert(Code.end(), std::begin(OracleSave), std::end(OracleSave));
  void *P = mmap(nullptr, PageBytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (P == MAP_FAILED)
    _exit(SetupFailure);
  std::memcpy(P, Code.data(), Code.size());
  if (mprotect(P, PageBytes, PROT_READ | PROT_EXEC))
    _exit(SetupFailure);
  FaultPC = reinterpret_cast<uintptr_t>(P) + Offset;
  reinterpret_cast<void (*)(State *)>(P)(&S);
  munmap(P, PageBytes);
}
int main() {
  unsigned A = 0, B = 0, C = 0, D = 0;
  __get_cpuid(VendorLeaf, &A, &B, &C, &D);
  char Vendor[VendorBytes + 1]{};
  memcpy(Vendor, &B, sizeof(B));
  memcpy(Vendor + sizeof(B), &D, sizeof(D));
  memcpy(Vendor + sizeof(B) + sizeof(D), &C, sizeof(C));
  __get_cpuid(FeatureLeaf, &A, &B, &C, &D);
  printf(HostFormat, Vendor, A, int(C >> HypervisorBit));
  for (const auto &Op : Operations)
    for (unsigned Prefix : {unsigned(Rep), unsigned(Repne)})
      for (bool SourceFault : {false, true})
        for (unsigned Mode : {0u, 1u, 2u}) {
          if (SourceFault && !Op.Source)
            continue;
          auto *Memory = static_cast<uint8_t *>(
              mmap(nullptr, PageBytes * OraclePages, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0));
          if (Memory == MAP_FAILED)
            return 1;
          Record = reinterpret_cast<Result *>(Memory + RecordPage * PageBytes);
          const unsigned Done = Mode != 0;
          auto *Source = Memory + (SourceFault ? PageBytes - Done * Op.Size
                                               : SourceOffset);
          auto *Destination =
              Memory +
              (SourceFault ? DestinationOffset : PageBytes - Done * Op.Size);
          uint64_t Left = FillValue,
                   Right = Prefix == Rep ? FillValue : DifferentValue;
          if (Op.Source)
            memcpy(Source, &Left, Op.Size);
          memcpy(Destination, &Right, Op.Size);
          uint64_t Input = Prefix == Rep ? PlainFlags : (AllFlags & ~Direction);
          State First{FillValue, RepeatCount,
                      Op.Source ? reinterpret_cast<uintptr_t>(Source)
                                : InvalidPointer,
                      reinterpret_cast<uintptr_t>(Destination), Input};
          run(Op.Bytes, Op.Length, First);
          State S{FillValue, RepeatCount,
                  Op.Source ? reinterpret_cast<uintptr_t>(Source)
                            : InvalidPointer,
                  reinterpret_cast<uintptr_t>(Destination),
                  Input | (Mode == 2 ? TrapFlag : 0)};
          pid_t Child = fork();
          if (Child < 0)
            return 2;
          if (!Child) {
            struct sigaction Act{};
            Act.sa_sigaction = handler;
            Act.sa_flags = SA_SIGINFO;
            sigemptyset(&Act.sa_mask);
            sigset_t Set;
            sigemptyset(&Set);
            sigaddset(&Set, SIGSEGV);
            sigaddset(&Set, SIGTRAP);
            if (sigaction(SIGSEGV, &Act, nullptr) ||
                sigaction(SIGTRAP, &Act, nullptr) ||
                sigprocmask(SIG_UNBLOCK, &Set, nullptr) ||
                mprotect(Memory + PageBytes, PageBytes, PROT_NONE))
              _exit(SetupFailure);
            std::vector<uint8_t> Code{uint8_t(Prefix)};
            Code.insert(Code.end(), Op.Bytes, Op.Bytes + Op.Length);
            run(Code.data(), Code.size(), S);
            _exit(SetupFailure);
          }
          int Status = 0;
          pid_t Wait;
          do {
            Wait = waitpid(Child, &Status, 0);
          } while (Wait < 0 && errno == EINTR);
          printf(RecordFormat, Op.Name, Prefix, int(SourceFault), Mode,
                 WIFEXITED(Status) ? WEXITSTATUS(Status) : -1,
                 (unsigned long long)Input, (unsigned long long)First.Flags,
                 (unsigned long long)Record->Flags,
                 (unsigned long long)Record->CX, Record->Traps,
                 (unsigned long long)Record->TrapFlags,
                 (unsigned long long)Record->TrapCX);
          munmap(Memory, PageBytes * OraclePages);
        }
}
