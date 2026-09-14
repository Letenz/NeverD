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
    DarwinRuntimeCall,
    /// The fixed String-to-NSString bridge, emitted with the Swift convention.
    /// TargetAddress is its exact imported slot, not a local Swift function.
    SwiftStringBridge,
    /// Bytes copied for a proven bounded, read-only, nonescaping consumer.
    /// This reproduces contents, not the original pointer's identity.
    RuntimeBorrowedBytes,
    /// The fixed optional-NSString-to-String bridge. Its owned String bits
    /// occupy two return registers; emitted calls retain the Swift convention.
    SwiftStringFromNSString,
    /// Address loaded from an exact Darwin runtime data import. This binds
    /// the platform object's identity; it does not copy or fold its contents.
    DarwinRuntimeGlobalAddress
  };
  Kind CallKind = Kind::Native;
  /// The bound source routine has a noreturn contract. Runtime bindings must
  /// revalidate this effect against their authoritative catalog.
  bool DoesNotReturn = false;
  SourceFunctionTypeHint Signature;
  va_t TargetAddress = 0;
  std::string TargetName;
  std::string Selector;
  /// For a runtime ivar offset query, the class that declared the ivar.
  std::string OwnerClass;
  /// Nonzero only when a verified selector stub loads this exact runtime slot.
  va_t SelectorReferenceAddress = 0;
  /// Pairs of pointer and byte-count parameter indices. The imported routine
  /// reads at most that nonnegative count, never writes/retains the pointer,
  /// and does not observe its identity. These are call effects, not ABI types.
  std::vector<std::pair<unsigned, unsigned>> BorrowedByteInputs;
  /// Only RuntimeBorrowedBytes uses this exact byte extent at TargetAddress.
  uint32_t ByteCount = 0;
};

} // namespace neverd
#endif
