//===- FunctionDeclaration.h - A function's C declaration -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The declaration of a function the user edits with Y, the user's or the
/// signature the decompiler prints (NeverDCAPIFunctionType.cpp), and where
/// it passes a parameter, for the C API's other requests.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_FUNCTIONDECLARATION_H
#define NEVERD_SDK_CAPI_FUNCTIONDECLARATION_H

#include "SessionImpl.h"

#include "neverd/ir/SourceTypeHint.h"
#include "neverd/types/TypeParse.h"

#include <optional>
#include <string>

namespace neverd::sdk {

/// The declaration of the function at \p Entry: the user's, else the
/// signature the decompiler prints for it, as \p Library reads it.
struct CurrentDeclaration {
  std::string Text;
  types::FunctionDeclaration Read;
  bool Stated = false;
};
/// None with the session's error when the function prints no signature.
std::optional<CurrentDeclaration>
currentDeclaration(neverd_session_t Sess, Session &S, va_t Entry,
                   const types::TypeLibrary &Library);

/// Where the image's calling convention passes the parameter \p Index (from
/// 0, as the declaration lists it) of the function at \p Entry; none with
/// \p Error when the declaration does not read, has no such parameter, or
/// NeverD has no rules for the convention.
std::optional<SourceABIValueLocation> parameterLocation(neverd_session_t Sess,
                                                        Session &S, va_t Entry,
                                                        size_t Index,
                                                        std::string &Error);

} // namespace neverd::sdk

#endif // NEVERD_SDK_CAPI_FUNCTIONDECLARATION_H
