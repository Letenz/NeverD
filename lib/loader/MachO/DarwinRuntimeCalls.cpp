#include "neverd/loader/MachO/DarwinRuntimeCalls.h"

#include "DarwinRuntimeImport.h"
#include "DarwinSourceDeclarations.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/DarwinImportVeneer.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include "llvm/ADT/StringRef.h"

#include <algorithm>

namespace neverd {

std::optional<SourceFunctionTypeHint>
darwinHFAImportVeneerSourceABI(const BinaryImage &Image, va_t Address) {
  if (Image.Arch != Arch::AArch64)
    return std::nullopt;
  const auto Slot = darwinImportVeneerSlot(Image, Address);
  if (!Slot || !isImmutableImageImportSlot(Image, *Slot))
    return std::nullopt;
  const auto Declaration = darwinRuntimeSourceCallHint(Image, *Slot);
  if (!Declaration || Declaration->WeakImport || Declaration->DoesNotReturn ||
      Declaration->Format || Declaration->NilTerminated ||
      Declaration->CallKind != SourceCallTypeHint::Kind::DarwinRuntimeCall ||
      Declaration->TargetAddress != *Slot)
    return std::nullopt;
  const auto &Provider = Image.DyldBindSlots.at(*Slot).Module;
  if (std::find(Image.DynInfo.NeededLibs.begin(),
                Image.DynInfo.NeededLibs.end(),
                Provider) == Image.DynInfo.NeededLibs.end())
    return std::nullopt;
  const auto &Signature = Declaration->Signature;
  std::string Error;
  if (Signature.Convention != SourceFunctionTypeHint::ConventionKind::C ||
      !Signature.ReturnType ||
      Signature.ReturnType->Kind != NdTypeKind::Struct ||
      Signature.ReturnComponents.size() < 2 ||
      Signature.ReturnComponents.size() > 4 ||
      !validateSourceABI(Signature, Error))
    return std::nullopt;
  const auto Members = sourceAggregateMembers(Signature.ReturnType);
  if (Members.size() != Signature.ReturnComponents.size())
    return std::nullopt;
  for (size_t I = 0; I != Members.size(); ++I)
    if (!Members[I].Type || Members[I].Type->Kind != NdTypeKind::Float ||
        Signature.ReturnComponents[I].Kind !=
            SourceABICarrierKind::FloatingRegister)
      return std::nullopt;
  auto Canonical = Signature;
  if (!assignDarwinFixedSourceABI(Canonical, Image.Arch, Error) ||
      !equalSourceABIs(Signature, Canonical))
    return std::nullopt;
  return Signature;
}

std::optional<SourceFunctionTypeHint>
darwinIndirectAffineTransformSignature(Arch Architecture,
                                       const std::string &Name) {
  const bool Matrix = Name == "CATransform3DScale";
  if (Architecture != Arch::AArch64 ||
      (Name != "CGContextConcatCTM" && Name != "CGAffineTransformTranslate" &&
       Name != "CGAffineTransformScale" && Name != "CGAffineTransformRotate" &&
       Name != "CGAffineTransformConcat" &&
       Name != "CGRectApplyAffineTransform" && !Matrix))
    return std::nullopt;
  SourceFunctionTypeHint Signature;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinSDK;
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  if (Name == "CGContextConcatCTM") {
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"context", Pointer}, {"transform", Pointer}};
  } else if (Name == "CGRectApplyAffineTransform") {
    // CGRect is a four-double HFA in d0..d3, independently of the six-double
    // transform's indirect x0 input. The result also occupies d0..d3.
    const auto Pair =
        NdType::makeStruct({NdType::makeFloat(8), NdType::makeFloat(8)});
    Signature.ReturnType = NdType::makeStruct({Pair, Pair});
    Signature.Parameters = {{"rect", Signature.ReturnType},
                            {"transform", Pointer}};
  } else {
    // The six-double affine and sixteen-double 3D transforms are not HFAs:
    // AAPCS64 passes the complete input via x0 and the result via the hidden
    // x8 pointer. Floating scalars use the independent d-register bank.
    // Source emission copies the input into its genuine record value.
    const auto Double = NdType::makeFloat(8);
    Signature.ReturnType =
        NdType::makeStruct(std::vector<TypeRef>(Matrix ? 16 : 6, Double));
    Signature.Parameters = {{"transform", Pointer}};
    if (Name == "CGAffineTransformConcat") {
      Signature.Parameters.push_back({"second_transform", Pointer});
    } else {
      Signature.Parameters.push_back({"first", Double});
      if (Name != "CGAffineTransformRotate")
        Signature.Parameters.push_back({"second", Double});
      if (Matrix)
        Signature.Parameters.push_back({"third", Double});
    }
  }
  std::string Diagnostic;
  if (!assignDarwinFixedSourceABI(Signature, Architecture, Diagnostic))
    return std::nullopt;
  return Signature;
}

