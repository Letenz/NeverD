//===- X64BranchModel.h - Immutable relative-branch model ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_X64BRANCHMODEL_H
#define NEVERD_EMULATION_X64BRANCHMODEL_H
namespace neverd::emulation {
/// Relative-branch decoding rules, not a complete CPU or CPUID identity.
enum class X64BranchModel {
#define NEVERD_X64_BRANCH_MODEL(Name, Mode) Name,
#include "neverd/emulation/X64BranchModels.def"
#undef NEVERD_X64_BRANCH_MODEL
};
} // namespace neverd::emulation
#endif
