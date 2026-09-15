#ifndef NEVERD_PIPELINE_NATIVESOURCEHINTS_H
#define NEVERD_PIPELINE_NATIVESOURCEHINTS_H

#include "neverd/ir/SourceTypeHint.h"

#include <optional>

namespace neverd {
struct BinaryImage;
struct LowFunc;
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
/// calls and partial writes, including narrowed self copies.
/// A leaf forwarding only to declared external void tail calls may instead
/// supply a void source summary, with no usable result. This requires complete
/// LowIR evidence and forbids preserved-register and frame writes. CFG checks
/// still apply, and callers observing a result must fail source validation.
/// The final source body and dependency closure must still pass projection
/// validation after the second run. No symbol names participate in inference.
/// Complete integer inputs forwarded to known pointer parameters can refine
/// the source candidate through conflict-free COPY/PHI uses. Physical carriers
/// remain unchanged; this does not modify generic lifting or rewrite types.
/// When Low is supplied from the same pipeline, observed full-width integer
/// entry registers can become explicit auxiliary parameters. Caller-saved
/// inputs may later be overwritten; preserved context inputs require that the
/// native body never writes preserved non-frame/link registers. Implicit call
/// definitions do not establish entry inputs, including SSA version zero.
/// Both callers and definitions must use the resulting source projection;
/// these parameters do not describe an external C or Swift calling convention.
std::optional<SourceFunctionTypeHint> inferNativeSourceTypeHint(
    const BinaryImage &Image, const MedFunc &Med, const HighFunc &High,
    const PipelineFunctionAudit &Audit, std::string &Diagnostic,
    const LowFunc *Low = nullptr);
} // namespace neverd
#endif
