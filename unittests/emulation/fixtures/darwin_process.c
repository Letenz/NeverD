/* Independently authored freestanding Darwin guest; no Apple SDK/libSystem. */
typedef unsigned long u64;
#if defined(__aarch64__)
#define PAGE 16384UL
#else
#define PAGE 4096UL
#endif
static volatile u64 data = 0x1234;
static volatile u64 bss;
static u64 secondary;

static u64 call(u64 number, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5,
                unsigned *error) {
#if defined(__aarch64__)
  register u64 x0 __asm__("x0") = a0;
  register u64 x1 __asm__("x1") = a1;
  register u64 x2 __asm__("x2") = a2;
  register u64 x3 __asm__("x3") = a3;
  register u64 x4 __asm__("x4") = a4;
  register u64 x5 __asm__("x5") = a5;
  register u64 x16 __asm__("x16") = number;
  unsigned carry;
  __asm__ volatile("svc #0x80\n\tcset %w2, cs"
                   : "+r"(x0), "+r"(x1), "=r"(carry)
                   : "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x16)
                   : "cc", "memory");
  *error = carry;
  secondary = x1;
  return x0;
#else
  register u64 r10 __asm__("r10") = a3;
  register u64 r8 __asm__("r8") = a4;
  register u64 r9 __asm__("r9") = a5;
  u64 result = number | 0x2000000;
  unsigned char carry;
  __asm__ volatile("syscall\n\tsetc %2"
                   : "+a"(result), "+d"(a2), "=qm"(carry)
                   : "D"(a0), "S"(a1), "r"(r10), "r"(r8), "r"(r9)
                   : "rcx", "r11", "cc", "memory");
  *error = carry;
  secondary = a2;
  return result;
#endif
}
static int equal(const char *a, const char *b) {
  while (*a && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}
static u64 little_integer(const unsigned char *p, unsigned width) {
  u64 value = 0;
  for (unsigned i = 0; i != width; ++i)
    value |= (u64)p[i] << (i * 8);
  return value;
}
/* Raw Mach calls have a distinct return ABI. Record flags and live argument
 * carriers directly, without a libSystem wrapper or a BSD carry adapter. */
struct mach_observation {
  u64 value, flags, second, third;
};
static u64 mach_number(unsigned index) {
#if defined(__aarch64__)
  return (u64) - (long)index;
#else
  return 0x01000000UL | index;
#endif
}
static u64 mach_flags(unsigned carry) {
#if defined(__aarch64__)
  return carry ? 0x70000000UL : 0x90000000UL;
#else
  return 0x882UL | carry;
#endif
}
static struct mach_observation raw_trap(u64 number, u64 pointer, u64 flags) {
#if defined(__aarch64__)
  register u64 x0 __asm__("x0") = pointer;
  register u64 x1 __asm__("x1") = 0x1122334455667788UL;
  register u64 x2 __asm__("x2") = 0x8877665544332211UL;
  register u64 x16 __asm__("x16") = number;
  u64 after;
  __asm__ volatile("msr nzcv, %5\n\tsvc #0x80\n\tmrs %4, nzcv"
                   : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x16), "=&r"(after)
                   : "r"(flags)
                   : "cc", "memory");
  return (struct mach_observation){x0, after, x1, x2};
#else
  u64 result = number, second = 0x1122334455667788UL;
  u64 third = 0x8877665544332211UL, after;
  // Keep temporary flag pushes below the compiler's 128-byte red zone.
  // LEA leaves the flags untouched, and every asm operand is a register.
  __asm__ volatile("leaq -128(%%rsp), %%rsp\n\tpushq %5\n\tpopfq\n\t"
                   "syscall\n\tpushfq\n\tpopq %3\n\tleaq 128(%%rsp), %%rsp"
                   : "+a"(result), "+d"(second), "+S"(third), "=&r"(after)
                   : "D"(pointer), "r"(flags)
                   : "rcx", "r11", "cc", "memory");
  return (struct mach_observation){result, after, second, third};
#endif
}
static int preserved_flags(struct mach_observation value, u64 expected) {
#if defined(__aarch64__)
  return value.flags == expected;
#else
  return (value.flags & 0x8d5) == (expected & 0x8d5);
#endif
}
static int preserved_mach(struct mach_observation value, unsigned carry) {
  return preserved_flags(value, mach_flags(carry)) &&
         value.second == 0x1122334455667788UL &&
         value.third == 0x8877665544332211UL;
}
static int mach_timebase(int emit_values) {
  unsigned char bytes[10];
  unsigned error;
  for (unsigned i = 0; i != sizeof(bytes); ++i)
    bytes[i] = 0xa5;
  int check = 50;
#define MACH_EXPECT(expression)                                                \
  do {                                                                         \
    ++check;                                                                   \
    if (!(expression))                                                         \
      return check;                                                            \
  } while (0)
  for (unsigned carry = 0; carry != 2; ++carry) {
    const u64 number = mach_number(89);
    struct mach_observation value =
        raw_trap(number, (u64)(bytes + 1), mach_flags(carry));
    MACH_EXPECT(!value.value && preserved_mach(value, carry));
    MACH_EXPECT(little_integer(bytes + 1, 4) && little_integer(bytes + 5, 4) &&
                bytes[0] == 0xa5 && bytes[9] == 0xa5);
    const u64 pointers[] = {0, 1, (u64)-1};
    for (unsigned i = 0; i != 3; ++i) {
      value = raw_trap(number, pointers[i], mach_flags(carry));
      MACH_EXPECT(!value.value && preserved_mach(value, carry));
    }
    const u64 numbers[] = {number & 0xffffffffUL,
                           (number & 0xffffffffUL) | 0x1234567800000000UL};
    for (unsigned i = 0; i != 2; ++i) {
      value = raw_trap(numbers[i], (u64)(bytes + 1), mach_flags(carry));
      MACH_EXPECT(!value.value && preserved_mach(value, carry));
    }
  }
  if (!emit_values) {
    u64 pid = call(20, 0, 0, 0, 0, 0, 0, &error);
    MACH_EXPECT(!error && !secondary);
#if defined(__aarch64__)
    const u64 bsd_pid = 20;
    const u64 carry_mask = 0x20000000UL;
#else
    const u64 bsd_pid = 0x02000014;
    const u64 carry_mask = 1;
#endif
    struct mach_observation value =
        raw_trap(bsd_pid | 0x1234567800000000UL, 0, mach_flags(1));
    MACH_EXPECT(value.value == pid && !value.second &&
                preserved_flags(value, mach_flags(1) & ~carry_mask) &&
                value.third == 0x8877665544332211UL);
    // Positive ARM64 3/4 and x64 BSD-class 3/4 remain read/write, not clocks.
    MACH_EXPECT(call(3, 999, 0, 0, 0, 0, 0, &error) == 9 && error);
    MACH_EXPECT(call(4, 999, 0, 0, 0, 0, 0, &error) == 9 && error);
#if defined(__aarch64__)
    for (unsigned index = 3; index != 5; ++index)
      for (unsigned carry = 0; carry != 2; ++carry) {
        value = raw_trap(mach_number(index), 0, mach_flags(carry));
        MACH_EXPECT(preserved_mach(value, carry));
        value =
            raw_trap((mach_number(index) & 0xffffffffUL) | 0x1234567800000000UL,
                     0, mach_flags(carry));
        MACH_EXPECT(preserved_mach(value, carry));
      }
#endif
  }
  const char marker = 'h';
  u64 length = emit_values ? 8 : 1;
  MACH_EXPECT(call(4, 1, emit_values ? (u64)(bytes + 1) : (u64)&marker, length,
                   0, 0, 0, &error) == length &&
              !error);
#undef MACH_EXPECT
  return 37;
}
/* Slot 3/4 is invalid on native x64. Only the ARM64 native workload calls
 * them; x64 guest tests require an explicit unsupported-service stop. */
static int mach_clocks(unsigned selection) {
  u64 ticks[2];
  unsigned count = 0, error;
  for (unsigned index = 3; index != 5; ++index) {
    if (selection && index != selection + 2)
      continue;
    struct mach_observation value =
        raw_trap(mach_number(index), (u64)-1, mach_flags(1));
    if (!preserved_mach(value, 1))
      return 70;
    ticks[count++] = value.value;
  }
  u64 size = count * sizeof(u64);
  return call(4, 1, (u64)ticks, size, 0, 0, 0, &error) == size && !error ? 37
                                                                         : 71;
}
static int valid_timeval(const unsigned char *p) {
  return little_integer(p, 8) <= 0xffffffffUL &&
         little_integer(p + 8, 4) < 1000000 && !little_integer(p + 12, 4);
}
static void time_canaries(unsigned char *bytes) {
  for (unsigned i = 0; i != 40; ++i)
    bytes[i] = 0xa5;
}
static int time_error_secondary(u64 absolute_pointer) {
#if defined(__aarch64__)
  (void)absolute_pointer;
  return secondary == 0;
#else
  return secondary == absolute_pointer;
#endif
}
/* One original workload executes unchanged under the host kernel and model.
 * Live host clock values may change between calls; only the first tuple is
 * emitted by time-values for exact caller-supplied guest comparisons. */
static int time_calls(int emit_values) {
  unsigned error;
  unsigned char bytes[40], first[32];
  unsigned char *tv = bytes + 1, *tz = bytes + 17, *ticks = bytes + 25;
  int check = 50;
#define TIME_EXPECT(expression)                                                \
  do {                                                                         \
    ++check;                                                                   \
    if (!(expression))                                                         \
      return check;                                                            \
  } while (0)
  TIME_EXPECT(call(116, 0, 0, 0, 0, 0, 0, &error) == 0 && !error && !secondary);
  time_canaries(bytes);
  TIME_EXPECT(call(116, (u64)tv, (u64)tz, (u64)ticks, 0, 0, 0, &error) == 0 &&
              !error && !secondary);
  TIME_EXPECT(valid_timeval(tv) && little_integer(ticks, 8) != 0);
  TIME_EXPECT(bytes[0] == 0xa5 && bytes[33] == 0xa5);
  for (unsigned i = 0; i != 32; ++i)
    first[i] = bytes[i + 1];
  time_canaries(bytes);
  TIME_EXPECT(call(116, 1, (u64)tz, (u64)ticks, 0, 0, 0, &error) == 14 &&
              error && time_error_secondary((u64)ticks));
  TIME_EXPECT(little_integer(tv, 8) == 0xa5a5a5a5a5a5a5a5UL &&
              little_integer(tz, 8) == 0xa5a5a5a5a5a5a5a5UL &&
              little_integer(ticks, 8) == 0xa5a5a5a5a5a5a5a5UL);
  TIME_EXPECT(call(116, (u64)tv, 1, (u64)ticks, 0, 0, 0, &error) == 14 &&
              error && time_error_secondary((u64)ticks));
  TIME_EXPECT(valid_timeval(tv) &&
              little_integer(ticks, 8) == 0xa5a5a5a5a5a5a5a5UL);
  time_canaries(bytes);
  TIME_EXPECT(call(116, (u64)tv, (u64)tz, 1, 0, 0, 0, &error) == 14 && error &&
              time_error_secondary(1));
  TIME_EXPECT(valid_timeval(tv) &&
              little_integer(tz, 8) == little_integer(first + 16, 8));
  time_canaries(bytes);
  TIME_EXPECT(call(116, 0, (u64)tz, 0, 0, 0, 0, &error) == 0 && !error &&
              !secondary);
  TIME_EXPECT(little_integer(tz, 8) == little_integer(first + 16, 8) &&
              little_integer(tv, 8) == 0xa5a5a5a5a5a5a5a5UL &&
              little_integer(ticks, 8) == 0xa5a5a5a5a5a5a5a5UL);
  TIME_EXPECT(call(116, 0, 0, (u64)ticks, 0, 0, 0, &error) == 0 && !error &&
              !secondary);
  TIME_EXPECT(little_integer(ticks, 8) != 0 &&
              little_integer(tv, 8) == 0xa5a5a5a5a5a5a5a5UL);
  TIME_EXPECT(call(116, (u64)tv, 0, 0, 0, 0, 0, &error) == 0 && !error &&
              !secondary);
  TIME_EXPECT(valid_timeval(tv));
  // A final overlapping absolute write replaces timezone and seconds but
  // leaves the earlier microseconds and zero timeval padding visible.
  TIME_EXPECT(call(116, (u64)tv, (u64)tv, (u64)tv, 0, 0, 0, &error) == 0 &&
              !error && !secondary);
  TIME_EXPECT(
      little_integer(tv, 8) != 0 && little_integer(tv + 8, 4) < 1000000 &&
      !little_integer(tv + 12, 4) && bytes[0] == 0xa5 && bytes[33] == 0xa5);
  const char marker = 't';
  u64 length = emit_values ? 32 : 1;
  TIME_EXPECT(call(4, 1, emit_values ? (u64)first : (u64)&marker, length, 0, 0,
                   0, &error) == length &&
              !error);
#undef TIME_EXPECT
  return 37;
}
/* Inspect bounded records independently from the model's serializer. The
 * input directory contains only data and empty, in filesystem-defined order. */
