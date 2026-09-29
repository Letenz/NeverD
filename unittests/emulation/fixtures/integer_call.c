//===- integer_call.c - Compiler-owned scalar call frame -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned long long U64;

// Ten arguments cross the register/stack boundary of all three conventions.
// Volatile locals force real stack accesses, including the SysV red zone.
U64 integer_call(U64 A0, U64 A1, U64 A2, U64 A3, U64 A4, U64 A5, U64 A6, U64 A7,
                 U64 A8, U64 A9) {
  volatile U64 Local[4];
  Local[0] = A0 ^ A9;
  Local[1] = A1 + A8;
  Local[2] = A2 * A7;
  Local[3] = A3 - A6;
  return ((Local[0] + Local[1]) ^ (Local[2] + Local[3])) + A4 * A5;
}
