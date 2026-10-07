//===- MedCallConvention.cpp - Calling-convention registry ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedCallConvention.h"

namespace neverd {

extern const CallArgumentConvention Win64CallArguments;
extern const CallArgumentConvention SysVX64CallArguments;
extern const CallArgumentConvention I386CallArguments;
extern const CallArgumentConvention DarwinAArch64CallArguments;
extern const CallArgumentConvention AAPCS32CallArguments;

const CallArgumentConvention *callArgumentConvention(Arch A, BinaryFormat F) {
  static constexpr const CallArgumentConvention *Conventions[] = {
      &Win64CallArguments, &SysVX64CallArguments, &I386CallArguments,
      &DarwinAArch64CallArguments, &AAPCS32CallArguments};
  for (BinaryFormat Wanted : {F, BinaryFormat::Unknown})
    for (const CallArgumentConvention *C : Conventions)
      if (C->TheArch == A && C->Format == Wanted)
        return C;
  return nullptr;
}

} // namespace neverd
