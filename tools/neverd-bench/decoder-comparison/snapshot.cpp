//===- snapshot.cpp - Serialize public x86 decoder results
//-------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Serialize public x86 decoder results. Benchmark-only; not linked into
/// NeverD.

// Canonical public x86 results for baseline/candidate byte-for-byte comparison.
// Deliberately independent of the timing checksum and struct padding.
#include <algorithm>
#include <capstone/capstone.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static std::vector<unsigned char> output;
static void value(uint64_t v) {
  for (unsigned i = 0; i != 8; ++i) {
    output.push_back(v & 255);
    v >>= 8;
  }
}
static void text(const char *s) {
  value(std::strlen(s));
  while (*s)
    output.push_back(static_cast<unsigned char>(*s++));
}
static void flush() {
  if (std::fwrite(output.data(), 1, output.size(), stdout) != output.size())
    throw std::runtime_error("write");
  output.clear();
}

int main(int argc, char **argv) {
  try {
#ifdef _WIN32
    if (_setmode(_fileno(stdout), _O_BINARY) == -1)
      throw std::runtime_error("binary stdout");
#endif
    if (argc != 7)
      throw std::runtime_error(
          "snapshot bits att text detail every_offset input");
    int bits = std::atoi(argv[1]);
    bool att = std::atoi(argv[2]), format = std::atoi(argv[3]);
    bool detail = std::atoi(argv[4]), every_offset = std::atoi(argv[5]);
    std::ifstream file(argv[6], std::ios::binary);
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file),
                               std::istreambuf_iterator<char>()};
    if (bytes.empty())
      throw std::runtime_error("empty input");
    csh h;
    if (cs_open(CS_ARCH_X86,
                bits == 64   ? CS_MODE_64
                : bits == 32 ? CS_MODE_32
                             : CS_MODE_16,
                &h))
      throw std::runtime_error("open");
    if (cs_option(h, CS_OPT_SYNTAX,
                  att ? CS_OPT_SYNTAX_ATT : CS_OPT_SYNTAX_INTEL) ||
        cs_option(h, CS_OPT_TEXT, format ? CS_OPT_ON : CS_OPT_OFF) ||
        cs_option(h, CS_OPT_DETAIL, detail ? CS_OPT_ON : CS_OPT_OFF))
      throw std::runtime_error("option");
    cs_insn *insn = cs_malloc(h);
    if (!insn)
      throw std::runtime_error("allocation");
    uint64_t accepted = 0, rejected = 0;
    for (size_t offset = 0; offset < bytes.size();) {
      auto *d = insn->detail;
      std::memset(insn, 0, sizeof(*insn));
      insn->detail = d;
      if (d)
        std::memset(d, 0, sizeof(*d));
      const uint8_t *cursor = bytes.data() + offset;
      size_t remaining = bytes.size() - offset;
      uint64_t address = 0x100000 + offset;
      bool ok = cs_disasm_iter(h, &cursor, &remaining, &address, insn);
      value(offset);
      value(ok);
      value(cs_errno(h));
      value(cursor - bytes.data());
      value(remaining);
      value(address);
      if (ok) {
        ++accepted;
        if (!insn->size || insn->size > bytes.size() - offset)
          throw std::runtime_error("length");
        value(insn->id);
        value(insn->alias_id);
        value(insn->address);
        value(insn->size);
        for (unsigned i = 0; i != insn->size; ++i)
          value(insn->bytes[i]);
        text(insn->mnemonic);
        text(insn->op_str);
        value(insn->is_alias);
        value(insn->usesAliasDetails);
        value(insn->illegal);
        if (detail) {
          value(d->writeback);
          value(d->groups_count);
          for (unsigned i = 0; i < d->groups_count; ++i)
            value(d->groups[i]);
          value(d->regs_read_count);
          value(d->regs_write_count);
          for (unsigned i = 0; i < d->regs_read_count; ++i)
            value(d->regs_read[i]);
          for (unsigned i = 0; i < d->regs_write_count; ++i)
            value(d->regs_write[i]);
          cs_regs reads{}, writes{};
          uint8_t nr = 0, nw = 0;
          value(cs_regs_access(h, insn, reads, &nr, writes, &nw));
          value(nr);
          value(nw);
          for (unsigned i = 0; i < nr; ++i)
            value(reads[i]);
          for (unsigned i = 0; i < nw; ++i)
            value(writes[i]);
          const auto &x = d->x86;
          for (auto v : x.prefix)
            value(v);
          for (auto v : x.opcode)
            value(v);
          value(x.rex);
          value(x.rex2);
          value(x.addr_size);
          value(x.modrm);
          value(x.sib);
          value(x.disp);
          value(x.sib_index);
          value(x.sib_scale);
          value(x.sib_base);
          value(x.xop_cc);
          value(x.sse_cc);
          value(x.avx_cc);
          value(x.avx_sae);
          value(x.avx_rm);
          value(x.eflags);
          value(x.op_count);
          value(x.encoding.modrm_offset);
          value(x.encoding.disp_offset);
          value(x.encoding.disp_size);
          value(x.encoding.imm_offset);
          value(x.encoding.imm_size);
          for (unsigned i = 0; i < x.op_count; ++i) {
            const auto &o = x.operands[i];
            value(o.type);
            value(o.size);
            value(o.access);
            value(o.avx_bcast);
            value(o.avx_zero_opmask);
            if (o.type == X86_OP_REG)
              value(o.reg);
            if (o.type == X86_OP_IMM)
              value(o.imm);
            if (o.type == X86_OP_MEM) {
              value(o.mem.segment);
              value(o.mem.base);
              value(o.mem.index);
              value(o.mem.scale);
              value(o.mem.disp);
            }
          }
        }
      } else
        ++rejected;
      offset += every_offset || !ok ? 1 : insn->size;
      if (output.size() >= 65536)
        flush();
    }
    value(accepted);
    value(rejected);
    flush();
    std::fprintf(stderr, "{\"accepted\":%llu,\"rejected\":%llu}\n",
                 static_cast<unsigned long long>(accepted),
                 static_cast<unsigned long long>(rejected));
    cs_free(insn, 1);
    cs_close(&h);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
