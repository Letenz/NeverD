#ifndef NEVERD_LOADER_OBJC_OBJCMETADATAJSON_H
#define NEVERD_LOADER_OBJC_OBJCMETADATAJSON_H

#include "llvm/Support/JSON.h"

namespace neverd {
struct BinaryImage;

/// Serialize recovered runtime records consistently for metadata-only and
/// source exports. Protocol declarations remain separate from implementations.
llvm::json::Object objcMetadataJSON(const BinaryImage &Image);
} // namespace neverd
#endif
