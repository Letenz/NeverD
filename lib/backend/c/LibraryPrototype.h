//===- LibraryPrototype.h - C declarations of library routines -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_BACKEND_C_LIBRARYPROTOTYPE_H
#define NEVERD_BACKEND_C_LIBRARYPROTOTYPE_H
#include "neverd/Common.h"
#include "neverd/libc/LibCNames.h"

#include <string>
#include <string_view>

namespace neverd::c_library {
/// \p Type, a parameter or result type of a C library prototype, as C spells
/// it for \p TheArch: the Windows API convention a function pointer carries
/// is stdcall on 32-bit x86 and the only convention elsewhere.
inline std::string typeSpelling(std::string_view Type, Arch TheArch) {
  std::string Text(Type);
  const size_t At = Text.find(libc::kWinapiMarker);
  if (At == std::string::npos)
    return Text;
  return Text.replace(At, libc::kWinapiMarker.size(),
                      TheArch == Arch::X86 ? "__attribute__((stdcall)) " : "");
}

/// A function's \p Declarator, `name(parameters)`, returning \p Return: one
/// that returns a function pointer declares itself inside the pointer's own
/// declarator, `int (*_onexit(int (*)(void)))(void)`.
inline std::string declarator(std::string_view Return,
                              std::string_view Declarator) {
  if (const size_t Star = Return.find("*)"); Star != std::string_view::npos)
    return std::string(Return.substr(0, Star + 1)) + std::string(Declarator) +
           std::string(Return.substr(Star + 1));
  std::string Text(Return);
  if (Text.empty() || Text.back() != '*')
    Text += ' ';
  return Text + std::string(Declarator);
}

/// The C declaration of the routine \p Prototype types, named
/// \p Identifier, without `extern` or the closing `;`: `void Sleep(uint32_t)`,
/// stdcall where the Windows API convention is on 32-bit x86.
inline std::string declaration(const libc::LibCPrototype &Prototype,
                               std::string_view Identifier, Arch TheArch) {
  std::string Function(Identifier);
  Function += '(';
  for (unsigned I = 0; I < Prototype.ParamCount; ++I) {
    if (I)
      Function += ", ";
    Function += typeSpelling(Prototype.Params[I], TheArch);
  }
  // ISO C before C23 spells no prototype of `...` alone.
  if (Prototype.Variadic && Prototype.ParamCount)
    Function += ", ...";
  else if (!Prototype.Variadic && !Prototype.ParamCount)
    Function += "void";
  Function += ')';
  std::string Text;
  if (Prototype.Winapi && TheArch == Arch::X86)
    Text += "__attribute__((stdcall)) ";
  return Text + declarator(typeSpelling(Prototype.Return, TheArch), Function);
}
} // namespace neverd::c_library
#endif
