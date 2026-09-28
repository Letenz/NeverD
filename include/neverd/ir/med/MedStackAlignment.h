//===- MedStackAlignment.h - Proven entry-stack alignment -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDSTACKALIGNMENT_H
#define NEVERD_IR_MED_MEDSTACKALIGNMENT_H

#include "neverd/ir/med/MedIR.h"

namespace neverd {

/// In source mode, simplify an alignment mask only when its operand is an
/// exact offset from an authenticated entry stack pointer and the target ABI
/// fixes every bit discarded by the mask.
void simplifyProvenStackAlignment(MedFunc &Func, Arch Architecture,
                                  BinaryFormat Format);

} // namespace neverd

#endif // NEVERD_IR_MED_MEDSTACKALIGNMENT_H
