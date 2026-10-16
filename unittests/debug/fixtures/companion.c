/* A shared object whose DWARF lives in a separate file, as distribution
   debug packages and objcopy --only-keep-debug split it. */

int named_by_dwarf(int value) { return value * 3 + 1; }

int also_named_by_dwarf(int value) { return named_by_dwarf(value) - 2; }
