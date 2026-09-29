//===- ARMUnmarkedModeFixtures.inc - ARM and Thumb cases ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

struct UnmarkedModeCase {
  const char *Name;
  InstructionMode Mode;
  const char *Assembly;
  const char *Hint;
};

constexpr UnmarkedModeCase UnmarkedModeCases[] = {
    {"ARM", InstructionMode::ARM, R"(
.syntax unified
.text
.arm
.globl generic_add
.type generic_add,%function
generic_add:
  eor r2, r0, r1
  and r0, r0, r1
  add r0, r2, r0, lsl #1
  bx lr
.size generic_add, .-generic_add
)",
     "--arm-function-mode=0x0:arm"},
    {"Thumb", InstructionMode::Thumb, R"(
.syntax unified
.text
.thumb
.globl generic_add
.type generic_add,%function
.thumb_func
generic_add:
  eors r2, r0, r1
  ands r0, r0, r1
  lsls r0, r0, #1
  adds r0, r0, r2
  bx lr
.size generic_add, .-generic_add
)",
     "--arm-function-mode=0x0:thumb"},
};

constexpr char UnmarkedModeExecutionHarness[] = R"(
static int check(uint32_t x, uint32_t y) {
  return (uint32_t)sub_0(x, y) == x + y;
}
int main(void) {
  static const uint32_t edges[] = {
      0, 1, 2, 0x7fffffffU, 0x80000000U, 0xffffffffU};
  for (unsigned i = 0; i < 6; ++i)
    for (unsigned j = 0; j < 6; ++j)
      if (!check(edges[i], edges[j]))
        return 1;
  uint64_t state = UINT64_C(0xa0f6e732d41c895b);
  for (unsigned i = 0; i < 1024; ++i) {
    state = state * UINT64_C(6364136223846793005) + 1;
    uint32_t x = (uint32_t)(state >> 32);
    state = state * UINT64_C(6364136223846793005) + 1;
    if (!check(x, (uint32_t)(state >> 32)))
      return 1;
  }
  return 0;
}
)";
