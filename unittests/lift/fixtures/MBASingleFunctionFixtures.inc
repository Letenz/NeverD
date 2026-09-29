//===- MBASingleFunctionFixtures.inc - Single-function C oracles ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

constexpr char SubtractionExecutionHarness[] = R"(
int main(void) {
  static const uint32_t edges[] = {
      0, 1, 2, 0x7fffffffU, 0x80000000U, 0xffffffffU};
  for (unsigned i = 0; i < 6; ++i)
    for (unsigned j = 0; j < 6; ++j)
      if ((uint32_t)mba_sub32(edges[i], edges[j]) != edges[i] - edges[j])
        return 1;
  return 0;
}
)";

constexpr char NestedAdditionExecutionHarness[] = R"(
int main(void) {
  static const uint32_t edges[] = {
      0, 1, 2, 0x7fffffffU, 0x80000000U, 0xffffffffU};
  for (unsigned i = 0; i < 6; ++i)
    for (unsigned j = 0; j < 6; ++j)
      if ((uint32_t)mba_nested_add(edges[i], edges[j]) != edges[i] + edges[j])
        return 1;
  uint64_t state = UINT64_C(0xf346ab70d21985ce);
  for (unsigned i = 0; i < 1024; ++i) {
    state = state * UINT64_C(6364136223846793005) + 1;
    uint32_t x = (uint32_t)(state >> 32);
    state = state * UINT64_C(6364136223846793005) + 1;
    uint32_t y = (uint32_t)(state >> 32);
    if ((uint32_t)mba_nested_add(x, y) != x + y)
      return 1;
  }
  return 0;
}
)";