static unsigned directory_names(const unsigned char *bytes, u64 size,
                                u64 file_inode) {
  unsigned seen = 0;
  for (u64 offset = 0; offset != size;) {
    if (size - offset < 32)
      return 0;
    const unsigned char *record = bytes + offset;
    u64 length = little_integer(record + 16, 2);
    u64 name_length = little_integer(record + 18, 2);
    if (length != 32 || name_length > 5 || !little_integer(record, 8) ||
        little_integer(record + 8, 8))
      return 0;
    for (u64 i = 21 + name_length; i != length; ++i)
      if (record[i])
        return 0;
    unsigned bit = equal((const char *)record + 21, ".")       ? 1
                   : equal((const char *)record + 21, "..")    ? 2
                   : equal((const char *)record + 21, "empty") ? 4
                   : equal((const char *)record + 21, "data")  ? 8
                                                               : 0;
    if (!bit || (seen & bit) || record[20] != (bit == 8 ? 8 : 4) ||
        (bit == 8 && little_integer(record, 8) != file_inode))
      return 0;
    seen |= bit;
    offset += length;
  }
  return seen;
}
static int directory_entries(const char *path) {
  unsigned error = 0;
  int check = 40;
#define ENTRY_EXPECT(expression)                                               \
  do {                                                                         \
    ++check;                                                                   \
    if (!(expression))                                                         \
      return check;                                                            \
  } while (0)
  char parent[1024];
  unsigned last = 0, length = 0;
  while (path[length] && length < 1023) {
    parent[length] = path[length];
    if (path[length] == '/')
      last = length;
    ++length;
  }
  ENTRY_EXPECT(path[0] == '/' && !path[length] && length > last + 1);
  parent[last ? last : 1] = 0;
  unsigned char bytes[1040], status[144];
  u64 position = 0;
  u64 file = call(5, (u64)path, 0, 0, 0, 0, 0, &error);
  ENTRY_EXPECT(!error);
  ENTRY_EXPECT(call(339, file, (u64)status, 0, 0, 0, 0, &error) == 0 && !error);
  const u64 inode = little_integer(status + 8, 8);
  ENTRY_EXPECT(
      call(344, file, (u64)bytes, 1024, (u64)&position, 0, 0, &error) == 22 &&
      error);
  ENTRY_EXPECT(call(344, 999, 0, 0, 0, 0, 0, &error) == 9 && error);
  u64 dir = call(5, (u64)parent, 0x100000, 0, 0, 0, 0, &error);
  ENTRY_EXPECT(!error);
  ENTRY_EXPECT(call(344, dir, (u64)bytes, 63, (u64)&position, 0, 0, &error) ==
                   22 &&
               error);
  ENTRY_EXPECT(call(344, dir, 0, 64, (u64)&position, 0, 0, &error) == 14 &&
               error);
  ENTRY_EXPECT(call(199, dir, 0, 1, 0, 0, 0, &error) == 0 && !error);
  u64 independent = call(5, (u64)parent, 0x100000, 0, 0, 0, 0, &error);
  ENTRY_EXPECT(!error);
  ENTRY_EXPECT(call(344, dir, (u64)bytes, 64, (u64)&position, 0, 0, &error) ==
                   64 &&
               !error && position == 0);
  ENTRY_EXPECT(directory_names(bytes, 64, inode) == 3);
  u64 copy = call(41, dir, 0, 0, 0, 0, 0, &error);
  ENTRY_EXPECT(!error);
  ENTRY_EXPECT(call(6, dir, 0, 0, 0, 0, 0, &error) == 0 && !error);
  unsigned seen = 3;
  for (unsigned i = 0; i != 2; ++i) {
    u64 before = call(199, copy, 0, 1, 0, 0, 0, &error);
    ENTRY_EXPECT(!error && before);
    ENTRY_EXPECT(
        call(344, copy, (u64)bytes, 32, (u64)&position, 0, 0, &error) == 32 &&
        !error && position == before);
    unsigned bit = directory_names(bytes, 32, inode);
    ENTRY_EXPECT((bit == 4 || bit == 8) && !(seen & bit));
    seen |= bit;
  }
  ENTRY_EXPECT(seen == 15);
  u64 terminal = call(199, copy, 0, 1, 0, 0, 0, &error);
  ENTRY_EXPECT(!error && terminal);
  ENTRY_EXPECT(call(344, copy, 0, 1, (u64)&position, 0, 0, &error) == 0 &&
               !error && position == terminal);
  ENTRY_EXPECT(call(344, copy, 0, 0, (u64)&position, 0, 0, &error) == 22 &&
               error);
  for (unsigned i = 0; i != sizeof(bytes); ++i)
    bytes[i] = 0xa5;
  ENTRY_EXPECT(call(344, independent, (u64)bytes, 1024, (u64)&position, 0, 0,
                    &error) == 128 &&
               !error && position == 0);
  ENTRY_EXPECT(directory_names(bytes, 128, inode) == 15 && bytes[128] == 0xa5 &&
               little_integer(bytes + 1020, 4) == 1 && bytes[1024] == 0xa5);
  ENTRY_EXPECT(call(199, copy, 0, 0, 0, 0, 0, &error) == 0 && !error);
  ENTRY_EXPECT(call(344, copy, (u64)bytes, 1024, 0, 0, 0, &error) == 14 &&
               error);
  ENTRY_EXPECT(directory_names(bytes, 128, inode) == 15);
  ENTRY_EXPECT(call(344, copy, 0, 1, (u64)&position, 0, 0, &error) == 0 &&
               !error && position);
  ENTRY_EXPECT(call(199, copy, 0, 0, 0, 0, 0, &error) == 0 && !error);
  /* A huge unsigned count wraps the flags address to zero. The data and
   * position copies still precede that fault; no huge allocation is needed. */
  ENTRY_EXPECT(call(344, copy, (u64)bytes, 4 - (u64)bytes, (u64)&position, 0, 0,
                    &error) == 14 &&
               error && position == 0);
  ENTRY_EXPECT(directory_names(bytes, 128, inode) == 15);
  ENTRY_EXPECT(call(344, copy, 0, 1, (u64)&position, 0, 0, &error) == 0 &&
               !error && position);
  ENTRY_EXPECT(call(199, copy, 0, 0, 0, 0, 0, &error) == 0 && !error);
  ENTRY_EXPECT(call(344, copy, (u64)(bytes + 16), (u64)-1, (u64)&position, 0, 0,
                    &error) == 128 &&
               !error && position == 0);
  ENTRY_EXPECT(directory_names(bytes + 16, 128, inode) == 15 &&
               little_integer(bytes + 11, 4) == 1);
  ENTRY_EXPECT(call(4, 1, (u64) "e", 1, 0, 0, 0, &error) == 1 && !error);
#undef ENTRY_EXPECT
  return 37;
}
/* Relative lookup and directory lifetime are compared with the native kernel.
 * The host harness supplies an isolated parent containing data and empty/. */
static int directory_calls(const char *path) {
  unsigned error = 0;
  int check = 180;
#define DIRECTORY_EXPECT(expression)                                           \
  do {                                                                         \
    ++check;                                                                   \
    if (!(expression))                                                         \
      return check;                                                            \
  } while (0)
  char parent[1024], canonical[1024];
  unsigned last = 0, length = 0;
  while (path[length] && length < 1023) {
    parent[length] = path[length];
    if (path[length] == '/')
      last = length;
    ++length;
  }
  DIRECTORY_EXPECT(path[0] == '/' && !path[length] && length > last + 1);
  parent[last ? last : 1] = 0;
  const char *name = path + last + 1;
  DIRECTORY_EXPECT(equal(name, "data"));
  u64 dir = call(463, (u64)-1, (u64)parent, 0x100000, 0, 0, 0, &error);
  DIRECTORY_EXPECT(!error);
  DIRECTORY_EXPECT(call(92, dir, 3, 0, 0, 0, 0, &error) == 0 && !error);
  DIRECTORY_EXPECT(call(92, dir, 50, (u64)canonical, 0, 0, 0, &error) == 0 &&
                   !error && canonical[0] == '/');
  DIRECTORY_EXPECT(call(12, (u64)canonical, 0, 0, 0, 0, 0, &error) == 0 &&
                   !error);
  u64 other = call(398, (u64) "empty/../.", 0x100000, 0, 0, 0, 0, &error);
  DIRECTORY_EXPECT(!error);
  DIRECTORY_EXPECT(call(3, dir, 0, 0, 0, 0, 0, &error) == 21 && error);
  DIRECTORY_EXPECT(call(153, dir, 0, 1, 0, 0, 0, &error) == 21 && error);
  DIRECTORY_EXPECT(call(153, dir, 0, 0, (u64)-1, 0, 0, &error) == 22 && error);
  DIRECTORY_EXPECT(call(199, dir, 7, 0, 0, 0, 0, &error) == 7 && !error);
  DIRECTORY_EXPECT(call(199, dir, (u64)-8, 1, 0, 0, 0, &error) == 22 && error);
  DIRECTORY_EXPECT(call(199, dir, 0, 1, 0, 0, 0, &error) == 7 && !error);
  DIRECTORY_EXPECT(call(197, 0, PAGE, 1, 2, dir, 0, &error) == 22 && error);
  DIRECTORY_EXPECT(call(463, (u64)-1, (u64) "", 0, 0, 0, 0, &error) == 9 &&
                   error);
  DIRECTORY_EXPECT(call(463, (u64)-1, 0, 0, 0, 0, 0, &error) == 14 && error);
  DIRECTORY_EXPECT(call(5, (u64) "data/../data", 0, 0, 0, 0, 0, &error) == 20 &&
                   error);
  DIRECTORY_EXPECT(
      call(5, (u64) "absent/../data", 0, 0, 0, 0, 0, &error) == 2 && error);
  u64 file = call(464, other, (u64) "empty/../data", 0, 0, 0, 0, &error);
  DIRECTORY_EXPECT(!error);
  DIRECTORY_EXPECT(call(463, file, (u64) "", 0, 0, 0, 0, &error) == 20 &&
                   error);
  DIRECTORY_EXPECT(call(13, file, 0, 0, 0, 0, 0, &error) == 20 && error);
  DIRECTORY_EXPECT(call(12, (u64) "absent", 0, 0, 0, 0, 0, &error) == 2 &&
                   error);
  unsigned char a[144], b[144];
  DIRECTORY_EXPECT(
      call(470, other, (u64) "./data", (u64)a, 0x20, 0, 0, &error) == 0 &&
      !error);
  DIRECTORY_EXPECT(call(470, file, 0, (u64)b, 0x400, 0, 0, &error) == 0 &&
                   !error);
  for (unsigned i = 0; i != 144; ++i)
    if (a[i] != b[i])
      return 250;
  DIRECTORY_EXPECT(call(470, (u64)-1, 0, 0, 1, 0, 0, &error) == 22 && error);
  DIRECTORY_EXPECT(call(13, other, 0, 0, 0, 0, 0, &error) == 0 && !error);
  DIRECTORY_EXPECT(call(6, other, 0, 0, 0, 0, 0, &error) == 0 && !error);
  DIRECTORY_EXPECT(call(90, file, dir, 0, 0, 0, 0, &error) == dir && !error);
  u64 fresh = call(5, (u64)name, 0, 0, 0, 0, 0, &error);
  DIRECTORY_EXPECT(!error);
  unsigned char byte = 0;
  DIRECTORY_EXPECT(call(3, fresh, (u64)&byte, 1, 0, 0, 0, &error) == 1 &&
                   !error && byte == '0');
  DIRECTORY_EXPECT(call(92, file, 50, (u64)canonical, 0, 0, 0, &error) == 0 &&
                   !error);
  u64 copy = call(41, file, 0, 0, 0, 0, 0, &error);
  DIRECTORY_EXPECT(!error);
  DIRECTORY_EXPECT(call(92, copy, 50, (u64)parent, 0, 0, 0, &error) == 0 &&
                   !error);
  DIRECTORY_EXPECT(equal(canonical, parent));
  DIRECTORY_EXPECT(call(4, 1, (u64) "d", 1, 0, 0, 0, &error) == 1 && !error);
#undef DIRECTORY_EXPECT
  return 37;
}
/* The same original raw-call program runs on native macOS and every guest
 * profile. The input is a read-only ten-byte regular file. */
static int file_mapping(const char *path) {
  unsigned error = 0;
  int check = 140;
#define MAPPING_EXPECT(expression)                                             \
  do {                                                                         \
    ++check;                                                                   \
    if (!(expression))                                                         \
      return check;                                                            \
  } while (0)
  u64 fd = call(5, (u64)path, 0, 0, 0, 0, 0, &error);
  MAPPING_EXPECT(!error);
  MAPPING_EXPECT(call(197, 0, 0, 1, 2, (u64)-1, 0, &error) == 9 && error);
  MAPPING_EXPECT(call(197, 0, 0, 1, 0x40002, (u64)-1, 0, &error) == 22 &&
                 error);
  MAPPING_EXPECT(call(197, 0, 1, 1, 0x40002, (u64)-1, 1, &error) == 22 &&
                 error);
  MAPPING_EXPECT(call(197, 0, (u64)-1, 1, 2, (u64)-1, 0, &error) == 22 &&
                 error);
  MAPPING_EXPECT(call(197, 0, PAGE, 1, 2, (u64)-1, (u64)-PAGE, &error) == 22 &&
                 error);
  MAPPING_EXPECT(call(197, 0, 0, 1, 2, fd, 0, &error) == 0 && !error);
  MAPPING_EXPECT(call(199, fd, 2, 0, 0, 0, 0, &error) == 2 && !error);
  u64 first = call(197, 0, 1, 3, 2, fd, 0, &error);
  MAPPING_EXPECT(!error && first && first % PAGE == 0);
  volatile unsigned char *a = (void *)first;
  for (unsigned i = 0; i != 10; ++i)
    if (a[i] != '0' + i)
      return 180;
  MAPPING_EXPECT(a[10] == 0 && a[PAGE - 1] == 0);
  a[0] = 'm';
  a[PAGE - 1] = 'x';
  u64 second = call(197, 0, PAGE, 0, 0x40002, fd, 0, &error);
  MAPPING_EXPECT(!error && second != first && second % PAGE == 0);
  MAPPING_EXPECT(call(74, second, PAGE, 2, 0, 0, 0, &error) == 0 && !error);
  volatile unsigned char *b = (void *)second;
  MAPPING_EXPECT(b[0] == '0' && b[9] == '9' && b[PAGE - 1] == 0);
  b[0] = 'q';
  MAPPING_EXPECT(a[0] == 'm');
  unsigned char original = 0;
  MAPPING_EXPECT(call(153, fd, (u64)&original, 1, 0, 0, 0, &error) == 1 &&
                 !error);
  MAPPING_EXPECT(original == '0');
  MAPPING_EXPECT(call(199, fd, 0, 1, 0, 0, 0, &error) == 2 && !error);
  MAPPING_EXPECT(call(6, fd, 0, 0, 0, 0, 0, &error) == 0 && !error);
  MAPPING_EXPECT(call(197, 0, PAGE, 1, 2, fd, 0, &error) == 9 && error);
  MAPPING_EXPECT(a[0] == 'm' && a[PAGE - 1] == 'x' && b[0] == 'q');
  MAPPING_EXPECT(call(73, second, PAGE, 0, 0, 0, 0, &error) == 0 && !error);
  MAPPING_EXPECT(a[0] == 'm');
  MAPPING_EXPECT(call(4, 1, first, 1, 0, 0, 0, &error) == 1 && !error);
  MAPPING_EXPECT(call(73, first, PAGE, 0, 0, 0, 0, &error) == 0 && !error);
  u64 fresh = call(197, first, PAGE, 3, 0x41002, (u64)-1, 0, &error);
  MAPPING_EXPECT(!error && fresh == first);
  MAPPING_EXPECT(*(volatile unsigned char *)fresh == 0);
  MAPPING_EXPECT(call(73, fresh, PAGE, 0, 0, 0, 0, &error) == 0 && !error);
#undef MAPPING_EXPECT
  return 37;
}
/* Independent LP64 stat64 ABI checks; all three calls must describe the same
 * supplied regular file and must leave the open description's offset alone. */
static int file_status(const char *path) {
  unsigned error = 0;
  unsigned char a[146], b[146];
  int check = 80;
#define STATUS_EXPECT(expression)                                              \
  do {                                                                         \
    ++check;                                                                   \
    if (!(expression))                                                         \
      return check;                                                            \
  } while (0)
  a[0] = b[0] = 0xab;
  a[145] = b[145] = 0xcd;
  STATUS_EXPECT(call(338, (u64)path, (u64)(a + 1), 0, 0, 0, 0, &error) == 0 &&
                !error);
  STATUS_EXPECT(a[0] == 0xab && a[145] == 0xcd);
  STATUS_EXPECT((little_integer(a + 5, 2) & 0170000) == 0100000);
  STATUS_EXPECT(little_integer(a + 7, 2) != 0);
  STATUS_EXPECT(little_integer(a + 97, 8) == 10);
  STATUS_EXPECT(little_integer(a + 113, 4) > 0);
  for (unsigned i = 41; i <= 89; i += 16)
    STATUS_EXPECT(little_integer(a + i, 8) < 1000000000);
  for (unsigned i = 25; i != 33; ++i)
    STATUS_EXPECT(a[i] == 0);
  for (unsigned i = 125; i != 145; ++i)
    STATUS_EXPECT(a[i] == 0);
  u64 fd = call(5, (u64)path, 0, 0, 0, 0, 0, &error);
  STATUS_EXPECT(!error);
  STATUS_EXPECT(call(199, fd, 2, 0, 0, 0, 0, &error) == 2 && !error);
  u64 copy = call(41, fd, 0, 0, 0, 0, 0, &error);
  STATUS_EXPECT(!error && copy != fd);
  STATUS_EXPECT(call(6, fd, 0, 0, 0, 0, 0, &error) == 0 && !error);
  STATUS_EXPECT(call(339, fd, 0, 0, 0, 0, 0, &error) == 9 && error);
  STATUS_EXPECT(call(339, copy, (u64)(b + 1), 0, 0, 0, 0, &error) == 0 &&
                !error);
  for (unsigned i = 0; i != 146; ++i)
    if (a[i] != b[i])
      return 130;
  STATUS_EXPECT(call(199, copy, 0, 1, 0, 0, 0, &error) == 2 && !error);
  STATUS_EXPECT(call(340, (u64)path, (u64)(b + 1), 0, 0, 0, 0, &error) == 0 &&
                !error);
  for (unsigned i = 0; i != 146; ++i)
    if (a[i] != b[i])
      return 131;
  STATUS_EXPECT(call(339, copy, 0, 0, 0, 0, 0, &error) == 14 && error);
  STATUS_EXPECT(call(338, (u64)path, 0, 0, 0, 0, 0, &error) == 14 && error);
  STATUS_EXPECT(call(340, (u64) "", 0, 0, 0, 0, 0, &error) == 2 && error);
  char descendant[1024];
  unsigned length = 0;
  while (path[length] && length < 700) {
    descendant[length] = path[length];
    ++length;
  }
  STATUS_EXPECT(!path[length]);
  descendant[length] = '/';
  for (unsigned i = 0; i != 256; ++i)
    descendant[length + 1 + i] = 'a';
  descendant[length + 257] = 0;
  STATUS_EXPECT(call(338, (u64)descendant, 0, 0, 0, 0, 0, &error) == 20 &&
                error);
  descendant[length] = '-';
  descendant[length + 1] = 'x';
  descendant[length + 2] = 0;
  STATUS_EXPECT(call(340, (u64)descendant, 0, 0, 0, 0, 0, &error) == 2 &&
                error);
  STATUS_EXPECT(call(6, copy, 0, 0, 0, 0, 0, &error) == 0 && !error);
  STATUS_EXPECT(call(4, 1, (u64) "s", 1, 0, 0, 0, &error) == 1 && !error);
#undef STATUS_EXPECT
  return 37;
}
/* Existing-file mutations execute unchanged against the native BSD ABI. */
/* Creation metadata uses native ownership/mode rules. Exact virtual inode,
 * timestamps and allocation records are emitted only in the policy mode. */