uint16_t darwinIndirectAffineTransformInputBytes(Arch Architecture,
                                                 const std::string &Name) {
  const auto Signature =
      darwinIndirectAffineTransformSignature(Architecture, Name);
  if (!Signature)
    return 0;
  // An affine input remains 48 bytes even when its result is void or CGRect.
  // The larger matrix bridge returns the same complete record it accepts.
  return Signature->ReturnLocation.Kind ==
                 SourceABICarrierKind::IndirectResultPointer
             ? Signature->ReturnType->Size
             : 48;
}

std::optional<SourceFrameEffects>
darwinMatrixSourceFrameEffects(const BinaryImage &Image,
                               const SourceCallTypeHint &Binding) {
  const bool Rect = Binding.TargetName == "CGRectApplyAffineTransform";
  const bool Affine = Binding.TargetName == "CGAffineTransformMakeRotation" ||
                      Binding.TargetName == "CGAffineTransformMakeScale" ||
                      Binding.TargetName == "CGAffineTransformConcat" || Rect;
  const bool Matrix = Binding.TargetName == "CATransform3DMakeTranslation" ||
                      Binding.TargetName == "CATransform3DScale";
  if (Image.Arch != Arch::AArch64 ||
      Binding.CallKind != SourceCallTypeHint::Kind::DarwinRuntimeCall ||
      (!Affine && !Matrix) || Binding.WeakImport || Binding.DoesNotReturn ||
      Binding.Format || Binding.NilTerminated)
    return std::nullopt;
  const auto Expected =
      darwinRuntimeSourceCallHint(Image, Binding.TargetAddress);
  const auto Bind = Image.DyldBindSlots.find(Binding.TargetAddress);
  if (!Expected || Expected->WeakImport || Expected->DoesNotReturn ||
      Expected->CallKind != Binding.CallKind ||
      Expected->TargetName != Binding.TargetName ||
      Expected->ByteCount != Binding.ByteCount ||
      !equalSourceABIs(Expected->Signature, Binding.Signature) ||
      Bind == Image.DyldBindSlots.end() || Bind->second.WeakImport ||
      !darwinExportModuleMatches(
          Affine ? "/System/Library/Frameworks/CoreGraphics.framework/"
                   "CoreGraphics|/System/Library/Frameworks/CoreGraphics."
                   "framework/Versions/A/CoreGraphics"
                 : "/System/Library/Frameworks/QuartzCore.framework/QuartzCore|"
                   "/System/Library/Frameworks/QuartzCore.framework/Versions/A/"
                   "QuartzCore",
          Bind->second.Module) ||
      std::find(Image.DynInfo.NeededLibs.begin(),
                Image.DynInfo.NeededLibs.end(),
                Bind->second.Module) == Image.DynInfo.NeededLibs.end())
    return std::nullopt;
  // The exact SDK operations define every double in their six- or sixteen-
  // field records, with no padding or pointer contents on Darwin arm64. The
  // current declaration/bridge owns the physical ABI; an arbitrary record
  // return does not acquire this definite-write contract. Physical by-value
  // input pointers may be written, so invalidate each complete private copy.
  SourceFrameEffects Effects;
  Effects.InitializesIndirectResult = !Rect;
  if (Binding.TargetName == "CATransform3DScale" ||
      Binding.TargetName == "CGAffineTransformConcat" || Rect) {
    const auto Bytes =
        darwinIndirectAffineTransformInputBytes(Image.Arch, Binding.TargetName);
    if (Bytes != (Matrix ? 128 : 48))
      return std::nullopt;
    const unsigned FirstInput = Rect ? 1 : 0;
    Effects.WritableFrameParameters.emplace(FirstInput, Bytes);
    Effects.InitializedFrameParameters.insert(FirstInput);
    if (Binding.TargetName == "CGAffineTransformConcat") {
      Effects.WritableFrameParameters.emplace(1, Bytes);
      Effects.InitializedFrameParameters.insert(1);
    }
  }
  return sourceFrameEffectsMatchABI(Effects, Binding.Signature)
             ? std::optional(Effects)
             : std::nullopt;
}

