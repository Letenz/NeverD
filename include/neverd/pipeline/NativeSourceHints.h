#ifndef NEVERD_PIPELINE_NATIVESOURCEHINTS_H
#define NEVERD_PIPELINE_NATIVESOURCEHINTS_H

#include "neverd/ir/SourceTypeHint.h"

#include <optional>

namespace neverd {
struct BinaryImage;
struct MedFunc;
struct HighFunc;
struct PipelineFunctionAudit;

/// Describe observed scalar machine inputs and a defined result for a native
/// helper. The caller must select an exact local function target and supply IR
/// and audit from the same pipeline/image. This is a candidate for a second
/// source-only pipeline run, not the original C declaration, authentication,
/// a body-completeness certificate, or evidence of semantic equivalence.
///
/// Unknown calls, floating/vector ABI ambiguity, variadics, partial stack
/// slots, and results without a complete carrier on every return path fail
/// closed. A full-width observed parameter at the return register can supply
/// its incoming value; unused placeholders, seeds and PHIs cannot. The bounded
/// CFG proof meets the initial entry fact with backedges and invalidates
/// calls and partial writes, including narrowed self copies. The
/// final source body and dependency closure must still pass projection
/// validation after the second run. No symbol names participate in inference.
/// Complete integer inputs forwarded to known pointer parameters can refine
/// the source candidate through conflict-free COPY/PHI uses. Physical carriers
/// remain unchanged; this does not modify generic lifting or rewrite types.
std::optional<SourceFunctionTypeHint> inferNativeSourceTypeHint(
    const BinaryImage &Image, const MedFunc &Med, const HighFunc &High,
    const PipelineFunctionAudit &Audit, std::string &Diagnostic);
} // namespace neverd
#endif
