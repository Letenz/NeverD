//===- BranchEncoding.h - Encodings of direct branches ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The rows of BranchEncoding.def as typed constants: the instruction forms,
/// the fields and the other constants of the direct branches and linker
/// thunks NeverD decodes, which ISAEncoding.h's constants of the same
/// instructions are defined from.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_BRANCHENCODING_H
#define NEVERD_SUPPORT_BRANCHENCODING_H

#include "neverd/support/InstructionFields.h"

#include <cstdint>

namespace neverd {
namespace branch {

#define NEVERD_BRANCH_FORM(Name, Mask, Match)                                  \
  inline constexpr InstructionForm Name{Mask, Match};
#define NEVERD_BRANCH_FIELD(Name, Low, Width)                                  \
  inline constexpr BitField Name{Low, Width};
#define NEVERD_BRANCH_VALUE(Name, Value) inline constexpr uint32_t Name = Value;
#include "neverd/support/BranchEncoding.def"

} // namespace branch
} // namespace neverd

#endif // NEVERD_SUPPORT_BRANCHENCODING_H
