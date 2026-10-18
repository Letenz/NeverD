/* Clang -O0 -fPIC -mstackrealign aligns ESP in every prologue (`and $-16,
   %esp`; a leaf aligns to 4).  Every local, the spilled GOT base included, is
   then addressed from the aligned ESP, while the arguments stay at EBP
   offsets. */
int jt_i386_realigned_loop(const int *p) {
  int r = 0;
  for (;;) {
    int c = *p++;
    switch (c) {
    case 0:
      return r;
    case 1:
      r = 2;
      break;
    case 2:
      r = 3;
      break;
    case 3:
      r = 4;
      break;
    case 4:
      r = 5;
      break;
    case 5:
      r = 6;
      break;
    case 6:
      r = 7;
      break;
    case 7:
      r = 8;
      break;
    default:
      r = 9;
      break;
    }
  }
}

struct jt_i386_text {
  char *data;
  unsigned size, capacity;
};
extern void jt_i386_text_init(struct jt_i386_text *);
extern void jt_i386_text_append(struct jt_i386_text *, char);
extern int jt_i386_next_token(void *);

/* A local object's address reaches every call, and the cases store through
   an argument; the GOT base spill is a different cell of the aligned frame. */
int jt_i386_realigned_lexer(void *self, const char *cur, int *out) {
  struct jt_i386_text text;
  jt_i386_text_init(&text);
  *out = 1;
  for (;;) {
    if (jt_i386_next_token(self) != 0x22)
      return 0;
    char c = *cur++;
    switch (c) {
    case '\0':
      return 1;
    case ' ':
      *out = 2;
      break;
    case '!':
      *out = 3;
      break;
    case '#':
      *out = 4;
      break;
    case '%':
      *out = 5;
      break;
    case '&':
      *out = 6;
      break;
    case '(':
      *out = 7;
      break;
    case ')':
      *out = 8;
      break;
    case '*':
      *out = 9;
      break;
    default:
      jt_i386_text_append(&text, c);
      break;
    }
  }
}