std::optional<SourceCallTypeHint>
darwinCompilerRTSourceCallHint(const BinaryImage &Image, va_t TargetAddress) {
  constexpr llvm::StringLiteral SymbolName = "___isPlatformVersionAtLeast";
  if (Image.Format != BinaryFormat::MachO || Image.Bits != Bitness::Bits64 ||
      Image.IsRelocatable ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64) ||
      !Image.isCodeAddress(TargetAddress))
    return std::nullopt;
  size_t Matches = 0;
  for (const auto &Symbol : Image.Symbols) {
    if (Symbol.Name != SymbolName)
      continue;
    ++Matches;
    if (Symbol.Addr != TargetAddress || !Symbol.IsFunc)
      return std::nullopt;
  }
  if (Matches != 1)
    return std::nullopt;

  SourceCallTypeHint Result;
  Result.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
  Result.TargetAddress = TargetAddress;
  Result.TargetName = "__isPlatformVersionAtLeast";
  auto &Signature = Result.Signature;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinRuntime;
  Signature.ReturnType = NdType::makeInt(4, true);
  Signature.Parameters = {
      {"platform", NdType::makeInt(4, false)},
      {"major", NdType::makeInt(4, false)},
      {"minor", NdType::makeInt(4, false)},
      {"subminor", NdType::makeInt(4, false)},
  };
  std::string Diagnostic;
  if (!assignDarwinFixedSourceABI(Signature, Image.Arch, Diagnostic))
    return std::nullopt;
  return Result;
}

