// Code that reaches records through the members of others, which no type
// of a function's own names: HighC defines them as it renders.  Regenerate
// the object with:
//   gcc -g -O1 -c dwarf_chains.c -o dwarf_chains.gcc.o

struct node {
  struct node *next;
  struct node *child;
  int value;
};

struct list {
  struct node *head;
  long count;
};

int sum_list(struct list *l) {
  int s = 0;
  for (struct node *n = l->head; n; n = n->next)
    s += n->value;
  return s;
}

int first_child(struct list *l) {
  struct node *c = l->head ? l->head->child : 0;
  return c ? c->value : -1;
}
