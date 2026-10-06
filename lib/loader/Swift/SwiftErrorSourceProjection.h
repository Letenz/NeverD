#ifndef NEVERD_LOADER_SWIFT_SWIFTERRORSOURCEPROJECTION_H
#define NEVERD_LOADER_SWIFT_SWIFTERRORSOURCEPROJECTION_H

#include "neverd/ir/SourceABI.h"

namespace neverd {

/// Internal source result, separate from the callee's logical Swift ABI.
/// Both words belong to the same call: the ordinary result and error value.
/// Narrow, floating, aggregate and indirect results need separate support.
inline TypeRef
swiftErrorCallResultType(const SourceFunctionTypeHint &Signature) {
  const auto Error = sourceABIErrorResult(Signature);
  if (!Error || hasIndirectSourceParameters(Signature) ||
      !Signature.ReturnType || Signature.ReturnType->Size != 8 ||
      (Signature.ReturnType->Kind != NdTypeKind::Int &&
       Signature.ReturnType->Kind != NdTypeKind::Ptr) ||
      Signature.ReturnLocation.Kind != SourceABICarrierKind::IntegerRegister ||
      !Signature.ReturnComponents.empty())
    return nullptr;
  return NdType::makeStruct({NdType::makeInt(8, false), Error->Type});
}

/// This recognizes a transport shape, not native instruction ownership. A
/// consumer must also authenticate the current direct occurrence and callee.
inline bool isNativeSwiftErrorSourceCall(const SourceCallTypeHint &Hint,
                                         Arch Architecture) {
  return Hint.CallKind == SourceCallTypeHint::Kind::Native &&
         Hint.Signature.Architecture == Architecture && Hint.TargetAddress &&
         swiftErrorCallResultType(Hint.Signature) && !Hint.DoesNotReturn &&
         !Hint.WeakImport && !Hint.ReturnedArgument &&
         !Hint.RuntimeObjCResultType && !Hint.ValueWitness &&
         !Hint.BooleanResult && !Hint.SwiftWitnessUndefDescriptor &&
         !Hint.FunctionParameterCall && !Hint.ImmutableNativeCall &&
         !Hint.SwiftWitnessFrame && !Hint.SwiftConsumedInput &&
         !Hint.SwiftOpaqueValue && !Hint.SwiftValueConstructor &&
         !Hint.Virtual && Hint.Selector.empty() && Hint.OwnerClass.empty() &&
         !Hint.SelectorReferenceAddress && Hint.BorrowedByteInputs.empty() &&
         Hint.SwiftStaticStringInputs.empty() &&
         Hint.CanonicalBooleanInputs.empty() &&
         Hint.SwiftStringInputs.empty() && !Hint.Format &&
         !Hint.NilTerminated && !Hint.SwiftTypeMetadata && !Hint.Receiver &&
         !Hint.NativeSwiftReceiver && !Hint.ByValueCopy &&
         !Hint.SelectorResultUse && !Hint.SelectorResultTypeUse &&
         !Hint.SelectorArgumentTypeUse && !Hint.SelectorForwardingUse &&
         !Hint.SelectorArgumentStorageUse && !Hint.ObjCIndirectResultStorage &&
         !Hint.ByteCount && !Hint.ImmutablePointerSlot &&
         !Hint.AddressedFunctionABI;
}

} // namespace neverd
#endif
