//===- WindowsSectionFixture.inc - Independent image section lifetime ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef struct {
  U16 Length, Maximum;
  U32 Padding;
  const U16 *Buffer;
} SectionName;
typedef struct {
  U32 Length, Padding;
  void *Root;
  SectionName *Name;
  U32 Attributes, Padding2;
  void *Security, *Quality;
} SectionAttributes;
_Static_assert(sizeof(SectionName) == 16, "UNICODE_STRING size");
_Static_assert(sizeof(SectionAttributes) == 48, "OBJECT_ATTRIBUTES size");
typedef long (*MapSection)(void *, void *, void **, U64, U64, U64 *, U64 *, U32,
                           U32, U32);
static MapSection volatile MapTarget __attribute__((used));
__attribute__((naked)) static long mapLowWords(void *Section, void *Process,
                                               void **Base, U64 ZeroBits,
                                               U64 Commit, U64 *Offset,
                                               U64 *Size, U32 Inherit,
                                               U32 Allocation, U32 Protection) {
#if defined(_M_X64) || defined(__x86_64__)
  __asm__("movabsq $0xffff000000000000, %r10\n"
          "orq %r10, 0x40(%rsp)\norq %r10, 0x48(%rsp)\n"
          "orq %r10, 0x50(%rsp)\njmpq *MapTarget(%rip)");
#else
  __asm__(
      "orr x7, x7, #0xffff000000000000\n"
      "ldr x9, [sp]\norr x9, x9, #0xffff000000000000\nstr x9, [sp]\n"
      "ldr x9, [sp, #8]\norr x9, x9, #0xffff000000000000\nstr x9, [sp, #8]\n"
      "adrp x16, MapTarget\nldr x16, [x16, :lo12:MapTarget]\nbr x16");
#endif
}

static void inspectSection(void *Base, U64 Size) {
  MemoryInformation Info;
  require(Base && VirtualQuery(Base, &Info, sizeof(Info)) == sizeof(Info), 43);
  require(Info.AllocationBase == Base && Info.Type == MemImage &&
              Info.State == MemCommit,
          44);
  require(*(const U16 *)Base == DOSMagic, 45);
  const U8 *PE = (const U8 *)Base + word(Base, PEHeaderPointer);
  require(word(PE, 0) == PEMagic && word(PE, PEImageSize) <= Size, 46);
  const U32 Exports = word(PE, PEExportDirectory);
  require(Exports && Exports < Size, 47);
  const U8 *Directory = (const U8 *)Base + Exports;
  const U32 Count = word(Directory, ExportNameCount);
  require(Count && Count < MaxExports, 48);
  const U32 *Names =
      (const U32 *)((const U8 *)Base + word(Directory, ExportNames));
  U32 Found = 0;
  for (U32 I = 0; I < Count; ++I)
    if (equal((const char *)Base + Names[I], "ZwClose"))
      ++Found;
  require(Found == 1, 49);
}

static void sections(void *Native, U32 Mode) {
  long (*Open)(void **, U32, SectionAttributes *) =
      lookup(Native, "ZwOpenSection");
  MapTarget = lookup(Native, "ZwMapViewOfSection");
  long (*Close)(void *) = lookup(Native, "ZwClose");
  long (*Unmap)(void *, void *) = lookup(Native, "ZwUnmapViewOfSection");
  static const U16 Path[] = L"\\KnownDlls\\ntdll.dll";
  static const U16 Folded[] = L"\\kNoWnDlLs\\NtDlL.DlL";
  static const U16 Other[] = L"\\UnknownDirectory\\ntdll.dll";
  SectionName Name = {sizeof(Path) - 2, sizeof(Path), 0, Path};
  if (Mode == SectionNamespaceMode) {
    Name.Length = sizeof(Other) - 2;
    Name.Maximum = sizeof(Other);
    Name.Buffer = Other;
  }
  SectionAttributes Attr = {sizeof(Attr), 0, 0, &Name, 0, 0, 0, 0};
  void *First = 0, *Second = 0, *Third = 0;
  require(Open(&First, Mode == SectionAccessMode ? 2 : 5, &Attr) >= 0, 50);
  require(Open(&Second, 5, &Attr) >= 0 && First != Second, 51);
  require(Close(First) == 0, 52);
  Name.Buffer = Folded;
  Name.Length = sizeof(Folded) - 2;
  Name.Maximum = sizeof(Folded);
  Attr.Attributes = 0x40;
  require(Open(&Third, 5, &Attr) >= 0 && Third != Second, 53);
  require(Close(Second) == 0, 54);
  void *A = Mode == SectionFixedMode ? (void *)0x40000000ull : 0;
  void *B = 0;
  U64 SizeA = 0, SizeB = 0;
  U64 Offset = Mode == SectionOffsetMode ? 0x10000 : 0;
  void *Process = (void *)~(U64)0;
  require(mapLowWords(Third, Process, &A, 0, 0, &Offset, &SizeA, 1, 0, 2) >= 0,
          55);
  // Image page protections do not grant write access to a read-only handle.
  require((U32)mapLowWords(Third, Process, &B, 0, 0, 0, &SizeB, 2, 0, 4) ==
                  0xc0000022u &&
              B == 0 && SizeB == 0,
          65);
  require(mapLowWords(Third, Process, &B, 0, 0, 0, &SizeB, 2, 0, 2) >= 0 &&
              A != B,
          56);
  require(Close(Third) == 0, 57);
  inspectSection(A, SizeA);
  inspectSection(B, SizeB);
  U32 Previous;
  require(VirtualProtect(A, PageSize, PageReadWrite, &Previous), 63);
  *(U16 *)A = 0;
  require(*(const U16 *)B == DOSMagic && *(const U16 *)Native == DOSMagic, 64);
  require(Unmap(Process, A) == 0, 58);
  MemoryInformation Info;
  require(VirtualQuery(A, &Info, sizeof(Info)) == sizeof(Info) &&
              Info.State == 0x10000,
          59);
  inspectSection(B, SizeB);
  require(Unmap(Process, B) == 0, 60);
  require(VirtualQuery(Native, &Info, sizeof(Info)) == sizeof(Info) &&
              Info.Type == MemImage && Info.State == MemCommit,
          61);
  require(GetLastError() == LastErrorSeed, 62);
}
