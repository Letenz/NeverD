#ifndef NEVERD_IR_SOURCECALLTYPEHINT_H
#define NEVERD_IR_SOURCECALLTYPEHINT_H

#include "neverd/Common.h"
#include "neverd/ir/SourceTypeHint.h"

namespace neverd {

/// A source projection binding, never authenticated ABI or safety evidence.
/// It describes the dispatch operation, not a statically selected method IMP.
struct SourceCallTypeHint {
  enum class Kind {
    Native,
    ObjCMessage,
    ObjCSuper2,
    BlockInvoke,
    RuntimeSelector,
    RuntimeClass,
    RuntimeMetaclass,
    RuntimeIvarOffset,
    NativeAddress,
    RuntimeBlockIsa,
    RuntimeBlockDescriptor,
    RuntimeBlockLiteral,
    /// Imported runtime routine with a known scalar ABI. TargetAddress is
    /// the import pointer slot, not a native source definition.
    ObjCRuntimeCall,
    /// A static key consumed only by associated-object runtime operations.
    /// TargetAddress identifies the original key; rebuilt methods share one
    /// opaque storage identity. This does not bind readable image contents.
    RuntimeAssociationKey,
    /// Base of a rebuilt numeric profiling-counter section. The SDK proves
    /// storage extents and permits only bounded, unordered memory accesses.
    RuntimeProfileCounterStorage,
    /// A Swift runtime import with an explicitly declared ordinary C ABI.
    /// Swift calling conventions and register-specialized entries are excluded.
    SwiftRuntimeCall,
    /// A verified Darwin constant-string object with one rebuilt identity.
    RuntimeConstantString,
    /// A fixed Darwin platform C ABI emitted against its public SDK header.
    DarwinRuntimeCall
  };
  Kind CallKind = Kind::Native;
  SourceFunctionTypeHint Signature;
  va_t TargetAddress = 0;
  std::string TargetName;
  std::string Selector;
  /// For a runtime ivar offset query, the class that declared the ivar.
  std::string OwnerClass;
  /// Nonzero only when a verified selector stub loads this exact runtime slot.
  va_t SelectorReferenceAddress = 0;
};

} // namespace neverd
#endif
