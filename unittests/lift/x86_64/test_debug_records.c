// Records the debug information describes but C cannot spell from it: bit
// fields leave a record with no supported layout.  Their pointers, values
// and arrays must keep the machine types the decompiler recovered.

struct Flags {
  struct Flags *next;
  int value;
  unsigned kind : 3;
  unsigned mode : 5;
};

__attribute__((noinline)) int flags_sum(const struct Flags *f) {
  int s = 0;
  for (; f; f = f->next)
    s += f->value + (int)f->kind;
  return s;
}

__attribute__((noinline)) struct Flags flags_make(int value, unsigned kind) {
  struct Flags f = {0, value, kind, 1};
  return f;
}

__attribute__((noinline)) int flags_mode(struct Flags f) {
  return (int)f.mode + f.value;
}

int flags_walk(int count) {
  struct Flags list[3];
  for (int i = 0; i < 3; i++) {
    list[i] = flags_make(i + count, (unsigned)i);
    list[i].next = i < 2 ? &list[i + 1] : 0;
  }
  return flags_sum(list) + flags_sum(&list[1]) + flags_mode(list[2]);
}
