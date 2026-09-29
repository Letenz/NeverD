//===- NeverDCAPICPU.h - CPU configuration and capability queries -*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_SDK_CAPI_CPU_H
#define NEVERD_SDK_CAPI_CPU_H

#include "neverd/sdk/NeverDCAPISession.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
#define NEVERD_EXECUTION_REPORT_LIMIT(Name, CName, Value) CName = Value,
#include "neverd/emulation/ExecutionReport.def"
#undef NEVERD_EXECUTION_REPORT_LIMIT
};

/// Query CPU execution independently of a loaded binary or guest OS. A NULL
/// ConfigurationJSON selects auto/software-cpu-v1/x86_64. Otherwise provide a
/// NUL-terminated UTF-8 JSON object, at most
/// NEVERD_CPU_CONFIGURATION_JSON_LIMIT bytes. Optional keys: backend, contract,
/// architecture, privilege, virtual_address_bits, page_size, required_features
/// (an array of names). Unknown keys/names, null field values and unsupported
/// requirements fail. Omitted privilege/address/page values select the
/// contract's fixed profile.
///
/// Reports static semantic capabilities, normalized configuration and compiled
/// backend availability separately. ProbeHost must be 0 or 1. With 0, host is
/// null and no CPU is created. With 1, a temporary initialization probe reports
/// live availability; this does not establish workload compatibility or native
/// execution coverage. Unavailable backends produce a report, never fallback.
///
/// Returns owned JSON; release with neverd_free_string(). NULL indicates an
/// invalid session/configuration, allocation failure or disabled CPU emulation;
/// inspect neverd_last_error(Sess). Requires CPU or driver emulation at build
/// time. Does not load a file or mutate the session image.
NEVERD_API const char *
neverd_cpu_capabilities_json(neverd_session_t Sess,
                             const char *ConfigurationJSON, int ProbeHost);

#ifdef __cplusplus
}
#endif
#endif