static int created_file_metadata(const char *path, int virtual_record) {
  unsigned error;
  int check = 80;
#define CREATE_META_EXPECT(expression)                                         \
  do {                                                                         \
    ++check;                                                                   \
    if (!(expression))                                                         \
      return check;                                                            \
  } while (0)
  u64 saved_mask = call(60, 07027, 0, 0, 0, 0, 0, &error);
  CREATE_META_EXPECT(!error && saved_mask <= 07777);
  CREATE_META_EXPECT(call(60, (u64)-1, 0, 0, 0, 0, 0, &error) == 07027 &&
                     !error);
  CREATE_META_EXPECT(
      call(60, 0x1234567800000017UL, 0, 0, 0, 0, 0, &error) == 07777 && !error);
  char parent[1024];
  unsigned size = 0, slash = 0;
  while (path[size]) {
    parent[size] = path[size];
    if (path[size] == '/')
      slash = size;
    ++size;
  }
  parent[slash ? slash : 1] = 0;
  u64 directory = call(5, (u64)parent, 0, 0, 0, 0, 0, &error);
  CREATE_META_EXPECT(!error);
  unsigned char parent_status[144], original_status[144], status[144],
      fresh[144];
  CREATE_META_EXPECT(
      call(339, directory, (u64)parent_status, 0, 0, 0, 0, &error) == 0 &&
      !error);
  u64 original = call(5, (u64)path, 0, 0, 0, 0, 0, &error);
  CREATE_META_EXPECT(!error);
  CREATE_META_EXPECT(
      call(339, original, (u64)original_status, 0, 0, 0, 0, &error) == 0 &&
      !error);
  CREATE_META_EXPECT(call(10, (u64)path, 0, 0, 0, 0, 0, &error) == 0 && !error);
  u64 a = call(398, (u64)path, 0xe02, 0x12345678ffff0fffUL, 0, 0, 0, &error);
  CREATE_META_EXPECT(!error);
  CREATE_META_EXPECT(call(339, a, (u64)status, 0, 0, 0, 0, &error) == 0 &&
                     !error);
  u64 uid = call(25, 0, 0, 0, 0, 0, 0, &error);
  CREATE_META_EXPECT(!error && little_integer(status + 16, 4) == uid);
  CREATE_META_EXPECT(little_integer(status, 4) ==
                     little_integer(parent_status, 4));
  CREATE_META_EXPECT(little_integer(status + 20, 4) ==
                     little_integer(parent_status + 20, 4));
  CREATE_META_EXPECT(little_integer(status + 4, 2) == 0100750 &&
                     little_integer(status + 6, 2) == 1 &&
                     little_integer(status + 96, 8) == 0 &&
                     little_integer(status + 104, 8) == 0);
  u64 inode = little_integer(status + 8, 8);
  CREATE_META_EXPECT(inode && inode != little_integer(original_status + 8, 8));
  CREATE_META_EXPECT(call(92, a, 3, 0, 0, 0, 0, &error) == 2 && !error);
  CREATE_META_EXPECT(call(154, a, (u64) "x", 1, 4096, 0, 0, &error) == 1 &&
                     !error);
  CREATE_META_EXPECT(call(201, a, 8200, 0, 0, 0, 0, &error) == 0 && !error);
  u64 d = call(41, a, 0, 0, 0, 0, 0, &error);
  CREATE_META_EXPECT(!error);
  CREATE_META_EXPECT(call(10, (u64)path, 0, 0, 0, 0, 0, &error) == 0 && !error);
  CREATE_META_EXPECT(call(339, d, (u64)status, 0, 0, 0, 0, &error) == 0 &&
                     !error);
  CREATE_META_EXPECT(little_integer(status + 8, 8) == inode &&
                     little_integer(status + 6, 2) == 0 &&
                     little_integer(status + 96, 8) == 8200 &&
                     little_integer(status + 104, 8) > 0);
  CREATE_META_EXPECT(call(60, 0022, 0, 0, 0, 0, 0, &error) == 0027 && !error);
  u64 b =
      call(463, directory, (u64)(path + slash + 1), 0xa00, 0666, 0, 0, &error);
  CREATE_META_EXPECT(!error);
  CREATE_META_EXPECT(call(339, b, (u64)fresh, 0, 0, 0, 0, &error) == 0 &&
                     !error);
  CREATE_META_EXPECT(little_integer(fresh + 8, 8) != inode &&
                     little_integer(fresh + 4, 2) == 0100644 &&
                     little_integer(fresh + 6, 2) == 1 &&
                     little_integer(fresh + 96, 8) == 0);
  CREATE_META_EXPECT(little_integer(fresh + 20, 4) ==
                     little_integer(parent_status + 20, 4));
  CREATE_META_EXPECT(call(60, saved_mask, 0, 0, 0, 0, 0, &error) == 0022 &&
                     !error);
  CREATE_META_EXPECT(call(10, (u64)path, 0, 0, 0, 0, 0, &error) == 0 && !error);
  const u64 fds[] = {original, a, b, d, directory};
  for (unsigned i = 0; i != 5; ++i)
    CREATE_META_EXPECT(call(6, fds[i], 0, 0, 0, 0, 0, &error) == 0 && !error);
  if (virtual_record) {
    CREATE_META_EXPECT(call(4, 1, (u64)status, sizeof(status), 0, 0, 0,
                            &error) == sizeof(status) &&
                       !error);
  } else {
    CREATE_META_EXPECT(call(4, 1, (u64) "q", 1, 0, 0, 0, &error) == 1 &&
                       !error);
  }
#undef CREATE_META_EXPECT
  return 37;
}

/* Original native/guest same-parent rename and replacement comparison. */
static int renamed_file(const char *path) {
  unsigned error;
  int check = 80;
#define RENAME_EXPECT(expression)                                              \
  do {                                                                         \
    ++check;                                                                   \
    if (!(expression))                                                         \
      return check;                                                            \
  } while (0)
  u64 old = call(5, (u64)path, 2, 0, 0, 0, 0, &error);
  RENAME_EXPECT(!error);
  char original[1024], parent[1024], temporary[1024], final[1024], after[1024],
      trailing[1024], bytes[10];
  RENAME_EXPECT(call(92, old, 50, (u64)original, 0, 0, 0, &error) == 0 &&
                !error);
  unsigned size = 0, slash = 0;
  while (original[size]) {
    parent[size] = original[size];
    if (original[size] == '/')
      slash = size;
    ++size;
  }
  RENAME_EXPECT(slash + 12 < sizeof(temporary));
  parent[slash ? slash : 1] = 0;
  for (unsigned i = 0; i <= slash; ++i)
    temporary[i] = final[i] = trailing[i] = original[i];
  const char names[][12] = {"rename.tmp", "rename.end", "rename.end/"};
  for (unsigned i = 0; i != 12; ++i) {
    temporary[slash + 1 + i] = names[0][i];
    final[slash + 1 + i] = names[1][i];
    trailing[slash + 1 + i] = names[2][i];
  }
  u64 directory = call(5, (u64)parent, 0, 0, 0, 0, 0, &error);
  RENAME_EXPECT(!error);
  unsigned char old_status[144], source_status[144], status[144];
  RENAME_EXPECT(call(339, old, (u64)old_status, 0, 0, 0, 0, &error) == 0 &&
                !error);
  u64 a = call(5, (u64)temporary, 0x01000a02, 0600, 0, 0, 0, &error);
  RENAME_EXPECT(!error);
  RENAME_EXPECT(call(4, a, (u64) "new", 3, 0, 0, 0, &error) == 3 && !error);
  u64 d = call(41, a, 0, 0, 0, 0, 0, &error);
  RENAME_EXPECT(!error);
  u64 independent = call(5, (u64)temporary, 0, 0, 0, 0, 0, &error);
  RENAME_EXPECT(!error);
  RENAME_EXPECT(call(339, a, (u64)source_status, 0, 0, 0, 0, &error) == 0 &&
                !error);
  RENAME_EXPECT(call(199, old, 4, 0, 0, 0, 0, &error) == 4 && !error);
  u64 source_map = call(197, 0, PAGE, 1, 2, a, 0, &error);
  RENAME_EXPECT(!error);
  u64 old_map = call(197, 0, PAGE, 1, 2, old, 0, &error);
  RENAME_EXPECT(!error);
  RENAME_EXPECT(call(128, (u64)temporary, (u64)temporary, 0, 0, 0, 0, &error) ==
                    0 &&
                !error);
  RENAME_EXPECT(call(128, (u64) final, (u64)-1, 0, 0, 0, 0, &error) == 2 &&
                error);
  RENAME_EXPECT(call(128, (u64)temporary, (u64)trailing, 0, 0, 0, 0, &error) ==
                    2 &&
                error);
  RENAME_EXPECT(call(465, directory, (u64)(temporary + slash + 1), directory,
                     (u64) ".", 0, 0, &error) == 22 &&
                error);
  RENAME_EXPECT(call(465, directory, (u64)(temporary + slash + 1), directory,
                     (u64) "..", 0, 0, &error) == 22 &&
                error);
  RENAME_EXPECT(call(488, (u64)-1, (u64)-1, (u64)-1, (u64)-1, 8, 0, &error) ==
                    22 &&
                error);
  RENAME_EXPECT(call(488, (u64)-1, (u64)-1, (u64)-1, (u64)-1, 6, 0, &error) ==
                    22 &&
                error);
  RENAME_EXPECT(call(465, directory, (u64)(temporary + slash + 1), directory,
                     (u64)(final + slash + 1), 0, 0, &error) == 0 &&
                !error);
  const u64 source_fds[] = {a, d, independent};
  for (unsigned i = 0; i != 3; ++i) {
    RENAME_EXPECT(call(92, source_fds[i], 50, (u64)after, 0, 0, 0, &error) ==
                      0 &&
                  !error);
    RENAME_EXPECT(equal(after, final));
  }
  RENAME_EXPECT(call(488, (u64)-1, (u64) final, (u64)-1, (u64)original,
                     0x1234567800000010UL, 0, &error) == 0 &&
                !error);
  RENAME_EXPECT(call(339, old, (u64)status, 0, 0, 0, 0, &error) == 0 && !error);
  RENAME_EXPECT(little_integer(status + 8, 8) ==
                    little_integer(old_status + 8, 8) &&
                little_integer(status + 6, 2) == 0 &&
                little_integer(status + 96, 8) == 10);
  RENAME_EXPECT(call(339, a, (u64)status, 0, 0, 0, 0, &error) == 0 && !error);
  RENAME_EXPECT(little_integer(status + 8, 8) ==
                    little_integer(source_status + 8, 8) &&
                little_integer(status + 6, 2) == 1 &&
                little_integer(status + 96, 8) == 3);
  unsigned preserved = 1;
  for (unsigned i = 0; i != sizeof(status); ++i)
    // Identity, ownership, mtime, birth time and size survive the new name.
    if (i < 24 || (i >= 48 && i < 64) || (i >= 80 && i < 104))
      preserved &= status[i] == source_status[i];
  RENAME_EXPECT(preserved);
  RENAME_EXPECT(
      call(128, (u64)original, (u64) final, 0, 0, 0, 0, &error) == 0 && !error);
  for (unsigned i = 0; i != 3; ++i) {
    RENAME_EXPECT(call(92, source_fds[i], 50, (u64)after, 0, 0, 0, &error) ==
                      0 &&
                  !error);
    RENAME_EXPECT(equal(after, final));
  }
  RENAME_EXPECT(call(92, old, 50, (u64)after, 0, 0, 0, &error) == 0 && !error);
  RENAME_EXPECT(equal(after, original));
  RENAME_EXPECT(call(92, a, 3, 0, 0, 0, 0, &error) == 0x10002 && !error);
  RENAME_EXPECT(call(92, a, 1, 0, 0, 0, 0, &error) == 1 && !error);
  RENAME_EXPECT(call(92, d, 1, 0, 0, 0, 0, &error) == 0 && !error);
  RENAME_EXPECT(call(199, d, 0, 1, 0, 0, 0, &error) == 3 && !error);
  RENAME_EXPECT(call(199, independent, 0, 1, 0, 0, 0, &error) == 0 && !error);
  RENAME_EXPECT(call(199, old, 0, 1, 0, 0, 0, &error) == 4 && !error);
  RENAME_EXPECT(call(153, old, (u64)bytes, 10, 0, 0, 0, &error) == 10 &&
                !error);
  RENAME_EXPECT(bytes[0] == '0' && bytes[9] == '9');
  RENAME_EXPECT(((volatile char *)source_map)[0] == 'n' &&
                ((volatile char *)source_map)[2] == 'w' &&
                ((volatile char *)old_map)[0] == '0');
  RENAME_EXPECT(call(3, independent, (u64)bytes, 10, 0, 0, 0, &error) == 3 &&
                !error);
  RENAME_EXPECT(bytes[0] == 'n' && bytes[2] == 'w');
  RENAME_EXPECT(call(5, (u64)original, 0, 0, 0, 0, 0, &error) == 2 && error);
  u64 fresh = call(5, (u64) final, 0, 0, 0, 0, 0, &error);
  RENAME_EXPECT(!error);
  RENAME_EXPECT(call(3, fresh, (u64)bytes, 10, 0, 0, 0, &error) == 3 && !error);
  RENAME_EXPECT(bytes[0] == 'n' && bytes[2] == 'w');
  RENAME_EXPECT(call(73, source_map, PAGE, 0, 0, 0, 0, &error) == 0 && !error);
  RENAME_EXPECT(call(73, old_map, PAGE, 0, 0, 0, 0, &error) == 0 && !error);
  RENAME_EXPECT(call(154, old, (u64) "x", 1, 0, 0, 0, &error) == 1 && !error);
  RENAME_EXPECT(call(153, a, (u64)bytes, 10, 0, 0, 0, &error) == 3 && !error);
  RENAME_EXPECT(bytes[0] == 'n' && bytes[2] == 'w');
  RENAME_EXPECT(call(10, (u64) final, 0, 0, 0, 0, 0, &error) == 0 && !error);
  const u64 fds[] = {old, a, d, independent, fresh, directory};
  for (unsigned i = 0; i != 6; ++i)
    RENAME_EXPECT(call(6, fds[i], 0, 0, 0, 0, 0, &error) == 0 && !error);
  RENAME_EXPECT(call(4, 1, (u64) "r", 1, 0, 0, 0, &error) == 1 && !error);
#undef RENAME_EXPECT
  return 37;
}

