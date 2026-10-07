//===- InstructionRelocations.h - Relocations inside an instruction -*- C++
//-*-//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The relocation fields the loader recorded inside one decoded instruction:
/// the addresses it resolved, which give a constant operand the provenance of
/// an address, and the scalar fields it left numbers.  Every lift of an
/// instruction passes them to Decoder::liftToLow, so that a relocated
/// immediate means the same address to the pipeline and to the C API.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DECODE_INSTRUCTIONRELOCATIONS_H
#define NEVERD_DECODE_INSTRUCTIONRELOCATIONS_H

#include "neverd/decode/Decoder.h"

#include <vector>

namespace neverd {

struct BinaryImage;

struct InstructionRelocations {
  std::vector<RelocatedAddressOperand> Addresses;
  std::vector<RelocatedScalarOperand> Scalars;
};

/// The relocation fields inside \p DI, an instruction of \p Img.
InstructionRelocations instructionRelocations(const BinaryImage &Img,
                                              const DecodedInsn &DI);

} // namespace neverd

#endif // NEVERD_DECODE_INSTRUCTIONRELOCATIONS_H
