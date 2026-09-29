//===- SignedTernaryFixture.inc - Signed comparison C oracle -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

constexpr char SignedTernaryExecutionHarness[] = R"(
int main(void) {
  static const int32_t edges[] = {
      INT32_MIN, INT32_MIN + 1, -1, 0, 1, INT32_MAX - 1, INT32_MAX};
  for (unsigned i = 0; i < 7; ++i)
    for (unsigned j = 0; j < 7; ++j)
      if (rt_ternary(edges[i], edges[j]) !=
          (edges[i] > edges[j] ? edges[i] : edges[j]))
        return 1;
  return 0;
}
)";