/* Original native/guest creation and same-name object lifetime comparison. */
static int created_file(const char *path) {
  unsigned error;
  int check = 80;
#define CREATE_EXPECT(expression)                                              \
  do {                                                                         \
    ++check;                                                                   \
    if (!(expression))                                                         \
      return check;                                                            \
  } while (0)
  char parent[1024], before[1024], after[1024], trailing[1024], bytes[10];
  unsigned size = 0, slash = 0;
  while (path[size]) {
    parent[size] = trailing[size] = path[size];
    if (path[size] == '/')
      slash = size;
    ++size;
  }
  parent[slash ? slash : 1] = 0;
  u64 directory = call(5, (u64)parent, 0x200, 0600, 0, 0, 0, &error);
  CREATE_EXPECT(!error);
  CREATE_EXPECT(call(5, (u64)parent, 0xe02, 0600, 0, 0, 0, &error) == 17 &&
                error);
  u64 old = call(5, (u64)path, 2, 0, 0, 0, 0, &error);
  CREATE_EXPECT(!error);
  CREATE_EXPECT(call(92, old, 50, (u64)before, 0, 0, 0, &error) == 0 && !error);
  CREATE_EXPECT(call(5, (u64)path, 0xe02, 0600, 0, 0, 0, &error) == 17 &&
                error);
  CREATE_EXPECT(call(5, (u64)-1, 0x100200, 0600, 0, 0, 0, &error) == 22 &&
                error);
  CREATE_EXPECT(call(10, (u64)path, 0, 0, 0, 0, 0, &error) == 0 && !error);
  // Inline bytes keep this freestanding fixture free of pointer rebases.
  const char suffixes[][8] = {"/", "//", "/.", "/..", "/../new"};
  for (unsigned i = 0; i != 5; ++i) {
    unsigned j = 0;
    do {
      trailing[size + j] = suffixes[i][j];
    } while (suffixes[i][j++]);
    CREATE_EXPECT(call(5, (u64)trailing, 0xa02, 0600, 0, 0, 0, &error) == 2 &&
                  error);
  }
  CREATE_EXPECT(call(5, (u64)path, 0x800, 0, 0, 0, 0, &error) == 2 && error);
  u64 a = call(464, directory, (u64)(path + slash + 1), 0x1234567801000e00UL,
               0600, 0, 0, &error);
  CREATE_EXPECT(!error);
  CREATE_EXPECT(call(92, a, 3, 0, 0, 0, 0, &error) == 0 && !error);
  CREATE_EXPECT(call(92, a, 1, 0, 0, 0, 0, &error) == 1 && !error);
  CREATE_EXPECT(call(199, a, 0, 2, 0, 0, 0, &error) == 0 && !error);
  CREATE_EXPECT(call(4, a, (u64) "x", 1, 0, 0, 0, &error) == 9 && error);
  u64 b = call(398, (u64)path, 10, 0, 0, 0, 0, &error);
  CREATE_EXPECT(!error);
  u64 d = call(41, b, 0, 0, 0, 0, 0, &error);
  CREATE_EXPECT(!error);
  CREATE_EXPECT(call(4, b, (u64) "abc", 3, 0, 0, 0, &error) == 3 && !error);
  CREATE_EXPECT(call(199, d, 0, 1, 0, 0, 0, &error) == 3 && !error);
  CREATE_EXPECT(call(199, a, 0, 1, 0, 0, 0, &error) == 0 && !error);
  CREATE_EXPECT(call(154, d, (u64) "Z", 1, 5, 0, 0, &error) == 1 && !error);
  CREATE_EXPECT(call(153, a, (u64)bytes, 10, 0, 0, 0, &error) == 6 && !error);
  CREATE_EXPECT(bytes[0] == 'a' && bytes[2] == 'c' && !bytes[3] && !bytes[4] &&
                bytes[5] == 'Z');
  CREATE_EXPECT(call(92, b, 3, 0, 0, 0, 0, &error) == 0x1000a && !error);
  CREATE_EXPECT(call(92, old, 50, (u64)after, 0, 0, 0, &error) == 0 && !error);
  CREATE_EXPECT(equal(before, after));
  CREATE_EXPECT(call(92, a, 50, (u64)after, 0, 0, 0, &error) == 0 && !error);
  CREATE_EXPECT(equal(before, after));
  unsigned char status[144];
  CREATE_EXPECT(call(339, old, (u64)status, 0, 0, 0, 0, &error) == 0 && !error);
  CREATE_EXPECT(little_integer(status + 6, 2) == 0 &&
                little_integer(status + 96, 8) == 10);
  CREATE_EXPECT(call(5, (u64)path, 0xe02, 0600, 0, 0, 0, &error) == 17 &&
                error);
  CREATE_EXPECT(call(199, a, 0, 2, 0, 0, 0, &error) == 6 && !error);
  u64 truncated = call(5, (u64)path, 0x600, 0600, 0, 0, 0, &error);
  CREATE_EXPECT(!error);
  CREATE_EXPECT(call(92, truncated, 3, 0, 0, 0, 0, &error) == 0x10000 &&
                !error);
  CREATE_EXPECT(call(199, a, 0, 2, 0, 0, 0, &error) == 0 && !error);
  CREATE_EXPECT(call(153, old, (u64)bytes, 10, 0, 0, 0, &error) == 10 &&
                !error);
  CREATE_EXPECT(bytes[0] == '0' && bytes[9] == '9');
  const u64 fds[] = {old, a, b, d, truncated};
  for (unsigned i = 0; i != 5; ++i)
    CREATE_EXPECT(call(6, fds[i], 0, 0, 0, 0, 0, &error) == 0 && !error);
  CREATE_EXPECT(
      call(472, directory, (u64)(path + slash + 1), 0, 0, 0, 0, &error) == 0 &&
      !error);
  for (u64 access = 0; access != 3; ++access) {
    u64 fresh = call(398, (u64)path, 0x600 | access, 0600, 0, 0, 0, &error);
    CREATE_EXPECT(!error);
    CREATE_EXPECT(call(92, fresh, 3, 0, 0, 0, 0, &error) == access && !error);
    CREATE_EXPECT(call(6, fresh, 0, 0, 0, 0, 0, &error) == 0 && !error);
    CREATE_EXPECT(call(10, (u64)path, 0, 0, 0, 0, 0, &error) == 0 && !error);
  }
  CREATE_EXPECT(call(6, directory, 0, 0, 0, 0, 0, &error) == 0 && !error);
  CREATE_EXPECT(call(4, 1, (u64) "c", 1, 0, 0, 0, &error) == 1 && !error);
#undef CREATE_EXPECT
  return 37;
}

/* Original native/guest namespace and open-object lifetime comparison. */
static int unlinked_file(const char *path) {
  unsigned error;
  int check = 80;
#define UNLINK_EXPECT(expression)                                              \
  do {                                                                         \
    ++check;                                                                   \
    if (!(expression))                                                         \
      return check;                                                            \
  } while (0)
  u64 a = call(5, (u64)path, 2, 0, 0, 0, 0, &error);
  UNLINK_EXPECT(!error);
  u64 b = call(5, (u64)path, 0, 0, 0, 0, 0, &error);
  UNLINK_EXPECT(!error);
  u64 d = call(41, a, 0, 0, 0, 0, 0, &error);
  UNLINK_EXPECT(!error);
  char before[1024], after[1024], parent[1024], data[10];
  unsigned size = 0, slash = 0;
  while (path[size]) {
    parent[size] = path[size];
    if (path[size] == '/')
      slash = size;
    ++size;
  }
  parent[slash ? slash : 1] = 0;
  u64 directory = call(5, (u64)parent, 0, 0, 0, 0, 0, &error);
  UNLINK_EXPECT(!error);
  UNLINK_EXPECT(call(92, a, 50, (u64)before, 0, 0, 0, &error) == 0 && !error);
  UNLINK_EXPECT(call(199, a, 4, 0, 0, 0, 0, &error) == 4 && !error);
  UNLINK_EXPECT(call(472, 999, (u64)-1, 0x80000000, 0, 0, 0, &error) == 22 &&
                error);
  UNLINK_EXPECT(call(472, 999, (u64)-1, 0, 0, 0, 0, &error) == 14 && error);
  UNLINK_EXPECT(call(472, 999, (u64) "", 0, 0, 0, 0, &error) == 9 && error);
  UNLINK_EXPECT(
      call(472, a, (u64)(path + slash + 1), 0, 0, 0, 0, &error) == 20 && error);
  UNLINK_EXPECT(call(472, directory, (u64)(path + slash + 1),
                     0x1234567800000800UL, 0, 0, 0, &error) == 0 &&
                !error);
  UNLINK_EXPECT(call(5, (u64)path, 0, 0, 0, 0, 0, &error) == 2 && error);
  UNLINK_EXPECT(call(10, (u64)path, 0, 0, 0, 0, 0, &error) == 2 && error);
  UNLINK_EXPECT(call(92, d, 50, (u64)after, 0, 0, 0, &error) == 0 && !error);
  UNLINK_EXPECT(equal(before, after));
  UNLINK_EXPECT(call(92, d, 50, (u64)-1, 0, 0, 0, &error) == 14 && error);
  UNLINK_EXPECT(call(199, d, 0, 1, 0, 0, 0, &error) == 4 && !error);
  UNLINK_EXPECT(call(199, b, 0, 1, 0, 0, 0, &error) == 0 && !error);
  UNLINK_EXPECT(call(92, d, 3, 0, 0, 0, 0, &error) == 2 && !error);
  unsigned char status[144];
  UNLINK_EXPECT(call(339, b, (u64)status, 0, 0, 0, 0, &error) == 0 && !error);
  UNLINK_EXPECT(little_integer(status + 6, 2) == 0 &&
                little_integer(status + 96, 8) == 10);
  UNLINK_EXPECT(call(154, a, (u64) "XY", 2, 0, 0, 0, &error) == 2 && !error);
  UNLINK_EXPECT(call(153, b, (u64)data, 10, 0, 0, 0, &error) == 10 && !error);
  UNLINK_EXPECT(data[0] == 'X' && data[1] == 'Y' && data[2] == '2' &&
                data[9] == '9');
  UNLINK_EXPECT(call(201, d, 3, 0, 0, 0, 0, &error) == 0 && !error);
  UNLINK_EXPECT(call(339, b, (u64)status, 0, 0, 0, 0, &error) == 0 && !error);
  UNLINK_EXPECT(little_integer(status + 6, 2) == 0 &&
                little_integer(status + 96, 8) == 3);
  UNLINK_EXPECT(call(199, a, 0, 1, 0, 0, 0, &error) == 4 && !error);
  UNLINK_EXPECT(call(13, directory, 0, 0, 0, 0, 0, &error) == 0 && !error);
  UNLINK_EXPECT(call(5, (u64)(path + slash + 1), 0, 0, 0, 0, 0, &error) == 2 &&
                error);
  const u64 fds[] = {a, b, d, directory};
  for (unsigned i = 0; i != 4; ++i)
    UNLINK_EXPECT(call(6, fds[i], 0, 0, 0, 0, 0, &error) == 0 && !error);
  UNLINK_EXPECT(call(5, (u64)path, 0, 0, 0, 0, 0, &error) == 2 && error);
  UNLINK_EXPECT(call(4, 1, (u64) "u", 1, 0, 0, 0, &error) == 1 && !error);
#undef UNLINK_EXPECT
  return 37;
}

/* Native and guest comparison of sparse-seek boundaries and cursor lifetime.
 * Earlier data/hole placement is filesystem-specific; only occupied bytes,
 * dense initial input and EOF have fixed expected positions here. */
static int sparse_file_seek(const char *path) {
  unsigned error;
  int check = 80;
#define SPARSE_EXPECT(expression)                                              \
  do {                                                                         \
    ++check;                                                                   \
    if (!(expression))                                                         \
      return check;                                                            \
  } while (0)
  u64 a = call(5, (u64)path, 2, 0, 0, 0, 0, &error);
  SPARSE_EXPECT(!error);
  u64 b = call(5, (u64)path, 0, 0, 0, 0, 0, &error);
  SPARSE_EXPECT(!error);
  u64 d = call(41, a, 0, 0, 0, 0, 0, &error);
  SPARSE_EXPECT(!error);
  SPARSE_EXPECT(call(199, a, 2, 4, 0, 0, 0, &error) == 2 && !error);
  SPARSE_EXPECT(call(199, d, 0, 1, 0, 0, 0, &error) == 2 && !error);
  SPARSE_EXPECT(call(199, b, 0, 1, 0, 0, 0, &error) == 0 && !error);
  SPARSE_EXPECT(call(199, d, 2, 0x1234567800000003UL, 0, 0, 0, &error) == 10 &&
                !error);
  for (unsigned whence = 3; whence != 5; ++whence) {
    SPARSE_EXPECT(call(199, a, (u64)-1, whence, 0, 0, 0, &error) == 22 &&
                  error);
    SPARSE_EXPECT(call(199, a, 10, whence, 0, 0, 0, &error) == 6 && error);
    SPARSE_EXPECT(call(199, a, 11, whence, 0, 0, 0, &error) == 6 && error);
    SPARSE_EXPECT(call(199, a, 0x7fffffffffffffffUL, whence, 0, 0, 0, &error) ==
                      6 &&
                  error);
    SPARSE_EXPECT(call(199, d, 0, 1, 0, 0, 0, &error) == 10 && !error);
    SPARSE_EXPECT(call(199, 999, (u64)-1, whence, 0, 0, 0, &error) == 9 &&
                  error);
  }
  SPARSE_EXPECT(call(201, a, 0, 0, 0, 0, 0, &error) == 0 && !error);
  SPARSE_EXPECT(call(199, a, 0, 3, 0, 0, 0, &error) == 6 && error);
  SPARSE_EXPECT(call(199, a, 0, 4, 0, 0, 0, &error) == 6 && error);
  SPARSE_EXPECT(call(199, d, 0, 1, 0, 0, 0, &error) == 10 && !error);
  SPARSE_EXPECT(call(201, a, 8193, 0, 0, 0, 0, &error) == 0 && !error);
  SPARSE_EXPECT(call(154, d, (u64) "X", 1, 8192, 0, 0, &error) == 1 && !error);
  SPARSE_EXPECT(call(199, a, 8192, 4, 0, 0, 0, &error) == 8192 && !error);
  SPARSE_EXPECT(call(199, d, 8192, 3, 0, 0, 0, &error) == 8193 && !error);
  u64 found = call(199, a, 0, 4, 0, 0, 0, &error);
  SPARSE_EXPECT(!error && found <= 8192);
  SPARSE_EXPECT(call(199, d, 0, 1, 0, 0, 0, &error) == found && !error);
  SPARSE_EXPECT(call(199, b, 0, 1, 0, 0, 0, &error) == 0 && !error);
  const u64 fds[] = {a, b, d};
  for (unsigned i = 0; i != 3; ++i)
    SPARSE_EXPECT(call(6, fds[i], 0, 0, 0, 0, 0, &error) == 0 && !error);
  b = call(5, (u64)path, 0, 0, 0, 0, 0, &error);
  SPARSE_EXPECT(!error);
  SPARSE_EXPECT(call(199, b, 8192, 4, 0, 0, 0, &error) == 8192 && !error);
  SPARSE_EXPECT(call(199, b, 8192, 3, 0, 0, 0, &error) == 8193 && !error);
  SPARSE_EXPECT(call(6, b, 0, 0, 0, 0, 0, &error) == 0 && !error);
  SPARSE_EXPECT(call(4, 1, (u64) "s", 1, 0, 0, 0, &error) == 1 && !error);
#undef SPARSE_EXPECT
  return 37;
}

/* Guest-only explicit sparse-unit policy: this does not claim APFS allocation
 * equivalence. Native workloads separately verify the kernel syscall rules. */
static int virtual_file_metadata(const char *path) {
  unsigned error;
  unsigned char initial[144], changed[144], current[144];
  int check = 80;
#define METADATA_EXPECT(expression)                                            \
  do {                                                                         \
    ++check;                                                                   \
    if (!(expression))                                                         \
      return check;                                                            \
  } while (0)
  u64 a = call(5, (u64)path, 2, 0, 0, 0, 0, &error);
  METADATA_EXPECT(!error);
  u64 b = call(5, (u64)path, 0, 0, 0, 0, 0, &error);
  METADATA_EXPECT(!error);
  u64 d = call(41, a, 0, 0, 0, 0, 0, &error);
  METADATA_EXPECT(!error);
  METADATA_EXPECT(call(339, b, (u64)initial, 0, 0, 0, 0, &error) == 0 &&
                  !error);
  METADATA_EXPECT(call(154, d, (u64) "XY", 2, 8191, 0, 0, &error) == 2 &&
                  !error);
  METADATA_EXPECT(call(339, b, (u64)changed, 0, 0, 0, 0, &error) == 0 &&
                  !error);
  METADATA_EXPECT(little_integer(changed + 96, 8) == 8193);
  METADATA_EXPECT(little_integer(changed + 104, 8) == 24);
  METADATA_EXPECT(call(199, a, 6, 4, 0, 0, 0, &error) == 6 && !error);
  METADATA_EXPECT(call(199, d, 0, 3, 0, 0, 0, &error) == 8193 && !error);
  METADATA_EXPECT(little_integer(changed + 48, 8) == (u64)-7 &&
                  little_integer(changed + 56, 8) == 123456789);
  METADATA_EXPECT(little_integer(changed + 64, 8) == (u64)-7 &&
                  little_integer(changed + 72, 8) == 123456789);
  for (unsigned i = 0; i != 144; ++i)
    if (!((i >= 48 && i < 80) || (i >= 96 && i < 112)))
      METADATA_EXPECT(changed[i] == initial[i]);
  METADATA_EXPECT(call(200, (u64)path, 4097, 0, 0, 0, 0, &error) == 0 &&
                  !error);
  METADATA_EXPECT(call(201, a, 16385, 0, 0, 0, 0, &error) == 0 && !error);
  METADATA_EXPECT(call(338, (u64)path, (u64)current, 0, 0, 0, 0, &error) == 0 &&
                  !error);
  METADATA_EXPECT(little_integer(current + 96, 8) == 16385);
  METADATA_EXPECT(little_integer(current + 104, 8) == 16);
  METADATA_EXPECT(call(199, a, 0, 3, 0, 0, 0, &error) == 8192 && !error);
  METADATA_EXPECT(call(199, a, 8192, 4, 0, 0, 0, &error) == 6 && error);
  METADATA_EXPECT(call(154, a, (u64) "", 1, 16384, 0, 0, &error) == 1 &&
                  !error);
  METADATA_EXPECT(call(339, b, (u64)current, 0, 0, 0, 0, &error) == 0 &&
                  !error);
  METADATA_EXPECT(little_integer(current + 104, 8) == 24);
  METADATA_EXPECT(call(199, d, 8192, 4, 0, 0, 0, &error) == 16384 && !error);
  METADATA_EXPECT(call(199, d, 16384, 3, 0, 0, 0, &error) == 16385 && !error);
  METADATA_EXPECT(call(201, d, 0, 0, 0, 0, 0, &error) == 0 && !error);
  const u64 fds[] = {a, b, d};
  for (unsigned i = 0; i != 3; ++i)
    METADATA_EXPECT(call(6, fds[i], 0, 0, 0, 0, 0, &error) == 0 && !error);
  b = call(5, (u64)path, 0, 0, 0, 0, 0, &error);
  METADATA_EXPECT(!error);
  METADATA_EXPECT(call(339, b, (u64)current, 0, 0, 0, 0, &error) == 0 &&
                  !error);
  METADATA_EXPECT(little_integer(current + 96, 8) == 0 &&
                  little_integer(current + 104, 8) == 0);
  METADATA_EXPECT(little_integer(current + 48, 8) == (u64)-7);
  METADATA_EXPECT(call(4, 1, (u64)changed, sizeof(changed), 0, 0, 0, &error) ==
                      sizeof(changed) &&
                  !error);
  METADATA_EXPECT(call(6, b, 0, 0, 0, 0, 0, &error) == 0 && !error);
#undef METADATA_EXPECT
  return 37;
}

