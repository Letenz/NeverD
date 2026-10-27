// A program's own types holding the C library's, which its DWARF describes
// beside them: NeverD's C header takes the C library's types from the
// system's headers where the compiler has the C library.  Regenerate the
// objects with:
//   clang -g -O0 -c dwarf_libc.c -o dwarf_libc.x64.o
//   clang -m32 -g -O0 -c dwarf_libc.c -o dwarf_libc.x86.o
#define _GNU_SOURCE 1
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

typedef struct {
  int depth;
  unsigned flags;
} limits_t;

struct record {
  FILE *log;
  struct stat info;
  struct timespec when;
  sigset_t mask;
  limits_t limits;
  long count;
};

struct record g_record;

int main(void) { return (int)g_record.count; }
