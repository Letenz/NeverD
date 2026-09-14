#include "neverd/loader/MachO/DarwinRuntimeCalls.h"

#include "DarwinRuntimeImport.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringRef.h"

namespace neverd {
std::optional<SourceCallTypeHint>
darwinRuntimeSourceCallHint(const BinaryImage &Image, va_t ImportSlot) {
  const auto Import = darwinRuntimeImport(Image, ImportSlot);
  if (!Import)
    return std::nullopt;
  llvm::StringRef Name(*Import);
  if (!Name.consume_front("_"))
    return std::nullopt;

  // The public lock routines use one pointer to stable, opaque lock storage.
  // Call the real platform implementation, including its ownership checks.
  // https://github.com/apple-oss-distributions/libplatform/blob/main/include/os/lock.h
  const bool TryLock = Name == "os_unfair_lock_trylock";
  const bool StackFailure = Name == "__stack_chk_fail";
  if (!StackFailure && !TryLock && Name != "os_unfair_lock_lock" &&
      Name != "os_unfair_lock_unlock" &&
      Name != "os_unfair_lock_assert_owner" &&
      Name != "os_unfair_lock_assert_not_owner")
    return std::nullopt;
  SourceCallTypeHint Result;
  Result.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
  Result.DoesNotReturn = StackFailure;
  Result.TargetAddress = ImportSlot;
  Result.TargetName = Name.str();
  auto &Signature = Result.Signature;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinRuntime;
  Signature.ReturnType =
      TryLock ? NdType::makeInt(1, false) : NdType::makeVoid();
  if (!StackFailure)
    Signature.Parameters = {{"lock", NdType::makePtr(NdType::makeVoid())}};
  std::string Diagnostic;
  if (!assignDarwinScalarSourceABI(Signature, Image.Arch, Diagnostic))
    return std::nullopt;
  return Result;
}

std::optional<SourceCallTypeHint>
darwinRuntimeGlobalAddressHint(const BinaryImage &Image, va_t ImportSlot) {
  const auto Import = darwinRuntimeImport(Image, ImportSlot);
  if (!Import || *Import != "___stack_chk_guard")
    return std::nullopt;
  // Darwin exports long __stack_chk_guard[8]. Bind its address and preserve
  // every native memory access; a guard is not a constant or private storage.
  // https://github.com/apple-oss-distributions/Libc/blob/main/sys/OpenBSD/stack_protector.c
  SourceCallTypeHint Result;
  Result.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeGlobalAddress;
  Result.TargetAddress = ImportSlot;
  Result.TargetName = "__stack_chk_guard";
  Result.Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinRuntime;
  Result.Signature.ReturnType = NdType::makePtr(NdType::makeInt(8, true));
  std::string Diagnostic;
  if (!assignDarwinScalarSourceABI(Result.Signature, Image.Arch, Diagnostic))
    return std::nullopt;
  return Result;
}
} // namespace neverd
