// Independent unsigned oracle for the original finite-address bytecode VM.
// This describes observable arithmetic, not the VM's encoded representation.
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#ifndef GENERIC_VM_SOURCE
#if defined(GENERIC_VM_MS_ABI)
#define VM_ABI __attribute__((ms_abi))
#else
#define VM_ABI
#endif
uint64_t VM_ABI generic_vm_finite_arithmetic(uint64_t, uint64_t, uint64_t *);
uint64_t VM_ABI generic_vm_finite_branch(uint64_t, uint64_t, uint64_t *);
uint64_t VM_ABI generic_vm_finite_loop(uint64_t, uint64_t, uint64_t *);
#endif

static uint64_t finite_expected(unsigned kind, uint64_t x, uint64_t y,
                                uint64_t *status) {
  if (kind == 0) {
    static const uint64_t addend[] = {13, 29, 47, 71};
    *status = (x >> 2) & 3;
    return (x + addend[*status]) ^ y;
  }
  if (kind == 1) {
    *status = x & 1;
    return ((*status ? x + y + 9 : x - y + 5) ^ UINT64_C(0x2d));
  }
  uint64_t value = x;
  *status = y & 7;
  for (uint64_t i = 0; i < *status; ++i) {
    const uint64_t addend = ((value ^ i) & 1) ? 11 : 5;
    value = (value + y + addend) ^ i;
  }
  return value;
}

static int check_finite_pair(uint64_t x, uint64_t y) {
#ifdef GENERIC_VM_FUNCTION
  for (unsigned kind = GENERIC_VM_KIND; kind != GENERIC_VM_KIND + 1; ++kind) {
#else
  for (unsigned kind = 0; kind != 3; ++kind) {
#endif
    const uint64_t before = UINT64_C(0x24b6d8e013579acf);
    const uint64_t after = UINT64_C(0xe1c3a587694b2d0f);
    uint64_t status;
    const uint64_t want = finite_expected(kind, x, y, &status);
    uint64_t storage[] = {before, ~want, ~status, after};
    uint64_t got;
#ifdef GENERIC_VM_FUNCTION
    got = (uint64_t)GENERIC_VM_FUNCTION(x, y, (uintptr_t)&storage[1]);
#else
    if (kind == 0)
      got = generic_vm_finite_arithmetic(x, y, &storage[1]);
    else if (kind == 1)
      got = generic_vm_finite_branch(x, y, &storage[1]);
    else
      got = generic_vm_finite_loop(x, y, &storage[1]);
#endif
    if (got != want || storage[1] != want || storage[2] != status ||
        storage[0] != before || storage[3] != after) {
      fprintf(stderr,
              "kind=%u x=%" PRIx64 " y=%" PRIx64 " got=%" PRIx64
              " want=%" PRIx64 " status=%" PRIu64 " want_status=%" PRIu64 "\n",
              kind, x, y, got, want, storage[2], status);
      return 1;
    }
  }
  return 0;
}

int main(void) {
  static const uint64_t edges[] = {0,
                                   1,
                                   2,
                                   3,
                                   4,
                                   7,
                                   8,
                                   12,
                                   15,
                                   16,
                                   UINT64_C(0xffffffff),
                                   UINT64_C(0x100000000),
                                   UINT64_C(0x7fffffffffffffff),
                                   UINT64_C(0x8000000000000000),
                                   UINT64_MAX - 1,
                                   UINT64_MAX};
  for (unsigned i = 0; i != sizeof(edges) / sizeof(edges[0]); ++i)
    for (unsigned j = 0; j != sizeof(edges) / sizeof(edges[0]); ++j)
      if (check_finite_pair(edges[i], edges[j]))
        return 1;
  for (uint64_t x = 0; x != 32; ++x)
    for (uint64_t y = 0; y != 32; ++y)
      if (check_finite_pair(x, y))
        return 1;
  uint64_t state = UINT64_C(0x65c7a931bde0248f);
  for (unsigned i = 0; i != 2048; ++i) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    const uint64_t x = state;
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    if (check_finite_pair(x, state))
      return 1;
  }
  return 0;
}
