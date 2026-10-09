// Independently authored private heap lifetime and isolation oracle.
static void runPrivateHeaps(void) {
  void *process = GetProcessHeap();
  U8 *p = HeapAlloc(process, 0, 32);
  void *a = HeapCreate(0, 4096, 0), *b = HeapCreate(0, 0, 0);
  require(p && a && b && a != b && a != process && b != process, 1);
  U8 *x = HeapAlloc(a, 8, 257), *y = HeapAlloc(b, 8, 769);
  require(x && y, 2);
  for (U32 i = 0; i < 257; i++)
    x[i] = (U8)(i ^ 0x5a);
  for (U32 i = 0; i < 769; i++)
    y[i] = (U8)(i ^ 0xa5);
  for (U32 i = 0; i < 32; i++)
    p[i] = (U8)(i ^ 0x13);
  x = HeapReAlloc(a, 8, x, 8192);
  require(x && HeapSize(a, 0, x) == 8192, 3);
  for (U32 i = 0; i < 257; i++)
    require(x[i] == (U8)(i ^ 0x5a), 4);
  for (U32 i = 257; i < 8192; i++)
    require(x[i] == 0, 5);
  SetLastError(0x1234);
  require(HeapDestroy(a) && GetLastError() == 0x1234, 6);
  require(HeapSize(b, 0, y) == 769 && HeapSize(process, 0, p) == 32, 7);
  for (U32 i = 0; i < 769; i++)
    require(y[i] == (U8)(i ^ 0xa5), 8);
  for (U32 i = 0; i < 32; i++)
    require(p[i] == (U8)(i ^ 0x13), 9);
  require(HeapDestroy(b) && HeapFree(process, 0, p), 10);
  // Lifetime turnover must release capacity, including an empty heap.
  for (U32 i = 0; i < 8; i++) {
    void *h = HeapCreate(0, 4096, 0);
    require(h != 0, 11);
    if (i & 1)
      require(HeapAlloc(h, 0, 257) != 0, 12);
    require(HeapDestroy(h), 13);
  }
  SetLastError(LastErrorSeed);
}