std::optional<SourceCallTypeHint>
darwinRuntimeSourceCallHint(const BinaryImage &Image, va_t ImportSlot) {
  // compiler-rt probes this optional libSystem entry before calling it. Keep
  // the exact weak linkage and fixed ABI so the recovered guard remains valid.
  if (const auto Weak = darwinWeakRuntimeImport(Image, ImportSlot);
      Weak && *Weak == "__availability_version_check") {
    const auto &Bind = Image.DyldBindSlots.at(ImportSlot);
    if (!darwinExportModuleMatches("/usr/lib/libSystem.B.dylib", Bind.Module))
      return std::nullopt;
    SourceCallTypeHint Result;
    Result.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
    Result.WeakImport = true;
    Result.TargetAddress = ImportSlot;
    Result.TargetName = "_availability_version_check";
    auto &Signature = Result.Signature;
    Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinSDK;
    Signature.ReturnType = NdType::makeInt(1, false);
    Signature.Parameters = {
        {"count", NdType::makeInt(4, false)},
        {"versions", NdType::makePtr(NdType::makeVoid())},
    };
    std::string Diagnostic;
    if (!assignDarwinFixedSourceABI(Signature, Image.Arch, Diagnostic))
      return std::nullopt;
    return Result;
  }

  const auto Import = darwinRuntimeImport(Image, ImportSlot);
  if (!Import)
    return darwinDeclaredSourceCallHint(Image, ImportSlot);
  llvm::StringRef Name(*Import);
  if (!Name.consume_front("_"))
    return std::nullopt;

  // unwind_itanium.h declares one exception-record pointer and a void result.
  // libunwind resumes phase two or aborts; this call never returns normally.
  // https://github.com/llvm/llvm-project/blob/main/libunwind/src/UnwindLevel1.c
  if (Name == "_Unwind_Resume") {
    const auto Bind = Image.DyldBindSlots.find(ImportSlot);
    if (Bind == Image.DyldBindSlots.end() ||
        !darwinExportModuleMatches(
            "/usr/lib/libSystem.B.dylib|/usr/lib/system/libunwind.dylib",
            Bind->second.Module) ||
        std::find(Image.DynInfo.NeededLibs.begin(),
                  Image.DynInfo.NeededLibs.end(),
                  Bind->second.Module) == Image.DynInfo.NeededLibs.end())
      return std::nullopt;
    SourceCallTypeHint Result;
    Result.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
    Result.TargetAddress = ImportSlot;
    Result.TargetName = Name.str();
    Result.DoesNotReturn = true;
    auto &Signature = Result.Signature;
    Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinSDK;
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"exception", NdType::makePtr(NdType::makeVoid())}};
    std::string Diagnostic;
    if (!assignDarwinScalarSourceABI(Signature, Image.Arch, Diagnostic))
      return std::nullopt;
    return Result;
  }

  // These non-HFA transforms use an indirect input pointer under AAPCS64.
  // Keep that physical carrier in the source hint; the C emitter copies the
  // complete record into a genuine by-value argument before calling its SDK.
  // Other record imports continue through the ordinary declaration catalog.
  if (Name == "CGContextConcatCTM" || Name == "CGAffineTransformTranslate" ||
      Name == "CGAffineTransformScale" || Name == "CGAffineTransformRotate" ||
      Name == "CGAffineTransformConcat" || Name == "CATransform3DScale" ||
      Name == "CGRectApplyAffineTransform") {
    const auto Bind = Image.DyldBindSlots.find(ImportSlot);
    const auto Signature =
        darwinIndirectAffineTransformSignature(Image.Arch, Name.str());
    const bool Matrix = Name == "CATransform3DScale";
    if (!Signature || Bind == Image.DyldBindSlots.end() ||
        !darwinExportModuleMatches(
            Matrix
                ? "/System/Library/Frameworks/QuartzCore.framework/QuartzCore|"
                  "/System/Library/Frameworks/QuartzCore.framework/Versions/A/"
                  "QuartzCore"
                : "/System/Library/Frameworks/CoreGraphics.framework/"
                  "CoreGraphics|/System/Library/Frameworks/CoreGraphics."
                  "framework/Versions/A/CoreGraphics",
            Bind->second.Module))
      return std::nullopt;
    SourceCallTypeHint Result;
    Result.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
    Result.TargetAddress = ImportSlot;
    Result.TargetName = Name.str();
    Result.ByteCount =
        darwinIndirectAffineTransformInputBytes(Image.Arch, Name.str());
    Result.Signature = *Signature;
    return Result;
  }

  // SCNetworkReachability.h declares these fixed C calls. Keep the
  // callback's complete input shape even when a caller passes null: other
  // callers can install a real callback and context through the same import.
  if (Name == "SCNetworkReachabilityCreateWithName" ||
      Name == "SCNetworkReachabilitySetCallback" ||
      Name == "SCNetworkReachabilitySetDispatchQueue") {
    const auto Bind = Image.DyldBindSlots.find(ImportSlot);
    if (Bind == Image.DyldBindSlots.end() ||
        !darwinExportModuleMatches(
            "/System/Library/Frameworks/SystemConfiguration.framework/"
            "SystemConfiguration",
            Bind->second.Module))
      return std::nullopt;
    SourceCallTypeHint Result;
    Result.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
    Result.TargetAddress = ImportSlot;
    Result.TargetName = Name.str();
    auto &Signature = Result.Signature;
    Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinSDK;
    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    if (Name == "SCNetworkReachabilityCreateWithName") {
      Signature.ReturnType = Pointer;
      Signature.Parameters = {{"allocator", Pointer}, {"nodename", Pointer}};
    } else if (Name == "SCNetworkReachabilitySetCallback") {
      Signature.ReturnType = NdType::makeInt(1, false); // Boolean
      Signature.Parameters.push_back({"target", Pointer});
      const auto Callback = NdType::makePtr(NdType::makeFunc(
          NdType::makeVoid(), {Pointer, NdType::makeInt(4, false), Pointer}));
      Signature.Parameters.push_back({"callout", Callback});
      Signature.Parameters.push_back({"context", Pointer});
    } else {
      Signature.ReturnType = NdType::makeInt(1, false); // Boolean
      Signature.Parameters = {{"target", Pointer}, {"queue", Pointer}};
    }
    std::string Diagnostic;
    if (!assignDarwinFixedSourceABI(Signature, Image.Arch, Diagnostic))
      return std::nullopt;
    return Result;
  }

  // UIKit declares these fixed source contracts. The command-line-tools SDK
  // used for DarwinSourceDeclarations.inc has no UIKit headers or binary, so
  // retain the public contracts at the same exact symbol/provider boundary as
  // the UIKit external storage below.
  // https://developer.apple.com/documentation/uikit/nsstringfromcgsize
  // https://developer.apple.com/documentation/uikit/uigraphicsbeginimagecontext(_:)
  const bool UIKitFixedFunction =
      Name == "NSStringFromCGSize" || Name == "CGSizeFromString" ||
      Name == "UIAccessibilityPostNotification" ||
      Name == "UIGraphicsBeginImageContext" ||
      Name == "UIGraphicsBeginImageContextWithOptions" ||
      Name == "UIGraphicsGetCurrentContext" ||
      Name == "UIGraphicsGetImageFromCurrentImageContext" ||
      Name == "UIGraphicsEndImageContext";
  if (UIKitFixedFunction) {
    const auto Bind = Image.DyldBindSlots.find(ImportSlot);
    if (Image.Arch != Arch::AArch64 || Bind == Image.DyldBindSlots.end() ||
        !darwinExportModuleMatches(
            "/System/Library/Frameworks/UIKit.framework/UIKit",
            Bind->second.Module))
      return std::nullopt;
    SourceCallTypeHint Result;
    Result.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
    Result.TargetAddress = ImportSlot;
    Result.TargetName = Name.str();
    auto &Signature = Result.Signature;
    Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinSDK;
    const bool ReturnsPointer =
        Name == "NSStringFromCGSize" || Name == "UIGraphicsGetCurrentContext" ||
        Name == "UIGraphicsGetImageFromCurrentImageContext";
    Signature.ReturnType = ReturnsPointer ? NdType::makePtr(NdType::makeVoid())
                                          : NdType::makeVoid();
    if (Name == "NSStringFromCGSize" || Name == "UIGraphicsBeginImageContext" ||
        Name == "UIGraphicsBeginImageContextWithOptions") {
      const auto Size =
          NdType::makeStruct({NdType::makeFloat(8), NdType::makeFloat(8)});
      if (!Size)
        return std::nullopt;
      Signature.Parameters = {{"size", Size}};
    }
    if (Name == "CGSizeFromString") {
      // The same complete device/simulator SDK ASTs and UIKit reexport map
      // establish CGSize(NSString *). The shared ABI assigns both FP results.
      Signature.ReturnType =
          NdType::makeStruct({NdType::makeFloat(8), NdType::makeFloat(8)});
      if (!Signature.ReturnType)
        return std::nullopt;
      Signature.Parameters = {{"string", NdType::makePtr(NdType::makeVoid())}};
    }
    if (Name == "UIAccessibilityPostNotification") {
      // Device and simulator SDK ASTs agree on void(uint32_t, id nullable).
      // The notification is an integer value; its argument may be nil.
      Signature.Parameters = {
          {"notification", NdType::makeInt(4, false)},
          {"argument", NdType::makePtr(NdType::makeVoid())},
      };
    }
    if (Name == "UIGraphicsBeginImageContextWithOptions") {
      // Complete Xcode 26.5 device and arm64 simulator ASTs agree on
      // void(CGSize, BOOL=bool, CGFloat=double). Their UIKit TBDs reexport
      // this exact symbol from the embedded UIKitCore export map.
      // SDK evidence: NeverSight/NeverD Actions run 35648792995.
      Signature.Parameters.push_back({"opaque", NdType::makeInt(1, false)});
      Signature.Parameters.push_back({"scale", NdType::makeFloat(8)});
    }
    std::string Diagnostic;
    if (!assignDarwinFixedSourceABI(Signature, Image.Arch, Diagnostic))
      return std::nullopt;
    return Result;
  }

  // dispatch_once_f has a fixed callback contract that the generated Clang
  // encoding can only spell as the intentionally unsupported opaque `^?`.
  // Preserve the complete public prototype here rather than accepting unknown
  // callback signatures throughout the declaration parser.
  if (Name == "dispatch_once_f") {
    const auto Bind = Image.DyldBindSlots.find(ImportSlot);
    if (Bind == Image.DyldBindSlots.end() ||
        !darwinExportModuleMatches(
            "/usr/lib/libSystem.B.dylib|/usr/lib/system/libdispatch.dylib",
            Bind->second.Module))
      return std::nullopt;
    SourceCallTypeHint Result;
    Result.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
    Result.TargetAddress = ImportSlot;
    Result.TargetName = Name.str();
    auto &Signature = Result.Signature;
    Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinSDK;
    Signature.ReturnType = NdType::makeVoid();
    const auto Context = NdType::makePtr(NdType::makeVoid());
    const auto Callback =
        NdType::makePtr(NdType::makeFunc(NdType::makeVoid(), {Context}));
    Signature.Parameters = {
        {"predicate", NdType::makePtr(NdType::makeInt(8, true))},
        {"context", Context},
        {"function", Callback},
    };
    std::string Diagnostic;
    if (!assignDarwinFixedSourceABI(Signature, Image.Arch, Diagnostic))
      return std::nullopt;
    return Result;
  }

  // dispatch_queue_set_specific has a fixed destructor callback contract that
  // the generated Clang encoding spells as the intentionally unsupported
  // opaque `^?`. Keep the public void (*)(void *) prototype at the same exact
  // libdispatch export boundary instead of accepting unknown callbacks in the
  // declaration parser.
  if (Name == "dispatch_queue_set_specific") {
    const auto Bind = Image.DyldBindSlots.find(ImportSlot);
    if (Bind == Image.DyldBindSlots.end() ||
        !darwinExportModuleMatches(
            "/usr/lib/libSystem.B.dylib|/usr/lib/system/libdispatch.dylib",
            Bind->second.Module))
      return std::nullopt;
    SourceCallTypeHint Result;
    Result.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
    Result.TargetAddress = ImportSlot;
    Result.TargetName = Name.str();
    auto &Signature = Result.Signature;
    Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinSDK;
    Signature.ReturnType = NdType::makeVoid();
    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    const auto Destructor =
        NdType::makePtr(NdType::makeFunc(NdType::makeVoid(), {Pointer}));
    Signature.Parameters = {
        {"queue", Pointer},
        {"key", Pointer},
        {"context", Pointer},
        {"destructor", Destructor},
    };
    std::string Diagnostic;
    if (!assignDarwinScalarSourceABI(Signature, Image.Arch, Diagnostic))
      return std::nullopt;
    return Result;
  }

  // The public lock routines use one pointer to stable, opaque lock storage.
  // Call the real platform implementation, including its ownership checks.
  // https://github.com/apple-oss-distributions/libplatform/blob/main/include/os/lock.h
  const bool TryLock = Name == "os_unfair_lock_trylock";
  const bool StackFailure = Name == "__stack_chk_fail";
  // Block.h declares these fixed C ABIs. Ownership work stays in the real
  // runtime and, for captured objects, the recovered descriptor helpers.
  const bool BlockCopy = Name == "_Block_copy";
  const bool BlockRelease = Name == "_Block_release";
  const bool BlockAssign = Name == "_Block_object_assign";
  const bool BlockDispose = Name == "_Block_object_dispose";
  const bool BlockRuntime =
      BlockCopy || BlockRelease || BlockAssign || BlockDispose;
  if (!StackFailure && !BlockRuntime && !TryLock &&
      Name != "os_unfair_lock_lock" && Name != "os_unfair_lock_unlock" &&
      Name != "os_unfair_lock_assert_owner" &&
      Name != "os_unfair_lock_assert_not_owner")
    return darwinDeclaredSourceCallHint(Image, ImportSlot);
  SourceCallTypeHint Result;
  Result.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
  Result.DoesNotReturn = StackFailure;
  Result.TargetAddress = ImportSlot;
  Result.TargetName = Name.str();
  auto &Signature = Result.Signature;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinRuntime;
  Signature.ReturnType =
      TryLock ? NdType::makeInt(1, false) : NdType::makeVoid();
  if (BlockRuntime) {
    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    if (BlockCopy)
      Signature.ReturnType = Pointer;
    if (BlockAssign)
      Signature.Parameters.push_back({"destination", Pointer});
    Signature.Parameters.push_back({"object", Pointer});
    if (BlockAssign || BlockDispose)
      Signature.Parameters.push_back({"flags", NdType::makeInt(4, true)});
  } else if (!StackFailure)
    Signature.Parameters = {{"lock", NdType::makePtr(NdType::makeVoid())}};
  std::string Diagnostic;
  if (!assignDarwinScalarSourceABI(Signature, Image.Arch, Diagnostic))
    return std::nullopt;
  return Result;
}