static int writable_files(const char *path, unsigned nocancel) {
  const u64 op = nocancel ? 398 : 5, wr = nocancel ? 397 : 4;
  const u64 pr = nocancel ? 414 : 153, pw = nocancel ? 415 : 154;
  const u64 cl = nocancel ? 399 : 6, fc = nocancel ? 406 : 92;
  unsigned error;
  unsigned char bytes[16];
  int check = 50;
#define MUTATE_EXPECT(expression)                                              \
  do {                                                                         \
    ++check;                                                                   \
    if (!(expression))                                                         \
      return check;                                                            \
  } while (0)
  u64 a = call(op, (u64)path, 0x1000002, 0, 0, 0, 0, &error);
  MUTATE_EXPECT(!error);
  u64 b = call(op, (u64)path, 10, 0, 0, 0, 0, &error);
  MUTATE_EXPECT(!error && b != a);
  u64 d = call(41, a, 0, 0, 0, 0, 0, &error);
  MUTATE_EXPECT(!error && d != a);
  u64 ro = call(op, (u64)path, 0, 0, 0, 0, 0, &error);
  MUTATE_EXPECT(!error);
  MUTATE_EXPECT(call(fc, a, 3, 0, 0, 0, 0, &error) == 2 && !error);
  MUTATE_EXPECT(call(201, d, 10, 0, 0, 0, 0, &error) == 0 && !error);
  MUTATE_EXPECT(call(fc, a, 3, 0, 0, 0, 0, &error) == 0x10002 && !error);
  MUTATE_EXPECT(call(fc, d, 3, 0, 0, 0, 0, &error) == 0x10002 && !error);
  MUTATE_EXPECT(call(200, (u64)path, 10, 0, 0, 0, 0, &error) == 0 && !error);
  MUTATE_EXPECT(call(fc, b, 3, 0, 0, 0, 0, &error) == 10 && !error);
  MUTATE_EXPECT(call(199, a, 2, 0, 0, 0, 0, &error) == 2 && !error);
  MUTATE_EXPECT(call(wr, d, (u64) "XY", 2, 0, 0, 0, &error) == 2 && !error);
  MUTATE_EXPECT(call(199, a, 0, 1, 0, 0, 0, &error) == 4 && !error);
  MUTATE_EXPECT(call(pw, b, (u64) "ab", 2, 1, 0, 0, &error) == 2 && !error);
  MUTATE_EXPECT(call(199, b, 0, 1, 0, 0, 0, &error) == 0 && !error);
  MUTATE_EXPECT(call(wr, b, (u64) "Z", 1, 0, 0, 0, &error) == 1 && !error);
  MUTATE_EXPECT(call(pr, ro, (u64)bytes, 16, 0, 0, 0, &error) == 11 && !error);
  for (unsigned i = 0; i != 11; ++i)
    MUTATE_EXPECT(bytes[i] == "0abY456789Z"[i]);
  MUTATE_EXPECT(call(fc, d, 4, 8, 0, 0, 0, &error) == 0 && !error);
  MUTATE_EXPECT(call(fc, a, 3, 0, 0, 0, 0, &error) == 0x1000a && !error);
  MUTATE_EXPECT(call(fc, a, 1, 0, 0, 0, 0, &error) == 1 && !error);
  MUTATE_EXPECT(call(fc, d, 1, 0, 0, 0, 0, &error) == 0 && !error);
  MUTATE_EXPECT(call(fc, b, 4, 1, 0, 0, 0, &error) == 0 && !error);
  MUTATE_EXPECT(call(fc, b, 3, 0, 0, 0, 0, &error) == 0x10002 && !error);
  MUTATE_EXPECT(call(fc, a, 3, 0, 0, 0, 0, &error) == 0x1000a && !error);
  MUTATE_EXPECT(call(wr, a, (u64)-1, 0, 0, 0, 0, &error) == 0 && !error);
  MUTATE_EXPECT(call(199, a, 0, 1, 0, 0, 0, &error) == 4 && !error);
  MUTATE_EXPECT(call(wr, a, (u64)-1, 1, 0, 0, 0, &error) == 14 && error);
  MUTATE_EXPECT(call(199, d, 0, 1, 0, 0, 0, &error) == 11 && !error);
  MUTATE_EXPECT(call(pw, 999, 0, 0, (u64)-1, 0, 0, &error) == 22 && error);
  MUTATE_EXPECT(call(pw, 999, 0, 0, (u64)-2, 0, 0, &error) == 9 && error);
  const u64 maximum = 0x7fffffffffffffffUL;
  MUTATE_EXPECT(call(199, a, maximum, 0, 0, 0, 0, &error) == maximum && !error);
  MUTATE_EXPECT(call(wr, a, 0, 0, 0, 0, 0, &error) == 27 && error);
  MUTATE_EXPECT(call(199, a, maximum - 1, 0, 0, 0, 0, &error) == maximum - 1 &&
                !error);
  MUTATE_EXPECT(call(wr, d, (u64) "KL", 2, 0, 0, 0, &error) == 1 && !error);
  MUTATE_EXPECT(call(199, a, 0, 1, 0, 0, 0, &error) == 12 && !error);
  MUTATE_EXPECT(call(pw, d, (u64) "QR", 2, 14, 0, 0, &error) == 2 && !error);
  MUTATE_EXPECT(call(pr, ro, (u64)bytes, 16, 0, 0, 0, &error) == 16 && !error);
  MUTATE_EXPECT(bytes[11] == 'K' && !bytes[12] && !bytes[13] &&
                bytes[14] == 'Q' && bytes[15] == 'R');
  MUTATE_EXPECT(call(201, b, 5, 0, 0, 0, 0, &error) == 0 && !error);
  MUTATE_EXPECT(call(201, b, 8, 0, 0, 0, 0, &error) == 0 && !error);
  MUTATE_EXPECT(call(pr, ro, (u64)bytes, 16, 0, 0, 0, &error) == 8 && !error);
  for (unsigned i = 0; i != 8; ++i)
    MUTATE_EXPECT(bytes[i] == (i < 5 ? "0abY4"[i] : 0));
  MUTATE_EXPECT(call(199, b, 0, 1, 0, 0, 0, &error) == 11 && !error);
  MUTATE_EXPECT(call(201, ro, 0, 0, 0, 0, 0, &error) == 22 && error);
  MUTATE_EXPECT(call(201, 999, (u64)-1, 0, 0, 0, 0, &error) == 22 && error);
  MUTATE_EXPECT(call(200, 0, (u64)-1, 0, 0, 0, 0, &error) == 22 && error);
  MUTATE_EXPECT(call(200, (u64)path, 2, 0, 0, 0, 0, &error) == 0 && !error);
  u64 trunc = call(op, (u64)path, 0x400, 0, 0, 0, 0, &error);
  MUTATE_EXPECT(!error);
  MUTATE_EXPECT(call(fc, trunc, 3, 0, 0, 0, 0, &error) == 0x10000 && !error);
  MUTATE_EXPECT(call(pr, ro, (u64)bytes, 16, 0, 0, 0, &error) == 0 && !error);
  MUTATE_EXPECT(call(cl, trunc, 0, 0, 0, 0, 0, &error) == 0 && !error);
  MUTATE_EXPECT(call(pw, b, (u64) "n", 1, 2, 0, 0, &error) == 1 && !error);
  const u64 fds[] = {a, b, d, ro};
  for (unsigned i = 0; i != 4; ++i)
    MUTATE_EXPECT(call(cl, fds[i], 0, 0, 0, 0, 0, &error) == 0 && !error);
  u64 wo = call(op, (u64)path, 1, 0, 0, 0, 0, &error);
  MUTATE_EXPECT(!error);
  MUTATE_EXPECT(call(3, wo, 0, 0, 0, 0, 0, &error) == 9 && error);
  MUTATE_EXPECT(call(197, 0, PAGE, 1, 2, wo, 0, &error) == 13 && error);
  u64 mapped = call(197, 0, PAGE, 0, 2, wo, 0, &error);
  MUTATE_EXPECT(!error);
  MUTATE_EXPECT(call(74, mapped, PAGE, 3, 0, 0, 0, &error) == 0 && !error);
  volatile unsigned char *memory = (volatile unsigned char *)mapped;
  MUTATE_EXPECT(!memory[0] && !memory[1] && memory[2] == 'n');
  MUTATE_EXPECT(call(73, mapped, PAGE, 0, 0, 0, 0, &error) == 0 && !error);
  MUTATE_EXPECT(call(cl, wo, 0, 0, 0, 0, 0, &error) == 0 && !error);
  ro = call(op, (u64)path, 0, 0, 0, 0, 0, &error);
  MUTATE_EXPECT(!error);
  MUTATE_EXPECT(call(pr, ro, (u64)bytes, 16, 0, 0, 0, &error) == 3 && !error);
  MUTATE_EXPECT(!bytes[0] && !bytes[1] && bytes[2] == 'n');
  unsigned char hex[6];
  const char digits[] = "0123456789abcdef";
  for (unsigned i = 0; i != 3; ++i) {
    hex[i * 2] = digits[bytes[i] >> 4];
    hex[i * 2 + 1] = digits[bytes[i] & 15];
  }
  MUTATE_EXPECT(call(wr, 1, (u64)hex, 6, 0, 0, 0, &error) == 6 && !error);
  MUTATE_EXPECT(call(cl, ro, 0, 0, 0, 0, 0, &error) == 0 && !error);
#undef MUTATE_EXPECT
  return 37;
}

/* The same raw-call object runs in the guest and against the native kernel.
 * The harness supplies a regular file containing exactly 0123456789. */
static int file_calls(const char *path, unsigned nocancel) {
  const u64 open_call = nocancel ? 398 : 5;
  const u64 read_call = nocancel ? 396 : 3;
  const u64 pread_call = nocancel ? 414 : 153;
  const u64 close_call = nocancel ? 399 : 6;
  const u64 fcntl_call = nocancel ? 406 : 92;
  unsigned error = 0;
  unsigned char bytes[8];
  int check = 140;
#define FILE_EXPECT(expression)                                                \
  do {                                                                         \
    ++check;                                                                   \
    if (!(expression))                                                         \
      return check;                                                            \
  } while (0)
  u64 fd = call(open_call, (u64)path, 0x1000000, 0, 0, 0, 0, &error);
  FILE_EXPECT(!error);
  char descendant[1024];
  unsigned length = 0;
  while (path[length] && length < 700) {
    descendant[length] = path[length];
    ++length;
  }
  FILE_EXPECT(!path[length]);
  descendant[length] = '/';
  for (unsigned i = 0; i != 256; ++i)
    descendant[length + 1 + i] = 'a';
  descendant[length + 257] = 0;
  FILE_EXPECT(call(open_call, (u64)descendant, 0, 0, 0, 0, 0, &error) == 20 &&
              error);
  const char missing[] = "-missing/";
  for (unsigned i = 0; i != 9; ++i)
    descendant[length + i] = missing[i];
  for (unsigned i = 0; i != 256; ++i)
    descendant[length + 9 + i] = 'a';
  descendant[length + 265] = 0;
  FILE_EXPECT(call(open_call, (u64)descendant, 0, 0, 0, 0, 0, &error) == 2 &&
              error);
  FILE_EXPECT(call(fcntl_call, fd, 1, 0, 0, 0, 0, &error) == 1 && !error);
  FILE_EXPECT(call(fcntl_call, fd, 3, 0, 0, 0, 0, &error) == 0 && !error);
  FILE_EXPECT(call(read_call, fd, (u64)bytes, 2, 0, 0, 0, &error) == 2 &&
              !error && bytes[0] == '0' && bytes[1] == '1');
  u64 copy = call(41, fd, 0, 0, 0, 0, 0, &error);
  FILE_EXPECT(!error && copy != fd);
  FILE_EXPECT(call(fcntl_call, copy, 1, 0, 0, 0, 0, &error) == 0 && !error);
  FILE_EXPECT(call(read_call, copy, (u64)bytes, 2, 0, 0, 0, &error) == 2 &&
              !error && bytes[0] == '2' && bytes[1] == '3');
  FILE_EXPECT(call(199, fd, 0, 1, 0, 0, 0, &error) == 4 && !error);
  FILE_EXPECT(call(pread_call, copy, (u64)bytes, 4, 8, 0, 0, &error) == 2 &&
              !error && bytes[0] == '8' && bytes[1] == '9');
  FILE_EXPECT(call(199, fd, 0, 1, 0, 0, 0, &error) == 4 && !error);
  u64 other = call(open_call, (u64)path, 0, 0, 0, 0, 0, &error);
  FILE_EXPECT(!error && other != fd && other != copy);
  FILE_EXPECT(call(read_call, other, (u64)bytes, 1, 0, 0, 0, &error) == 1 &&
              !error && bytes[0] == '0');
  FILE_EXPECT(call(90, fd, other, 0, 0, 0, 0, &error) == other && !error);
  FILE_EXPECT(call(read_call, other, (u64)bytes, 1, 0, 0, 0, &error) == 1 &&
              !error && bytes[0] == '4');
  FILE_EXPECT(call(close_call, fd, 0, 0, 0, 0, 0, &error) == 0 && !error);
  FILE_EXPECT(call(read_call, fd, (u64)bytes, 1, 0, 0, 0, &error) == 9 &&
              error);
  FILE_EXPECT(call(read_call, other, (u64)bytes, 1, 0, 0, 0, &error) == 1 &&
              !error && bytes[0] == '5');
  FILE_EXPECT(call(fcntl_call, other, 2, 3, 0, 0, 0, &error) == 0 && !error);
  FILE_EXPECT(call(90, other, other, 0, 0, 0, 0, &error) == other && !error);
  FILE_EXPECT(call(fcntl_call, other, 1, 0, 0, 0, 0, &error) == 1 && !error);
  FILE_EXPECT(call(fcntl_call, other, 67, 15, 0, 0, 0, &error) == 15 && !error);
  FILE_EXPECT(call(fcntl_call, 15, 1, 0, 0, 0, 0, &error) == 1 && !error);
  FILE_EXPECT(call(fcntl_call, other, 0, 15, 0, 0, 0, &error) == 16 && !error);
  FILE_EXPECT(call(fcntl_call, 16, 1, 0, 0, 0, 0, &error) == 0 && !error);
  FILE_EXPECT(call(read_call, 16, (u64)bytes, 1, 0, 0, 0, &error) == 1 &&
              !error && bytes[0] == '6');
  FILE_EXPECT(call(199, 15, 0, 1, 0, 0, 0, &error) == 7 && !error);
  FILE_EXPECT(call(199, 15, (u64)-8, 1, 0, 0, 0, &error) == 22 && error);
  FILE_EXPECT(call(199, 15, 0, 1, 0, 0, 0, &error) == 7 && !error);
  FILE_EXPECT(call(199, 15, 0x7fffffffffffffffUL, 0, 0, 0, 0, &error) ==
                  0x7fffffffffffffffUL &&
              !error);
  FILE_EXPECT(call(199, 15, 1, 1, 0, 0, 0, &error) == 84 && error);
  FILE_EXPECT(call(pread_call, other, 0, 0, (u64)-1, 0, 0, &error) == 22 &&
              error);
  FILE_EXPECT(call(read_call, other, 0, 1, 0, 0, 0, &error) == 0 && !error);
  FILE_EXPECT(call(read_call, 99, 0, 0x80000000UL, 0, 0, 0, &error) == 22 &&
              error);
  FILE_EXPECT(call(read_call, 99, 0, 1, 0, 0, 0, &error) == 9 && error);
  FILE_EXPECT(call(199, 15, 0, 0, 0, 0, 0, &error) == 0 && !error);
  FILE_EXPECT(call(read_call, other, 0, 1, 0, 0, 0, &error) == 14 && error);
  FILE_EXPECT(call(199, 15, 0, 1, 0, 0, 0, &error) == 0 && !error);
  FILE_EXPECT(call(read_call, other | (1UL << 32), (u64)bytes, 1, 0, 0, 0,
                   &error) == 1 &&
              !error && bytes[0] == '0');
  FILE_EXPECT(call(close_call, other, 0, 0, 0, 0, 0, &error) == 0 && !error);
  FILE_EXPECT(call(close_call, other, 0, 0, 0, 0, 0, &error) == 9 && error);
  FILE_EXPECT(call(close_call, copy, 0, 0, 0, 0, 0, &error) == 0 && !error);
  FILE_EXPECT(call(close_call, 15, 0, 0, 0, 0, 0, &error) == 0 && !error);
  FILE_EXPECT(call(close_call, 16, 0, 0, 0, 0, 0, &error) == 0 && !error);
  bytes[0] = 'f';
  FILE_EXPECT(call(nocancel ? 397 : 4, 1, (u64)bytes, 1, 0, 0, 0, &error) ==
                  1 &&
              !error);
#undef FILE_EXPECT
  return 37;
}

