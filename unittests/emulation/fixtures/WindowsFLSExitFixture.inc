//===- WindowsFLSExitFixture.inc - Original thread exit side effects ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
__declspec(dllimport) U32 FlsAlloc(void *);
__declspec(dllimport) void *FlsGetValue(U32);
__declspec(dllimport) int FlsSetValue(U32, void *);
static U32 FLSMode, FLSIndex, FLSChild;
static void flsRecord(U32 Tag) {
  U32 Values[] = {Tag, FLSMode}, Written;
  require(WriteFile(GetStdHandle(StdoutSelector), Values, sizeof(Values),
                    &Written, 0) &&
              Written == sizeof(Values),
          70);
}
static void flsChild(void *Value) {
  require(Value == (void *)8 && FlsGetValue(FLSChild) == Value, 71);
  // Completion clears the previous fiber value, including a callback write,
  // but keeps its process-wide index allocated.
  require(!FlsGetValue(FLSIndex) && FlsSetValue(FLSIndex, 0), 72);
  flsRecord(3);
}
static void flsCleanup(void *Value) {
  require(Value == (void *)7 && FlsGetValue(FLSIndex) == Value, 73);
  require(FlsSetValue(FLSIndex, (void *)99), 74);
  FLSChild = FlsAlloc((void *)flsChild);
  require(FLSChild != 0xffffffffU && FLSChild != FLSIndex &&
              FlsSetValue(FLSChild, (void *)8),
          75);
  flsRecord(2);
}
static int flsMode(U32 Mode) {
  return Mode == FLSExitMode || Mode == FLSReturnMode ||
         Mode == FLSNativeExitMode;
}
static void flsScenario(U32 Mode) {
  FLSMode = Mode;
  FLSIndex = FlsAlloc((void *)flsCleanup);
  require(FLSIndex != 0xffffffffU && FlsSetValue(FLSIndex, (void *)7), 76);
}
