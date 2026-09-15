#ifndef NEVERD_LOADER_OBJC_PROPERTY_READER_H
#define NEVERD_LOADER_OBJC_PROPERTY_READER_H

#include "neverd/loader/ObjC/ObjCMethods.h"

namespace neverd::objc {
/// Append bounded property declarations from one resolved runtime list slot.
/// The caller supplies owner identity, class/instance scope and a shared
/// budget.
bool readPropertyList(BinaryImage &Image, va_t Slot, const ObjCProperty &Owner,
                      size_t &Remaining);
/// The optional category class-property pointer requires the image ABI flag.
bool hasCategoryClassProperties(BinaryImage &Image);
} // namespace neverd::objc
#endif