/* Original system-query workload. Native runs check ABI shapes and repeated
 * values without assuming this host's version, model, CPU count or RAM size.
 * The same body emits explicitly configured guest observations on request. */
static int system_info(int emit_values) {
  static const struct {
    char name[20];
    unsigned mib[2], width;
  } keys[] = {{"kern.ostype", {1, 1}, 0},     {"kern.osrelease", {1, 2}, 0},
              {"kern.osrevision", {1, 3}, 4}, {"kern.version", {1, 4}, 0},
              {"kern.osversion", {1, 65}, 0}, {"hw.machine", {6, 1}, 0},
              {"hw.model", {6, 2}, 0},        {"hw.ncpu", {6, 3}, 4},
              {"hw.memsize", {6, 24}, 8}};
  unsigned char first[1026], second[1026];
  unsigned error;
  int check = 40;
#define SYSTEM_EXPECT(expression)                                              \
  do {                                                                         \
    ++check;                                                                   \
    if (!(expression))                                                         \
      return check;                                                            \
  } while (0)
  for (unsigned k = 0; k != sizeof(keys) / sizeof(keys[0]); ++k) {
    u64 count = 0, length = 0;
    while (keys[k].name[count])
      ++count;
    SYSTEM_EXPECT(call(274, (u64)keys[k].name, count, 0, (u64)&length, 0, 0,
                       &error) == 0 &&
                  !error && !secondary);
    SYSTEM_EXPECT(length && length <= 1024 &&
                  (!keys[k].width || length == keys[k].width));
    const u64 size = length;
    for (u64 i = 0; i != size + 2; ++i)
      first[i] = second[i] = 0xa5;
    SYSTEM_EXPECT(call(274, (u64)keys[k].name, count, (u64)(first + 1),
                       (u64)&length, 0, 0, &error) == 0 &&
                  !error && !secondary);
    SYSTEM_EXPECT(length == size && first[0] == 0xa5 &&
                  first[size + 1] == 0xa5 && (keys[k].width || !first[size]));
    length = 1024;
    SYSTEM_EXPECT(call(202, (u64)keys[k].mib, 0x1234567800000002UL,
                       (u64)(second + 1), (u64)&length, 0, 0, &error) == 0 &&
                  !error && !secondary && length == size);
    for (u64 i = 0; i != size + 2; ++i)
      if (first[i] != second[i])
        return 211;
    length = size - 1;
    SYSTEM_EXPECT(call(274, (u64)keys[k].name, count, (u64)(second + 1),
                       (u64)&length, 0, 0, &error) == 12 &&
                  error && !length);
    for (u64 i = 0; i != size + 2; ++i)
      if (first[i] != second[i])
        return 212;
    length = size;
    SYSTEM_EXPECT(call(274, (u64)keys[k].name, count, (u64)(second + 1),
                       (u64)&length, (u64)-1, 0, &error) == 0 &&
                  !error);
    SYSTEM_EXPECT(
        call(274, (u64)keys[k].name, count, 0, 0, 0, (u64)-1, &error) == 0 &&
        !error);
    if (emit_values)
      SYSTEM_EXPECT(call(4, 1, (u64)(first + 1), size, 0, 0, 0, &error) ==
                        size &&
                    !error);
  }
  const char page[] = "hw.pagesize", compat[] = "hw.pagesize_compat";
  for (u64 capacity = 0; capacity != 10; ++capacity) {
    u64 length = capacity, value = 0xa5a5a5a5a5a5a5a5UL;
    const u64 width = capacity == 4 ? 4 : 8;
    const u64 expected = capacity < width ? 12 : 0;
    SYSTEM_EXPECT(call(274, (u64)page, sizeof(page) - 1, (u64)&value,
                       (u64)&length, 0, 0, &error) == expected &&
                  error == (expected != 0));
    SYSTEM_EXPECT(length == (expected ? 0 : width) &&
                  value == (expected     ? 0xa5a5a5a5a5a5a5a5UL
                            : width == 4 ? 0xa5a5a5a500000000UL | PAGE
                                         : PAGE));
  }
  u64 length = 8, value = 0xa5a5a5a5a5a5a5a5UL;
  SYSTEM_EXPECT(call(274, (u64)compat, sizeof(compat) - 1, (u64)&value,
                     (u64)&length, 0, 0, &error) == 0 &&
                !error && length == 4 &&
                value == (0xa5a5a5a500000000UL | PAGE));
  const unsigned mib[] = {6, 7};
  length = 8;
  SYSTEM_EXPECT(
      call(202, (u64)mib, 2, (u64)&value, (u64)&length, 0, 0, &error) == 0 &&
      !error && length == 4);
  const char dotted[] = "hw.pagesize.", embedded[] = "hw.pagesize\0extra";
  length = 8;
  SYSTEM_EXPECT(call(274, (u64)dotted, sizeof(dotted) - 1, (u64)&value,
                     (u64)&length, 0, 0, &error) == 0 &&
                !error && length == 8 && value == PAGE);
  SYSTEM_EXPECT(call(274, (u64)embedded, sizeof(embedded) - 1, (u64)&value,
                     (u64)&length, 0, 0, &error) == 0 &&
                !error && value == PAGE);
  length = 8;
  SYSTEM_EXPECT(call(274, (u64)page, sizeof(page) - 1, 1, (u64)&length, 0, 0,
                     &error) == 14 &&
                error && length == 8);
  length = 3;
  SYSTEM_EXPECT(call(274, (u64)page, sizeof(page) - 1, 1, (u64)&length, 0, 0,
                     &error) == 12 &&
                error && !length);
  SYSTEM_EXPECT(call(274, (u64)page, sizeof(page) - 1, (u64)&value, 0, 0, 0,
                     &error) == 12 &&
                error);
  const char ostype[] = "kern.ostype"; // Intrinsically read-only even as root.
  SYSTEM_EXPECT(call(274, (u64)ostype, sizeof(ostype) - 1, 1, (u64)&length, 1,
                     1, &error) == 1 &&
                error && !length);
  SYSTEM_EXPECT(call(202, 1, 1, 1, 1, 1, 1, &error) == 22 && error);
  SYSTEM_EXPECT(call(274, 1, 1024, 1, 1, 1, 1, &error) == 63 && error);
  length = 8;
  SYSTEM_EXPECT(call(274, 1, 0, 1, (u64)&length, 0, 0, &error) == 2 && error &&
                length == 8);
  // Data is copied before its length overwrites that same output address.
  length = 8;
  SYSTEM_EXPECT(call(274, (u64)page, sizeof(page) - 1, (u64)&length,
                     (u64)&length, 0, 0, &error) == 0 &&
                !error && length == 8);
  if (!emit_values) {
    const char marker = 'i';
    SYSTEM_EXPECT(call(4, 1, (u64)&marker, 1, 0, 0, 0, &error) == 1 && !error);
  }
#undef SYSTEM_EXPECT
  return 37;
}

static int deleted_directories(const char *path) {
  unsigned error;
  char parent[1024], before[1024], after[1024];
  u64 length = 0, slash = 0;
  while (path[length] && length + 1 < sizeof(parent)) {
    parent[length] = path[length];
    if (path[length] == '/')
      slash = length;
    ++length;
  }
  if (path[0] != '/' || path[length])
    return 51;
  parent[slash ? slash : 1] = 0;
  int check = 51;
#define DELETED_EXPECT(expression)                                             \
  do {                                                                         \
    ++check;                                                                   \
    if (!(expression))                                                         \
      return check;                                                            \
  } while (0)
  const char name[] = "dir-held", child[] = "child", fresh[] = "fresh";
  const char dot[] = ".", dotdot[] = "..", absent[] = "absent";
  const char old_child[] = "../child", escape[] = "../../dir-held/child/fresh";
  u64 root = call(5, (u64)parent, 0x100000, 0, 0, 0, 0, &error);
  DELETED_EXPECT(!error && root >= 3);
  DELETED_EXPECT(call(13, root, 0, 0, 0, 0, 0, &error) == 0 && !error);
  DELETED_EXPECT(call(475, root, (u64)name, 0700, 0, 0, 0, &error) == 0 &&
                 !error);
  u64 old = call(463, root, (u64)name, 0x100000, 0, 0, 0, &error);
  DELETED_EXPECT(!error && old >= 3);
  u64 duplicate = call(41, old, 0, 0, 0, 0, 0, &error);
  DELETED_EXPECT(!error && duplicate != old);
  DELETED_EXPECT(call(475, old, (u64)child, 0700, 0, 0, 0, &error) == 0 &&
                 !error);
  u64 held = call(463, old, (u64)child, 0x100000, 0, 0, 0, &error);
  DELETED_EXPECT(!error && held >= 3);
  DELETED_EXPECT(call(92, held, 50, (u64)before, 0, 0, 0, &error) == 0 &&
                 !error);
  DELETED_EXPECT(call(13, held, 0, 0, 0, 0, 0, &error) == 0 && !error);
  DELETED_EXPECT(call(472, old, (u64)child, 0x80, 0, 0, 0, &error) == 0 &&
                 !error);
  DELETED_EXPECT(call(472, root, (u64)name, 0x80, 0, 0, 0, &error) == 0 &&
                 !error);
  DELETED_EXPECT(call(475, root, (u64)name, 0700, 0, 0, 0, &error) == 0 &&
                 !error);
  u64 newer = call(463, root, (u64)name, 0x100000, 0, 0, 0, &error);
  DELETED_EXPECT(!error && newer >= 3);
  DELETED_EXPECT(call(475, newer, (u64)child, 0700, 0, 0, 0, &error) == 0 &&
                 !error);
  u64 nested = call(463, newer, (u64)child, 0x100000, 0, 0, 0, &error);
  DELETED_EXPECT(!error && nested >= 3);
  DELETED_EXPECT(call(475, nested, (u64)fresh, 0700, 0, 0, 0, &error) == 0 &&
                 !error);
  DELETED_EXPECT(call(466, old, (u64)child, 0, 0, 0, 0, &error) == 2 && error);
  DELETED_EXPECT(call(466, duplicate, (u64)child, 0, 0, 0, 0, &error) == 2 &&
                 error);
  DELETED_EXPECT(call(466, held, (u64)old_child, 0, 0, 0, 0, &error) == 2 &&
                 error);
  DELETED_EXPECT(call(466, held, (u64)escape, 0, 0, 0, 0, &error) == 0 &&
                 !error);
  DELETED_EXPECT(call(475, held, (u64)absent, 0700, 0, 0, 0, &error) == 2 &&
                 error);
  DELETED_EXPECT(call(475, held, (u64)dot, 0700, 0, 0, 0, &error) == 17 &&
                 error);
  DELETED_EXPECT(call(475, held, (u64)dotdot, 0700, 0, 0, 0, &error) == 2 &&
                 error);
  DELETED_EXPECT(
      call(465, (u64)-1, (u64)path, held, (u64)dotdot, 0, 0, &error) == 22 &&
      error);
  DELETED_EXPECT(
      call(465, (u64)-1, (u64)path, held, (u64)escape, 0, 0, &error) == 2 &&
      error);
  DELETED_EXPECT(call(472, held, (u64)dot, 0, 0, 0, 0, &error) == 1 && error);
  DELETED_EXPECT(call(472, held, (u64)dot, 0x80, 0, 0, 0, &error) == 22 &&
                 error);
  u64 same = call(463, held, (u64)dot, 0x100000, 0, 0, 0, &error);
  DELETED_EXPECT(!error && same >= 3 && same != held);
  DELETED_EXPECT(call(92, same, 50, (u64)after, 0, 0, 0, &error) == 0 &&
                 !error && equal(before, after));
  DELETED_EXPECT(call(6, same, 0, 0, 0, 0, 0, &error) == 0 && !error);
  DELETED_EXPECT(call(6, held, 0, 0, 0, 0, 0, &error) == 0 && !error);
  DELETED_EXPECT(call(6, old, 0, 0, 0, 0, 0, &error) == 0 && !error);
  DELETED_EXPECT(call(6, duplicate, 0, 0, 0, 0, 0, &error) == 0 && !error);
  // CWD alone now retains both removed directory objects.
  DELETED_EXPECT(call(33, (u64)old_child, 0, 0, 0, 0, 0, &error) == 2 && error);
  DELETED_EXPECT(call(33, (u64)escape, 0, 0, 0, 0, 0, &error) == 0 && !error);
  u64 old_parent = call(5, (u64)dotdot, 0x100000, 0, 0, 0, 0, &error);
  DELETED_EXPECT(!error && old_parent >= 3);
  DELETED_EXPECT(call(466, old_parent, (u64)child, 0, 0, 0, 0, &error) == 2 &&
                 error);
  DELETED_EXPECT(
      call(90, newer, old_parent, 0, 0, 0, 0, &error) == old_parent && !error);
  DELETED_EXPECT(call(466, old_parent, (u64)child, 0, 0, 0, 0, &error) == 0 &&
                 !error);
  DELETED_EXPECT(call(13, root, 0, 0, 0, 0, 0, &error) == 0 && !error);
  DELETED_EXPECT(call(472, nested, (u64)fresh, 0x80, 0, 0, 0, &error) == 0 &&
                 !error);
  DELETED_EXPECT(call(472, newer, (u64)child, 0x80, 0, 0, 0, &error) == 0 &&
                 !error);
  DELETED_EXPECT(call(472, root, (u64)name, 0x80, 0, 0, 0, &error) == 0 &&
                 !error);
  DELETED_EXPECT(call(6, nested, 0, 0, 0, 0, 0, &error) == 0 && !error);
  DELETED_EXPECT(call(6, newer, 0, 0, 0, 0, 0, &error) == 0 && !error);
  DELETED_EXPECT(call(6, old_parent, 0, 0, 0, 0, 0, &error) == 0 && !error);
  DELETED_EXPECT(call(6, root, 0, 0, 0, 0, 0, &error) == 0 && !error);
  const char marker = 'h';
  DELETED_EXPECT(call(4, 1, (u64)&marker, 1, 0, 0, 0, &error) == 1 && !error);
#undef DELETED_EXPECT
  return 37;
}

