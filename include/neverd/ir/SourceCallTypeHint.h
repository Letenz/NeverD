#ifndef NEVERD_IR_SOURCECALLTYPEHINT_H
#define NEVERD_IR_SOURCECALLTYPEHINT_H

#include "neverd/Common.h"
#include "neverd/ir/SourceTypeHint.h"

#include <tuple>

namespace neverd {

/// Source receiver provenance carried through full-width machine copies.
/// Method self has a declared base class; an exact class reference denotes
/// that class object. Neither fact selects a dynamic method implementation.
struct ObjCReceiverTypeHint {
  enum class OriginKind { MethodEntry, ClassReference };
  OriginKind Origin = OriginKind::MethodEntry;
  va_t Address = 0;
  /// Root receiver's declared class, before any field loads.
  std::string ClassName;
  bool IsClassMethod = false;
  struct IvarAccess {
    va_t OffsetSlot = 0;
    /// Present only for a literal byte offset in the machine access. A
    /// runtime offset load may follow layout changes; a literal cannot.
    std::optional<uint32_t> ByteOffset;
    uint16_t OffsetWidth = 0;
    bool operator==(const IvarAccess &Other) const {
      return std::tie(OffsetSlot, ByteOffset, OffsetWidth) ==
             std::tie(Other.OffsetSlot, Other.ByteOffset, Other.OffsetWidth);
    }
    bool operator<(const IvarAccess &Other) const {
      return std::tie(OffsetSlot, ByteOffset, OffsetWidth) <
             std::tie(Other.OffsetSlot, Other.ByteOffset, Other.OffsetWidth);
    }
  };
  /// Exact runtime ivar-offset slots, in load order from the root receiver.
  /// Each step must name a declared object field in the current class lineage.
  /// This bounded path records type provenance, not loaded object identity.
  std::vector<IvarAccess> IvarLoads;

  bool operator==(const ObjCReceiverTypeHint &Other) const {
    return Origin == Other.Origin && Address == Other.Address &&
           ClassName == Other.ClassName &&
           IsClassMethod == Other.IsClassMethod && IvarLoads == Other.IvarLoads;
  }
};

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
    DarwinRuntimeGlobalAddress,
    /// A protocol reference slot resolved to a validated local declaration.
    RuntimeProtocol
  };
  Kind CallKind = Kind::Native;
  /// The bound source routine has a noreturn contract. Runtime bindings must
  /// revalidate this effect against their authoritative catalog.
  bool DoesNotReturn = false;
  /// The result is exactly this argument's pointer value. This does not
  /// remove call effects or establish memory immutability. Runtime bindings
  /// must revalidate the identity contract against the imported routine.
  std::optional<unsigned> ReturnedArgument;
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
  /// Proven actual arguments of an NSString format call. Signature contains
  /// every supplied value at its physical location; only FixedCount values
  /// belong in the emitted prototype. Revalidate the format object's identity
  /// and compiler-derived declaration before publishing source.
  struct FormatArguments {
    unsigned FixedCount = 0;
    unsigned FormatParameter = 0;
    va_t FormatAddress = 0;
  };
  std::optional<FormatArguments> Format;
  /// Restricts declaration agreement using revalidated receiver provenance.
  std::optional<ObjCReceiverTypeHint> Receiver;
  /// Only RuntimeBorrowedBytes uses this exact byte extent at TargetAddress.
  uint32_t ByteCount = 0;
};

} // namespace neverd
#endif
