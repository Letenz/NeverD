//===- unpack_pointer_state.h - Independent process-local pointer state ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
__declspec(dllimport) void *EncodePointer(void *);
__declspec(dllimport) void *DecodePointer(void *);
__declspec(dllimport) void *GetModuleHandleA(const char *);
__declspec(dllimport) void *GetProcAddress(void *, const char *);

// Keep these calls outside the generated section: unrelated import-tail
// tests transform exactly the original program's output and exit calls.
__attribute__((noinline)) static U64 encodeNull(U32 Mode) {
  if (Mode == NativeEncodedPointerMode) {
    void *(*Encode)(void *) = (void *(*)(void *))GetProcAddress(
        GetModuleHandleA("ntdll.dll"), "RtlEncodePointer");
    return (U64)Encode(0);
  }
  return (U64)EncodePointer(0);
}
__attribute__((noinline)) static int decodesToNull(U32 Mode, U64 Value) {
  if (Mode == NativeEncodedPointerMode || Mode == NativeDecodedPointerMode) {
    void *(*Decode)(void *) = (void *(*)(void *))GetProcAddress(
        GetModuleHandleA("ntdll.dll"), "RtlDecodePointer");
    return Decode((void *)Value) == 0;
  }
  return DecodePointer((void *)Value) == 0;
}
