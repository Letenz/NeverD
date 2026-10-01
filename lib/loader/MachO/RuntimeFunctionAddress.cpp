#include "neverd/loader/MachO/RuntimeFunctionAddress.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/DarwinRuntimeCalls.h"
#include "neverd/loader/ObjC/ObjCCallHints.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/loader/Swift/SwiftRuntimeCalls.h"

namespace neverd {
std::optional<SourceCallTypeHint>
runtimeCFunctionAddressHint(const BinaryImage &Image, va_t ImportSlot) {
  if (!isImmutableImageImportSlot(Image, ImportSlot))
    return std::nullopt;
  std::optional<SourceCallTypeHint> Declaration;
  for (auto Candidate : {objcRuntimeSourceCallHint(Image, ImportSlot),
                         swiftRuntimeSourceCallHint(Image, ImportSlot),
                         darwinRuntimeSourceCallHint(Image, ImportSlot)}) {
    if (!Candidate)
      continue;
    if (Declaration)
      return std::nullopt;
    Declaration = std::move(Candidate);
  }
  if (!Declaration || Declaration->TargetAddress != ImportSlot ||
      Declaration->TargetName.empty() || Declaration->WeakImport ||
      Declaration->DoesNotReturn || Declaration->Format ||
      Declaration->NilTerminated || Declaration->BooleanResult ||
      !Declaration->CanonicalBooleanInputs.empty() ||
      Declaration->ValueWitness || Declaration->AddressedFunctionABI)
    return std::nullopt;
  // Runtime calls may canonicalize register-specific veneers to an ordinary
  // operation. An address must retain the actual export's identity instead.
  llvm::StringRef Export(Image.DyldBindSlots.at(ImportSlot).Name);
  Export.consume_front("_");
  if (Export != Declaration->TargetName)
    return std::nullopt;
  const auto &Module = Image.DyldBindSlots.at(ImportSlot).Module;
  if (Declaration->CallKind == SourceCallTypeHint::Kind::SwiftRuntimeCall &&
      Module != "/usr/lib/swift/libswiftCore.dylib")
    return std::nullopt;
  if (Declaration->CallKind == SourceCallTypeHint::Kind::ObjCRuntimeCall) {
    const bool Block = Export == "_Block_copy" || Export == "_Block_release";
    if (Module !=
        (Block ? "/usr/lib/libSystem.B.dylib" : "/usr/lib/libobjc.A.dylib"))
      return std::nullopt;
  }
  const auto &Function = Declaration->Signature;
  std::string Error;
  if (Function.Convention != SourceFunctionTypeHint::ConventionKind::C ||
      Function.Architecture != Image.Arch || !Function.HasExplicitABI ||
      !validateSourceABI(Function, Error) ||
      !Function.ReturnComponents.empty() || Function.Parameters.size() > 64)
    return std::nullopt;
  auto Canonical = Function;
  if (!assignDarwinScalarSourceABI(Canonical, Image.Arch, Error) ||
      !equalSourceABIs(Function, Canonical))
    return std::nullopt;
  const auto Scalar = [](const TypeRef &Type, bool AllowVoid) {
    return Type && equalSourceTypes(Type, Type) &&
           ((AllowVoid && Type->Kind == NdTypeKind::Void) ||
            Type->Kind == NdTypeKind::Ptr || Type->Kind == NdTypeKind::Int ||
            Type->Kind == NdTypeKind::Float);
  };
  if (!Scalar(Function.ReturnType, true))
    return std::nullopt;
  std::vector<TypeRef> Parameters;
  for (const auto &Parameter : Function.Parameters) {
    if (Parameter.TheRole != SourceParameterTypeHint::Role::Ordinary ||
        !Parameter.Components.empty() || !Scalar(Parameter.Type, false))
      return std::nullopt;
    Parameters.push_back(Parameter.Type);
  }
  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::RuntimeCFunctionAddress;
  Hint.TargetAddress = ImportSlot;
  Hint.TargetName = Declaration->TargetName;
  Hint.AddressedFunctionABI = Function;
  Hint.Signature.Origin = Function.Origin;
  Hint.Signature.ReturnType = NdType::makePtr(
      NdType::makeFunc(Function.ReturnType, std::move(Parameters)));
  return assignDarwinScalarSourceABI(Hint.Signature, Image.Arch, Error)
             ? std::optional<SourceCallTypeHint>(std::move(Hint))
             : std::nullopt;
}
} // namespace neverd
