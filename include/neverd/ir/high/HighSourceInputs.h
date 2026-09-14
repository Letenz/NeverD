#ifndef NEVERD_IR_HIGH_HIGHSOURCEINPUTS_H
#define NEVERD_IR_HIGH_HIGHSOURCEINPUTS_H

#include "neverd/ir/high/HighIR.h"

namespace neverd {
/// A pointer parameter read exactly once by an unconditional entry load,
/// before any observable operation. The pointer has no other occurrence,
/// including writes, escapes or later reads. Unknown control flow and work
/// exhaustion supply no summary. This is a source-only value-use fact; the
/// caller must separately establish the target, ABI and replacement storage.
TypeRef entryScalarLoadInput(const HighFunc &Function, unsigned Parameter);
} // namespace neverd
#endif
