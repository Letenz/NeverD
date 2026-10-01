#ifndef NEVERD_ANALYSIS_BYTECODE_INTERNAL_H
#define NEVERD_ANALYSIS_BYTECODE_INTERNAL_H

#include "neverd/ir/low/LowIR.h"

namespace neverd::analysis::detail {
llvm::Error validateBytecodeOperation(const LowOp &Op);
}

#endif
