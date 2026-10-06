// A freestanding PE32+ console program for unpacking tests. It imports only
// services that the Windows process model implements, so both the original
// and a recovered image can run to completion under every CPU backend.
typedef unsigned long long u64;
typedef unsigned int u32;

__declspec(dllimport) void *GetStdHandle(u32);
__declspec(dllimport) int WriteFile(void *, const void *, u32, u32 *, void *);
__declspec(dllimport) void ExitProcess(u32);

static const char Banner[] = "unpack fixture\n";
static u32 Table[4096];
volatile u32 Seed = 0x1234567;

static u32 mix(u32 X) {
  X ^= X << 13;
  X ^= X >> 17;
  X ^= X << 5;
  return X;
}

static void hex(u32 Value, char *Out) {
  static const char Digits[] = "0123456789abcdef";
  for (int I = 0; I < 8; ++I)
    Out[I] = Digits[(Value >> (28 - 4 * I)) & 15];
}

// Distinct bodies give the image enough code for a compressor to matter.
// Each use ends in an empty declaration so that it formats as its own line.
#define BODY(N)                                                                \
  u32 work##N(u32 X) {                                                         \
    for (int I = 0; I < 16 + N; ++I)                                           \
      X = mix(X + Table[(X + N) & 4095] + N);                                  \
    return X;                                                                  \
  }
BODY(0);
BODY(1);
BODY(2);
BODY(3);
BODY(4);
BODY(5);
BODY(6);
BODY(7);
BODY(8);
BODY(9);
BODY(10);
BODY(11);
BODY(12);
BODY(13);
BODY(14);
BODY(15);
BODY(16);
BODY(17);
BODY(18);
BODY(19);
BODY(20);
BODY(21);
BODY(22);
BODY(23);
BODY(24);
BODY(25);
BODY(26);
BODY(27);
BODY(28);
BODY(29);
BODY(30);
BODY(31);
BODY(32);
BODY(33);
BODY(34);
BODY(35);
BODY(36);
BODY(37);
BODY(38);
BODY(39);
BODY(40);
BODY(41);
BODY(42);
BODY(43);
BODY(44);
BODY(45);
BODY(46);
BODY(47);

typedef u32 (*Work)(u32);
// An array of code pointers gives the image base relocations to carry.
static const Work Works[] = {
    work0,  work1,  work2,  work3,  work4,  work5,  work6,  work7,
    work8,  work9,  work10, work11, work12, work13, work14, work15,
    work16, work17, work18, work19, work20, work21, work22, work23,
    work24, work25, work26, work27, work28, work29, work30, work31,
    work32, work33, work34, work35, work36, work37, work38, work39,
    work40, work41, work42, work43, work44, work45, work46, work47};

void entry(void) {
  void *Out = GetStdHandle((u32)-11);
  u32 Written = 0, X = Seed;
  for (u32 I = 0; I < 4096; ++I)
    Table[I] = mix(I * 2654435761u + 1);
  for (u32 I = 0; I < sizeof(Works) / sizeof(Works[0]); ++I)
    X = Works[I](X);
  char Line[9];
  hex(X, Line);
  Line[8] = '\n';
  WriteFile(Out, Banner, sizeof(Banner) - 1, &Written, 0);
  WriteFile(Out, Line, sizeof(Line), &Written, 0);
  ExitProcess(X & 0x3f);
}