std::optional<SourceCallTypeHint>
darwinRuntimeGlobalAddressHint(const BinaryImage &Image, va_t ImportSlot) {
  const auto Import = darwinRuntimeImport(Image, ImportSlot);
  if (!Import)
    return darwinDeclaredSourceGlobalAddressHint(Image, ImportSlot);

  // These UIKit constants are public external storage, rather than functions
  // or implementation-owned objects. The base Darwin data catalog excludes
  // UIKit, so retain the same exact symbol/provider proof for independently
  // verified supplemental declarations here.
  // https://developer.apple.com/documentation/uikit/uiapplicationdidreceivememorywarningnotification
  llvm::StringRef FrameworkData;
  const auto Bind = Image.DyldBindSlots.find(ImportSlot);
  auto MatchFrameworkData = [&](llvm::StringRef Name, llvm::StringRef Module) {
    if (Import->starts_with("_") && Import->drop_front() == Name &&
        Bind != Image.DyldBindSlots.end() &&
        darwinExportModuleMatches(Module, Bind->second.Module))
      FrameworkData = Name;
  };
  for (llvm::StringRef Name :
       {"UIApplicationDidReceiveMemoryWarningNotification",
        "UIApplicationWillTerminateNotification", "UIBackgroundTaskInvalid",
        "UIAccessibilityTraitButton", "UIEdgeInsetsZero",
        "UIViewNoIntrinsicMetric"})
    MatchFrameworkData(Name,
                       "/System/Library/Frameworks/UIKit.framework/UIKit");
  // Both complete Xcode 26.5 ARM64 SDK ASTs declare this notification as
  // external, non-TLS NSString pointer storage. Keep the original load.
  // https://developer.apple.com/documentation/uikit/uiapplication/didenterbackgroundnotification
  if (Image.Arch == Arch::AArch64)
    MatchFrameworkData("UIApplicationDidEnterBackgroundNotification",
                       "/System/Library/Frameworks/UIKit.framework/UIKit");
  // UIAccessibilityNotifications is uint32_t in both complete ARM64 SDK
  // ASTs. Bind the external const object's address and keep the native load;
  // neither the notification value nor pointer-sized contents are invented.
  if (Image.Arch == Arch::AArch64)
    MatchFrameworkData("UIAccessibilityAnnouncementNotification",
                       "/System/Library/Frameworks/UIKit.framework/UIKit");
  // Complete Xcode 26.5 ARM64 device/simulator ASTs declare these UIKit
  // attributed-string keys as external, non-TLS NSString *const storage.
  // Both UIKit TBD reexport maps authenticate the exact linker identities.
  // Bind only their addresses; retain native loads and object identities.
  if (Image.Arch == Arch::AArch64)
    for (llvm::StringRef Name :
         {"NSBackgroundColorAttributeName", "NSBaselineOffsetAttributeName",
          "NSFontAttributeName", "NSForegroundColorAttributeName",
          "NSKernAttributeName", "NSLigatureAttributeName",
          "NSLinkAttributeName", "NSParagraphStyleAttributeName",
          "NSStrikethroughStyleAttributeName", "NSStrokeColorAttributeName",
          "NSStrokeWidthAttributeName", "NSUnderlineStyleAttributeName"})
      MatchFrameworkData(Name,
                         "/System/Library/Frameworks/UIKit.framework/UIKit");
  // Complete device/simulator ASTs and fresh Mac Catalyst compiler probes
  // declare external, non-TLS NSString *const storage. Bind its address and
  // retain the load; the name does not establish an NSString value or layout.
  if (Image.Arch == Arch::AArch64)
    MatchFrameworkData("UIContentSizeCategoryLarge",
                       "/System/Library/Frameworks/UIKit.framework/UIKit");
  // CIContext.h imports OpenGLES on iOS, unavailable in the CLT SDK used by
  // the generated catalog. Complete Xcode 26.5 iPhoneOS and arm64 simulator
  // ASTs agree that these are external, non-TLS NSString pointer objects.
  // The CoreImage export authenticates their storage, not their contents.
  // https://developer.apple.com/documentation/coreimage/kcicontextpriorityrequestlow
  // https://developer.apple.com/documentation/coreimage/kcicontextusesoftwarerenderer
  if (Image.Arch == Arch::AArch64)
    for (llvm::StringRef Name :
         {"kCIContextPriorityRequestLow", "kCIContextUseSoftwareRenderer"})
      MatchFrameworkData(
          Name, "/System/Library/Frameworks/CoreImage.framework/CoreImage|"
                "/System/Library/Frameworks/CoreImage.framework/Versions/A/"
                "CoreImage");
  // Swift's inlinable collection implementations take these singletons'
  // addresses, making their external storage identities part of the
  // stdlib/runtime ABI. Apple Swift 6.1.2 emits all three as external globals.
  // https://github.com/swiftlang/swift/blob/main/stdlib/public/core/ContiguousArrayBuffer.swift
  // https://github.com/swiftlang/swift/blob/main/stdlib/public/core/Dictionary.swift
  // https://github.com/swiftlang/swift/blob/main/stdlib/public/core/Set.swift
  llvm::StringRef SwiftEmptyCollection;
  if (*Import == "__swiftEmptyArrayStorage")
    SwiftEmptyCollection = "_swiftEmptyArrayStorage";
  else if (*Import == "__swiftEmptyDictionarySingleton")
    SwiftEmptyCollection = "_swiftEmptyDictionarySingleton";
  else if (*Import == "__swiftEmptySetSingleton")
    SwiftEmptyCollection = "_swiftEmptySetSingleton";
  const bool SwiftEmptyStorage =
      !SwiftEmptyCollection.empty() && Bind != Image.DyldBindSlots.end() &&
      darwinExportModuleMatches("/usr/lib/swift/libswiftCore.dylib",
                                Bind->second.Module);
  // Swift class dispatch loads the exported runtime mask as data. Preserve
  // the import cell and its native load instead of materializing its value.
  const bool SwiftIsaMask =
      Image.Arch == Arch::AArch64 && *Import == "_swift_isaMask" &&
      Bind != Image.DyldBindSlots.end() &&
      darwinExportModuleMatches("/usr/lib/swift/libswiftCore.dylib",
                                Bind->second.Module);
  // Compiler .self queries prove these are external non-TLS data addresses,
  // not metadata accessors. Exact per-architecture exports authenticate the
  // provider; neither a mangled-name suffix nor metadata contents are guessed.
  static constexpr struct {
    const char *Name;
    const char *AArch64Modules;
    const char *X64Modules;
  } SwiftData[] = {
#include "SwiftSourceDataDeclarations.inc"
  };
  llvm::StringRef SwiftMetadata;
  if (Import->starts_with("_") && Bind != Image.DyldBindSlots.end())
    for (const auto &D : SwiftData)
      if (Import->drop_front() == D.Name &&
          darwinExportModuleMatches(
              Image.Arch == Arch::AArch64 ? D.AArch64Modules : D.X64Modules,
              Bind->second.Module))
        SwiftMetadata = D.Name;
  // Compiler evidence names ordinary external storage. A current TLS slot
  // supplies a different runtime access contract, even with the same spelling.
  if (!SwiftMetadata.empty() || !FrameworkData.empty())
    if (const auto *Section = Image.getSectionFor(ImportSlot))
      switch (Section->Type & llvm::MachO::SECTION_TYPE) {
      case llvm::MachO::S_THREAD_LOCAL_REGULAR:
      case llvm::MachO::S_THREAD_LOCAL_ZEROFILL:
      case llvm::MachO::S_THREAD_LOCAL_VARIABLES:
      case llvm::MachO::S_THREAD_LOCAL_VARIABLE_POINTERS:
      case llvm::MachO::S_THREAD_LOCAL_INIT_FUNCTION_POINTERS:
        return std::nullopt;
      default:
        break;
      }
  if (FrameworkData.empty() && !SwiftEmptyStorage && !SwiftIsaMask &&
      SwiftMetadata.empty() && *Import != "___stack_chk_guard")
    return darwinDeclaredSourceGlobalAddressHint(Image, ImportSlot);

  SourceCallTypeHint Result;
  Result.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeGlobalAddress;
  Result.TargetAddress = ImportSlot;
  if (!FrameworkData.empty()) {
    Result.TargetName = FrameworkData.str();
    Result.Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinSDK;
    Result.Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
  } else if (SwiftEmptyStorage || SwiftIsaMask || !SwiftMetadata.empty()) {
    Result.TargetName = (SwiftEmptyStorage ? SwiftEmptyCollection
                         : SwiftIsaMask    ? llvm::StringRef("swift_isaMask")
                                           : SwiftMetadata)
                            .str();
    Result.Signature.Origin = SourceFunctionTypeHint::OriginKind::SwiftRuntime;
    Result.Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
  } else {
    // Darwin exports long __stack_chk_guard[8]. Bind its address and preserve
    // every native memory access; a guard is not a constant or private storage.
    // https://github.com/apple-oss-distributions/Libc/blob/main/sys/OpenBSD/stack_protector.c
    Result.TargetName = "__stack_chk_guard";
    Result.Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinRuntime;
    Result.Signature.ReturnType = NdType::makePtr(NdType::makeInt(8, true));
  }
  std::string Diagnostic;
  if (!assignDarwinScalarSourceABI(Result.Signature, Image.Arch, Diagnostic))
    return std::nullopt;
  return Result;
}
} // namespace neverd
