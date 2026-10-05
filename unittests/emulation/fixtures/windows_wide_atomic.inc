//===- windows_wide_atomic.inc - Original ring0/ring3 atomic probes -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef struct __declspec(align(16)) {
  unsigned long long Low, High;
} WideAtomicPair;

static const unsigned long long WideAtomicOriginalLow = 0x123456789abcdef0ULL;
static const unsigned long long WideAtomicOriginalHigh = 0x76543210fedcba98ULL;
static const unsigned long long WideAtomicExchangeLow = 0x1122334455667788ULL;
static const unsigned long long WideAtomicExchangeHigh = 0x99aabbccddeeff00ULL;
static const unsigned long long WideAtomicAddressHigh = 0xa5a5a5a500000000ULL;
enum { WideAtomicHalfBits = 32 };

#define NEVERD_WINDOWS_WIDE_ATOMIC(Name, IsWide, Instruction)                  \
  static int checkWideAtomic##Name(void) {                                     \
    for (unsigned Mismatch = 0; Mismatch != 3; ++Mismatch) {                   \
      volatile WideAtomicPair Value = {WideAtomicOriginalLow,                  \
                                       WideAtomicOriginalHigh};                \
      unsigned long long Low =                                                 \
          IsWide ? WideAtomicOriginalLow                                       \
                 : WideAtomicAddressHigh | (unsigned)WideAtomicOriginalLow;    \
      unsigned long long High =                                                \
          IsWide ? WideAtomicOriginalHigh                                      \
                 : WideAtomicAddressHigh |                                     \
                       (WideAtomicOriginalLow >> WideAtomicHalfBits);          \
      Low ^= Mismatch == 1;                                                    \
      High ^= Mismatch == 2;                                                   \
      unsigned char Matched;                                                   \
      __asm__ volatile(Instruction "\n\tsetz %0"                               \
                       : "=q"(Matched), "+m"(Value), "+a"(Low), "+d"(High)     \
                       : "b"(WideAtomicExchangeLow),                           \
                         "c"(WideAtomicExchangeHigh)                           \
                       : "memory", "cc");                                      \
      const unsigned long long ReplacedLow =                                   \
          IsWide ? WideAtomicExchangeLow                                       \
                 : ((unsigned long long)(unsigned)WideAtomicExchangeHigh       \
                    << WideAtomicHalfBits) |                                   \
                       (unsigned)WideAtomicExchangeLow;                        \
      if (Matched != !Mismatch ||                                              \
          Value.Low != (Mismatch ? WideAtomicOriginalLow : ReplacedLow) ||     \
          Value.High != (Mismatch || !IsWide ? WideAtomicOriginalHigh          \
                                             : WideAtomicExchangeHigh))        \
        return 0;                                                              \
      if (Mismatch &&                                                          \
          (Low != (IsWide ? WideAtomicOriginalLow                              \
                          : (unsigned)WideAtomicOriginalLow) ||                \
           High != (IsWide ? WideAtomicOriginalHigh                            \
                           : WideAtomicOriginalLow >> WideAtomicHalfBits)))    \
        return 0;                                                              \
    }                                                                          \
    return 1;                                                                  \
  }
NEVERD_WINDOWS_WIDE_ATOMIC(Unlocked64, 0, "cmpxchg8b %1")
NEVERD_WINDOWS_WIDE_ATOMIC(Locked64, 0, "lock cmpxchg8b %1")
NEVERD_WINDOWS_WIDE_ATOMIC(Unlocked128, 1, "cmpxchg16b %1")
NEVERD_WINDOWS_WIDE_ATOMIC(Locked128, 1, "lock cmpxchg16b %1")
#undef NEVERD_WINDOWS_WIDE_ATOMIC

static int checkWideAtomic(void) {
  return checkWideAtomicUnlocked64() && checkWideAtomicLocked64() &&
         checkWideAtomicUnlocked128() && checkWideAtomicLocked128();
}
