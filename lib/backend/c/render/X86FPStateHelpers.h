//===- X86FPStateHelpers.h - Readable scalar FP state helpers -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_X86FPSTATEHELPERS_H
#define NEVERD_BACKEND_C_RENDER_X86FPSTATEHELPERS_H

#include "neverd/ir/X86FPState.h"

#include "llvm/Support/raw_ostream.h"

#include <map>
#include <string>
#include <utility>

namespace neverd {

using X86FPStateCHelperNames =
    std::map<std::pair<Intrinsic, unsigned>, std::string>;

inline std::string x86FPScalarValueCHelper(Intrinsic Id, unsigned Bytes) {
  return std::string("neverd_x86_") + x86ScalarFPStateMnemonic(Id) +
         "_value_f" + std::to_string(Bytes * 8);
}

template <typename Stream>
inline void writeX86FPScalarValueCHelpers(Stream &OS,
                                          const X86FPStateCHelperNames &Used) {
  for (const auto &[Shape, Name] : Used) {
    const auto [Id, Bytes] = Shape;
    if (!isX86ScalarFPStateIntrinsic(Id))
      continue;
    const std::string Raw = "uint" + std::to_string(Bytes * 8) + "_t";
    const char *Scalar = Bytes == 4 ? "float" : "double";
    OS << "static inline " << Raw << " " << Name << "(" << Raw << " a_bits, "
       << Raw << " b_bits, void *state_address) {\n"
       << "    " << Scalar << " a, b; " << Raw << " result;\n"
       << "    uint32_t state;\n"
       << "    __builtin_memcpy(&state, state_address, 4);\n"
       << "    __builtin_memcpy(&a, &a_bits, " << Bytes << ");\n"
       << "    __builtin_memcpy(&b, &b_bits, " << Bytes << ");\n"
       << "    __asm__ volatile(\"ldmxcsr %1\\n\\t"
       << x86ScalarFPStateMnemonic(Id) << (Bytes == 4 ? "ss" : "sd")
       << " %2,%0\\n\\tstmxcsr %1\"\n"
       << "        : \"+x\"(a), \"+m\"(state) : \"x\"(b) : \"memory\");\n"
       << "    __builtin_memcpy(state_address, &state, 4);\n"
       << "    __builtin_memcpy(&result, &a, " << Bytes << ");\n"
       << "    return result;\n}\n\n";
  }
}

inline std::string x86FPStateCHelper(Intrinsic Id, unsigned ScalarBytes) {
  return std::string(intrinsicCName(Id)) +
         (isX86ScalarFPStateIntrinsic(Id)
              ? "_f" + std::to_string(ScalarBytes * 8)
              : "");
}

inline void writeX86FPStateCHelpers(llvm::raw_ostream &OS,
                                    const X86FPStateCHelperNames &Used) {
  for (const auto &[Shape, Name] : Used) {
    const auto [Id, Bytes] = Shape;
    if (Id == Intrinsic::X86ReadMXCSR) {
      OS << "static inline uint32_t " << Name << "(void) {\n"
         << "    uint32_t state;\n"
         << "    __asm__ volatile(\"stmxcsr %0\" : \"=m\"(state) :: "
            "\"memory\");\n"
         << "    return state;\n}\n\n";
      continue;
    }
    if (Id == Intrinsic::X86WriteMXCSR) {
      OS << "static inline void " << Name << "(uint32_t state) {\n"
         << "    __asm__ volatile(\"ldmxcsr %0\" :: \"m\"(state) : "
            "\"memory\");\n"
         << "}\n\n";
      continue;
    }
    const unsigned Bits = Bytes * 8;
    const std::string Raw = "uint" + std::to_string(Bits) + "_t";
    const std::string Result = Bytes == 4 ? "uint64_t" : "unsigned _BitInt(96)";
    const char *Scalar = Bytes == 4 ? "float" : "double";
    const char *Mnemonic = x86ScalarFPStateMnemonic(Id);
    const char *Operator = Id == Intrinsic::X86FPAddState   ? "+"
                           : Id == Intrinsic::X86FPSubState ? "-"
                           : Id == Intrinsic::X86FPMulState ? "*"
                                                            : "/";
    OS << "/* a " << Operator
       << " b under incoming MXCSR; result bits and outgoing state. */\n"
       << "static inline " << Result << " " << Name << "(" << Raw << " a_bits, "
       << Raw << " b_bits, uint32_t state) {\n"
       << "    " << Scalar << " a, b;\n"
       << "    " << Raw << " result;\n"
       << "    __builtin_memcpy(&a, &a_bits, " << Bytes << ");\n"
       << "    __builtin_memcpy(&b, &b_bits, " << Bytes << ");\n"
       << "    __asm__ volatile(\"ldmxcsr %1\\n\\t" << Mnemonic
       << (Bytes == 4 ? "ss" : "sd") << " %2,%0\\n\\tstmxcsr %1\"\n"
       << "        : \"+x\"(a), \"+m\"(state) : \"x\"(b) : \"memory\");\n"
       << "    __builtin_memcpy(&result, &a, " << Bytes << ");\n"
       << "    return (" << Result << ")result | ((" << Result << ")state << "
       << Bits << ");\n}\n\n";
  }
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_X86FPSTATEHELPERS_H
