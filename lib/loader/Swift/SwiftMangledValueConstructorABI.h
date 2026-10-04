#ifndef NEVERD_LOADER_SWIFT_SWIFTMANGLEDVALUECONSTRUCTORABI_H
#define NEVERD_LOADER_SWIFT_SWIFTMANGLEDVALUECONSTRUCTORABI_H

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/Swift/SwiftMetadata.h"

namespace neverd {
struct SwiftFixedRecordConstructorDeclaration {
  SwiftFixedRecordStorage Storage;
  SourceFunctionTypeHint Signature;
};

/// A complete nongeneric value-constructor declaration for the independently
/// authenticated fixed record shape. The three floating arguments and optional
/// thick Bool callback are distinct from its strong-reference stored field.
/// This supplies a declaration ABI only: current body, caller, ownership and
/// publication proofs remain independent obligations.
std::optional<SwiftFixedRecordConstructorDeclaration>
swiftMangledFixedRecordConstructorDeclaration(const BinaryImage &Image,
                                              va_t Entry);
} // namespace neverd
#endif
