//===- linux_memory.c - Real anonymous mapping and heap workload ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned long long U64;
#define NEVERD_LINUX_MEMORY_VALUE(Name, Value) static const U64 Name = Value;
#define NEVERD_LINUX_MEMORY_TEXT(Name, Text) static const char Name[] = Text;
#define NEVERD_LINUX_MEMORY_SERVICE(Name, Number)                              \
  enum { Service##Name = Number };
#define NEVERD_LINUX_MEMORY_BYTES(Name, ...)                                   \
  static const unsigned char Name[] = {__VA_ARGS__};
#include "LinuxMemoryCases.def"
#undef NEVERD_LINUX_MEMORY_VALUE
#undef NEVERD_LINUX_MEMORY_TEXT
#undef NEVERD_LINUX_MEMORY_SERVICE
#undef NEVERD_LINUX_MEMORY_BYTES

extern U64 linux_memory_service(U64 Number, U64 A0, U64 A1, U64 A2, U64 A3,
                                U64 A4, U64 A5);
static U64 call(U64 Number, U64 A0, U64 A1, U64 A2) {
  return linux_memory_service(Number, A0, A1, A2, 0, 0, 0);
}
__attribute__((noreturn)) static void finish(U64 Status) {
  call(ServiceExit, Status, 0, 0);
  __builtin_trap();
}
static void require(int Condition, U64 Stage) {
  if (!Condition)
    finish(FailureStatus + Stage);
}
static U64 map(U64 Hint, U64 Length, U64 Flags, U64 Offset) {
  return linux_memory_service(ServiceMmap, Hint, Length, ProtRead | ProtWrite,
                              Flags, (U64)-1, Offset);
}
static void writeByte(volatile unsigned char *Address, unsigned char Value) {
  *Address = Value;
}

void process_main(U64 *Stack) {
  char Mode = Stack[0] > 1 ? ((const char *)Stack[2])[0] : Normal[0];
  if (Mode == Unsupported[0]) {
    map(0, PageSize, PrivateAnonymous | MapFixed, 0);
    finish(FailureStatus);
  }
  const U64 Length = RegionPages * PageSize;
  U64 Address = map(0, Length - 1, PrivateAnonymous, 0);
  require(Address && !(Address % PageSize) && Address < (U64)0 - PageSize,
          CheckMap);
  volatile unsigned char *Bytes = (volatile unsigned char *)Address;
  for (U64 I = 0; I < RegionPages; ++I)
    require(!Bytes[I * PageSize] && !Bytes[(I + 1) * PageSize - 1], CheckZero);
  writeByte(Bytes, FirstByte);
  writeByte(Bytes + Length - 1, SecondByte);
  require(!call(ServiceMadvise, Address, Length - 1, 12) &&
              !call(ServiceMadvise, Address + PageSize, 1, 13) &&
              call(ServiceMadvise, Address + 1, 0, 12) ==
                  (U64)0 - InvalidArgument,
          CheckAdvice);
  require(!call(ServiceMprotect, Address, Length, ProtRead), CheckProtect);
  if (Mode == ProtectionFault[0]) {
    writeByte(Bytes, SecondByte);
    finish(FailureStatus);
  }
  require(Bytes[0] == FirstByte && Bytes[Length - 1] == SecondByte,
          CheckProtect);
  require(!call(ServiceMprotect, Address + PageSize, PageSize, 0),
          CheckProtect);
  require(call(ServiceWrite, StandardOutput, Address + PageSize, 1) ==
              (U64)0 - BadAddress,
          CheckProtect);
  require(!call(ServiceMprotect, Address, Length, ProtRead | ProtWrite),
          CheckProtect);
  require(!call(ServiceMunmap, Address + PageSize, PageSize, 0), CheckHole);
  require(call(ServiceMadvise, Address, Length, 12) == (U64)0 - NoMemory &&
              call(ServiceMadvise, Address, Length, 13) == (U64)0 - NoMemory,
          CheckAdvice);
  require(call(ServiceMprotect, Address, Length, ProtRead) == (U64)0 - NoMemory,
          CheckHole);
  if (Mode == HoleFault[0]) {
    // Linux changed the mapped prefix before discovering the unmapped page.
    writeByte(Bytes, SecondByte);
    finish(FailureStatus);
  }
  // The page after the hole must retain its original write permission.
  writeByte(Bytes + Length - 1, FirstByte);
  require(!call(ServiceMprotect, Address, PageSize, ProtRead | ProtWrite),
          CheckHole);
  U64 Middle = map(Address + PageSize, PageSize, PrivateAnonymous, 0);
  require(Middle == Address + PageSize && !*(volatile unsigned char *)Middle,
          CheckRemap);
  require(!call(ServiceMunmap, Middle, PageSize, 0) &&
              !call(ServiceMunmap, Address, Length, 0) &&
              !call(ServiceMunmap, Address, Length, 0),
          CheckRemap);

#if defined(__x86_64__)
  Address = map(0, PageSize, PrivateAnonymous, 0);
  require(Address && Address < (U64)0 - PageSize, CheckCode);
  Bytes = (volatile unsigned char *)Address;
  for (unsigned Pass = 0; Pass < 2; ++Pass) {
    const unsigned char *Code = Pass ? SecondCode : FirstCode;
    for (U64 I = 0; I < sizeof(FirstCode); ++I)
      Bytes[I] = Code[I];
    require(!call(ServiceMprotect, Address, PageSize, ProtRead | ProtExecute),
            CheckCode);
    require(((U64 (*)(void))Address)() == (Pass ? SecondByte : FirstByte),
            CheckCode);
    require(!call(ServiceMprotect, Address, PageSize, ProtRead | ProtWrite),
            CheckCode);
  }
  require(!call(ServiceMunmap, Address, PageSize, 0), CheckCode);
#endif

  U64 Initial = call(ServiceBrk, 0, 0, 0);
  require(Initial && !(Initial % PageSize), CheckHeap);
  U64 End = Initial + HeapPages * PageSize + HeapOffset;
  require(call(ServiceBrk, End, 0, 0) == End, CheckHeap);
  Bytes = (volatile unsigned char *)Initial;
  require(!Bytes[0] && !Bytes[HeapPages * PageSize], CheckHeap);
  writeByte(Bytes, FirstByte);
  writeByte(Bytes + HeapPages * PageSize, SecondByte);
  require(call(ServiceBrk, Initial + HeapOffset, 0, 0) == Initial + HeapOffset,
          CheckHeap);
  require(Bytes[0] == FirstByte, CheckHeap);
  require(call(ServiceBrk, End, 0, 0) == End && !Bytes[HeapPages * PageSize] &&
              Bytes[0] == FirstByte,
          CheckHeap);
  require(call(ServiceBrk, Initial - 1, 0, 0) == End &&
              call(ServiceBrk, (U64)-1, 0, 0) == End &&
              call(ServiceBrk, Initial, 0, 0) == Initial,
          CheckHeap);

  require(map(0, 0, PrivateAnonymous, 0) == (U64)0 - InvalidArgument &&
              map(0, PageSize, PrivateAnonymous, 1) ==
                  (U64)0 - InvalidArgument &&
              map(0, (U64)-1, PrivateAnonymous, 0) == (U64)0 - NoMemory &&
              call(ServiceMprotect, 1, PageSize, ProtRead) ==
                  (U64)0 - InvalidArgument &&
              !call(ServiceMprotect, 0, 0, ProtRead) &&
              call(ServiceMunmap, 1, PageSize, 0) == (U64)0 - InvalidArgument &&
              call(ServiceMunmap, 0, 0, 0) == (U64)0 - InvalidArgument,
          CheckErrors);
  require(call(ServiceWrite, StandardOutput, (U64)Message,
               sizeof(Message) - 1) == sizeof(Message) - 1,
          CheckErrors);
  finish(ExitStatus);
}
