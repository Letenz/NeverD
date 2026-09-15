//===- X64_LegacyBitRTTests.cpp - x86 legacy/bit-manip roundtrip *- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests x86 legacy instructions and TBM bit manipulation through full lift
// pipeline.  Covers instructions from X86LiftSIMDLegacy.cpp that are
// underrepresented in existing tests.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64LegacyBitRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64LegacyBitRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kX64LegacyBit = {

  // ===== SALC (set AL from carry) — undocumented but lifted =====
  // Can't easily test via C, skip SALC.

  // ===== ENDBR64 — CET no-op, should roundtrip as identity =====
  {"endbr64_nop",
   "long endbr64_nop(long a) {\n"
   "  return a + 1;\n"
   "}\n",
   {41}, "LegacyBit", 0, "-fcf-protection=branch"},

  // ===== Bit manipulation via C expressions (compiler picks instructions) =====
  {"bit_isolate_lowest_set",
   "long bit_isolate_lowest_set(long a) {\n"
   "  return a & (-a);\n"
   "}\n",
   {0x12340000ULL}, "LegacyBit", 1},

  {"bit_reset_lowest_set",
   "long bit_reset_lowest_set(long a) {\n"
   "  return a & (a - 1);\n"
   "}\n",
   {0x12340000ULL}, "LegacyBit", 1},

  {"bit_set_lowest_clear",
   "long bit_set_lowest_clear(long a) {\n"
   "  return a | (a + 1);\n"
   "}\n",
   {0x1234FFFFULL}, "LegacyBit", 1},

  {"bit_mask_up_to_lowest_set",
   "long bit_mask_up_to_lowest_set(long a) {\n"
   "  return a ^ (a - 1);\n"
   "}\n",
   {0x00100000ULL}, "LegacyBit", 1},

  {"bit_fill_from_lowest_clear",
   "long bit_fill_from_lowest_clear(long a) {\n"
   "  return a | ~(a + 1);\n"
   "}\n",
   {0x000FFFFFULL}, "LegacyBit", 1},

  // ===== Byte-swap via __builtin_bswap =====
  {"bswap16",
   "long bswap16(long a) {\n"
   "  unsigned short v = (unsigned short)a;\n"
   "  return __builtin_bswap16(v);\n"
   "}\n",
   {0x1234ULL}, "LegacyBit", 1},

  {"bswap32",
   "long bswap32(long a) {\n"
   "  unsigned int v = (unsigned int)a;\n"
   "  return __builtin_bswap32(v);\n"
   "}\n",
   {0x12345678ULL}, "LegacyBit", 1},

  {"bswap64",
   "long bswap64(long a) {\n"
   "  return __builtin_bswap64(a);\n"
   "}\n",
   {0x0123456789ABCDEFULL}, "LegacyBit", 1},

  // ===== Rotate via builtins (exercises ROL/ROR) =====
  {"rotl32",
   "long rotl32(long a, long b) {\n"
   "  unsigned int v = (unsigned int)a;\n"
   "  unsigned int n = (unsigned int)b & 31;\n"
   "  return (v << n) | (v >> (32 - n));\n"
   "}\n",
   {0xDEADBEEFULL, 12}, "LegacyBit", 1},

  {"rotr32",
   "long rotr32(long a, long b) {\n"
   "  unsigned int v = (unsigned int)a;\n"
   "  unsigned int n = (unsigned int)b & 31;\n"
   "  return (v >> n) | (v << (32 - n));\n"
   "}\n",
   {0xDEADBEEFULL, 12}, "LegacyBit", 1},

  {"rotl64",
   "long rotl64(long a, long b) {\n"
   "  unsigned long v = (unsigned long)a;\n"
   "  unsigned long n = (unsigned long)b & 63;\n"
   "  return (v << n) | (v >> (64 - n));\n"
   "}\n",
   {0xDEADBEEF12345678ULL, 20}, "LegacyBit", 1},

  // ===== Conditional select (CMOV patterns) =====
  {"cmov_signed_max",
   "long cmov_signed_max(long a, long b) {\n"
   "  return a > b ? a : b;\n"
   "}\n",
   {42, 99}, "LegacyBit", 1},

  {"cmov_signed_min",
   "long cmov_signed_min(long a, long b) {\n"
   "  return a < b ? a : b;\n"
   "}\n",
   {42, 99}, "LegacyBit", 1},

  {"cmov_unsigned_max",
   "long cmov_unsigned_max(long a, long b) {\n"
   "  unsigned long ua = (unsigned long)a, ub = (unsigned long)b;\n"
   "  return ua > ub ? ua : ub;\n"
   "}\n",
   {42, 0xFFFFFFFF00000000ULL}, "LegacyBit", 1},

  // ===== ANDN (BMI1) =====
  {"andn64",
   "long andn64(long a, long b) {\n"
   "  return ~a & b;\n"
   "}\n",
   {0xFF00FF00FF00FF00ULL, 0x0F0F0F0F0F0F0F0FULL}, "LegacyBit", 1, "-mbmi"},

  // ===== PDEP/PEXT (BMI2) =====
  {"pdep_basic",
   "long pdep_basic(long a, long b) {\n"
   "  return __builtin_ia32_pdep_di(a, b);\n"
   "}\n",
   {0x1234ULL, 0xFF00FF00ULL}, "LegacyBit", 1, "-mbmi2"},

  {"pext_basic",
   "long pext_basic(long a, long b) {\n"
   "  return __builtin_ia32_pext_di(a, b);\n"
   "}\n",
   {0x12345678ABCDEF00ULL, 0xFF00FF00FF00FF00ULL}, "LegacyBit", 1, "-mbmi2"},

  // ===== MULX (BMI2) — unsigned multiply high =====
  {"mulx64",
   "long mulx64(long a, long b) {\n"
   "  unsigned __int128 r = (unsigned __int128)(unsigned long)a * (unsigned long)b;\n"
   "  return (long)(r >> 64);\n"
   "}\n",
   {0xDEADBEEFULL, 0xCAFEBABEULL}, "LegacyBit", 1},

  // ===== BT/BTS/BTR/BTC via bit array operations =====
  {"bt_basic",
   "long bt_basic(long a, long b) {\n"
   "  return (a >> (b & 63)) & 1;\n"
   "}\n",
   {0xFF00FF00FF00FF00ULL, 8}, "LegacyBit", 1},

  // clang -O2 `x % 7` is a 64-bit MUL high-half + `lea (,%rdx,8); sub`.
  // The same recipe is the selector for x64swm_m7's jump table.
  {"urem7_magic",
   "long urem7_magic(long a) {\n"
   "  return (long)((unsigned)a % 7u);\n"
   "}\n",
   {0x1234ULL}, "LegacyBit", 2},

  // clang -O2 `x % 54` is mulq of a 64-bit widen magic, then `imul $54, %edx`.
  // Tokenize uses this as the rodata GEP index.
  {"urem54_magic",
   "long urem54_magic(long a) {\n"
   "  return (long)((unsigned)a % 54u);\n"
   "}\n",
   {0x1234ULL}, "LegacyBit", 2},

  // Tokenize -O2 classifies rodata bytes by `shr %cl` of two masks, 32-bit
  // AND, bit 0.  Positions 19-21 of 0x77BBFF03BE1FF & 0x7884400FFC1E00.
  {"shr_and_bit19",
   "long shr_and_bit19(long a){\n"
   "  unsigned out;\n"
   "  __asm__ volatile(\n"
   "    \"movabs $0x77BBFF03BE1FF, %%rax\\n\\t\"\n"
   "    \"movabs $0x7884400FFC1E00, %%rdx\\n\\t\"\n"
   "    \"movl %1, %%ecx\\n\\t\"\n"
   "    \"shrq %%cl, %%rax\\n\\t\"\n"
   "    \"shrq %%cl, %%rdx\\n\\t\"\n"
   "    \"andl %%eax, %%edx\\n\\t\"\n"
   "    \"andl $1, %%edx\\n\\t\"\n"
   "    \"movl %%edx, %0\\n\\t\"\n"
   "    : \"=m\"(out) : \"r\"((int)a) : \"rax\", \"rdx\", \"rcx\");\n"
   "  return (long)out;\n}\n",
   {19ULL}, "LegacyBit", 0},

  {"bts_basic",
   "long bts_basic(long a, long b) {\n"
   "  return a | (1UL << (b & 63));\n"
   "}\n",
   {0, 42}, "LegacyBit", 1},

  {"btr_basic",
   "long btr_basic(long a, long b) {\n"
   "  return a & ~(1UL << (b & 63));\n"
   "}\n",
   {0xFFFFFFFFFFFFFFFFULL, 42}, "LegacyBit", 1},

  {"btc_basic",
   "long btc_basic(long a, long b) {\n"
   "  return a ^ (1UL << (b & 63));\n"
   "}\n",
   {0xFFFFFFFFFFFFFFFFULL, 42}, "LegacyBit", 1},

  // ===== BZHI (BMI2) — zero high bits from index =====
  {"bzhi_basic",
   "long bzhi_basic(long a, long b) {\n"
   "  unsigned long idx = (unsigned long)b & 63;\n"
   "  if (idx >= 64) return a;\n"
   "  return a & ((1UL << idx) - 1);\n"
   "}\n",
   {0xFFFFFFFFFFFFFFFFULL, 32}, "LegacyBit", 1},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(LegacyBit, X64LegacyBitRT,
                         ::testing::ValuesIn(kX64LegacyBit), rtTCName);
