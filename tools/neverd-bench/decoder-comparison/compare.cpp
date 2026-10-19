//===- compare.cpp - Compare x86 decoder API workloads -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare x86 decoder API workloads. Benchmark-only; not linked into NeverD.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <Zydis/Zydis.h>
#include <capstone/capstone.h>
extern "C" {
#include <xed/xed-interface.h>
}

struct Result {
  uint64_t instructions = 0, rejected = 0;
  uint64_t boundaries = 0xcbf29ce484222325ULL, values = 0;
};
extern "C" void *iced_create(unsigned bits, unsigned mode);
extern "C" void iced_run(void *, const uint8_t *, size_t, unsigned, Result *);
extern "C" void iced_destroy(void *);

static double process_cpu_ns() {
#ifdef _WIN32
  FILETIME created, exited, kernel, user;
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
    throw std::runtime_error("GetProcessTimes");
  auto ticks = [](FILETIME t) {
    return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
  };
  return double(ticks(kernel) + ticks(user)) * 100;
#else
  const auto ticks = std::clock();
  if (ticks == std::clock_t(-1))
    throw std::runtime_error("clock");
  return double(ticks) * 1e9 / CLOCKS_PER_SEC;
#endif
}

static void record(Result &r, size_t offset, size_t length, bool ok) {
  r.boundaries =
      (r.boundaries ^ ((offset << 5) | (length << 1) | ok)) * 1099511628211ULL;
  if (ok)
    ++r.instructions;
  else
    ++r.rejected;
}
static void text_value(Result &r, const char *s) {
  size_t n = 0;
  while (s[n])
    ++n;
  r.values += n + (n ? static_cast<unsigned char>(s[n - 1]) : 0);
}

static Result capstone_run(const uint8_t *bytes, size_t count, unsigned bits,
                           unsigned mode, unsigned repeats) {
  csh handle = 0;
  cs_mode m = bits == 64 ? CS_MODE_64 : bits == 32 ? CS_MODE_32 : CS_MODE_16;
  if (cs_open(CS_ARCH_X86, m, &handle) != CS_ERR_OK)
    throw std::runtime_error("cs_open");
  if (cs_option(handle, CS_OPT_DETAIL, mode == 2 ? CS_OPT_ON : CS_OPT_OFF) !=
          CS_ERR_OK ||
      cs_option(handle, CS_OPT_TEXT, mode ? CS_OPT_ON : CS_OPT_OFF) !=
          CS_ERR_OK)
    throw std::runtime_error("cs_option");
  cs_insn *insn = cs_malloc(handle);
  if (!insn)
    throw std::runtime_error("cs_malloc");
  Result result;
  for (unsigned repeat = 0; repeat < repeats; ++repeat) {
    const uint8_t *cursor = bytes;
    size_t left = count;
    uint64_t ip = 0x100000;
    while (left) {
      size_t offset = cursor - bytes;
      bool ok = cs_disasm_iter(handle, &cursor, &left, &ip, insn);
      record(result, offset, ok ? insn->size : 1, ok);
      if (!ok) {
        ++cursor;
        --left;
        ++ip;
        continue;
      }
      if (!insn->size)
        throw std::runtime_error("zero instruction length");
      result.values += insn->id;
      if (mode) {
        text_value(result, insn->mnemonic);
        text_value(result, insn->op_str);
      }
      if (mode == 2) {
        cs_regs read, written;
        uint8_t nr = 0, nw = 0;
        if (cs_regs_access(handle, insn, read, &nr, written, &nw) != CS_ERR_OK)
          throw std::runtime_error("cs_regs_access");
        for (unsigned i = 0; i < nr; ++i)
          result.values += read[i];
        for (unsigned i = 0; i < nw; ++i)
          result.values += written[i];
        const auto &x = insn->detail->x86;
        result.values += x.eflags + insn->detail->groups_count;
        for (unsigned i = 0; i < x.op_count; ++i) {
          const auto &o = x.operands[i];
          result.values += o.type + o.size + o.access;
          if (o.type == X86_OP_REG)
            result.values += o.reg;
          if (o.type == X86_OP_IMM)
            result.values += o.imm;
          if (o.type == X86_OP_MEM)
            result.values += uint64_t(o.mem.segment) + o.mem.base +
                             o.mem.index + o.mem.scale + o.mem.disp;
        }
      }
    }
  }
  cs_free(insn, 1);
  cs_close(&handle);
  return result;
}

