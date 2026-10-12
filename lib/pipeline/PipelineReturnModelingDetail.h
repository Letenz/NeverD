//===- PipelineReturnModelingDetail.h - Return ABI helpers ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private declarations shared by the wide-integer and aggregate return
/// recovery implementations and the patch/lift pipeline driver.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_PIPELINERETURNMODELINGDETAIL_H
#define NEVERD_LIB_PIPELINE_PIPELINERETURNMODELINGDETAIL_H

#include <cstdint>

namespace neverd {

struct BinaryImage;
struct PipelineResult;
class DebugContext;

/// Widen every callee that returns a 64-bit integer in a register pair to an
/// i64 return, and remodel the call sites (including loop-carried i64
/// accumulators) so both halves flow through.  32-bit targets only.
void modelWideIntReturns(const BinaryImage &Img, PipelineResult &Result);

/// Learn each callee's multi-register return shape from its direct callers'
/// field-extract remodeling and re-type the callee to match.  64-bit targets.
void recoverStructReturnFromCallers(const BinaryImage &Img,
                                    PipelineResult &Result);

/// Propagate a proven multi-register return shape forward through tail-call
/// forwarders (`struct S f(a){return g(a);}`) to callees reached only that way.
void propagateStructReturnForwarderShapes(const BinaryImage &Img,
                                          PipelineResult &Result);

/// What may reveal a callee's multi-register return shape.
enum class StructReturnEvidence : uint8_t {
  /// Only a direct caller's reads of the fields after the call.  A body that
  /// merely writes a second return register, a clobbered scratch, reveals
  /// nothing: C prints the callee's signature, which an extra field changes.
  Callers,
  /// Callers, or else the callee's own writes before each RETURN.  A rewrite
  /// may return an extra register that its callers ignore.
  CallersAndBody,
};

/// Recover a multi-register (or single wide-vector) return shape from a
/// callee's own body when no direct caller reveals it.  AArch64 only.
void recoverStructReturnFromBody(const BinaryImage &Img, PipelineResult &Result,
                                 StructReturnEvidence Evidence);

/// Materialize the multi-register return at every direct call site to a known
/// register-returning callee, including chained sites whose result no explicit
/// op reads.  AArch64 only.
void materializeKnownStructReturnCallSites(const BinaryImage &Img,
                                           PipelineResult &Result);

/// Settle which functions return no value a caller could rely on
/// (MedFunc::ReturnsNoValue), once for both C backends.
void settleReturnContracts(const BinaryImage &Img, PipelineResult &Result,
                           const DebugContext *Dbg = nullptr);

/// Remodel struct-return tail-call forwarders after call-ABI recovery so the
/// forwarding call produces every field register the RETURN reassembles.
/// 64-bit targets.
void remodelStructReturnForwarderCalls(const BinaryImage &Img,
                                       PipelineResult &Result);

} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_PIPELINERETURNMODELINGDETAIL_H
