// Independent unsigned oracle for the original public register/stack VMs.
// Also append this file to recovered C: define GENERIC_VM_SOURCE first so the
// declarations below do not replace ABI types inferred by the source emitter.
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#ifndef GENERIC_VM_SOURCE
#if defined(GENERIC_VM_MS_ABI)
#define VM_ABI __attribute__((ms_abi))
#else
#define VM_ABI
#endif
uint64_t VM_ABI generic_vm_register_arithmetic(uint64_t, uint64_t, uint64_t *);
uint64_t VM_ABI generic_vm_register_branch(uint64_t, uint64_t, uint64_t *);
uint64_t VM_ABI generic_vm_register_loop(uint64_t, uint64_t, uint64_t *);
uint64_t VM_ABI generic_vm_stack_arithmetic(uint64_t, uint64_t, uint64_t *);
uint64_t VM_ABI generic_vm_stack_branch(uint64_t, uint64_t, uint64_t *);
uint64_t VM_ABI generic_vm_stack_loop(uint64_t, uint64_t, uint64_t *);
#endif

static uint64_t expected(unsigned kind, uint64_t x, uint64_t y,
                         uint64_t *status) {
  uint64_t a;
  if (kind == 2) {
    a = x;
    *status = y & 7;
    for (uint64_t i = 0; i < *status; ++i)
      a = (a + y) ^ i;
    return a;
  }
  if (kind == 0 || (x & 1)) {
    a = x + y;
    *status = a < x;
  } else {
    a = x - y;
    *status = x < y;
  }
  return a ^ UINT64_C(0x13579bdf);
}

static int check_pair(uint64_t x, uint64_t y) {
  const uint64_t before = UINT64_C(0x13579bdf2468ace0);
  const uint64_t after = UINT64_C(0xfedcba9876543210);
#ifdef GENERIC_VM_FUNCTION
  for (unsigned kind = GENERIC_VM_KIND; kind != GENERIC_VM_KIND + 1; ++kind) {
#else
  for (unsigned kind = 0; kind != 3; ++kind) {
#endif
    uint64_t status;
    uint64_t want = expected(kind, x, y, &status);
#ifdef GENERIC_VM_FUNCTION
    for (unsigned family = 0; family != 1; ++family) {
#else
    for (unsigned family = 0; family != 2; ++family) {
#endif
      uint64_t storage[] = {before, ~want, ~status, after};
      uint64_t got;
#ifdef GENERIC_VM_FUNCTION
      // Recovered source exposes the untyped machine-address ABI for arg2.
      got = (uint64_t)GENERIC_VM_FUNCTION(x, y, (uintptr_t)&storage[1]);
#else
      // Direct calls let this harness test both SysV and Win64 fixtures.
      if (family == 0) {
        if (kind == 0)
          got = (uint64_t)generic_vm_register_arithmetic(x, y, &storage[1]);
        else if (kind == 1)
          got = (uint64_t)generic_vm_register_branch(x, y, &storage[1]);
        else
          got = (uint64_t)generic_vm_register_loop(x, y, &storage[1]);
      } else {
        if (kind == 0)
          got = (uint64_t)generic_vm_stack_arithmetic(x, y, &storage[1]);
        else if (kind == 1)
          got = (uint64_t)generic_vm_stack_branch(x, y, &storage[1]);
        else
          got = (uint64_t)generic_vm_stack_loop(x, y, &storage[1]);
      }
#endif
      if (got != want || storage[1] != want || storage[2] != status ||
          storage[0] != before || storage[3] != after) {
        fprintf(stderr,
                "family=%u kind=%u x=%" PRIx64 " y=%" PRIx64 " got=%" PRIx64
                " expected=%" PRIx64 " status=%" PRIu64
                " expected_status=%" PRIu64 "\n",
                family, kind, x, y, got, want, storage[2], status);
        return 1;
      }
    }
  }
  return 0;
}

int main(void) {
  static const uint64_t edges[] = {0,
                                   1,
                                   2,
                                   3,
                                   7,
                                   8,
                                   15,
                                   16,
                                   0xff,
                                   0x100,
                                   UINT64_C(0xffffffff),
                                   UINT64_C(0x100000000),
                                   UINT64_C(0x7fffffffffffffff),
                                   UINT64_C(0x8000000000000000),
                                   UINT64_MAX - 1,
                                   UINT64_MAX,
                                   UINT64_C(0xaaaaaaaaaaaaaaaa),
                                   UINT64_C(0x5555555555555555)};
  for (unsigned i = 0; i != sizeof(edges) / sizeof(edges[0]); ++i)
    for (unsigned j = 0; j != sizeof(edges) / sizeof(edges[0]); ++j)
      if (check_pair(edges[i], edges[j]))
        return 1;
  for (uint64_t x = 0; x != 32; ++x)
    for (uint64_t y = 0; y != 32; ++y)
      if (check_pair(x, y))
        return 1;
  uint64_t state = UINT64_C(0x92d68ca2f53b17e9);
  for (unsigned i = 0; i != 2048; ++i) {
    state = state * UINT64_C(6364136223846793005) + 1;
    uint64_t x = state;
    state = state * UINT64_C(6364136223846793005) + 1;
    if (check_pair(x, state))
      return 1;
  }
  return 0;
}
