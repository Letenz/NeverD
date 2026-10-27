// Calls through function pointers that typed members point to, which the
// output must read once each: t->out->write reads t->out, then the slot 8
// bytes into what it points to.  Built with clang and lld, with DWARF:
//   clang -O1 -g -fno-stack-protector -fuse-ld=lld \
//     -o member_calls_x64 member_calls.c
struct ops {
  long (*reset)(void *, int);
  long (*write)(void *, const char *);
};
struct table {
  long hdr[8];
  void *in;
  struct ops *out;
  struct ops *boot;
};

__attribute__((noinline)) long entry(void *handle, struct table *t) {
  t->out->write(t->out, "hi");
  return t->boot->reset(handle, 3);
}

int main(int Count, char **Arguments) {
  return (int)entry(Arguments, (struct table *)Arguments[1]);
}
