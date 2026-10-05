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
