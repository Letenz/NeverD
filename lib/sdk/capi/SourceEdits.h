//===- SourceEdits.h - Checked analyst source assertions -----*- C++ -*-===//
#ifndef NEVERD_SDK_SOURCEEDITS_H
#define NEVERD_SDK_SOURCEEDITS_H

#include "neverd/backend/c/SourceVariableTypes.h"
#include "neverd/ir/SourceCallEdits.h"

#include "llvm/ADT/StringRef.h"

namespace neverd {
struct HighFunc;
struct CSourceMap;
struct LowFunc;
struct SourceViewOptions;
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
std::string sourceCallId(const SourceCallOccurrenceKey &Site);
std::string sourceCallSnapshot(const LowFunc &Function);
std::string variableCallProfile(std::string Snapshot,
                                const SourceViewOptions &Options);
std::optional<SourceCallTypeHint>
parseSourceCallType(Session &S, const SourceCallEdit &Edit,
                    const SourceCallOccurrenceKey &Site, std::string &Error);
SourceStackFrame readSourceStackFrame(const llvm::json::Object &);
std::string sourceStackSnapshot(const LowFunc &);
void validateSourceStackFrame(Session &, const LowFunc &,
                              const SourceStackFrame &);
void validateSourceStackBindings(const SourceStackFrame &, const CSourceMap &,
                                 va_t Entry, llvm::StringRef Route);
void prepareSourceCallVariant(Session &S, va_t Entry,
                              const SourceViewOptions &Options);
} // namespace neverd::sdk
#endif
