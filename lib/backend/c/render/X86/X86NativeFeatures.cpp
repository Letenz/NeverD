//===- X86NativeFeatures.cpp - Native C feature ownership ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Repeated target attributes do not merge. Collect instruction requirements
/// and emit exactly one attribute for the compiler interfaces in use.
///
//===----------------------------------------------------------------------===//

#include "X86NativeFeatures.h"

#include "X86AmxState.h"

#include "neverd/backend/llvm/X86/X86SgxNative.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"

#include <functional>
#include <set>

namespace neverd {
namespace {
std::string featureNames(const std::set<std::string> &Features) {
  std::string Text;
  for (const auto &Name : Features) {
    if (!Text.empty())
      Text += ',';
    Text += Name;
  }
  return Text;
}
std::string preamble(const std::set<std::string> &Amx, bool Sgx) {
  if (Amx.empty() && !Sgx)
    return {};
  std::string Text;
  if (!Amx.empty())
    Text += "#ifndef NEVERD_X86_AMX\n"
            "#if defined(__NEVERC_X86_AMX_CONTROLS__) && "
            "__NEVERC_X86_AMX_CONTROLS__ >= 1\n"
            "#define NEVERD_X86_NATIVE_AMX 1\n"
            "#endif\n"
            "#if !defined(NEVERD_X86_NATIVE_AMX) && defined(__has_builtin)\n"
            "#if __has_builtin(__builtin_ia32_ldtilecfg_address) && "
            "__has_builtin(__builtin_ia32_tilemovrow_reg)\n"
            "#define NEVERD_X86_NATIVE_AMX 1\n"
            "#endif\n#endif\n"
            "#if defined(NEVERD_X86_NATIVE_AMX)\n"
            "#define NEVERD_X86_AMX(native, ...) native\n"
            "#else\n"
            "#define NEVERD_X86_AMX(native, ...) __VA_ARGS__\n"
            "#endif\n#endif\n";
  if (Sgx)
    Text += "#ifndef NEVERD_X86_SGX\n"
            "#if defined(__NEVERC_X86_SGX_COMPLETION__) && "
            "__NEVERC_X86_SGX_COMPLETION__ >= 1\n"
            "#define NEVERD_X86_NATIVE_SGX 1\n"
            "#define NEVERD_X86_SGX(native, ...) native\n"
            "#else\n"
            "#define NEVERD_X86_SGX(native, ...) __VA_ARGS__\n"
            "#endif\n#endif\n";
  const auto AmxNames = featureNames(Amx);
  const auto Attribute = [](const std::string &Names) {
    return "__attribute__((target(\"" + Names + "\")))\n";
  };
  if (!Amx.empty() && Sgx)
    Text += "#if defined(NEVERD_X86_NATIVE_AMX) && "
            "defined(NEVERD_X86_NATIVE_SGX)\n" +
            Attribute(AmxNames + ",sgx") +
            "#elif defined(NEVERD_X86_NATIVE_AMX)\n" + Attribute(AmxNames) +
            "#elif defined(NEVERD_X86_NATIVE_SGX)\n" + Attribute("sgx") +
            "#endif\n";
  else if (!Amx.empty())
    Text += "#if defined(NEVERD_X86_NATIVE_AMX)\n" + Attribute(AmxNames) +
            "#endif\n";
  else
    Text +=
        "#if defined(NEVERD_X86_NATIVE_SGX)\n" + Attribute("sgx") + "#endif\n";
  return Text;
}
} // namespace

std::string x86NativeFunctionPreamble(const HighFunc &Function) {
  bool Sgx = false;
  std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &E) {
    if (!E)
      return;
    Sgx |= isX86SgxIntrinsic(E->IntrinsicId);
    for (const auto &Operand : E->Operands)
      Visit(Operand);
  };
  walkStmts(Function.Body, [&](const HighStmt &S) { forEachExpr(S, Visit); });
  return preamble(x86AmxFunctionFeatures(Function), Sgx);
}
std::string x86NativeFunctionPreamble(const llvm::Function &Function) {
  bool Sgx = false;
  for (const auto &Block : Function)
    for (const auto &Instruction : Block) {
      const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction);
      if (!Call)
        continue;
      if (const auto *Callee = Call->getCalledFunction())
        Sgx |= isX86SgxNativeName(Callee->getName().str());
      if (const auto *Asm =
              llvm::dyn_cast<llvm::InlineAsm>(Call->getCalledOperand())) {
        const auto Mnemonic =
            llvm::StringRef(Asm->getAsmString()).split('\n').first;
        Sgx |= Mnemonic == "encls" || Mnemonic == "enclv";
      }
    }
  return preamble(x86AmxFunctionFeatures(Function), Sgx);
}
} // namespace neverd
