//===- PipelineCallAbiDetail.h - Module call-ABI recovery -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private declaration of the whole-module call-ABI recovery that the
/// patch/lift and HighIR pipeline drivers share.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_PIPELINECALLABIDETAIL_H
#define NEVERD_LIB_PIPELINE_PIPELINECALLABIDETAIL_H

#include "neverd/Common.h"

#include <map>
#include <string>

namespace neverd {

struct BinaryImage;
struct PipelineResult;

/// Recover every call's arguments in \p Result, and the parameters each
/// forwarder passes straight through, from the setup its caller writes
/// before the call (recoverCallAbi).  Each callee's recovered signature
/// bounds its calls: its integer register and total argument counts, its
/// floating-point argument registers, its scalar floating-point return
/// width, its hidden indirect-result pointer, and whether it is variadic or
/// consumes a va_list.  A forwarder's signature grows from its callee's to a
/// fixed point first, so the order of the functions does not matter.  Reads
/// the types the functions have already been given (inferMedTypes).
void recoverModuleCallAbi(const BinaryImage &Img, PipelineResult &Result,
                          const std::map<va_t, std::string> &AllFuncNames);

} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_PIPELINECALLABIDETAIL_H
