// A Linux program one of whose routines calls two that <unistd.h> declares,
// read and write.  A user who declares read otherwise keeps the header for
// write, so read's calls go through another identifier, which links by
// read's name.  Built against glibc 2.43 with clang and lld:
//   clang -O1 -fno-stack-protector -fuse-ld=lld \
//     -o user_declared_calls_x64 user_declared_calls.c
#include <unistd.h>

__attribute__((noinline)) long Copy(int In, int Out, char *Buffer) {
  const long Count = read(In, Buffer, 64);
  return write(Out, Buffer, Count);
}

int main(int Count, char **Arguments) {
  (void)Arguments;
  char Buffer[64];
  return (int)Copy(Count, 1, Buffer);
}
