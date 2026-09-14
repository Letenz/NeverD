#include <unistd.h>

extern void __stack_chk_fail(void) __attribute__((noreturn));
static int receiptFD = -1;

void NDSetStackFailureReceipt(int fd) { receiptFD = fd; }

// Keep this probe in its own dylib so interposition covers both the original
// dylib and recovered methods in the executable. Observe the exact terminal
// target without invoking the host's uncatchable crash-reporting path.
static void recordFailure(void) __attribute__((noreturn));
static void recordFailure(void) {
  const unsigned char receipt = 93;
  if (write(receiptFD, &receipt, 1) != 1)
    _exit(12);
  _exit(receipt);
}

static const struct {
  const void *replacement;
  const void *original;
} failureInterposition __attribute__((used, section("__DATA,__interpose"))) = {
    (const void *)recordFailure, (const void *)__stack_chk_fail};
