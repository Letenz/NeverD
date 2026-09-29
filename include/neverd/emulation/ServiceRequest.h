//===- ServiceRequest.h - Service handoff before architectural entry -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_SERVICEREQUEST_H
#define NEVERD_EMULATION_SERVICEREQUEST_H

#include <cstdint>

namespace neverd::emulation {
enum class ServiceRequestKind {
#define NEVERD_SERVICE_REQUEST(Name, Text) Name,
#include "neverd/emulation/ServiceRequest.def"
#undef NEVERD_SERVICE_REQUEST
};
const char *serviceRequestKindName(ServiceRequestKind Kind);

/// A checked user instruction intercepted before architectural service entry.
/// No register, PC, stack or privilege transition has occurred. NextPC is the
/// sequential address, not an applied return. Immediate is the SVC operand (or
/// zero for SYSCALL), never an inferred OS service number. The runtime owns ABI
/// decoding, result/clobber registers and explicit return or exception
/// transfer.
struct ServiceRequest {
  ServiceRequestKind Kind;
  uint64_t PC;
  uint64_t NextPC;
  uint16_t Immediate = 0;

  bool operator==(const ServiceRequest &) const = default;
};
} // namespace neverd::emulation
#endif
