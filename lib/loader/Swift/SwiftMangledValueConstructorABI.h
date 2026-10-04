#ifndef NEVERD_LOADER_SWIFT_SWIFTMANGLEDVALUECONSTRUCTORABI_H
#define NEVERD_LOADER_SWIFT_SWIFTMANGLEDVALUECONSTRUCTORABI_H

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/SourceCallTypeHint.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/Swift/SwiftMetadata.h"

#include <map>

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

/// A saved candidate only triggers re-authentication; it never grants an ABI.
bool requiresSwiftFixedRecordConstructorProof(
    const SourceFunctionTypeHint &Signature);

/// Current complete machine/LowIR authentication, without granting frame or
/// source-publication effects. The declaration remains the sole ABI owner.
std::optional<SourceFunctionTypeHint>
swiftFixedRecordConstructorEntryABI(const BinaryImage &Image,
                                    const LowFunc &Function);

std::map<va_t, SourceCallTypeHint> buildSwiftFixedRecordConstructorCallHints(
    const BinaryImage &Image, const LowFunc &Caller,
    const std::map<va_t, SourceFunctionTypeHint> &CalleeABIs,
    const std::map<va_t, const LowFunc *> &CalleeBodies);
} // namespace neverd
#endif
