/* Independent character-search callers; no libc implementation is embedded. */
typedef unsigned long u64;
extern char *strchr(const char *, int);
extern char *strrchr(const char *, int);
extern char *__strchr_chk(const char *, int, u64);
extern char *__strrchr_chk(const char *, int, u64);
extern int *__errno(void);
extern int mprotect(void *, u64, int);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);

u64 search_sequence(u64 *out) {
  const char text[] = {'a', (char)0x80, ':', 'b', ':', (char)0xff, 0};
  *__errno() = 733;
  out[0] = strchr(text, ':') - text;
  out[1] = strrchr(text, ':') - text;
  out[2] = __strchr_chk(text, ':', 3) - text;
  out[3] = __strrchr_chk(text, ':', sizeof(text)) - text;
  out[4] = strchr(text, 0x180) - text;
  out[5] = strrchr(text, -1) - text;
  out[6] = __strchr_chk(text, 0, sizeof(text)) - text;
  out[7] = __strrchr_chk(text, 0, sizeof(text)) - text;
  out[8] = (u64)strchr(text, 'z');
  out[9] = (u64)__strchr_chk(text, 'z', sizeof(text));
  out[10] = (u64)strrchr(text, 'z');
  out[11] = (u64)__strrchr_chk(text, 'z', sizeof(text));
  out[12] = __strchr_chk(text, ':', ~(u64)0) - text;
  out[13] = __strrchr_chk(text, ':', 0x100000007UL) - text;
  out[14] = *__errno();
  const volatile unsigned char *bytes = (const unsigned char *)text;
  out[15] = bytes[0] == 'a' && bytes[1] == 0x80 && bytes[2] == ':' &&
            bytes[3] == 'b' && bytes[4] == ':' && bytes[5] == 0xff && !bytes[6];
  return 0;
}

u64 search_supplied(const char *input, int character, u64 bound, u64 mode) {
  switch (mode) {
  case 0:
    return (u64)strchr(input, character);
  case 1:
    return (u64)strrchr(input, character);
  case 2:
    return (u64)__strchr_chk(input, character, bound);
  case 3:
    return (u64)__strrchr_chk(input, character, bound);
  default:
    return 99;
  }
}

u64 search_readonly(void *page, const char *input, int character, u64 bound,
                    u64 mode) {
  if (mprotect(page, 4096, 1))
    return 98;
  return search_supplied(input, character, bound, mode);
}

u64 search_dynamic(u64 *out, u64 close_first, u64 reverse) {
  const char text[] = "left:right:tail";
  void *library = dlopen("libsearch.so", 2);
  if (!library)
    return 97;
  char *(*find)(const char *, int, u64) =
      dlsym(library, reverse ? "__strrchr_chk" : "__strchr_chk");
  if (!find)
    return 96;
  if (close_first && dlclose(library))
    return 95;
  *__errno() = 733;
  out[0] = find(text, ':', sizeof(text)) - text;
  out[1] = find(text, 0, sizeof(text)) - text;
  out[2] = *__errno();
  return (unsigned int)dlclose(library);
}
