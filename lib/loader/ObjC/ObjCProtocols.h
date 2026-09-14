#ifndef NEVERD_LOADER_OBJC_PROTOCOL_READER_H
#define NEVERD_LOADER_OBJC_PROTOCOL_READER_H

namespace neverd {
struct BinaryImage;
namespace objc {
/// Called after parseObjCMethods validates the format, architecture and fixups.
void readProtocolDeclarations(BinaryImage &Image);
} // namespace objc
} // namespace neverd
#endif
