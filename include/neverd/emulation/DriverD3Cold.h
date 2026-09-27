//===- DriverD3Cold.h - Dedicated supply capabilities ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit bus and platform capabilities for a dedicated device power supply.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_DRIVERD3COLD_H
#define NEVERD_EMULATION_DRIVERD3COLD_H

namespace neverd::emulation {

/// Facts supplied by the modeled bus and platform, independent of the driver's
/// policy. Each provider has a dedicated power supply; no shared rail is
/// inferred.
struct DriverD3ColdCapabilities {
  bool Supported = false;
  bool EnabledByDefault = false;
  bool WakeS0 = false;
  bool WakeSx = false;
};

} // namespace neverd::emulation
#endif // NEVERD_EMULATION_DRIVERD3COLD_H
