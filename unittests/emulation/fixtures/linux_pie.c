//===- linux_pie.c - Original static PIE with guest-owned relocation ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned long long U64;
#define NEVERD_PIE_VALUE(Name, Value) enum { Name = Value };
#define NEVERD_PIE_TEXT(Name, Text) static const char Name[] = Text;
#include "LinuxPIECases.def"
#undef NEVERD_PIE_VALUE
#undef NEVERD_PIE_TEXT
#define NEVERD_LINUX_FIXTURE_SERVICE(Name, Number)                             \
  enum { Service##Name = Number };
#include "LinuxProcessCases.def"
#undef NEVERD_LINUX_FIXTURE_SERVICE

extern U64 linux_service(U64 Number, U64 A0, U64 A1, U64 A2);
extern void _start(void) __attribute__((visibility("hidden")));
static volatile U64 Data = DataValue;
static U64 calculate(U64 Value) { return Value + Data; }
static const char *volatile TextPointer = Message;
static volatile U64 *volatile DataPointer = &Data;
static U64 (*volatile FunctionPointer)(U64) = calculate;

struct ProgramHeader {
  unsigned Type, Flags;
  U64 Offset, VirtualAddress, PhysicalAddress, FileSize, MemorySize, Alignment;
};
struct DynamicEntry {
  U64 Tag, Value;
};
struct Relocation {
  U64 Offset, Info, Addend;
};

static void finish(U64 Status) {
  linux_service(ServiceExit, Status, 0, 0);
  __builtin_trap();
}

void process_main(U64 *Stack) {
  U64 *Cursor = Stack + Stack[0] + 2;
  while (*Cursor)
    ++Cursor;
  ++Cursor;
  const struct ProgramHeader *Headers = 0;
  U64 Count = 0, Entry = 0, Interpreter = 0;
  for (U64 I = 0; I < MaxEntries && Cursor[0]; ++I, Cursor += 2) {
    if (Cursor[0] == PhdrTag)
      Headers = (const struct ProgramHeader *)Cursor[1];
    if (Cursor[0] == PhnumTag)
      Count = Cursor[1];
    if (Cursor[0] == EntryTag)
      Entry = Cursor[1];
    if (Cursor[0] == BaseTag)
      Interpreter = Cursor[1];
  }
  if (!Headers || !Count || Count > MaxEntries || Entry != (U64)_start ||
      Interpreter)
    finish(FailureStatus);
  U64 Bias = 0, DynamicAddress = 0;
  for (U64 I = 0; I < Count; ++I) {
    if (Headers[I].Type == PhdrHeader)
      Bias = (U64)Headers - Headers[I].VirtualAddress;
    if (Headers[I].Type == DynamicHeader)
      DynamicAddress = Headers[I].VirtualAddress;
  }
  if (!Bias || !DynamicAddress)
    finish(FailureStatus);
  const struct DynamicEntry *Dynamic =
      (const struct DynamicEntry *)(Bias + DynamicAddress);
  U64 TableAddress = 0, Size = 0, Width = 0;
  for (U64 I = 0; I < MaxEntries && Dynamic[I].Tag; ++I) {
    if (Dynamic[I].Tag == RelaTag)
      TableAddress = Bias + Dynamic[I].Value;
    if (Dynamic[I].Tag == RelaSizeTag)
      Size = Dynamic[I].Value;
    if (Dynamic[I].Tag == RelaEntryTag)
      Width = Dynamic[I].Value;
  }
  if (!TableAddress || Width != sizeof(struct Relocation) ||
      Size != RelocationCount * Width)
    finish(FailureStatus);
  const struct Relocation *Table = (const struct Relocation *)TableAddress;
  for (U64 I = 0; I < RelocationCount; ++I) {
    U64 *Slot = (U64 *)(Bias + Table[I].Offset);
    // LLD emits zero RELA slots. Analysis fixups must not leak into startup.
    if ((Table[I].Info >> SymbolShift) || (unsigned)Table[I].Info != Relative ||
        *Slot)
      finish(FailureStatus);
    *Slot = Bias + Table[I].Addend;
  }
  if (TextPointer != Message || DataPointer != &Data ||
      FunctionPointer != calculate || *DataPointer != DataValue ||
      FunctionPointer(ArgumentValue) != ComputedValue)
    finish(FailureStatus);
  if (linux_service(ServiceWrite, StandardOutput, (U64)TextPointer,
                    sizeof(Message) - 1) != sizeof(Message) - 1)
    finish(FailureStatus);
  finish(ExitStatus);
}
