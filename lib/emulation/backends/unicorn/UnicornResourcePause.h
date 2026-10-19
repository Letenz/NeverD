//===- UnicornResourcePause.h - Software restart admission ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Software restart admission.
///

#ifndef NEVERD_EMULATION_UNICORNRESOURCEPAUSE_H
#define NEVERD_EMULATION_UNICORNRESOURCEPAUSE_H
#include "neverd/emulation/ExecutionBackend.h"
#include "neverd/emulation/ResourceEvent.h"

#include <unicorn/unicorn.h>

namespace neverd::emulation {
class MemoryProjection;
std::optional<ResourcePause> classifyUnicornResourcePause(
    MemoryProjection &Memory, GuestArchitecture Architecture,
    uint64_t InstructionPC, const BackendFault &Fault, bool InstructionWrote,
    bool InstructionTouchedMMIO);
} // namespace neverd::emulation
#endif
