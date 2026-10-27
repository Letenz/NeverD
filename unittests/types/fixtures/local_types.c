// Types that exercise NeverD's PDB type import, laid out as the Windows
// kernel's: anonymous unions and structures, bitfields in unions, sibling
// flag unions, enumerations, function pointers, packing and a type only
// declared.  Regenerate local_types.pdb with:
//   clang-cl --target=x86_64-pc-windows-msvc /Zi /O1 /GS- /c local_types.c \
//     /Folocal_types.obj /clang:-fstandalone-debug
//   lld-link /DEBUG /ENTRY:main /NODEFAULTLIB /SUBSYSTEM:CONSOLE \
//     /OUT:local_types.exe /PDB:local_types.pdb local_types.obj

typedef unsigned char UCHAR;
typedef unsigned short USHORT;
typedef unsigned long ULONG;
typedef long LONG;
typedef unsigned long long ULONGLONG;
typedef long long LONGLONG;
typedef void *PVOID;

typedef struct _LIST_ENTRY {
  struct _LIST_ENTRY *Flink;
  struct _LIST_ENTRY *Blink;
} LIST_ENTRY;

typedef union _LARGE_INTEGER {
  struct {
    ULONG LowPart;
    LONG HighPart;
  };
  struct {
    ULONG LowPart;
    LONG HighPart;
  } u;
  LONGLONG QuadPart;
} LARGE_INTEGER;

typedef struct _PUSH_LOCK {
  union {
    struct {
      ULONGLONG Locked : 1;
      ULONGLONG Waiting : 1;
      ULONGLONG Waking : 1;
      ULONGLONG MultipleShared : 1;
      ULONGLONG Shared : 60;
    };
    ULONGLONG Value;
    PVOID Ptr;
  };
} PUSH_LOCK;

// Two flag unions side by side, as _KTHREAD has at 0x74 and 0x78.
typedef struct _THREAD_LIKE {
  ULONGLONG Header;
  union {
    struct {
      ULONG AutoBoostActive : 1;
      ULONG ReadyTransition : 1;
      ULONG MiscSpare : 30;
    };
    LONG MiscFlags;
  };
  union {
    struct {
      ULONG ThreadFlagsSpare : 2;
      ULONG AutoAlignment : 1;
      ULONG DisableBoost : 1;
      ULONG ThreadReserved : 28;
    };
    LONG ThreadFlags;
  };
  UCHAR Tail[3];
} THREAD_LIKE;

typedef enum _POOL_KIND {
  NonPaged = 0,
  Paged = 1,
  MustSucceed = 0x80000000u
} POOL_KIND;

typedef enum _SIGNED_KIND { Negative = -1, Zero, Positive } SIGNED_KIND;

typedef long (*DISPATCH)(struct _DEVICE *Device, PVOID Irp);

typedef struct _DEVICE {
  USHORT Type;
  USHORT Size;
  LIST_ENTRY Links;
  DISPATCH MajorFunction[4];
  struct _DEVICE *Next;
  LARGE_INTEGER Time;
  PUSH_LOCK Lock;
  POOL_KIND Pool;
} DEVICE;

#pragma pack(push, 1)
typedef struct _PACKED {
  UCHAR Tag;
  ULONG Value;
  USHORT Count;
} PACKED;
#pragma pack(pop)

struct _FORWARD_ONLY;

typedef struct _USES_FORWARD {
  struct _FORWARD_ONLY *Pointer;
} USES_FORWARD;

__declspec(dllexport) int use(DEVICE *Device, THREAD_LIKE *Thread,
                              PACKED *Packed, USES_FORWARD *Forward,
                              SIGNED_KIND Kind) {
  return Device->Size + Thread->MiscFlags + Packed->Count + (Forward != 0) +
         Kind;
}

int main(void) { return 0; }