static int directory_mutations(const char *path) {
  unsigned error;
  char parent[1024], old_path[1024], current_path[1024];
  u64 length = 0, slash = 0;
  while (path[length] && length + 1 < sizeof(parent)) {
    parent[length] = path[length];
    if (path[length] == '/')
      slash = length;
    ++length;
  }
  if (path[0] != '/' || path[length])
    return 51;
  parent[slash ? slash : 1] = 0;
  int check = 51;
#define DIRECTORY_EXPECT(expression)                                           \
  do {                                                                         \
    ++check;                                                                   \
    if (!(expression))                                                         \
      return check;                                                            \
  } while (0)
  const char roots[][7] = {"/", "//", "///", "/.", "/..", "//.//"};
  for (unsigned i = 0; i != sizeof(roots) / sizeof(*roots); ++i) {
    const u64 code = i < 3 ? 21 : 16;
    DIRECTORY_EXPECT(call(137, (u64)roots[i], 0, 0, 0, 0, 0, &error) == code &&
                     error);
    DIRECTORY_EXPECT(call(10, (u64)roots[i], 0, 0, 0, 0, 0, &error) == code &&
                     error);
    DIRECTORY_EXPECT(
        call(472, (u64)-1, (u64)roots[i], 0, 0, 0, 0, &error) == code && error);
    DIRECTORY_EXPECT(call(472, (u64)-1, (u64)roots[i], 0x80, 0, 0, 0, &error) ==
                         code &&
                     error);
  }
  const char name[] = "dir-work", trailing[] = "dir-work///";
  const char child[] = "dir-work/child//", missing[] = "no-dir-work/.";
  const char dot[] = "dir-work/.", leaf[] = "file", moved[] = "moved";
  const char more[] = "more", child_name[] = "child";
  u64 root = call(5, (u64)parent, 0x100000, 0, 0, 0, 0, &error);
  DIRECTORY_EXPECT(!error && root >= 3);
  DIRECTORY_EXPECT(call(13, root, 0, 0, 0, 0, 0, &error) == 0 && !error);
  DIRECTORY_EXPECT(call(136, (u64)path, 0700, 0, 0, 0, 0, &error) == 17 &&
                   error);
  DIRECTORY_EXPECT(call(475, (u64)-1, 1, 0700, 0, 0, 0, &error) == 14 && error);
  DIRECTORY_EXPECT(call(136, (u64)missing, 0700, 0, 0, 0, 0, &error) == 2 &&
                   error);
  DIRECTORY_EXPECT(
      call(136, (u64)trailing, 0x12345678000001c0UL, 0, 0, 0, 0, &error) == 0 &&
      !error && !secondary);
  DIRECTORY_EXPECT(call(475, 0x1234567800000000UL | root, (u64)child, 0700, 0,
                        0, 0, &error) == 0 &&
                   !error && !secondary);
  DIRECTORY_EXPECT(call(136, (u64)dot, 0700, 0, 0, 0, 0, &error) == 17 &&
                   error);
  DIRECTORY_EXPECT(call(137, (u64)dot, 0, 0, 0, 0, 0, &error) == 22 && error);
  DIRECTORY_EXPECT(call(137, (u64)name, 0, 0, 0, 0, 0, &error) == 66 && error);
  u64 directory = call(463, root, (u64)name, 0x100000, 0, 0, 0, &error);
  DIRECTORY_EXPECT(!error && directory != root);
  u64 nested = call(463, directory, (u64)child_name, 0x100000, 0, 0, 0, &error);
  DIRECTORY_EXPECT(!error && nested != directory);
  DIRECTORY_EXPECT(
      call(475, directory, (u64)more, 0700, 0, 0, 0, &error) == 0 && !error);
  u64 old = call(463, nested, (u64)leaf, 0xa02, 0600, 0, 0, &error);
  DIRECTORY_EXPECT(!error && old >= 3);
  const char original[] = "old!", replacement[] = "new!";
  DIRECTORY_EXPECT(call(4, old, (u64)original, 4, 0, 0, 0, &error) == 4 &&
                   !error);
  DIRECTORY_EXPECT(
      call(465, nested, (u64)leaf, nested, (u64)moved, 0, 0, &error) == 0 &&
      !error);
  DIRECTORY_EXPECT(call(92, old, 50, (u64)old_path, 0, 0, 0, &error) == 0 &&
                   !error);
  DIRECTORY_EXPECT(call(472, nested, (u64)moved, 0, 0, 0, 0, &error) == 0 &&
                   !error);
  DIRECTORY_EXPECT(call(6, nested, 0, 0, 0, 0, 0, &error) == 0 && !error);
  DIRECTORY_EXPECT(call(472, directory, (u64)more, 0xfedcba9800000880UL, 0, 0,
                        0, &error) == 0 &&
                   !error);
  DIRECTORY_EXPECT(call(6, directory, 0, 0, 0, 0, 0, &error) == 0 && !error);
  DIRECTORY_EXPECT(call(137, (u64)child, 0, 0, 0, 0, 0, &error) == 0 && !error);
  DIRECTORY_EXPECT(call(137, (u64)trailing, 0, 0, 0, 0, 0, &error) == 0 &&
                   !error && !secondary);
  DIRECTORY_EXPECT(call(33, (u64)name, 0, 0, 0, 0, 0, &error) == 2 && error);
  DIRECTORY_EXPECT(call(475, root, (u64)name, 0700, 0, 0, 0, &error) == 0 &&
                   !error);
  DIRECTORY_EXPECT(call(475, root, (u64)child, 0700, 0, 0, 0, &error) == 0 &&
                   !error);
  u64 newer = call(5, (u64)old_path, 0xa02, 0600, 0, 0, 0, &error);
  DIRECTORY_EXPECT(!error && newer >= 3 && newer != old);
  DIRECTORY_EXPECT(call(4, newer, (u64)replacement, 4, 0, 0, 0, &error) == 4 &&
                   !error);
  char bytes[4];
  DIRECTORY_EXPECT(call(153, old, (u64)bytes, 4, 0, 0, 0, &error) == 4 &&
                   !error);
  for (unsigned i = 0; i != 4; ++i)
    DIRECTORY_EXPECT(bytes[i] == original[i]);
  DIRECTORY_EXPECT(call(92, old, 50, (u64)current_path, 0, 0, 0, &error) == 0 &&
                   !error);
  DIRECTORY_EXPECT(equal(old_path, current_path));
  DIRECTORY_EXPECT(call(6, old, 0, 0, 0, 0, 0, &error) == 0 && !error);
  DIRECTORY_EXPECT(call(6, newer, 0, 0, 0, 0, 0, &error) == 0 && !error);
  DIRECTORY_EXPECT(call(10, (u64)old_path, 0, 0, 0, 0, 0, &error) == 0 &&
                   !error);
  DIRECTORY_EXPECT(call(137, (u64)child, 0, 0, 0, 0, 0, &error) == 0 && !error);
  DIRECTORY_EXPECT(call(472, root, (u64)name, 0x80, 0, 0, 0, &error) == 0 &&
                   !error);
  DIRECTORY_EXPECT(call(6, root, 0, 0, 0, 0, 0, &error) == 0 && !error);
  const char marker = 'm';
  DIRECTORY_EXPECT(call(4, 1, (u64)&marker, 1, 0, 0, 0, &error) == 1 && !error);
#undef DIRECTORY_EXPECT
  return 37;
}

static int file_access(const char *path) {
  unsigned error;
  char parent[1024];
  u64 length = 0, slash = 0;
  while (path[length] && length + 1 < sizeof(parent)) {
    parent[length] = path[length];
    if (path[length] == '/')
      slash = length;
    ++length;
  }
  if (path[0] != '/' || path[length])
    return 51;
  parent[slash ? slash : 1] = 0;
  const char *leaf = path + slash + 1;
  int check = 51;
#define ACCESS_EXPECT(expression)                                              \
  do {                                                                         \
    ++check;                                                                   \
    if (!(expression))                                                         \
      return check;                                                            \
  } while (0)
  u64 dir = call(5, (u64)parent, 0x100000, 0, 0, 0, 0, &error);
  ACCESS_EXPECT(!error && dir >= 3);
  u64 fd = call(5, (u64)path, 0, 0, 0, 0, 0, &error);
  ACCESS_EXPECT(!error && fd != dir);
  ACCESS_EXPECT(call(199, fd, 1, 0, 0, 0, 0, &error) == 1 && !error);
  const u64 modes[] = {0,        8,          0x80,       0x100,
                       0x400000, 0x80000000, 0xffc001f8, 0x1234567800000000UL};
  for (unsigned i = 0; i != sizeof(modes) / sizeof(*modes); ++i) {
    ACCESS_EXPECT(call(33, (u64)path, modes[i], 0, 0, 0, 0, &error) == 0 &&
                  !error && !secondary);
    ACCESS_EXPECT(call(466, dir, (u64)leaf, modes[i], 0, 0, 0, &error) == 0 &&
                  !error && !secondary);
    ACCESS_EXPECT(call(33, 1, modes[i], 0, 0, 0, 0, &error) == 14 && error);
  }
  const u64 flags[] = {0, 0x10, 0x20, 0x30, 0x800, 0x810, 0x820, 0x830};
  for (unsigned i = 0; i != sizeof(flags) / sizeof(*flags); ++i) {
    // Relative lookup avoids host /tmp or /var symlinks under NOFOLLOW_ANY.
    ACCESS_EXPECT(call(466, 0x1234567800000000UL | dir, (u64)leaf, 0,
                       0xfedcba9800000000UL | flags[i], 0, 0, &error) == 0 &&
                  !error);
  }
  const u64 invalid[] = {1, 0x40, 0x400, 0xffffffff};
  for (unsigned i = 0; i != sizeof(invalid) / sizeof(*invalid); ++i)
    ACCESS_EXPECT(call(466, (u64)-1, 1, 0, invalid[i], 0, 0, &error) == 22 &&
                  error);
  ACCESS_EXPECT(call(466, (u64)-1, (u64)path, 0, 0, 0, 0, &error) == 0 &&
                !error);
  ACCESS_EXPECT(call(466, fd, (u64)path, 0, 0x30, 0, 0, &error) == 0 && !error);
  ACCESS_EXPECT(call(466, (u64)-1, (u64)leaf, 0, 0, 0, 0, &error) == 9 &&
                error);
  ACCESS_EXPECT(call(466, fd, (u64)leaf, 0, 0, 0, 0, &error) == 20 && error);
  ACCESS_EXPECT(call(466, (u64)-1, 1, 0, 0, 0, 0, &error) == 14 && error);
  const char empty[] = "", missing[] = "no-access-entry", current[] = ".";
  ACCESS_EXPECT(call(466, (u64)-1, (u64)empty, 0, 0, 0, 0, &error) == 9 &&
                error);
  ACCESS_EXPECT(call(466, fd, (u64)empty, 0, 0, 0, 0, &error) == 20 && error);
  ACCESS_EXPECT(call(466, dir, (u64)empty, 0, 0, 0, 0, &error) == 2 && error);
  ACCESS_EXPECT(call(466, dir, (u64)missing, 0, 0, 0, 0, &error) == 2 && error);
  ACCESS_EXPECT(call(466, dir, (u64)current, 0, 0x830, 0, 0, &error) == 0 &&
                !error);
  char child[64];
  unsigned n = 0;
  while (leaf[n] && n + 4 < sizeof(child)) {
    child[n] = leaf[n];
    ++n;
  }
  ACCESS_EXPECT(!leaf[n]);
  child[n] = '/';
  child[n + 1] = '.';
  child[n + 2] = '.';
  child[n + 3] = 0;
  ACCESS_EXPECT(call(466, dir, (u64)child, 0, 0, 0, 0, &error) == 20 && error);
  ACCESS_EXPECT(call(199, fd, 0, 1, 0, 0, 0, &error) == 1 && !error);
  ACCESS_EXPECT(call(92, fd, 3, 0, 0, 0, 0, &error) == 0 && !error);
  ACCESS_EXPECT(call(6, fd, 0, 0, 0, 0, 0, &error) == 0 && !error);
  ACCESS_EXPECT(call(6, dir, 0, 0, 0, 0, 0, &error) == 0 && !error);
  const char marker = 'a';
  ACCESS_EXPECT(call(4, 1, (u64)&marker, 1, 0, 0, 0, &error) == 1 && !error);
#undef ACCESS_EXPECT
  return 37;
}

