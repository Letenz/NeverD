"""Run with python3 -m unittest discover -s tools/neverd-mcp/tests -v."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from reading import reading_view


FUNCTION = """int32_t f(int64_t arg0) {
    _Alignas(16) struct frame_1000 {
        uint8_t gap0[8];
        int64_t var_m10;
        uint32_t var_m8;
    } stack_storage;
    int64_t t1_2;
    int64_t t1_3;
    uint8_t t4_5;
    stack_storage.var_m10 = arg0;
    t1_2 = *(int32_t *)(uintptr_t)(&g_A000_bytes[16]);
    *(int32_t *)(uintptr_t)(&g_A000_bytes[16]) = (int32_t)t1_2 + 1;
    puts("int64_t stack_storage.var_m10 /* not code */");
    return (int32_t)(uintptr_t)&stack_storage.var_m8;
}
"""


class ReadingViewTests(unittest.TestCase):
    def test_abbreviations_are_listed_and_reversible(self):
        view, legend = reading_view(FUNCTION)
        self.assertEqual(view.splitlines()[0], "i32 f(i64 arg0) {")
        self.assertIn("_Alignas(16) struct frame_1000 { u8 gap0[8]; i64 var_m10; u32 var_m8; } stack_storage;", view)
        self.assertIn("    i64 t1_2, t1_3;\n    u8 t4_5;\n", view)
        self.assertIn("    var_m10 = arg0;\n", view)
        self.assertIn("    t1_2 = g_A010;\n    g_A010 = (i32)t1_2 + 1;\n", view)
        self.assertIn("return (i32)(uptr)&var_m8;", view)
        self.assertEqual(legend, "iN/uN: intN_t/uintN_t, uptr: uintptr_t; var_mN/gapN: stack_storage members; "
                                 "globals: i32 g_A010")

    def test_literals_and_comments_keep_their_text(self):
        view, _ = reading_view(FUNCTION)
        self.assertIn('puts("int64_t stack_storage.var_m10 /* not code */");', view)
        commented = "/* int64_t stays */\nint64_t x;\n"
        self.assertEqual(reading_view(commented)[0], "/* int64_t stays */\ni64 x;\n")

    def test_an_address_read_with_two_types_keeps_its_casts(self):
        text = ("void f(void) {\n    t = *(int32_t *)(uintptr_t)(&g_A000_bytes[4]);\n"
                "    u = *(uint32_t *)(uintptr_t)(&g_A000_bytes[4]);\n}\n")
        view, legend = reading_view(text)
        self.assertIn("*(i32 *)(uptr)(&g_A000_bytes[4])", view)
        self.assertIn("*(u32 *)(uptr)(&g_A000_bytes[4])", view)
        self.assertNotIn("globals", legend)

    def test_names_the_text_already_uses_are_not_reused(self):
        text = ("void f(int64_t i64) {\n    int32_t g_A004 = 0;\n    var_m8 = 1;\n"
                "    stack_storage.var_m8 = *(int32_t *)(uintptr_t)(&g_A000_bytes[4]);\n}\n")
        view, legend = reading_view(text)
        self.assertIn("void f(int64_t i64)", view)
        self.assertIn("stack_storage.var_m8", view)
        self.assertIn("&g_A000_bytes[4]", view)
        self.assertNotIn("var_mN", legend)

    def test_text_without_abbreviations_has_no_legend(self):
        text = "int main(void) {\n    return 0;\n}\n"
        self.assertEqual(reading_view(text), (text, ""))


if __name__ == "__main__":
    unittest.main()
