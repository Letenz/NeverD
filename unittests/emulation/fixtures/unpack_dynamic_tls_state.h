//===- unpack_dynamic_tls_state.h - Independent dynamic slot state -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
__declspec(dllimport) U32 TlsAlloc(void);
__declspec(dllimport) int TlsFree(U32);
__declspec(dllimport) void *TlsGetValue(U32);
__declspec(dllimport) int TlsSetValue(U32, void *);
__declspec(dllimport) U32 FlsAlloc(void *);
__declspec(dllimport) int FlsFree(U32);
__declspec(dllimport) void *FlsGetValue(U32);
__declspec(dllimport) int FlsSetValue(U32, void *);
static U32 DynamicSlot;

static int dynamicMode(U32 Mode) {
  return Mode >= DynamicTLSMode && Mode <= ClearedUnallocatedTLSValueMode;
}
static int fiberMode(U32 Mode) {
  return Mode == DynamicFLSMode || Mode == ReleasedDynamicFLSMode ||
         Mode == LateDynamicFLSMode || Mode == ZeroDynamicFLSMode;
}
static int lateDynamicMode(U32 Mode) {
  return Mode == LateDynamicTLSMode || Mode == LateDynamicFLSMode;
}
static int releasedDynamicMode(U32 Mode) {
  return Mode == ReleasedDynamicTLSMode || Mode == ReleasedDynamicFLSMode;
}
static U64 dynamicValue(U32 Mode) {
  return Mode == ZeroDynamicTLSMode || Mode == ZeroDynamicFLSMode
             ? 0
             : InitializeResult;
}
static void createDynamicState(U32 Mode) {
  const int Fiber = fiberMode(Mode);
  DynamicSlot = Fiber ? FlsAlloc(0) : TlsAlloc();
  if (DynamicSlot == 0xffffffffU ||
      !(Fiber ? FlsSetValue(DynamicSlot, (void *)dynamicValue(Mode))
              : TlsSetValue(DynamicSlot, (void *)dynamicValue(Mode))))
    ExitProcess(FailureStatus);
}
static void prepareDynamicState(U32 Mode) {
  if (Mode == UnallocatedTLSValueMode ||
      Mode == ClearedUnallocatedTLSValueMode) {
    DynamicSlot = 63;
    if (!TlsSetValue(DynamicSlot, (void *)(U64)InitializeResult) ||
        (Mode == ClearedUnallocatedTLSValueMode &&
         !TlsSetValue(DynamicSlot, 0)))
      ExitProcess(FailureStatus);
    return;
  }
  if (!dynamicMode(Mode) || lateDynamicMode(Mode))
    return;
  createDynamicState(Mode);
  if (releasedDynamicMode(Mode)) {
    if (!(fiberMode(Mode) ? FlsFree(DynamicSlot) : TlsFree(DynamicSlot)))
      ExitProcess(FailureStatus);
    DynamicSlot = 0;
  }
}
static int validDynamicState(U32 Mode) {
  if (!dynamicMode(Mode) || Mode == ClearedUnallocatedTLSValueMode)
    return 1;
  if (lateDynamicMode(Mode) || releasedDynamicMode(Mode))
    createDynamicState(Mode);
  const U64 Value = (U64)(fiberMode(Mode) ? FlsGetValue(DynamicSlot)
                                          : TlsGetValue(DynamicSlot));
  return Value == dynamicValue(Mode);
}
