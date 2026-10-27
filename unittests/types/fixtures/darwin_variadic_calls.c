// Calls of routines Darwin's libSystem declares otherwise than glibc does,
// or variadic: Apple's AArch64 convention passes a variadic routine's named
// parameters in registers and the rest on the stack.  Built as
// darwin_libc_stub.c says for darwin_libc_calls_arm64, from this file:
//   clang OPTIONS -O1 -fno-stack-protector -fuse-ld=lld -nostdlib \
//     -Wl,-e,_main -o darwin_variadic_calls_arm64 darwin_variadic_calls.c \
//     libSystem.dylib
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <syslog.h>

int SetFlags(int Fd, int Flags) { return fcntl(Fd, F_SETFL, Flags); }
int Request(int Fd, unsigned long Code, void *Arg) {
  return ioctl(Fd, Code, Arg);
}
int OpenAt(int Dir, const char *Path, int Mode) {
  return openat(Dir, Path, O_CREAT | O_WRONLY, Mode);
}
void Log(int Value) { syslog(LOG_INFO, "value %d", Value); }
int Shared(const char *Name) { return shm_open(Name, O_CREAT | O_RDWR, 0600); }
int Send(int File, int Socket, off_t *Length) {
  return sendfile(File, Socket, 0, Length, 0, 0);
}
int Name(void) { return pthread_setname_np("worker"); }
int main(void) { return 0; }
