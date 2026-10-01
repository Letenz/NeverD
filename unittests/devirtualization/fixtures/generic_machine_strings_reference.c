// Independent full-state oracle; each element is captured before its write.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef GENERIC_STRING_SOURCE
#define DECLARE_WIDTH(w)                                                       \
  extern void generic_string_move_##w##_f(void);                               \
  extern void generic_string_move_##w##_b(void);                               \
  extern void generic_string_fill_##w##_f(void);                               \
  extern void generic_string_fill_##w##_b(void);
DECLARE_WIDTH(b)
DECLARE_WIDTH(w)
DECLARE_WIDTH(l)
DECLARE_WIDTH(q)
extern void generic_string_zero(void);
extern void generic_machine_observer_exit(void);
extern void generic_machine_observe(uint64_t *, void (*)(void));
#define WIDTH_ENTRIES(w)                                                       \
  generic_string_move_##w##_f, generic_string_move_##w##_b,                    \
      generic_string_fill_##w##_f, generic_string_fill_##w##_b,
static void (*const entries[])(void) = {WIDTH_ENTRIES(b) WIDTH_ENTRIES(w)
                                            WIDTH_ENTRIES(l) WIDTH_ENTRIES(q)
                                                generic_string_zero};
#define STRING_RETURN_ADDRESS                                                  \
  ((uint64_t)(uintptr_t)&generic_machine_observer_exit)
#endif

static int run_string(unsigned kind, unsigned flags, uint64_t value) {
  uint64_t state[17], expected[17], stack[32];
  uint8_t memory[sizeof(stack)];
  for (unsigned i = 0; i < 17; ++i)
    state[i] = UINT64_C(0x751a2e964bc08df3) ^ i;
  for (unsigned i = 0; i < sizeof(stack); ++i)
    ((uint8_t *)stack)[i] = (uint8_t)(i * 23 + i / 5);
  state[0] = value;
  state[4] = (uint64_t)(uintptr_t)&stack[24];
  state[6] = UINT64_MAX;
  state[7] = 1;
  state[16] = flags;
  stack[24] = STRING_RETURN_ADDRESS;
  memcpy(expected, state, sizeof(state));
  memcpy(memory, stack, sizeof(stack));
  expected[1] = 0;
  if (kind < 16) {
    const unsigned width = 1u << (kind / 4);
    const unsigned fill = (kind / 2) & 1;
    const unsigned backward = kind & 1;
    const int source = 192 - (backward ? 64 : 96);
    const int destination = 192 - (backward ? 65 : 95);
    const int delta = (backward ? -1 : 1) * (int)width;
    expected[16] = (flags & ~UINT64_C(0x400)) | (backward ? 0x400 : 0);
    if (!fill)
      expected[6] =
          (uint64_t)(uintptr_t)((uint8_t *)stack + source + 3 * delta);
    expected[7] =
        (uint64_t)(uintptr_t)((uint8_t *)stack + destination + 3 * delta);
    for (unsigned i = 0; i < 3; ++i) {
      uint8_t element[8];
      for (unsigned b = 0; b < width; ++b)
        element[b] = fill ? (uint8_t)(value >> (8 * b))
                          : memory[source + (int)i * delta + b];
      for (unsigned b = 0; b < width; ++b)
        memory[destination + (int)i * delta + b] = element[b];
    }
  }
#ifdef GENERIC_STRING_SOURCE
  if (GENERIC_STRING_FUNCTION((void *)state) != 0)
    return 1;
#else
  generic_machine_observe(state, entries[kind]);
  state[4] -= 8;
#endif
  for (unsigned i = 0; i < 17; ++i)
    if (state[i] != expected[i]) {
      fprintf(stderr, "kind=%u register=%u actual=%llx expected=%llx\n", kind,
              i, (unsigned long long)state[i], (unsigned long long)expected[i]);
      return 2;
    }
  if (memcmp(stack, memory, sizeof(stack)) != 0) {
    fprintf(stderr, "kind=%u stack mismatch\n", kind);
    return 3;
  }
  return 0;
}

int main(void) {
  const unsigned flag_bits[] = {0, 2, 4, 6, 7, 10, 11};
  const uint64_t values[] = {0, UINT64_MAX, UINT64_C(0x05a47fd0316cb298)};
  for (unsigned combination = 0; combination < 128; ++combination) {
    unsigned flags = 0x202;
    for (unsigned i = 0; i < 7; ++i)
      flags |= ((combination >> i) & 1) << flag_bits[i];
#ifdef GENERIC_STRING_KIND
    const unsigned first = GENERIC_STRING_KIND, last = first + 1;
#else
    const unsigned first = 0, last = 17;
#endif
    for (unsigned kind = first; kind < last; ++kind)
      for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
        if (run_string(kind, flags, values[i]))
          return 1;
  }
  return 0;
}
