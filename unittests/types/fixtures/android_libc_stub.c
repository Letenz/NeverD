// The routines android_libc_calls.c imports, which only need to exist for
// the link: NeverD reads the program, nothing runs it.
int stat(const char *Path, void *Info) { return 0; }
long read(int File, void *Buffer, unsigned long Count) { return 0; }
long strtol(const char *Text, char **End, int Base) { return 0; }
int pthread_mutex_init(void *Mutex, const void *Attributes) { return 0; }
