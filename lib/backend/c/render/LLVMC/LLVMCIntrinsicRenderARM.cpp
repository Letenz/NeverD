//===- LLVMCIntrinsicRenderARM.cpp - ARM inline-asm rendering ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ARM/AArch64-specific inline-assembly-to-C rendering: asm-mnemonic lookup
/// table and GCC __asm__ volatile / ACLE translation.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/render/ArmCoprocessorAcle.h"
#include "neverd/backend/c/render/LLVMC/LLVMCIntrinsicRender.h"
#include "neverd/ir/intrinsics/IntrinsicInstructionText.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/AtomicOrdering.h"

#include <algorithm>
#include <cstring>

namespace neverd {

namespace {

static const AsmToCEntry ARMAsmTable[] = {
#define M(a, c) {a, c},
#include "LLVMCAsmToCARM.inc"
#undef M
};

} // anonymous namespace

const char *lookupArmAsmToC(const char *Mnem) {
  for (const auto &E : ARMAsmTable)
    if (std::strcmp(Mnem, E.AsmStr) == 0)
      return E.CName;
  return nullptr;
}

InlineAsmRender renderARMInlineAsm(Arch TheArch, const std::string &AsmStr,
                                   const std::string &Mnemonic,
                                   const std::string &ResultName,
                                   bool ResultLive,
                                   const std::vector<std::string> &Args) {
  const bool AArch64 = TheArch == Arch::AArch64;
  if (Mnemonic == "sel" && Args.size() >= 2) {
    std::string Result;
    if (ResultLive && !ResultName.empty())
      Result = ResultName + " = ";
    Result += "__sel(" + Args[0] + ", " + Args[1] + ");\n";
    return {Result, true};
  }

  // A barrier runs as the ACLE intrinsic of its own option: `dsb ishst` is
  // `__dsb(0xA)`, not the mnemonic's default.
  if (Mnemonic == "dmb" || Mnemonic == "dsb" || Mnemonic == "isb") {
    const llvm::StringRef Option =
        llvm::StringRef(AsmStr).drop_front(Mnemonic.size()).trim();
    const std::optional<uint64_t> CRm = Option.empty()
                                            ? std::optional<uint64_t>(0xF)
                                            : armBarrierOptionValue(Option);
    if (CRm && Args.empty())
      return {"__" + Mnemonic + "(0x" + llvm::utohexstr(*CRm) + ");" +
                  (Option.empty() ? "" : " /* " + AsmStr + " */") + "\n",
              true};
  }
  // An AArch64 system register as ACLE names it: `mrs $0, tpidr_el0` reads
  // __arm_rsr64("tpidr_el0"), and `msr tpidr_el0, $0` writes __arm_wsr64.
  // AArch32's status registers (`mrs $0, apsr`) are no such register.
  const auto isRegisterName = [](llvm::StringRef Name) {
    return !Name.empty() && llvm::all_of(Name, [](char C) {
      return llvm::isAlnum(C) || C == '_';
    });
  };
  if (llvm::StringRef Register = AsmStr;
      AArch64 && Register.consume_front("mrs $0, ") && Args.empty() &&
      isRegisterName(Register))
    return {(ResultLive && !ResultName.empty() ? ResultName + " = "
                                               : std::string()) +
                "__arm_rsr64(\"" + Register.str() + "\");\n",
            true};
  if (llvm::StringRef Register = AsmStr;
      AArch64 && Register.consume_front("msr ") &&
      Register.consume_back(", $0") && Args.size() == 1 &&
      isRegisterName(Register))
    return {"__arm_wsr64(\"" + Register.str() + "\", " + Args[0] + ");\n",
            true};

  // A coprocessor transfer is ACLE's __arm_mrc or kin, with the numbers its
  // assembly carries.
  if (const auto Transfer = armCoprocessorAsmField(AsmStr)) {
    const auto [Id, Field] = *Transfer;
    const bool Writes = Id == Intrinsic::ArmMcr || Id == Intrinsic::ArmMcrr;
    if (Args.size() == (Writes ? 1u : 0u))
      return {(!Writes && ResultLive && !ResultName.empty() ? ResultName + " = "
                                                            : std::string()) +
                  armCoprocessorAcleCall(Id, Field,
                                         Writes ? Args[0] : std::string()) +
                  ";\n",
              true};
  }

  const char *ArmC = lookupArmAsmToC(AsmStr.c_str());
  // A trap or a breakpoint builtin takes no code: the instruction it was
  // lifted from goes beside it (`__builtin_trap(); /* udf #0xFE */`).
  std::string Comment;
  if (!ArmC) {
    ArmC = lookupArmAsmToC(Mnemonic.c_str());
    if (ArmC && AsmStr != Mnemonic)
      Comment = " /* " + AsmStr + " */";
  }
  if (ArmC)
    return {std::string(ArmC) + ";" + Comment + "\n", true};

  // LLVM's `$N` is GNU C's `%N`, and an operand the arguments do not fill is
  // the output, which comes first (`sysl $0, #3, c15, c0, #1`).
  std::string Template;
  size_t Operands = 0;
  for (size_t I = 0; I < AsmStr.size(); ++I) {
    if (AsmStr[I] == '$' && I + 1 < AsmStr.size() &&
        llvm::isDigit(AsmStr[I + 1])) {
      Operands = std::max<size_t>(Operands, AsmStr[I + 1] - '0' + 1);
      Template += '%';
      continue;
    }
    Template += AsmStr[I];
  }
  const bool HasOutput = Operands > Args.size();
  // An output nothing reads still needs a place to go, in a register.
  const bool ScratchOutput = HasOutput && (!ResultLive || ResultName.empty());
  std::string Result = ScratchOutput ? std::string("{ ") +
                                           (AArch64 ? "uint64_t" : "uint32_t") +
                                           " result; "
                                     : "";
  Result += "__asm__ volatile(\"" + Template + "\"";
  if (HasOutput || !Args.empty()) {
    Result += " : ";
    if (HasOutput)
      Result += "\"=r\"(" +
                (ScratchOutput ? std::string("result") : ResultName) + ") ";
    Result += ":";
    for (size_t I = 0; I < Args.size(); ++I)
      Result += (I > 0 ? ", " : " ") + ("\"r\"(" + Args[I] + ")");
    Result += " :";
  } else {
    Result += " :::";
  }
  // An AArch32 MSR writes the condition flags.
  Result +=
      !AArch64 && Mnemonic == "msr" ? " \"memory\", \"cc\");" : " \"memory\");";
  Result += ScratchOutput ? " }\n" : "\n";
  return {Result, false};
}

std::string renderARMFence(llvm::AtomicOrdering Ordering) {
  switch (Ordering) {
  case llvm::AtomicOrdering::SequentiallyConsistent:
    return "__dmb(0xF);\n";
  case llvm::AtomicOrdering::Acquire:
    return "__dmb(0x9);\n";
  case llvm::AtomicOrdering::Release:
    return "__dmb(0xA);\n";
  default:
    return "__asm__ volatile(\"dmb ish\" ::: \"memory\");\n";
  }
}

const char *renderARMDebugBreak() { return "__builtin_debugtrap();\n"; }

} // namespace neverd
