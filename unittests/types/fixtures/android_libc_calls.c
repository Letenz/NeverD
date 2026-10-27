// An Android program calling routines bionic exports, which NeverD's
// android-* libraries declare the prototypes of, with the records laid out
// as bionic lays them: a 32-bit pthread_mutex_t is one word, and bionic's
// struct stat is not glibc's.  android_libc_calls_arm64 is a program that
// asks for Android's dynamic linker; android_libc_calls_arm a library, as
// an APK holds one, that only the NDK's identification note marks.  Built
// with clang and lld against bionic's headers (I, below) and a libc.so of
// android_libc_stub.c, as nothing runs them:
//   I="-nostdinc -isystem $(clang -print-resource-dir)/include
//      -isystem $BIONIC/libc/include -isystem $BIONIC/libc/kernel/uapi
//      -isystem $BIONIC/libc/kernel/uapi/asm-arm64
//      -isystem $BIONIC/libc/kernel/android/uapi"
//   clang --target=aarch64-linux-android35 -shared -nostdlib -fuse-ld=lld \
//     -Wl,-soname,libc.so -o arm64/libc.so android_libc_stub.c
//   clang --target=aarch64-linux-android35 $I -O1 -fno-stack-protector \
//     -fuse-ld=lld -nostdlib -Wl,--dynamic-linker=/system/bin/linker64 \
//     -Wl,-e,Run -o android_libc_calls_arm64 android_libc_calls.c \
//     arm64/libc.so
//   clang --target=armv7a-linux-androideabi35 ... (asm-arm) -DANDROID_LIBRARY
//     -O1 -fno-stack-protector -fuse-ld=lld -nostdlib -shared -fPIC
//     -o android_libc_calls_arm android_libc_calls.c arm/libc.so
#include <pthread.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef ANDROID_LIBRARY
// The note the NDK's crtbegin links in: the API level, then the NDK's
// version and build.
__attribute__((section(".note.android.ident"), aligned(4),
               used)) static const struct {
  int NameSize, DescriptionSize, Type;
  char Name[8];
  int Api;
  char Version[64], Build[64];
} AndroidIdent = {8, 132, 1, "Android", 35, "r27", "12077973"};
#endif

__attribute__((noinline)) long Size(const char *Path) {
  struct stat Info;
  if (stat(Path, &Info) != 0)
    return -1;
  return Info.st_size;
}

__attribute__((noinline)) long Read(int File, char *Buffer) {
  return read(File, Buffer, 64);
}

__attribute__((noinline)) long Number(const char *Text) {
  return strtol(Text, 0, 16);
}

__attribute__((noinline)) int Lock(void) {
  static pthread_mutex_t Mutex;
  return pthread_mutex_init(&Mutex, 0);
}

int Run(int Count, char **Arguments) {
  char Buffer[64];
  return (int)(Size(Arguments[0]) + Read(Count, Buffer) + Number(Arguments[0]) +
               Lock());
}
