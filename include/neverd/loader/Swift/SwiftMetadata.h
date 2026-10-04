//===- SwiftMetadata.h - Proven Swift stored-property layouts -----*- C++
//-*-===//
#ifndef NEVERD_LOADER_SWIFT_SWIFTMETADATA_H
#define NEVERD_LOADER_SWIFT_SWIFTMETADATA_H
#include "neverd/loader/Swift/SwiftMethods.h"

#include "llvm/ADT/StringRef.h"

#include <array>
#include <optional>
#include <string>
#include <vector>
namespace neverd {
struct BinaryImage;
struct SwiftRecoveredType {
  std::string Module;
  std::string Name;
  std::string Kind;
  va_t Descriptor = 0;
  va_t Metadata = 0;
  uint64_t Size = 0;
  uint64_t Alignment = 0;
  std::vector<SwiftStorageField> Fields;
  std::string Status = "unrecovered";
  std::string Reason;
};
/// Read only bounded file-backed descriptors and individually resolved slots.
/// Generic/resilient or incomplete layouts remain explicit unrecovered rows.
std::vector<SwiftRecoveredType> recoverSwiftTypes(const BinaryImage &Image);

/// Return the exact byte width encoded by a bounded Swift static-property
/// storage symbol when its type is a standard-library scalar. Accessor and
/// unrelated manglings are rejected rather than classified by suffix text.
std::optional<uint64_t>
swiftStaticScalarStorageWidth(llvm::StringRef MangledSymbol);

/// Exact width of a file-private top-level Swift standard-library scalar.
/// The private declaration identity and scalar type must both be present in
/// the structured mangling; unrelated globals are not inferred from a suffix.
std::optional<uint64_t>
swiftPrivateScalarStorageWidth(llvm::StringRef MangledSymbol);

/// A complete immutable scalar object, identified by a bounded Swift static
/// storage declaration. This is a byte extent and address identity, not an
/// accessor ABI or permission to borrow arbitrary caller memory.
struct SwiftImmutableScalarStorage {
  std::string SymbolName;
  uint32_t ByteCount = 0;
};
std::optional<SwiftImmutableScalarStorage>
swiftImmutableScalarStorage(const BinaryImage &Image, va_t Address);

/// Stable identity of a registered internal nominal type or simple protocol.
/// The bounded declaration tree, current descriptor/context bytes, and every
/// registration record must agree. This supplies no layout or export address.
std::optional<std::string>
swiftLocalRegisteredNominalType(const BinaryImage &Image, va_t Descriptor);
std::optional<std::string>
swiftLocalRegisteredProtocolType(const BinaryImage &Image, va_t Descriptor);

/// Exact ordinary immutable CGFloat descriptor import. Identity only; the
/// frozen scalar representation must be established by its storage consumer.
bool swiftImportedCGFloatDescriptor(const BinaryImage &Image, va_t Slot);

struct SwiftFixedRecordField {
  enum class Kind { Float64, StrongReference };
  Kind Storage = Kind::Float64;
  std::string Name, MangledType;
  uint32_t Offset = 0;
  bool IsMutable = false;
};
struct SwiftFixedRecordStorage {
  std::string MangledType;
  va_t Descriptor = 0, Metadata = 0, ValueWitnessTable = 0;
  uint32_t Size = 0, Alignment = 0;
  std::array<SwiftFixedRecordField, 4> Fields;
};
/// Authenticate one bounded fixed struct storage shape: three Double/CGFloat
/// fields and one nonoptional strong local class reference. Complete registered
/// identity, reflection, fixed offsets and immutable value-witness layout must
/// agree. This is not a source type declaration, calling convention, ownership
/// operation, native-entry proof, or permission to replace dynamic witnesses.
std::optional<SwiftFixedRecordStorage>
swiftFixedRecordStorage(const BinaryImage &Image, va_t Descriptor);

/// Declared identity of a non-generic Swift class in Objective-C metadata.
/// This does not reconstruct its instance layout or freeze live offsets.
struct SwiftObjCClassIdentity {
  std::string Module, Name, RuntimeName;
  va_t Metadata = 0, Descriptor = 0;
};
std::optional<SwiftObjCClassIdentity>
swiftObjCClassIdentity(const BinaryImage &Image, va_t Metadata);

struct SwiftFixedRootClassField {
  std::string Name, MangledType;
  uint32_t Offset = 0, ByteCount = 0, Alignment = 0;
};
struct SwiftFixedRootClassStorage {
  SwiftObjCClassIdentity Identity;
  uint32_t Size = 0, Alignment = 0;
  std::vector<SwiftFixedRootClassField> Fields;
};
/// Bounded native Swift root-class storage with Optional<String>,
/// Optional<Optional<String>> or Optional<Bool> fields. Registered identity,
/// complete kind-1 reflection, immutable offset declarations and metadata
/// bounds must agree. This grants no initializer bytes, object identity,
/// accessor ABI, runtime call effects or source-publication permission.
std::optional<SwiftFixedRootClassStorage>
swiftFixedRootClassStorage(const BinaryImage &Image, va_t Metadata);

/// A separately authenticated Swift reflection field whose Objective-C type
/// string is empty. The exact ivar/offset symbol/vector must agree; callers
/// still load the runtime offset and prove the receiver's class provenance.
std::optional<std::string> swiftObjCStoredFieldClass(const BinaryImage &Image,
                                                     llvm::StringRef ClassName,
                                                     va_t OffsetSlot);
} // namespace neverd
#endif
