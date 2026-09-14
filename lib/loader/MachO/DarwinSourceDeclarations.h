#ifndef NEVERD_LOADER_MACHO_DARWINSOURCEDECLARATIONS_H
#define NEVERD_LOADER_MACHO_DARWINSOURCEDECLARATIONS_H

#include "neverd/loader/MachO/DarwinRuntimeCalls.h"

namespace neverd {
std::optional<SourceCallTypeHint>
darwinDeclaredSourceCallHint(const BinaryImage &Image, va_t ImportSlot);
} // namespace neverd
#endif
