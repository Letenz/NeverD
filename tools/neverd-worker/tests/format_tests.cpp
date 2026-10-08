// Classic x86 listing spellings: mnemonics, operand forms, size keywords,
// immediates, displacements and frame variables.
#include "OperandFormat.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace neverd::worker;

namespace {
void check(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

std::string hex(std::int64_t value) {
  char text[24];
  std::snprintf(text, sizeof(text), "%llX",
                static_cast<unsigned long long>(value));
  return text;
}

void expectInstruction(std::string_view mnemonic, std::string_view operands,
                       std::string_view bytes, std::string_view classic,
                       std::string_view classicOperands) {
  const auto spelled =
      classicInstruction(OperandDialect::X86, mnemonic, operands, bytes);
  check(spelled.mnemonic == classic && spelled.operands == classicOperands,
        std::string(mnemonic) + " " + std::string(operands) + " became " +
            spelled.mnemonic + " " + spelled.operands);
}

struct Format {
  std::string_view mnemonic, operands, bytes;
  std::string_view expected;
  bool wide = true;
  /// The user's number formats, by operand index.
  std::vector<NumberFormat> formats = {};
};

std::string format(const Format &input, const FrameNamer *frame = nullptr,
                   std::optional<std::int64_t> stackDepth = std::nullopt) {
  OperandFacts facts;
  facts.dialect = OperandDialect::X86;
  facts.mnemonic = input.mnemonic;
  facts.address = 0x401000;
  facts.size = 4;
  facts.wide = input.wide;
  facts.bytes = input.bytes;
  if (!input.formats.empty())
    facts.numberFormats = &input.formats;
  if (frame) {
    facts.frameRegister = "rbp";
    facts.frame = frame;
    facts.stackRegister = "rsp";
    facts.stackDepth = stackDepth;
  }
  // Every address but the rest of the first page has a name.
  return formatOperands(input.operands, facts,
                        [](std::uint64_t address, NameUse, std::string_view) {
                          return address == 0 || address >= 0x1000
                                     ? LocationName{"unk_" + hex(address)}
                                     : LocationName{};
                        })
      .text();
}

void expectFormat(const Format &input, const FrameNamer *frame = nullptr,
                  std::optional<std::int64_t> stackDepth = std::nullopt) {
  const auto text = format(input, frame, stackDepth);
  check(text == input.expected,
        std::string(input.mnemonic) + " " + std::string(input.operands) +
            " became " + text + ", not " + std::string(input.expected));
}
} // namespace

int main() {
  // Mnemonics name the flag a condition tests; hidden prefixes disappear.
  expectInstruction("je", "0x401020", "", "jz", "0x401020");
  expectInstruction("jae", "0x401020", "", "jnb", "0x401020");
  expectInstruction("ret", "", "", "retn", "");
  expectInstruction("seta", "al", "", "setnbe", "al");
  expectInstruction("cmovne", "eax, ecx", "", "cmovnz", "eax, ecx");
  expectInstruction("movabs", "rax, 0x1122334455667788", "", "mov",
                    "rax, 0x1122334455667788");
  expectInstruction("notrack jmp", "rax", "", "jmp", "rax");
  expectInstruction("lock cmpxchg", "qword ptr [rdi], rcx", "", "lock cmpxchg",
                    "qword ptr [rdi], rcx");
  // String instructions drop their implicit operands; SSE movsd keeps them.
  expectInstruction("rep movsb", "byte ptr [rdi], byte ptr [rsi]", "",
                    "rep movsb", "");
  expectInstruction("stosq", "qword ptr [rdi], rax", "", "stosq", "");
  expectInstruction("movsd", "xmm0, qword ptr [rax]", "", "movsd",
                    "xmm0, qword ptr [rax]");
  // Folded and reordered operand forms, and the two-byte no-op.
  expectInstruction("imul", "rdx, rdx, 7", "", "imul", "rdx, 7");
  expectInstruction("imul", "rdx, r13, 7", "", "imul", "rdx, r13, 7");
  expectInstruction("xchg", "rbx, rdi", "", "xchg", "rdi, rbx");
  expectInstruction("xchg", "qword ptr [rax], rdi", "", "xchg",
                    "qword ptr [rax], rdi");
  expectInstruction("nop", "", "6690", "xchg", "ax, ax");

  // A register of the same size states the size; vectors keep theirs.
  expectFormat({"mov", "qword ptr [rcx + 8], rdx", "", "[rcx+8], rdx"});
  expectFormat({"mov", "eax, dword ptr [rdi + 0x40]", "", "eax, [rdi+40h]"});
  expectFormat({"movzx", "eax, byte ptr [rdi]", "", "eax, byte ptr [rdi]"});
  expectFormat({"cmp", "byte ptr [rdi + 0xb], 0", "", "byte ptr [rdi+0Bh], 0"});
  expectFormat({"movups", "xmmword ptr [rdi + rax], xmm0", "",
                "xmmword ptr [rdi+rax], xmm0"});
  expectFormat({"call", "qword ptr [rax + 0x58]", "", "qword ptr [rax+58h]"});
  // Negative immediates are unsigned in the operation width, except where a
  // signed byte stays signed.
  expectFormat({"add", "r12, -0x180", "", "r12, 0FFFFFFFFFFFFFE80h"});
  expectFormat({"cmp", "eax, -1", "", "eax, 0FFFFFFFFh"});
  expectFormat({"cmp", "byte ptr [rax], -1", "", "byte ptr [rax], 0FFh"});
  expectFormat({"push", "-0x18", "", "0FFFFFFFFFFFFFFE8h"});
  expectFormat({"push", "-1", "", "0FFFFFFFFh", false});
  expectFormat({"imul", "rax, -0x38", "", "rax, -38h"});
  // The user's number formats: a base, a changed sign, inverted bits.
  const auto user = [](NumberBase base, bool negate = false,
                       bool invert = false) {
    return std::vector<NumberFormat>{{}, {base, negate, invert}};
  };
  expectFormat(
      {"mov", "eax, 0x1a", "", "eax, 26", true, user(NumberBase::Decimal)});
  expectFormat({"mov", "eax, 26", "", "eax, 1Ah", true, user(NumberBase::Hex)});
  expectFormat(
      {"mov", "eax, 0x1a", "", "eax, 11010b", true, user(NumberBase::Binary)});
  expectFormat(
      {"mov", "eax, 0x41", "", "eax, 'A'", true, user(NumberBase::Char)});
  expectFormat({"cmp", "eax, 0x41424344", "", "eax, 'ABCD'", true,
                user(NumberBase::Char)});
  expectFormat(
      {"cmp", "eax, -1", "", "eax, -1", true, user(NumberBase::Number, true)});
  expectFormat(
      {"cmp", "eax, -1", "", "eax, -1", true, user(NumberBase::Decimal, true)});
  expectFormat({"and", "eax, 0xfffffff0", "", "eax, ~0Fh", true,
                user(NumberBase::Number, false, true)});
  expectFormat({"mov", "eax, 0x401000", "", "eax, offset unk_401000", true,
                user(NumberBase::Offset)});
  // A format that cannot show the number leaves the listing's spelling: a
  // control character, an offset nothing names.
  expectFormat({"mov", "eax, 7", "", "eax, 7", true, user(NumberBase::Char)});
  expectFormat(
      {"mov", "eax, 0x10", "", "eax, 10h", true, user(NumberBase::Offset)});
  // Only the operand the user formats changes.
  expectFormat({"mov",
                "eax, 0x1a",
                "",
                "eax, 1Ah",
                true,
                {{NumberBase::Decimal, false, false}}});
  // Formats change plain numbers, signed or not, and only x86 operands.
  check(formattableOperands("rsp, 8", OperandDialect::X86) ==
            std::vector<bool>{false, true},
        "a register and a number");
  check(formattableOperands("dword ptr [rbp - 4], -5", OperandDialect::X86) ==
            std::vector<bool>{false, true},
        "a displacement is no plain number");
  check(formattableOperands("w0, #0x20", OperandDialect::AArch64) ==
            std::vector<bool>{false, false},
        "AArch64 operands show no formats");
  expectFormat({"imul", "rsi, qword ptr [rax], -0xc8", "",
                "rsi, [rax], 0FFFFFFFFFFFFFF38h"});
  // Thread storage offsets, missing base registers and zero displacements.
  expectFormat({"mov", "rax, qword ptr fs:[0x28]", "", "rax, fs:28h"});
  expectFormat({"lea", "rsi, [rsi*2 + 1]", "", "rsi, ds:1[rsi*2]"});
  expectFormat({"lea", "rax, [rax*4]", "", "rax, ds:0[rax*4]"});
  expectFormat({"mov", "rax, qword ptr [rdx*8 + 0x2000]", "",
                "rax, ds:unk_2000[rdx*8]"});
  expectFormat({"mov", "rax, qword ptr [r13]", "", "rax, [r13+0]"});
  expectFormat({"mov", "rax, qword ptr [rbx + rcx]", "", "rax, [rbx+rcx]"});
  expectFormat(
      {"movzx", "edx, byte ptr [r13 + rbx]", "", "edx, byte ptr [r13+rbx+0]"});
  expectFormat({"nop", "dword ptr [rax]", "0f1f4000", "dword ptr [rax+00h]"});
  expectFormat({"nop", "word ptr cs:[rax + rax]", "662e0f1f840000000000",
                "word ptr cs:[rax+rax+00000000h]"});
  expectFormat({"nop", "dword ptr [rax]", "0f1f00", "dword ptr [rax]"});

  // Frame variables: sized by their widest access; an access inside a
  // larger variable names it with an offset and keeps its own size.
  const std::map<std::int64_t, unsigned> variables = {
      {-0x88, 1}, {-0x78, 16}, {-0x30, 8}, {0x10, 8}};
  const FrameNamer frame =
      [&](std::int64_t offset) -> std::optional<FrameSlot> {
    auto next = variables.upper_bound(offset);
    if (next == variables.begin())
      return std::nullopt;
    const auto &[start, size] = *std::prev(next);
    if (offset >= start + static_cast<std::int64_t>(size))
      return std::nullopt;
    const std::string name =
        start < 0 ? "var_" + hex(-start) : "arg_" + hex(start - 0x10);
    return FrameSlot{name, offset - start, size};
  };
  expectFormat({"mov", "rdi, qword ptr [rbp - 0x30]", "", "rdi, [rbp+var_30]"},
               &frame);
  expectFormat({"lea", "rdx, [rbp - 0x88]", "", "rdx, [rbp+var_88]"}, &frame);
  expectFormat(
      {"mov", "rax, qword ptr [rbp - 0x88]", "", "rax, qword ptr [rbp+var_88]"},
      &frame);
  expectFormat({"mov", "qword ptr [rbp - 0x70], rax", "",
                "qword ptr [rbp+var_78+8], rax"},
               &frame);
  expectFormat({"mov", "qword ptr [rbp - 0x30], 0", "", "[rbp+var_30], 0"},
               &frame);
  expectFormat({"and", "dword ptr [rbp + rax*4 - 0x88], 0", "",
                "dword ptr [rbp+rax*4+var_88], 0"},
               &frame);
  expectFormat(
      {"mov", "rcx, qword ptr [rbp + rax - 0x30]", "", "rcx, [rbp+rax+var_30]"},
      &frame);
  expectFormat({"mov", "rax, qword ptr [rbp + 0x10]", "", "rax, [rbp+arg_0]"},
               &frame);
  // Offsets outside every variable stay numeric.
  expectFormat({"mov", "rax, qword ptr [rbp - 8]", "", "rax, [rbp-8]"}, &frame);
  // Through the stack pointer, with its distance down from the frame's base.
  expectFormat(
      {"mov", "qword ptr [rsp + 0x18], rax", "", "[rsp+48h+var_30], rax"},
      &frame, 0x48);
  expectFormat({"lea", "rdi, [rsp - 0x40]", "", "rdi, [rsp+48h+var_88]"},
               &frame, 0x48);
  expectFormat({"mov", "rcx, qword ptr [rsp + rax*8 + 0x18]", "",
                "rcx, [rsp+rax*8+48h+var_30]"},
               &frame, 0x48);
  // At the frame's base the distance is left out.
  expectFormat({"mov", "rax, qword ptr [rsp + 0x10]", "", "rax, [rsp+arg_0]"},
               &frame, 0);
  // Without a known distance the operand stays numeric.
  expectFormat({"mov", "qword ptr [rsp + 0x18], rax", "", "[rsp+18h], rax"},
               &frame);

  const auto access = x86FrameAccess("dword ptr [rbp - 0x34], edi", "rbp");
  check(access && access->offset == -0x34 && access->size == 4,
        "A dword frame store was not recognized");
  const auto address = x86FrameAccess("rdx, [rbp - 0x88]", "rbp");
  check(address && address->offset == -0x88 && address->size == 0,
        "A frame address was given a size");
  check(!x86FrameAccess("rax, qword ptr [rsp + 8]", "rbp"),
        "A stack pointer access was taken for a frame access");
  check(x86SizeKeyword(16) == "xmmword" && x86SizeKeyword(3).empty(),
        "Size keywords do not follow the size table");
  return 0;
}
