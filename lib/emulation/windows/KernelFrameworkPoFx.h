//===- KernelFrameworkPoFx.h - Framework-owned component settings -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Copied settings shared by the framework and its authoritative PoFx host.
///
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WINDOWS_KERNELFRAMEWORKPOFX_H
#define NEVERD_EMULATION_WINDOWS_KERNELFRAMEWORKPOFX_H

#include "KernelPoFx.h"

namespace neverd::emulation {
namespace framework {
#define NEVERD_FRAMEWORK_POFX_VALUE(Name, Value)                               \
  inline constexpr uint64_t Name = Value;
#include "KernelFrameworkPoFxValues.def"
#undef NEVERD_FRAMEWORK_POFX_VALUE
} // namespace framework

struct KernelFrameworkPoFxSettings {
  KernelPoFx::Component Component;
  KernelPoFx::Callbacks Routines;
  uint64_t Context = 0;
  uint64_t PostRegister = 0;
  uint64_t PreUnregister = 0;

  KernelFrameworkPoFxSettings() {
    Component.IdleStates.push_back({0, 0, pofx::UnknownPower});
  }
};
} // namespace neverd::emulation
#endif