static Result zydis_run(const uint8_t *bytes, size_t count, unsigned bits,
                        unsigned mode, unsigned repeats) {
  ZydisDecoder decoder;
  ZydisMachineMode machine = bits == 64   ? ZYDIS_MACHINE_MODE_LONG_64
                             : bits == 32 ? ZYDIS_MACHINE_MODE_LEGACY_32
                                          : ZYDIS_MACHINE_MODE_LEGACY_16;
  ZydisStackWidth stack = bits == 64   ? ZYDIS_STACK_WIDTH_64
                          : bits == 32 ? ZYDIS_STACK_WIDTH_32
                                       : ZYDIS_STACK_WIDTH_16;
  if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, machine, stack)))
    throw std::runtime_error("ZydisDecoderInit");
  ZydisFormatter formatter;
  if (!ZYAN_SUCCESS(
          ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL)))
    throw std::runtime_error("ZydisFormatterInit");
  Result result;
  for (unsigned repeat = 0; repeat < repeats; ++repeat) {
    for (size_t offset = 0; offset < count;) {
      ZydisDecodedInstruction insn;
      ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
      ZyanStatus status =
          mode ? ZydisDecoderDecodeFull(&decoder, bytes + offset,
                                        count - offset, &insn, operands)
               : ZydisDecoderDecodeInstruction(
                     &decoder, nullptr, bytes + offset, count - offset, &insn);
      bool ok = ZYAN_SUCCESS(status);
      record(result, offset, ok ? insn.length : 1, ok);
      if (!ok) {
        ++offset;
        continue;
      }
      if (!insn.length)
        throw std::runtime_error("zero instruction length");
      result.values += insn.mnemonic;
      if (mode) {
        char text[512];
        if (!ZYAN_SUCCESS(ZydisFormatterFormatInstruction(
                &formatter, &insn, operands, insn.operand_count_visible, text,
                sizeof(text), 0x100000 + offset, nullptr)))
          throw std::runtime_error("ZydisFormatterFormatInstruction");
        text_value(result, text);
      }
      if (mode == 2) {
        result.values += insn.meta.category;
        if (insn.cpu_flags)
          result.values += insn.cpu_flags->tested + insn.cpu_flags->modified +
                           insn.cpu_flags->set_0 + insn.cpu_flags->set_1 +
                           insn.cpu_flags->undefined;
        for (unsigned i = 0; i < insn.operand_count; ++i) {
          const auto &o = operands[i];
          result.values += o.type + o.size + o.actions;
          if (o.type == ZYDIS_OPERAND_TYPE_REGISTER)
            result.values += o.reg.value;
          if (o.type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
            result.values += o.imm.value.u;
          if (o.type == ZYDIS_OPERAND_TYPE_MEMORY)
            result.values += uint64_t(o.mem.segment) + o.mem.base +
                             o.mem.index + o.mem.scale + o.mem.disp.value;
        }
      }
      offset += insn.length;
    }
  }
  return result;
}

