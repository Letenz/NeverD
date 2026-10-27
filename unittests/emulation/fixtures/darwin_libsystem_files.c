/* Independent ordinary C calls. No SDK headers or assembly call shims. */
typedef unsigned long size_t;
extern int open(const char *, int, ...);
extern long read(int, void *, size_t);
extern long write(int, const void *, size_t);
extern int close(int);
extern int dup(int);
extern int dup2(int, int);
extern long long lseek(int, long long, int);
extern long pread(int, void *, size_t, long long);
extern long pwrite(int, const void *, size_t, long long);
extern int *__error(void);

int main(int argc, char **argv) {
  if (argc != 4)
    return 101;
  int *error = __error();
  *error = 71;
  int fd = open(argv[1], 2);
  if (fd < 0 || *error != 71)
    return 102;
  int separate = open(argv[1], 0);
  if (separate < 0 || separate == fd || *error != 71)
    return 103;
  if (open(argv[3], 0) != -1 || *error != 2)
    return 104;
  char bytes[12] = {0};
  if (read(fd, bytes, 2) != 2 || bytes[0] != '0' || bytes[1] != '1' ||
      *error != 2)
    return 105;
  int shared = dup(fd);
  if (shared < 0 || shared == fd || shared == separate || *error != 2)
    return 106;
  if (read(shared, bytes, 2) != 2 || bytes[0] != '2' || bytes[1] != '3' ||
      lseek(fd, 0, 1) != 4 || *error != 2)
    return 107;
  if (pread(shared, bytes, 3, 1) != 3 || bytes[0] != '1' || bytes[1] != '2' ||
      bytes[2] != '3' || lseek(shared, 0, 1) != 4 || *error != 2)
    return 108;
  const char patch[] = {'X', 0, 'Y'};
  if (pwrite(fd, patch, 3, 6) != 3 || lseek(fd, 0, 1) != 4 ||
      lseek(separate, 0, 1) != 0 || *error != 2)
    return 109;
  if (read(separate, bytes, 10) != 10 || *error != 2)
    return 110;
  const char expected[] = {'0', '1', '2', '3', '4', '5', 'X', 0, 'Y', '9'};
  for (unsigned i = 0; i != 10; ++i)
    if (bytes[i] != expected[i])
      return 111;
  if (dup2(shared, separate) != separate || read(separate, bytes, 2) != 2 ||
      bytes[0] != '4' || bytes[1] != '5' || lseek(fd, 0, 1) != 6 || *error != 2)
    return 112;
  if (dup2(fd, fd) != fd || *error != 2 || dup2(-1, separate) != -1 ||
      *error != 9 || lseek(separate, 0, 1) != 6 || *error != 9)
    return 121;
  if (lseek(fd, 0x100000007LL, 0) != 0x100000007LL ||
      lseek(shared, 0, 1) != 0x100000007LL ||
      pread(fd, bytes, 1, 0x100000000LL) != 0 || lseek(fd, 6, 0) != 6 ||
      *error != 9)
    return 122;
  if (lseek(fd, -1, 0) != -1 || *error != 22 || lseek(fd, 0, 1) != 6 ||
      *error != 22)
    return 113;
  if (pwrite(fd, patch, 1, -1) != -1 || *error != 22 ||
      pread(-1, (void *)0, 0, 0) != -1 || *error != 9)
    return 114;
  if (close(shared) || close(separate) || close(fd) || *error != 9)
    return 115;
  /* open receives an int vararg, then narrows it to the 16-bit mode_t. */
  int created = open(argv[2], 0xa02, 0x12340000 | 0641);
  if (created < 0 || *error != 9)
    return 116;
  const char fresh[] = {'N', 0, 'W'};
  if (write(created, fresh, 3) != 3 || close(created) || *error != 9)
    return 117;
  created = open(argv[2], 0);
  if (created < 0 || read(created, bytes, 3) != 3 || bytes[0] != 'N' ||
      bytes[1] || bytes[2] != 'W' || read(created, bytes, 1) ||
      close(created) || *error != 9)
    return 118;
  if (open(argv[2], 0xa02, 0641) != -1 || *error != 17)
    return 119;
  const char output[] = {'F', 0, 'D'};
  if (write(1, output, 3) != 3 || *error != 17)
    return 120;
  return 73;
}