/* Independent LP64 records: no SDK types or model constants. */
struct vector_span {
  u64 address, length;
};
static int vectored_io(const char *path) {
  unsigned error;
  unsigned char bytes[32];
  const char initial[] = "0123456789", replacement[] = "ABC";
  struct vector_span spans[3];
  int check = 50;
#define VECTOR_EXPECT(expression)                                              \
  do {                                                                         \
    ++check;                                                                   \
    if (!(expression))                                                         \
      return check;                                                            \
  } while (0)
  u64 fd = call(5, (u64)path, 2, 0, 0, 0, 0, &error);
  VECTOR_EXPECT(!error && fd >= 3);
  u64 duplicate = call(41, fd, 0, 0, 0, 0, 0, &error);
  VECTOR_EXPECT(!error && duplicate != fd);
  for (unsigned alias = 0; alias != 2; ++alias) {
    const u64 read_number = alias ? 411 : 120;
    const u64 write_number = alias ? 412 : 121;
    const u64 pread_number = alias ? 542 : 540;
    const u64 pwrite_number = alias ? 543 : 541;
    VECTOR_EXPECT(call(154, fd, (u64)initial, 10, 0, 0, 0, &error) == 10 &&
                  !error);
    VECTOR_EXPECT(call(199, fd, 0, 0, 0, 0, 0, &error) == 0 && !error);
    for (unsigned i = 0; i != sizeof(bytes); ++i)
      bytes[i] = 0xa5;
    spans[0] = (struct vector_span){(u64)(bytes + 1), 2};
    spans[1] = (struct vector_span){(u64)(bytes + 5), 3};
    VECTOR_EXPECT(call(read_number, duplicate, (u64)spans, 0x1234567800000002UL,
                       0, 0, 0, &error) == 5 &&
                  !error && !secondary);
    VECTOR_EXPECT(bytes[0] == 0xa5 && bytes[1] == '0' && bytes[2] == '1' &&
                  bytes[3] == 0xa5 && bytes[4] == 0xa5 && bytes[5] == '2' &&
                  bytes[6] == '3' && bytes[7] == '4' && bytes[8] == 0xa5);
    VECTOR_EXPECT(call(199, fd, 0, 1, 0, 0, 0, &error) == 5 && !error);
    VECTOR_EXPECT(call(pread_number, fd, (u64)spans, 2, 1, 0, 0, &error) == 5 &&
                  !error && bytes[1] == '1' && bytes[7] == '5');
    VECTOR_EXPECT(call(199, duplicate, 0, 1, 0, 0, 0, &error) == 5 && !error);
    VECTOR_EXPECT(call(read_number, (u64)-1, 1, 0, 0, 0, 0, &error) == 22 &&
                  error);
    VECTOR_EXPECT(call(write_number, (u64)-1, 1, 1025, 0, 0, 0, &error) == 22 &&
                  error);
    VECTOR_EXPECT(call(read_number, (u64)-1, 1, 2, 0, 0, 0, &error) == 14 &&
                  error);
    VECTOR_EXPECT(call(pwrite_number, (u64)-1, 1, 2, (u64)-2, 0, 0, &error) ==
                      22 &&
                  error);
    VECTOR_EXPECT(call(pread_number, (u64)-1, 1, 2, (u64)-2, 0, 0, &error) ==
                      14 &&
                  error);
    spans[0] = (struct vector_span){1, (u64)-1};
    VECTOR_EXPECT(call(write_number, (u64)-1, (u64)spans, 1, 0, 0, 0, &error) ==
                      9 &&
                  error);
    VECTOR_EXPECT(
        call(write_number, fd, (u64)spans, 1, 0, 0, 0, &error) == 22 && error);
    spans[0] = (struct vector_span){1, 0x80000000UL};
    VECTOR_EXPECT(call(read_number, fd, (u64)spans, 1, 0, 0, 0, &error) == 22 &&
                  error);
    spans[0] = (struct vector_span){(u64)-1, 0};
    spans[1] = (struct vector_span){1, 0};
    VECTOR_EXPECT(call(read_number, fd, (u64)spans, 2, 0, 0, 0, &error) == 0 &&
                  !error);
    VECTOR_EXPECT(call(pread_number, fd, (u64)spans, 2, 0x7fffffffffffffffUL, 0,
                       0, &error) == 0 &&
                  !error);
    VECTOR_EXPECT(call(pwrite_number, fd, (u64)spans, 2, 0x7fffffffffffffffUL,
                       0, 0, &error) == 27 &&
                  error);
    spans[0] = (struct vector_span){(u64)replacement, 1};
    spans[1] = (struct vector_span){(u64)(replacement + 1), 2};
    VECTOR_EXPECT(call(pwrite_number, fd, (u64)spans, 2, 2, 0, 0, &error) ==
                      3 &&
                  !error && !secondary);
    VECTOR_EXPECT(call(199, fd, 0, 1, 0, 0, 0, &error) == 5 && !error);
    VECTOR_EXPECT(
        call(write_number, duplicate, (u64)spans, 2, 0, 0, 0, &error) == 3 &&
        !error);
    VECTOR_EXPECT(call(199, fd, 0, 1, 0, 0, 0, &error) == 8 && !error);
    VECTOR_EXPECT(call(153, fd, (u64)bytes, 10, 0, 0, 0, &error) == 10 &&
                  !error);
    bytes[10] = 0;
    VECTOR_EXPECT(equal((char *)bytes, "01ABCABC89"));
    VECTOR_EXPECT(call(199, fd, 2, 0, 0, 0, 0, &error) == 2 && !error);
    spans[0] = (struct vector_span){(u64)bytes, 3};
    spans[1] = (struct vector_span){1, 3};
    VECTOR_EXPECT(call(read_number, fd, (u64)spans, 2, 0, 0, 0, &error) == 14 &&
                  error && bytes[0] == 'A' && bytes[1] == 'B' &&
                  bytes[2] == 'C');
    VECTOR_EXPECT(call(199, duplicate, 0, 1, 0, 0, 0, &error) == 5 && !error);
    spans[0] = (struct vector_span){(u64)replacement, 1};
    VECTOR_EXPECT(
        call(write_number, fd, (u64)spans, 2, 0, 0, 0, &error) == 14 && error);
    VECTOR_EXPECT(call(199, duplicate, 0, 1, 0, 0, 0, &error) == 6 && !error);
    VECTOR_EXPECT((call(92, fd, 3, 0, 0, 0, 0, &error) & 0x10000) && !error);
    spans[0] = (struct vector_span){(u64)bytes, 12};
    VECTOR_EXPECT(
        call(pread_number, fd, (u64)spans, 2, 0, 0, 0, &error) == 10 && !error);
    spans[0] = (struct vector_span){(u64)&spans[1], 8};
    spans[1] = (struct vector_span){(u64)bytes, 2};
    VECTOR_EXPECT(call(pread_number, fd, (u64)spans, 2, 0, 0, 0, &error) ==
                      10 &&
                  !error && bytes[0] == '8' && bytes[1] == '9');
    spans[0] = (struct vector_span){(u64)bytes, 3};
    spans[1] = (struct vector_span){(u64)bytes, 3};
    VECTOR_EXPECT(call(pread_number, fd, (u64)spans, 2, 0, 0, 0, &error) == 6 &&
                  !error && bytes[0] == 'B' && bytes[1] == 'C' &&
                  bytes[2] == 'A');
  }
  VECTOR_EXPECT(call(92, fd, 4, 8, 0, 0, 0, &error) == 0 && !error);
  VECTOR_EXPECT(call(199, fd, 0x7ffffffffffffffdUL, 0, 0, 0, 0, &error) ==
                    0x7ffffffffffffffdUL &&
                !error);
  spans[0] = (struct vector_span){(u64)replacement, 1};
  spans[1] = (struct vector_span){(u64)(replacement + 1), 2};
  VECTOR_EXPECT(call(121, fd, (u64)spans, 2, 0, 0, 0, &error) == 2 && !error);
  VECTOR_EXPECT(call(199, duplicate, 0, 1, 0, 0, 0, &error) == 12 && !error);
  VECTOR_EXPECT(call(541, fd, (u64)spans, 2, 1, 0, 0, &error) == 3 && !error);
  VECTOR_EXPECT(call(199, fd, 0, 1, 0, 0, 0, &error) == 12 && !error);
  VECTOR_EXPECT(call(153, fd, (u64)bytes, 12, 0, 0, 0, &error) == 12 && !error);
  bytes[12] = 0;
  VECTOR_EXPECT(equal((char *)bytes, "0ABCCABC89AB"));
  VECTOR_EXPECT(call(6, duplicate, 0, 0, 0, 0, 0, &error) == 0 && !error);
  VECTOR_EXPECT(call(6, fd, 0, 0, 0, 0, 0, &error) == 0 && !error);
  const char marker[] = "v!";
  spans[0] = (struct vector_span){(u64)marker, 1};
  spans[1] = (struct vector_span){(u64)-1, 0};
  spans[2] = (struct vector_span){(u64)(marker + 1), 1};
  VECTOR_EXPECT(call(412, 1, (u64)spans, 3, 0, 0, 0, &error) == 2 && !error &&
                !secondary);
#undef VECTOR_EXPECT
  return 37;
}

static int output_descriptors(void) {
  unsigned error;
  const char text[] = "ok";
  u64 fd = call(41, 1, 0, 0, 0, 0, 0, &error);
  if (error || call(6, 1, 0, 0, 0, 0, 0, &error) || error)
    return 201;
  if (call(4, 1, (u64)text, 1, 0, 0, 0, &error) != 9 || !error)
    return 202;
  if (call(90, fd, 2, 0, 0, 0, 0, &error) != 2 || error ||
      call(4, 2, (u64)text, 1, 0, 0, 0, &error) != 1 || error)
    return 203;
  if (call(6, fd, 0, 0, 0, 0, 0, &error) || error ||
      call(90, 2, 1, 0, 0, 0, 0, &error) != 1 || error ||
      call(397, 1, (u64)(text + 1), 1, 0, 0, 0, &error) != 1 || error)
    return 204;
  return 37;
}
static volatile unsigned char reclaimable[PAGE * 2]
    __attribute__((aligned(PAGE)));
#ifdef DARWIN_THREAD_ENTRY
#define main darwin_body
#endif
int main(int argc, char **argv, char **envp, char **apple) {
  unsigned error = 0;
  if (argc < 2 || data != 0x1234 || bss != 0)
    return 101;
  bss = 99;
  if (equal(argv[1], "deleted-directories"))
    return argc < 3 ? 79 : deleted_directories(argv[2]);
  if (equal(argv[1], "directory-mutations"))
    return argc < 3 ? 79 : directory_mutations(argv[2]);
  if (equal(argv[1], "file-access"))
    return argc < 3 ? 79 : file_access(argv[2]);
  if (equal(argv[1], "vectored-io"))
    return argc < 3 ? 79 : vectored_io(argv[2]);
  if (equal(argv[1], "system-info") || equal(argv[1], "virtual-system"))
    return system_info(equal(argv[1], "virtual-system"));
  if (equal(argv[1], "time-null"))
    return call(116, 0, 0, 0, 0, 0, 0, &error) || error || secondary ? 51 : 37;
  if (equal(argv[1], "time") || equal(argv[1], "time-values"))
    return time_calls(equal(argv[1], "time-values"));
  if (equal(argv[1], "mach-time") || equal(argv[1], "mach-timebase-values"))
    return mach_timebase(equal(argv[1], "mach-timebase-values"));
  if (equal(argv[1], "mach-clock-values"))
    return mach_clocks(0);
  if (equal(argv[1], "mach-absolute"))
    return mach_clocks(1);
  if (equal(argv[1], "mach-continuous"))
    return mach_clocks(2);
  if (equal(argv[1], "mach-int32-min"))
    return (int)raw_trap(0x80000000UL, 0, mach_flags(1)).value;
  if (equal(argv[1], "mach-wrong-class"))
    return (int)raw_trap(0x03000059UL, 0, mach_flags(1)).value;
  if (equal(argv[1], "mach-foreign-class"))
    return (int)raw_trap(
#if defined(__aarch64__)
               0x01000059UL,
#else
               (u64)-89,
#endif
               0, mach_flags(1))
        .value;
  if (equal(argv[1], "renamed-file"))
    return argc < 3 ? 79 : renamed_file(argv[2]);
  if (equal(argv[1], "created-file-metadata") ||
      equal(argv[1], "virtual-created-metadata"))
    return argc < 3 ? 79
                    : created_file_metadata(
                          argv[2], equal(argv[1], "virtual-created-metadata"));
  if (equal(argv[1], "created-file"))
    return argc < 3 ? 79 : created_file(argv[2]);
  if (equal(argv[1], "unlinked-file"))
    return argc < 3 ? 79 : unlinked_file(argv[2]);
  if (equal(argv[1], "sparse-file-seek"))
    return argc < 3 ? 79 : sparse_file_seek(argv[2]);
  if (equal(argv[1], "virtual-file-metadata"))
    return argc < 3 ? 79 : virtual_file_metadata(argv[2]);
  if (equal(argv[1], "writable-files") ||
      equal(argv[1], "writable-files-nocancel"))
    return argc < 3 ? 49
                    : writable_files(argv[2],
                                     equal(argv[1], "writable-files-nocancel"));
  if (equal(argv[1], "files") || equal(argv[1], "files-nocancel"))
    return argc < 3 ? 139
                    : file_calls(argv[2], equal(argv[1], "files-nocancel"));
  if (equal(argv[1], "directory-entries"))
    return argc < 3 ? 251 : directory_entries(argv[2]);
  if (equal(argv[1], "directories"))
    return argc < 3 ? 251 : directory_calls(argv[2]);
  if (equal(argv[1], "file-mapping"))
    return argc < 3 ? 181 : file_mapping(argv[2]);
  if (equal(argv[1], "file-status"))
    return argc < 3 ? 132 : file_status(argv[2]);
  if (equal(argv[1], "output-descriptors"))
    return output_descriptors();
  if (equal(argv[1], "stdin")) {
    unsigned char bytes[4];
    u64 fd = call(41, 0, 0, 0, 0, 0, 0, &error);
    if (error || call(3, fd, (u64)bytes, 2, 0, 0, 0, &error) != 2 || error ||
        bytes[0] != 0 || bytes[1] != 255)
      return 205;
    if (call(396, 0, (u64)(bytes + 2), 2, 0, 0, 0, &error) != 1 || error ||
        bytes[2] != 'x' || call(3, fd, 0, 1, 0, 0, 0, &error) || error)
      return 206;
    if (call(4, 1, (u64)bytes, 3, 0, 0, 0, &error) != 3 || error)
      return 207;
    return 37;
  }
  if (equal(argv[1], "loop"))
    for (;;)
      ++bss;
  if (equal(argv[1], "fault"))
    *(volatile u64 *)0 = 1;
  if (equal(argv[1], "unknown"))
    return (int)call(999, 0, 0, 0, 0, 0, 0, &error);
  if (equal(argv[1], "mach"))
    return (int)call((u64)-31, 0, 0, 0, 0, 0, 0, &error);
  if (equal(argv[1], "badtrap")) {
#if defined(__aarch64__)
    __asm__ volatile("svc #0" ::: "memory");
#else
    __asm__ volatile("mov $1, %%eax\n\tsyscall" ::
                         : "rax", "rcx", "r11", "memory");
#endif
  }
  if (equal(argv[1], "exit"))
    return (int)call(1, 37, 0, 0, 0, 0, 0, &error);
  if (equal(argv[1], "return"))
    return 37;
  if (equal(argv[1], "write-length")) {
    const u64 counts[] = {0x80000000UL, (u64)-1};
    for (unsigned i = 0; i < 2; ++i)
      for (unsigned j = 0; j < 2; ++j)
        if (call(4, j ? 99 : 1, 0, counts[i], 0, 0, 0, &error) != 22 || !error)
          return 127;
    if (call(4, 99, 0, 0x7fffffffUL, 0, 0, 0, &error) != 9 || !error)
      return 128;
    if (call(4, 1, 0, 1, 0, 0, 0, &error) != 14 || !error)
      return 129;
    const char byte = 'w';
    if (call(4, 1, (u64)&byte, 1, 0, 0, 0, &error) != 1 || error)
      return 130;
    return 37;
  }
  if (equal(argv[1], "release")) {
    u64 sp;
#if defined(__aarch64__)
    __asm__ volatile("mov %0, sp" : "=r"(sp));
#else
    __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
#endif
    const u64 pages[] = {(sp & ~(PAGE - 1)) - PAGE, (u64)&reclaimable[PAGE]};
    for (unsigned i = 0; i < 2; ++i) {
      if (call(197, 0, PAGE, 3, 0x1002, (u64)-1, 0, &error) != 12 || !error)
        return 124;
      if (call(73, pages[i], PAGE, 0, 0, 0, 0, &error) || error)
        return 125;
      u64 fresh = call(197, 0, PAGE, 3, 0x1002, (u64)-1, 0, &error);
      if (error || *(volatile u64 *)fresh)
        return 126;
    }
    return 37;
  }
  if (equal(argv[1], "identity")) {
    const u64 numbers[] = {24, 25, 39, 43, 47};
    for (unsigned i = 0; i < 5; ++i)
      if (call(numbers[i], 0, 0, 7, 0, 0, 0, &error) != (i == 2 ? 1 : 1000) ||
          error || secondary)
        return 120;
    return 37;
  }
  if (equal(argv[1], "partial")) {
    u64 address = call(197, 0, PAGE, 3, 0x1002, (u64)-1, 0, &error);
    if (error)
      return 121;
    volatile unsigned char *end = (void *)(address + PAGE - 2);
    end[0] = 'p';
    end[1] = 'q';
    if (call(4, 1, (u64)end, 4, 0, 0, 0, &error) != 14 || !error)
      return 122;
    if (call(4, 1, (u64)end, 1, 0, 0, 0, &error) != 1 || error)
      return 123;
    return 37;
  }
  if (equal(argv[1], "memory") || equal(argv[1], "permission")) {
    u64 address = call(197, 0, PAGE * 3, 3, 0x1002, (u64)-1, 0, &error);
    if (error || address % PAGE)
      return 110;
    volatile unsigned char *memory = (void *)address;
    memory[0] = 'a';
    memory[PAGE] = 'b';
    memory[PAGE * 2] = 'c';
    if (equal(argv[1], "permission")) {
      if (call(74, address, PAGE, 1, 0, 0, 0, &error) || error)
        return 111;
      memory[0] = 'x';
      return 112;
    }
    if (call(73, address + 1, PAGE, 0, 0, 0, 0, &error) != 22 || !error)
      return 113;
    if (call(73, address + PAGE, PAGE, 0, 0, 0, 0, &error) || error)
      return 114;
    if (call(74, address, PAGE * 3, 0, 0, 0, 0, &error) != 12 || !error)
      return 115;
    memory[0] = 'd'; /* Failed protect across a hole must preserve this page. */
    if (call(4, 1, address, 1, 0, 0, 0, &error) != 1 || error)
      return 116;
    if (call(4, 1, address + PAGE, 1, 0, 0, 0, &error) != 14 || !error)
      return 117;
    if (call(73, address, PAGE * 3, 0, 0, 0, 0, &error) || error)
      return 118;
    u64 reused = call(197, address, PAGE * 3, 2, 0x1002, (u64)-1, 0, &error);
    if (error || reused != address || *(volatile u64 *)reused != 0)
      return 119;
    return 37;
  }
  if (argc != 3 || !equal(argv[0], "guest") || !equal(argv[2], "argument") ||
      argv[3] || !envp[0] || !equal(envp[0], "MODE=test") || envp[1] ||
      !apple[0] || apple[1])
    return 102;
  const char prefix[] = "executable_path=";
  for (unsigned i = 0; i < sizeof(prefix) - 1; ++i)
    if (apple[0][i] != prefix[i])
      return 103;
  if (call(4, 99, 0, 7, 0, 0, 0, &error) != 9 || !error)
    return 104;
#if defined(__aarch64__)
  if (secondary != 0)
    return 105;
#else
  if (secondary != 7)
    return 105;
#endif
  if (call(20, 0, 0, 7, 0, 0, 0, &error) != 1000 || error || secondary)
    return 106;
  if (call(4, 1, 0, 1, 0, 0, 0, &error) != 14 || !error)
    return 107;
  const unsigned char bytes[] = {'d', 'a', 'r', 'w', 'i', 'n', 0, 255, '\n'};
  if (call(4, 1, (u64)bytes, sizeof(bytes), 0, 0, 0, &error) != sizeof(bytes) ||
      error)
    return 108;
  return 37;
}