static Result xed_run(const uint8_t *bytes, size_t count, unsigned bits,
                      unsigned mode, unsigned repeats) {
  xed_state_t state;
  xed_state_zero(&state);
  xed_state_init2(&state,
                  bits == 64   ? XED_MACHINE_MODE_LONG_64
                  : bits == 32 ? XED_MACHINE_MODE_LEGACY_32
                               : XED_MACHINE_MODE_LEGACY_16,
                  bits == 64   ? XED_ADDRESS_WIDTH_64b
                  : bits == 32 ? XED_ADDRESS_WIDTH_32b
                               : XED_ADDRESS_WIDTH_16b);
  Result result;
  for (unsigned repeat = 0; repeat < repeats; ++repeat) {
    for (size_t offset = 0; offset < count;) {
      xed_decoded_inst_t insn;
      xed_decoded_inst_zero_set_mode(&insn, &state);
      bool ok =
          xed_decode(&insn, bytes + offset,
                     std::min<size_t>(15, count - offset)) == XED_ERROR_NONE;
      auto length = ok ? xed_decoded_inst_get_length(&insn) : 1;
      record(result, offset, length, ok);
      if (!ok) {
        ++offset;
        continue;
      }
      if (!length)
        throw std::runtime_error("zero instruction length");
      result.values += xed_decoded_inst_get_iclass(&insn);
      if (mode) {
        char text[512];
        if (!xed_format_context(XED_SYNTAX_INTEL, &insn, text, sizeof(text),
                                0x100000 + offset, nullptr, nullptr))
          throw std::runtime_error("xed_format_context");
        text_value(result, text);
      }
      if (mode == 2) {
        const auto *pattern = xed_decoded_inst_inst(&insn);
        result.values += xed_decoded_inst_get_category(&insn);
        for (unsigned i = 0; i < xed_inst_noperands(pattern); ++i) {
          const auto *o = xed_inst_operand(pattern, i);
          auto name = xed_operand_name(o);
          result.values += name + xed_operand_rw(o) +
                           xed_decoded_inst_operand_length_bits(&insn, i);
          if (xed_operand_is_register(name))
            result.values += xed_decoded_inst_get_reg(&insn, name);
        }
        for (unsigned i = 0;
             i < xed_decoded_inst_number_of_memory_operands(&insn); ++i)
          result.values += uint64_t(xed_decoded_inst_get_seg_reg(&insn, i)) +
                           xed_decoded_inst_get_base_reg(&insn, i) +
                           xed_decoded_inst_get_index_reg(&insn, i) +
                           xed_decoded_inst_get_scale(&insn, i) +
                           xed_decoded_inst_get_memory_displacement(&insn, i) +
                           xed_decoded_inst_get_memory_operand_length(&insn, i);
        result.values += xed_decoded_inst_get_unsigned_immediate(&insn) +
                         xed_decoded_inst_get_branch_displacement(&insn);
        if (const auto *flags = xed_decoded_inst_get_rflags_info(&insn))
          result.values += xed_simple_flag_get_read_flag_set(flags)->flat +
                           xed_simple_flag_get_written_flag_set(flags)->flat +
                           xed_simple_flag_get_undefined_flag_set(flags)->flat;
      }
      offset += length;
    }
  }
  return result;
}

int main(int argc, char **argv) {
  try {
    if (argc != 6)
      throw std::runtime_error("usage: compare capstone|zydis|xed|iced "
                               "scan|format|detail BITS REPEATS INPUT");
    std::string library = argv[1], mode_name = argv[2];
    unsigned mode = mode_name == "scan"     ? 0
                    : mode_name == "format" ? 1
                    : mode_name == "detail" ? 2
                                            : 99;
    unsigned bits = std::stoul(argv[3]), repeats = std::stoul(argv[4]);
    if (mode > 2 || (bits != 16 && bits != 32 && bits != 64) || !repeats)
      throw std::runtime_error("invalid benchmark option");
    std::ifstream file(argv[5], std::ios::binary);
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file),
                               std::istreambuf_iterator<char>()};
    if (bytes.empty())
      throw std::runtime_error("empty or unreadable input");
    xed_tables_init();
    void *iced = iced_create(bits, mode);
    auto run = [&](unsigned n) {
      if (library == "capstone")
        return capstone_run(bytes.data(), bytes.size(), bits, mode, n);
      if (library == "zydis")
        return zydis_run(bytes.data(), bytes.size(), bits, mode, n);
      if (library == "xed")
        return xed_run(bytes.data(), bytes.size(), bits, mode, n);
      if (library == "iced") {
        Result r;
        iced_run(iced, bytes.data(), bytes.size(), n, &r);
        return r;
      }
      throw std::runtime_error("unknown library");
    };
    Result warm = run(1);
    auto cpu_start = process_cpu_ns();
    auto start = std::chrono::steady_clock::now();
    Result result = run(repeats);
    auto end = std::chrono::steady_clock::now();
    auto cpu_end = process_cpu_ns();
    iced_destroy(iced);
    if (result.instructions != warm.instructions * repeats ||
        result.rejected != warm.rejected * repeats)
      throw std::runtime_error("unstable traversal");
    std::printf("{\"library\":\"%s\",\"mode\":\"%s\",\"bits\":%u,\"repeats\":%"
                "u,\"bytes\":%zu,\"instructions\":%llu,\"rejected\":%llu,"
                "\"boundary_hash\":\"%016llx\",\"value_hash\":\"%016llx\","
                "\"wall_ns\":%lld,\"cpu_ns\":%.0f}\n",
                library.c_str(), mode_name.c_str(), bits, repeats, bytes.size(),
                (unsigned long long)result.instructions,
                (unsigned long long)result.rejected,
                (unsigned long long)result.boundaries,
                (unsigned long long)result.values,
                (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
                    end - start)
                    .count(),
                cpu_end - cpu_start);
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
