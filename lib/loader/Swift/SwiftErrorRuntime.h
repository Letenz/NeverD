#ifndef NEVERD_LOADER_SWIFT_SWIFTERRORRUNTIME_H
#define NEVERD_LOADER_SWIFT_SWIFTERRORRUNTIME_H

#include "neverd/ir/SourceABI.h"

namespace neverd {
inline constexpr char SwiftWillThrowValueSourceName[] =
    "neverd_swift_will_throw_value";

/// Swift 6.1.2 Error.h, ErrorObjectCommon.cpp and compiler IR agree on this
/// exact operation: swiftself plus a read-only swifterror slot. Its atomic
/// handler lookup and possible callback remain real effects. Only the error
/// slot is unchanged; neither the error object nor other memory is read-only.
/// This is not the signature of an ordinary two-pointer machine call.
inline std::optional<SourceFunctionTypeHint>
swiftWillThrowSourceSignature(Arch Architecture) {
  SourceFunctionTypeHint Signature;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::SwiftRuntime;
  Signature.ReturnType = NdType::makeVoid();
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  Signature.Parameters = {{"context", Pointer},
                          {"error", NdType::makePtr(Pointer)}};
  Signature.Parameters[0].TheRole = SourceParameterTypeHint::Role::SwiftContext;
  Signature.Parameters[1].TheRole =
      SourceParameterTypeHint::Role::SwiftErrorResult;
  std::string Diagnostic;
  if (!assignDarwinSwiftSourceABI(Signature, Architecture, Diagnostic))
    return std::nullopt;
  return Signature;
}

/// Only this complete runtime declaration can project the logical error-slot
/// input as a value and keep the original register. General in/out error
/// parameters remain unsupported until their outputs are explicitly modeled.
inline bool isSwiftWillThrowSourceCall(const SourceCallTypeHint &Hint,
                                       Arch Architecture) {
  const auto Expected = swiftWillThrowSourceSignature(Architecture);
  return Expected &&
         Hint.CallKind == SourceCallTypeHint::Kind::SwiftRuntimeCall &&
         Hint.TargetName == "swift_willThrow" && Hint.TargetAddress &&
         Hint.TargetAddress % 8 == 0 &&
         equalSourceABIs(Hint.Signature, *Expected) && !Hint.DoesNotReturn &&
         !Hint.WeakImport && !Hint.ReturnedArgument &&
         !Hint.RuntimeObjCResultType && !Hint.ValueWitness &&
         !Hint.BooleanResult && !Hint.SwiftWitnessUndefDescriptor &&
         !Hint.FunctionParameterCall && !Hint.ImmutableNativeCall &&
         !Hint.SwiftWitnessFrame && !Hint.SwiftConsumedInput &&
         !Hint.SwiftOpaqueValue && !Hint.Virtual && Hint.Selector.empty() &&
         Hint.OwnerClass.empty() && !Hint.SelectorReferenceAddress &&
         Hint.BorrowedByteInputs.empty() &&
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
