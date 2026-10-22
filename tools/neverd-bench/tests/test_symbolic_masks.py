"""Independent checks for the benchmark oracle's typed word semantics."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "symbolic_masks", Path(__file__).resolve().parents[1] / "benchmark_symbolic_masks.py")
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


class WordOracleTests(unittest.TestCase):
    def equivalent(self, before, after, width=32):
        bench.check_semantics([before], [{"output": after}], width)

    def test_narrow_arithmetic_and_default_width_promotion(self):
        self.equivalent("(x+1)&255", "zext(extract(x,0,8)+1#8,32)")
        self.equivalent("(x&255)+1", "extract(x,0,8)+1")
        self.equivalent("(x-1)&255", "zext(-1#8+extract(x,0,8),32)")

    def test_signed_extension_and_extract(self):
        self.equivalent("(x&255)-2*(x&128)", "sext(extract(x,0,8),32)")
        self.equivalent("255", "zext(extract(0xFF00#16,8,8),32)")
        self.equivalent("x&255", "zext(trunc(x,8),32)")

    def test_lookup_words_and_overshifts(self):
        self.equivalent("x&1", "extract(0xAAAAAAAAAAAAAAAA#64 >> zext(x&63,64),0,8)&1", 8)
        self.equivalent("0", "zext(extract(x,0,8)>>8#8,32)")
        self.equivalent("0", "zext(extract(x,0,8)<<8#8,32)")

    def test_rejects_wrong_result_and_unsupported_operations(self):
        with self.assertRaises(AssertionError):
            self.equivalent("(x&255)+1", "zext(extract(x,0,8)+1#8,32)")
        with self.assertRaises(ValueError):
            self.equivalent("x", "unknown(x)")


if __name__ == "__main__":
    unittest.main()
