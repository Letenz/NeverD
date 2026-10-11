//===- SyscallHelpers.cpp - C helpers that make supervisor calls ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/render/SyscallHelpers.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd {

namespace {

const char *cTargetMacro(Arch TheArch) {
#define NEVERD_C_TARGET_MACRO(TargetArch, Macro)                               \
  if (TheArch == Arch::TargetArch)                                             \
    return Macro;
#include "neverd/backend/c/render/CTargetMacros.def"
  return nullptr;
}

/// The name GNU register asm gives register \p Offset.
std::string registerName(const SyscallConvention &Convention, uint64_t Offset) {
  const RegNameFn Name = getTargetRegInfo(Convention.TheArch).GetRegName;
  const char *Text = Name ? Name(Offset, Convention.RegisterSize) : nullptr;
  if (!Text)
    llvm::report_fatal_error("supervisor call register has no name");
  return llvm::StringRef(Text).lower();
}

} // namespace

std::string linuxSyscallHelperDefinition(const SyscallConvention &Convention,
                                         llvm::StringRef Name) {
  const char *Macro = cTargetMacro(Convention.TheArch);
  const char *Mnemonic = intrinsicAsmMnemonic(Convention.Modeled);
  if (!Macro || !Mnemonic || Convention.ArgumentCount == 0 ||
      Convention.RegisterSize == 0)
    llvm::report_fatal_error("incomplete supervisor call convention");
  const std::string Type =
      "uint" + std::to_string(Convention.RegisterSize * 8) + "_t";
  // The result register is either an argument register, which the call
  // overwrites, or one it only writes.
  int ResultArgument = -1;
  for (unsigned I = 0; I < Convention.ArgumentCount; ++I)
    if (Convention.ArgumentRegisters[I] == Convention.ResultRegister)
      ResultArgument = static_cast<int>(I);
  auto argumentName = [&](unsigned I) {
    return static_cast<int>(I) == ResultArgument ? std::string("result")
                                                 : "in_arg" + std::to_string(I);
  };

  std::string Text;
  llvm::raw_string_ostream OS(Text);
  OS << "#if !defined(__linux__) || !defined(" << Macro << ")\n"
     << "#error \"" << Name << " requires Linux on its architecture\"\n"
     << "#endif\n"
     << "static inline " << Type << " " << Name << "(" << Type << " number";
  for (unsigned I = 0; I < Convention.ArgumentCount; ++I)
    OS << ", " << Type << " arg" << I;
  OS << ") {\n"
     << "    register " << Type << " in_number __asm__(\""
     << registerName(Convention, Convention.NumberRegister)
     << "\") = number;\n";
  for (unsigned I = 0; I < Convention.ArgumentCount; ++I)
    OS << "    register " << Type << " " << argumentName(I) << " __asm__(\""
       << registerName(Convention, Convention.ArgumentRegisters[I])
       << "\") = arg" << I << ";\n";
  if (ResultArgument < 0)
    OS << "    register " << Type << " result __asm__(\""
       << registerName(Convention, Convention.ResultRegister) << "\");\n";
  OS << "    __asm__ volatile(\"" << Mnemonic << " #" << Convention.Immediate
     << "\"\n"
     << "        : \"" << (ResultArgument < 0 ? "=r" : "+r") << "\"(result)\n"
     << "        : \"r\"(in_number)";
  for (unsigned I = 0; I < Convention.ArgumentCount; ++I)
    if (static_cast<int>(I) != ResultArgument)
      OS << ", \"r\"(" << argumentName(I) << ")";
  OS << "\n"
     << "        : \"memory\");\n"
     << "    return result;\n"
     << "}\n\n";
  return Text;
}

} // namespace neverd
