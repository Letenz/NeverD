//===- LinuxDeviceOptions.h - Explicit device transaction inputs -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded device observations, independent of a host driver or device ABI.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_LINUXDEVICEOPTIONS_H
#define NEVERD_EMULATION_LINUXDEVICEOPTIONS_H

#include <cstdint>
#include <optional>
#include <vector>

namespace neverd::emulation {
struct LinuxDeviceMemory {
  /// Relative to ioctl's argument, or to the little-endian guest pointer
  /// read at argument + PointerOffset when that offset is present.
  uint64_t Offset = 0;
  std::optional<uint64_t> PointerOffset;
  std::vector<uint8_t> Expected;
  std::vector<uint8_t> Output;
};

struct LinuxDeviceIoctl {
  uint32_t Request = 0;
  int64_t Result = 0;
  std::optional<uint64_t> Argument;
  std::vector<LinuxDeviceMemory> Memory;
};

struct LinuxDevicePageUpdate {
  /// Elapsed virtual nanoseconds from workload start, never host time.
  uint64_t AfterNanoseconds = 0;
  /// Byte offset in this mapping; each update contains at most one page.
  uint64_t Offset = 0;
  std::vector<uint8_t> Bytes;
};

struct LinuxDeviceMapping {
  /// Page-aligned device offset and complete page bytes. No implicit padding.
  uint64_t Offset = 0;
  std::vector<uint8_t> Bytes;
  /// Finite external producer input, in nondecreasing time order. Equal-time
  /// writes retain their listed order. This does not infer a hardware workload.
  std::vector<LinuxDevicePageUpdate> Updates;
};

struct LinuxKGSLContexts {
  /// Select an explicit 40-byte-slot LP64 memstore mapping. The narrow model
  /// supports idle PREAMBLE/NO_GMEM_ALLOC contexts and consumed/retired
  /// queries. Without SynchronousA3xx, queued is known zero only without a
  /// producer for that context; externally submitted queue state is unmodeled.
  uint64_t MemstoreOffset = 0;
  uint32_t ContextLimit = 0;
  /// Explicit deterministic A3xx PM4 subset. Legacy IB submissions drain
  /// synchronously; only CP_NOP and one-word CP_MEM_WRITE are executable.
  /// Not a shader engine or a hardware scheduling/timing model.
  bool SynchronousA3xx = false;
  /// Coherent, bounded GPUOBJ and legacy GPUMEM_ID RAM objects with shared
  /// identity and whole-object mappings. No shader, cache maintenance, secure
  /// or asynchronous allocation.
  bool MemoryObjects = false;
};

struct LinuxDeviceOptions {
  /// Exact admitted O_ACCMODE (0, 1 or 2). No creation or truncation contract.
  uint32_t AccessMode = 0;
  /// Explicit open outcome: zero admits an owned descriptor, else guest errno.
  uint32_t OpenErrno = 0;
  /// Ordered, single-use transactions for each independent open description.
  /// An absent/mismatched next transaction stops instead of inventing a reply.
  std::vector<LinuxDeviceIoctl> Ioctls;
  /// Explicit initial RAM bytes for MAP_SHARED, PROT_READ or PROT_NONE.
  /// All opens of this catalogue entry share live backing pages. These inputs
  /// supply no MMIO or inferred producer; updates must be explicitly provided.
  std::vector<LinuxDeviceMapping> ReadOnlyMappings;
  std::optional<LinuxKGSLContexts> KGSL;
  /// Explicit LP64 Binder protocol-8 control/looper and object-free synchronous
  /// transactions. A guest context manager must execute all service replies;
  /// remote services and translated object handles are never inferred.
  bool Binder = false;
};
} // namespace neverd::emulation
#endif
