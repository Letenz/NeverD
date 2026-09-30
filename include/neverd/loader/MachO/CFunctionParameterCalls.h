#ifndef NEVERD_LOADER_MACHO_CFUNCTIONPARAMETERCALLS_H
#define NEVERD_LOADER_MACHO_CFUNCTIONPARAMETERCALLS_H

#include "neverd/ir/SourceCallTypeHint.h"

#include <map>

namespace neverd {
struct BinaryImage;
struct LowFunc;
struct HighExpr;
struct HighFunc;

/// Derive the fixed ordinary C ABI of an explicitly typed function-pointer
/// parameter. This supplies a declaration, never evidence of a call target.
std::optional<SourceFunctionTypeHint>
cFunctionParameterSignature(const SourceFunctionTypeHint &Entry,
                            unsigned Parameter, Arch Architecture);

/// Check the declaration and evidence shape only. This cannot authenticate
/// the saved machine occurrence; publication additionally repeats the proof.
bool isCFunctionParameterCallHint(const SourceCallTypeHint &Hint,
                                  const SourceFunctionTypeHint &Entry,
                                  va_t FunctionEntry, Arch Architecture);

/// Shared declaration/operand validation for SDK publication and C emission.
/// The saved receipt still requires the separate current LowIR proof.
bool isCFunctionParameterSourceCall(const HighExpr &Expression,
                                    const HighFunc &Function,
                                    Arch Architecture);

/// Bind real ARM64 BLR occurrences whose target is the same complete entry
/// parameter on every reaching path. Copies, call preservation and CFG joins
/// are proved against current complete LowIR and immutable instructions. The
/// result retains the original indirect call, its arguments and its effects.
/// Publication must rerun this owner with the current caller and callees.
std::map<va_t, SourceCallTypeHint> buildCFunctionParameterCallHints(
    const BinaryImage &Image, const LowFunc &Function,
    const SourceFunctionTypeHint &Entry,
    const std::map<va_t, SourceFunctionTypeHint> *NativeCallees = nullptr);
} // namespace neverd
#endif
