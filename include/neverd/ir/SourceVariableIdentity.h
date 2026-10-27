//===- SourceVariableIdentity.h - High source value keys ------*- C++ -*-===//
#ifndef NEVERD_IR_SOURCEVARIABLEIDENTITY_H
#define NEVERD_IR_SOURCEVARIABLEIDENTITY_H
#include "neverd/ir/med/MedIR.h"

#include <string>
namespace neverd {
/// An actual value/group key in one unchanged source-analysis fingerprint.
/// A stack key names storage, not an independently proved lifetime partition.
inline std::string highSourceVariableId(const MedVar &V) {
  if (V.RenameTag >= 0)
    return "coalesced:" + std::to_string(V.RenameTag);
  if (V.Kind == MedVar::Stack)
    return "stack:" + std::to_string(V.StackOff);
  return "value:" + std::to_string(static_cast<unsigned>(V.Kind)) + ":" +
         std::to_string(V.Id) + ":" + std::to_string(V.SSAVer);
}
} // namespace neverd
#endif
