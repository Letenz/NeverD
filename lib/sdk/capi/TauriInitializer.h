//===- TauriInitializer.h - Native-to-web evidence join ------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Project the same captured byte image through the owning native pipeline.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/NativeInput.h"
#include "neverd/web/Tauri.h"

namespace neverd::sdk {
web::TauriInitializer tauriInitializer(const web::NativeInput &Input,
                                       uint64_t Start);
} // namespace neverd::sdk
