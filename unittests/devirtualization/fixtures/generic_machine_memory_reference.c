// Independent unsigned arithmetic and complete state oracle for the public
// memory-call fixtures. Expected code addresses come from linked symbols.
#include <stdint.h>
#include <stdio.h>

#ifndef GENERIC_MEMORY_SOURCE
extern void generic_memory_rip_call(void);
extern void generic_memory_table_call(void);
extern void generic_memory_stack_call(void);
extern void generic_memory_overwritten_slot_call(void);
extern void generic_memory_callee_zero(void);
extern void generic_memory_callee_one(void);
extern void generic_memory_rip_continuation(void);
extern void generic_memory_table_continuation(void);
extern void generic_memory_stack_continuation(void);
extern void generic_memory_overwritten_continuation(void);
extern const unsigned char generic_memory_readonly_table[];
extern void generic_machine_observer_exit(void);
extern void generic_machine_observe(uint64_t *, void (*)(void));
#define MEMORY_ADDRESS(symbol) ((uint64_t)(uintptr_t)&symbol)
#endif

static int check_memory_call(unsigned kind, uint64_t x, uint64_t y,
                             uint64_t flags) {
  uint64_t state[17], expected[17], stack[16], memory[16];
  for (unsigned i = 0; i < 16; ++i) {
    state[i] = UINT64_C(0x584b3a206d791ec0) + i;
    stack[i] = UINT64_C(0xa86309cde7b15420) ^ i;
  }
  state[4] = (uint64_t)(uintptr_t)&stack[8];
  state[6] = y;
  state[7] = x;
  state[16] = flags;
  stack[8] = MEMORY_ADDRESS(generic_machine_observer_exit);
  for (unsigned i = 0; i < 17; ++i)
    expected[i] = state[i];
  for (unsigned i = 0; i < 16; ++i)
    memory[i] = stack[i];
  const uint64_t continuations[] = {
      MEMORY_ADDRESS(generic_memory_rip_continuation),
      MEMORY_ADDRESS(generic_memory_table_continuation),
      MEMORY_ADDRESS(generic_memory_stack_continuation),
      MEMORY_ADDRESS(generic_memory_overwritten_continuation)};
  const int second = kind == 3 || (kind != 0 && (x & 1));
  expected[0] = x + (second ? 4 : 2) * y + (second ? 73 : 51);
  expected[3] = state[0];
  expected[9] = continuations[kind];
  expected[10] = state[4] - (kind == 2 ? 24 : 8);
  expected[11] = state[4];
  memory[kind == 2 ? 5 : 7] = continuations[kind];
  if (kind == 1) {
    expected[1] = MEMORY_ADDRESS(generic_memory_readonly_table);
    expected[2] = x & 1;
    expected[8] = flags;
  } else if (kind == 2) {
    expected[1] = MEMORY_ADDRESS(generic_memory_callee_one);
    expected[2] = second ? MEMORY_ADDRESS(generic_memory_callee_one)
                         : MEMORY_ADDRESS(generic_memory_callee_zero);
    expected[8] = flags;
    memory[6] = expected[2];
    memory[7] = flags;
  } else if (kind == 3) {
    expected[2] = MEMORY_ADDRESS(generic_memory_callee_one);
  }
#ifdef GENERIC_MEMORY_SOURCE
  if (GENERIC_MEMORY_FUNCTION((void *)state) != 0)
    return 1;
#else
  void (*const entries[])(void) = {
      generic_memory_rip_call, generic_memory_table_call,
      generic_memory_stack_call, generic_memory_overwritten_slot_call};
  generic_machine_observe(state, entries[kind]);
  // The source ABI stops before the outer RET; native observation follows it.
  state[4] -= 8;
#endif
  for (unsigned i = 0; i < 17; ++i)
    if (state[i] != expected[i]) {
      fprintf(stderr, "kind=%u register=%u actual=%llx expected=%llx\n", kind,
              i, (unsigned long long)state[i], (unsigned long long)expected[i]);
      return 2;
    }
  for (unsigned i = 0; i < 16; ++i)
    if (stack[i] != memory[i]) {
      fprintf(stderr, "kind=%u stack_slot=%u actual=%llx expected=%llx\n", kind,
              i, (unsigned long long)stack[i], (unsigned long long)memory[i]);
      return 3;
    }
  return 0;
}

int main(void) {
  const unsigned flag_bits[] = {0, 2, 4, 6, 7, 10, 11};
  const uint64_t inputs[] = {
      0,         1, 2, 3, 0x7fffffff, 0x80000000, UINT64_C(0x8000000000000000),
      UINT64_MAX};
  for (unsigned combination = 0; combination < 128; ++combination) {
    uint64_t flags = 0x202;
    for (unsigned i = 0; i < 7; ++i)
      flags |= ((uint64_t)((combination >> i) & 1)) << flag_bits[i];
    for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
#ifdef GENERIC_MEMORY_KIND
      const unsigned first = GENERIC_MEMORY_KIND;
      const unsigned last = first + 1;
#else
      const unsigned first = 0, last = 4;
#endif
      for (unsigned kind = first; kind < last; ++kind)
        if (check_memory_call(kind, inputs[i], inputs[7 - i], flags))
          return 1;
    }
  }
  return 0;
}
