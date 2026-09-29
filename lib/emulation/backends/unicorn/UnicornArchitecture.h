//===- UnicornArchitecture.h - Explicit software CPU reset state ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_UNICORNARCHITECTURE_H
#define NEVERD_EMULATION_UNICORNARCHITECTURE_H
#include "neverd/emulation/ExecutionBackend.h"

#include <unicorn/unicorn.h>
namespace neverd::emulation {
llvm::Error initializeUnicornArchitecture(uc_engine *CPU,
                                          GuestArchitecture Architecture);
}
#endif
