/* Independent ordinary C imports and observable eager lazy-pointer slots.
 * No SDK headers or assembly call shims are used. */
typedef unsigned long u64;
extern long read(int, void *, u64);
extern long write(int, const void *, u64);
extern int close(int);
extern int getpid(void);
extern int *__error(void);
extern void _exit(int) __attribute__((noreturn));
extern const unsigned char _mh_execute_header[];

static unsigned u32(const unsigned char *p) {
  return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 |
         (unsigned)p[3] << 24;
}
static u64 number(const unsigned char *p) {
  return (u64)u32(p) | (u64)u32(p + 4) << 32;
}
struct Slots {
  volatile const u64 *address[6];
  u64 value[6];
  u64 text, text_size;
};
static int snapshot(struct Slots *slots) {
  const unsigned char *header = _mh_execute_header;
  if (u32(header) != 0xfeedfacf)
    return 101;
  unsigned count = u32(header + 16), bytes = u32(header + 20), offset = 32;
  if (count > 256 || bytes > 65536)
    return 102;
  u64 slide = 0, text = 0, text_size = 0, lazy = 0;
  unsigned headers = 0, texts = 0, sections_found = 0;
  for (unsigned i = 0; i < count; ++i) {
    if (offset > 32 + bytes || 32 + bytes - offset < 8)
      return 103;
    const unsigned char *command = header + offset;
    unsigned size = u32(command + 4);
    if (size < 8 || size > 32 + bytes - offset)
      return 104;
    if (u32(command) == 0x19) {
      if (size < 72)
        return 105;
      unsigned sections = u32(command + 64);
      if (sections > (size - 72) / 80)
        return 106;
      if (!number(command + 40) && number(command + 48)) {
        slide = (u64)header - number(command + 24);
        ++headers;
      }
      if (u32(command + 60) & 4) {
        text = number(command + 24);
        text_size = number(command + 32);
        ++texts;
      }
      for (unsigned j = 0; j < sections; ++j) {
        const unsigned char *section = command + 72 + j * 80;
        if ((u32(section + 64) & 255) == 7) {
          if (number(section + 40) != 6 * sizeof(u64))
            return 107;
          lazy = number(section + 32);
          ++sections_found;
        }
      }
    }
    offset += size;
  }
  if (headers != 1 || texts != 1 || sections_found != 1 || offset != 32 + bytes)
    return 108;
  slots->text = text + slide;
  slots->text_size = text_size;
  for (unsigned i = 0; i != 6; ++i) {
    slots->address[i] = (volatile const u64 *)(lazy + slide + i * sizeof(u64));
    slots->value[i] = *slots->address[i];
    if (!slots->value[i] || (slots->value[i] >= slots->text &&
                             slots->value[i] - slots->text < slots->text_size))
      return 109;
  }
  return 0;
}

int main(int argc, char **argv) {
  struct Slots slots;
  int status = snapshot(&slots);
  if (status)
    return status;
  if (argc == 2 && argv[1][0] == 'e') {
    if (write(1, "X", 1) != 1)
      return 110;
    _exit(0x123);
  }
  if (argc != 1)
    return 111;
  int *error = __error();
  if (error != __error())
    return 112;
  *error = 71;
  int pid = getpid();
  if (pid <= 0 || pid != getpid() || *error != 71)
    return 113;
  const char output[] = {'A', 0, 'Z'};
  if (write(1, output, sizeof(output)) != sizeof(output) || *error != 71)
    return 114;
  if (write(-1, output, 1) != -1 || *error != 9)
    return 115;
  char input = 'Q';
  if (read(0, &input, 1) || input != 'Q' || *error != 9)
    return 116;
  if (close(0) || *error != 9 || read(0, &input, 0) != -1 || *error != 9)
    return 117;
  if (close(-1) != -1 || *error != 9 || getpid() != pid || *error != 9)
    return 118;
  for (unsigned i = 0; i != 6; ++i)
    if (*slots.address[i] != slots.value[i])
      return 119;
  if (write(1, "S", 1) != 1 || *error != 9)
    return 120;
  _exit(83);
}
