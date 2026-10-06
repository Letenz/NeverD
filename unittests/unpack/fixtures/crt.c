// A C runtime program for unpacking tests. Its startup code, imports and TLS
// callbacks are those of a toolchain rather than of this file: the callbacks
// run before the entry point, and its imports lie outside the Windows process
// model, so only the stub that packs it can execute there.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static int compare(const void *A, const void *B) {
  return *(const int *)A - *(const int *)B;
}

int main(int Count, char **Arguments) {
  int Values[64];
  unsigned State = 12345;
  (void)Arguments;
  for (int I = 0; I < 64; ++I) {
    State = State * 1103515245u + 12345u;
    Values[I] = (int)(State >> 16) & 0x7fff;
  }
  qsort(Values, 64, sizeof Values[0], compare);
  char Line[128];
  snprintf(Line, sizeof Line, "unpack fixture: min=%d max=%d tick=%s argc=%d",
           Values[0], Values[63], GetTickCount() ? "nonzero" : "zero", Count);
  puts(Line);
  return (int)(strlen(Line) & 0x7f);
}
