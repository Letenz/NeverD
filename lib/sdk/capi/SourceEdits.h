//===- SourceEdits.h - Checked analyst source assertions -----*- C++ -*-===//
#ifndef NEVERD_SDK_SOURCEEDITS_H
#define NEVERD_SDK_SOURCEEDITS_H

#include "neverd/backend/c/SourceVariableTypes.h"

#include "llvm/ADT/StringRef.h"

namespace neverd {
struct HighFunc;
struct CSourceMap;
} // namespace neverd
namespace neverd::sdk {
struct Session;
void noteUnboundVariableTypes(CSourceMap &Map, const SourceVariableTypes &Types,
                              va_t Function);
TypeRef parseSourceVariableType(Session &S, llvm::StringRef Text,
                                uint16_t Bytes, std::string &Error);
SourceVariableTypes sourceVariableTypes(Session &S, va_t Entry,
                                        llvm::StringRef Snapshot,
                                        std::vector<std::string> *Stale);
std::string sourceVariableSnapshot(llvm::StringRef IR);
std::string highVariableSnapshot(const HighFunc &Function, bool Symbolic);
std::string editedVariableSnapshot(llvm::StringRef Before,
                                   const SourceVariableEdit &Edit);
} // namespace neverd::sdk
#endif
