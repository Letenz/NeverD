#ifndef NEVERD_IR_MED_MEDSOURCEPARAMETERUSES_H
#define NEVERD_IR_MED_MEDSOURCEPARAMETERUSES_H

#include <vector>

namespace neverd {
struct MedFunc;

/// Identify complete pointer-sized inputs forwarded to source-bound pointer
/// parameters through COPY/PHI values, without conflicting scalar uses. The
/// result follows MedFunc::Params, including false entries for unused slots.
/// This refines source projection candidates only: it neither mutates MedIR
/// types nor establishes address provenance, ownership, or rewrite authority.
std::vector<bool> inferMedSourcePointerParameters(const MedFunc &Function);
} // namespace neverd
#endif
