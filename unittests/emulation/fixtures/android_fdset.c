/* Authored LP64 callers. No host fd_set, libc or descriptor table is used. */
typedef unsigned long U;
extern void __FD_SET_chk(int, void *, U);
extern void __FD_CLR_chk(int, void *, U);
extern int __FD_ISSET_chk(int, const void *, U);
extern int *__errno(void);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);
extern int mprotect(void *, U, int);

U fdset_call(U *out, int fd, void *set, U size, U mode) {
  void (*modify)(int, void *, U) =
      (mode & 3) == 0 ? __FD_SET_chk : __FD_CLR_chk;
  int (*query)(int, const void *, U) = __FD_ISSET_chk;
  if (mode & 4) {
    void *library = dlopen("libfdset.so", 2);
    if (!library)
      return 97;
    if ((mode & 3) == 2)
      query = dlsym(library, "__FD_ISSET_chk");
    else
      modify =
          dlsym(library, (mode & 3) == 0 ? "__FD_SET_chk" : "__FD_CLR_chk");
    if (!query || !modify)
      return 98;
    if (mode & 8)
      dlclose(library);
  }
  if (mode & 16)
    if (mprotect((void *)(((U)set) & ~4095UL), 4096, 1))
      return 99;
  *__errno() = 73;
  if ((mode & 3) == 2)
    out[0] = query(fd, set, size);
  else {
    modify(fd, set, size);
    out[0] = 0;
  }
  out[1] = (unsigned)*__errno();
  out[2] = 0x12345678;
  return 0;
}

U fdset_sequence(U *out, void *set) {
  *__errno() = 73;
  __FD_SET_chk(0, set, 128);
  __FD_SET_chk(63, set, 128);
  __FD_SET_chk(64, set, 128);
  __FD_SET_chk(1023, set, 128);
  __FD_SET_chk(64, set, 128);
  out[0] = __FD_ISSET_chk(0, set, 128);
  out[1] = __FD_ISSET_chk(63, set, 128);
  out[2] = __FD_ISSET_chk(64, set, 128);
  out[3] = __FD_ISSET_chk(1023, set, 128);
  __FD_CLR_chk(63, set, 128);
  __FD_CLR_chk(63, set, 128);
  out[4] = __FD_ISSET_chk(63, set, 128);
  out[5] = (unsigned)*__errno();
  return 0;
}

/* Leave the upper x0 bits intact to check the imported signed-int ABI. */
__asm__(".global fdset_wide_index\n"
        ".type fdset_wide_index, @function\n"
        "fdset_wide_index:\n"
        "b __FD_ISSET_chk\n");
__asm__(".type __FD_SET_chk, @function\n"
        ".type __FD_CLR_chk, @function\n"
        ".type __FD_ISSET_chk, @function\n");
