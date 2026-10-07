//===- MedCallConvention.cpp - Calling-convention registry ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedCallConvention.h"

namespace neverd {

extern const CallArgumentConvention Win64CallArguments;
extern const CallArgumentConvention SysVX64CallArguments;

const CallArgumentConvention *callArgumentConvention(Arch A, BinaryFormat F) {
  for (const CallArgumentConvention *C :
       {&Win64CallArguments, &SysVX64CallArguments})
    if (C->TheArch == A && C->Format == F)
      return C;
  return nullptr;
}

} // namespace neverd
