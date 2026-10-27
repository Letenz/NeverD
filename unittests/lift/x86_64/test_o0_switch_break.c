// A switch built at -O0. Each `case ...: break;` becomes a block that holds
// only a jump to the code after the switch. Another case keeps locals in its
// frame slots, and one dispatches again after a call, so the edges into the
// code after the switch carry different values.

struct value {
  signed char type;
  long long num;
};

extern int convert_error(int code);
extern int last_error(void);
extern int parse_number(const struct value *v, long long *out);
extern struct value *dereference(struct value *v, int flags);

int value_to_long(struct value *v) {
  if (!v)
    return convert_error(1531);
again:
  switch (v->type) {
  case 2:
    break;
  case 3: {
    long long number = 0;
    int parsed = parse_number(v, &number);
    v->num = parsed ? number : 0;
    v->type = 2;
    break;
  }
  case 4:
    v->num = 0;
    v->type = 2;
    break;
  case 7:
    break;
  case 8:
    v->num = -v->num;
    break;
  case 9:
    v->type = 2;
    break;
  case 10:
    if (!dereference(v, 2))
      return last_error();
    goto again;
  default:
    return convert_error(1531);
  }
  return 0;
}
