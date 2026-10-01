#ifndef NEVERD_SDK_CAPI_OBJCUNWINDSOURCE_H
#define NEVERD_SDK_CAPI_OBJCUNWINDSOURCE_H

#include "../../loader/MachO/DarwinRuntimeImport.h"

#include "neverd/loader/MachO/DarwinImportVeneer.h"
#include "neverd/loader/MachO/DarwinRuntimeCalls.h"
#include "neverd/loader/ObjC/ObjCCallHints.h"

#include <algorithm>

namespace neverd::sdk {

inline bool objcUnwindImportProvider(const BinaryImage &Image, va_t Slot,
                                     llvm::StringRef Providers) {
  const auto Bind = Image.DyldBindSlots.find(Slot);
  return Bind != Image.DyldBindSlots.end() &&
         darwinExportModuleMatches(Providers, Bind->second.Module) &&
         std::find(Image.DynInfo.NeededLibs.begin(),
                   Image.DynInfo.NeededLibs.end(),
                   Bind->second.Module) != Image.DynInfo.NeededLibs.end();
}

inline bool objcUnwindRuntimeTargetIs(const BinaryImage &Image, va_t Target,
                                      llvm::StringRef Name,
                                      llvm::StringRef Providers) {
  const auto Slot = darwinImportVeneerSlot(Image, Target);
  const auto Import = Slot ? darwinRuntimeImport(Image, *Slot) : std::nullopt;
  return Import && *Import == Name &&
         objcUnwindImportProvider(Image, *Slot, Providers);
}

// A symbol spelling or a cached hint cannot remove an ordinary fallthrough
// edge into an exceptional suffix. Reauthenticate the imported runtime call.
inline bool objcUnwindNoReturnTarget(const BinaryImage &Image, va_t Target) {
  const auto Slot = darwinImportVeneerSlot(Image, Target);
  if (!Slot)
    return false;
  const auto Runtime = darwinRuntimeSourceCallHint(Image, *Slot);
  const auto ObjC = objcRuntimeSourceCallHint(Image, *Slot);
  return (Runtime && Runtime->DoesNotReturn &&
          Runtime->TargetName == "__stack_chk_fail" &&
          objcUnwindImportProvider(Image, *Slot,
                                   "/usr/lib/libSystem.B.dylib|/usr/lib/system/"
                                   "libsystem_c.dylib")) ||
         (ObjC && ObjC->DoesNotReturn &&
          ObjC->TargetName == "objc_exception_throw" &&
          objcUnwindImportProvider(Image, *Slot, "/usr/lib/libobjc.A.dylib"));
}

} // namespace neverd::sdk
#endif
