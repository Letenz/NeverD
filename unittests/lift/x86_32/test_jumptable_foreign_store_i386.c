/* Clang -O0 -fPIC spills the GOT base to the frame before a loop and reloads
   it for the switch table in every iteration.  The cases store through a
   pointer the function loaded or through a global's GOT slot; neither can
   name the spill while no frame address escapes. */
int jt_i386_foreign_store_loop(const int *p, int *out) {
  for (;;) {
    int c = *p++;
    switch (c) {
    case 0:
      return 1;
    case 1:
      *out = 2;
      break;
    case 2:
      *out = 3;
      break;
    case 3:
      *out = 4;
      break;
    case 4:
      *out = 5;
      break;
    case 5:
      *out = 6;
      break;
    case 6:
      *out = 7;
      break;
    case 7:
      *out = 8;
      break;
    default:
      *out = 9;
      break;
    }
  }
}

int jt_i386_global_result;

int jt_i386_global_store_loop(const int *p) {
  for (;;) {
    int c = *p++;
    switch (c) {
    case 0:
      return jt_i386_global_result;
    case 1:
      jt_i386_global_result = 2;
      break;
    case 2:
      jt_i386_global_result = 3;
      break;
    case 3:
      jt_i386_global_result = 4;
      break;
    case 4:
      jt_i386_global_result = 5;
      break;
    case 5:
      jt_i386_global_result = 6;
      break;
    case 6:
      jt_i386_global_result = 7;
      break;
    case 7:
      jt_i386_global_result = 8;
      break;
    default:
      jt_i386_global_result = 9;
      break;
    }
  }
}

/* An element address with a variable index escapes to memory, and the cases
   store through the pointer read back from there.  That pointer may name any
   frame cell, the spilled GOT base included, so the table stays unproved.  No
   call lies between the spill and its reload. */
int jt_i386_escaped_frame_loop(const int *p, int **out) {
  int cells[4];
  cells[0] = cells[1] = cells[2] = cells[3] = 0;
  for (;;) {
    int c = *p++;
    *out = &cells[c & 3];
    switch (c) {
    case 0:
      return cells[0];
    case 1:
      **out = 2;
      break;
    case 2:
      **out = 3;
      break;
    case 3:
      **out = 4;
      break;
    case 4:
      **out = 5;
      break;
    case 5:
      **out = 6;
      break;
    case 6:
      **out = 7;
      break;
    case 7:
      **out = 8;
      break;
    default:
      **out = 9;
      break;
    }
  }
}

/* No frame address escapes, but the cases store into a local array at an
   index nothing bounds; that address is formed from the frame and may name
   the spill. */
int jt_i386_frame_index_loop(const int *p) {
  int cells[4];
  cells[0] = cells[1] = cells[2] = cells[3] = 0;
  for (;;) {
    int c = *p++;
    int k = *p++;
    switch (c) {
    case 0:
      return cells[0];
    case 1:
      cells[k] = 2;
      break;
    case 2:
      cells[k] = 3;
      break;
    case 3:
      cells[k] = 4;
      break;
    case 4:
      cells[k] = 5;
      break;
    case 5:
      cells[k] = 6;
      break;
    case 6:
      cells[k] = 7;
      break;
    case 7:
      cells[k] = 8;
      break;
    default:
      cells[k] = 9;
      break;
    }
  }
}
