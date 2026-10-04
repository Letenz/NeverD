/* Independently authored reentrant tokenization and pointer observations. */
typedef unsigned long u64;
extern char *strtok_r(char *, const char *, char **);
extern int *__errno(void);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);
extern int mprotect(void *, u64, int);

u64 token_sequence(u64 *out) {
  char first[] = ",,red:green,blue";
  char second[] = "|one||two|";
  char *a = (char *)1, *b = (char *)1;
  *__errno() = 733;
  out[0] = strtok_r(first, ",:", &a) - first;
  out[1] = a - first;
  out[2] = strtok_r(second, "|", &b) - second;
  out[3] = b - second;
  out[4] = strtok_r(0, ",", &a) - first;
  out[5] = a - first;
  out[6] = strtok_r(0, "|", &b) - second;
  out[7] = b - second;
  out[8] = strtok_r(0, "", &a) - first;
  out[9] = (u64)a;
  out[10] = (u64)strtok_r(0, "|", &b);
  out[11] = (u64)b;
  out[12] = (u64)strtok_r(0, (const char *)1, &a);
  out[13] = *__errno();
  const char expected_first[] = ",,red\0green\0blue";
  const char expected_second[] = "|one\0|two\0";
  out[14] = out[15] = 1;
  for (u64 i = 0; i < sizeof(first); ++i)
    if (first[i] != expected_first[i])
      out[14] = 0;
  for (u64 i = 0; i < sizeof(second); ++i)
    if (second[i] != expected_second[i])
      out[15] = 0;
  return 0;
}
u64 token_supplied(char *input, const char *delimiters, char **saved) {
  return (u64)strtok_r(input, delimiters, saved);
}
u64 token_protected(char *input, const char *delimiters, char **saved,
                    void *page) {
  if (mprotect(page, 4096, 1))
    return 99;
  return (u64)strtok_r(input, delimiters, saved);
}
u64 token_dynamic(u64 *out, u64 close_first) {
  char input[] = "north/south";
  char *saved = (char *)1;
  void *library = dlopen("libtokens.so", 2);
  if (!library)
    return 98;
  char *(*split)(char *, const char *, char **) = dlsym(library, "strtok_r");
  if (!split)
    return 97;
  if (close_first && dlclose(library))
    return 96;
  out[0] = split(input, "/", &saved) - input;
  out[1] = saved - input;
  out[2] = input[5];
  out[3] = split(0, "/", &saved) - input;
  out[4] = (u64)saved;
  return (unsigned int)dlclose(library);
}
