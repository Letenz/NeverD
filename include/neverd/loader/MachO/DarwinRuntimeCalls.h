#ifndef NEVERD_LOADER_MACHO_DARWINRUNTIMECALLS_H
#define NEVERD_LOADER_MACHO_DARWINRUNTIMECALLS_H

#include "neverd/ir/SourceCallTypeHint.h"
#include "neverd/ir/low/SourceFrameEffects.h"

#include <optional>

namespace neverd {
struct BinaryImage;

/// A source binding for an exact platform import with a declared Darwin C ABI.
/// Storage, checks, and runtime calls remain observable.
std::optional<SourceCallTypeHint>
darwinRuntimeSourceCallHint(const BinaryImage &Image, va_t ImportSlot);

/// Canonical arm64 carrier signature for the supported CoreGraphics affine
/// bridges and QuartzCore CATransform3DScale. The input transform is an
/// indirect pointer; transform results retain their complete hidden result
/// storage, while CGRect input/results use four floating registers. This
/// describes the bridge ABI, without authenticating a symbol or provider.
std::optional<SourceFunctionTypeHint>
darwinIndirectAffineTransformSignature(Arch Architecture,
                                       const std::string &Name);

/// Complete indirect input extent for the canonical bridge above. Returns
/// zero for unsupported names or architectures; result size is independent.
uint16_t darwinIndirectAffineTransformInputBytes(Arch Architecture,
                                                 const std::string &Name);

/// Exact strong QuartzCore matrix producers initialize a complete pointer-free
/// result. Scale consumes an initialized by-value input copy and may write it.
/// This authenticates the import/ABI contract, not a LowIR call occurrence.
std::optional<SourceFrameEffects>
darwinMatrixSourceFrameEffects(const BinaryImage &Image,
                               const SourceCallTypeHint &Binding);

/// A linked compiler-rt builtin with a stable public C contract. Unlike an
/// imported runtime call, TargetAddress is the exact local function entry.
/// The unique function symbol is required so ordinary native helpers never
/// acquire a declaration from an inferred name.
std::optional<SourceCallTypeHint>
darwinCompilerRTSourceCallHint(const BinaryImage &Image, va_t TargetAddress);

struct DarwinBlockParameterContract {
  enum class Lifetime { NonEscaping, Copied };
  SourceFunctionTypeHint Signature;
  Lifetime Storage;
};

/// Exact imported block consumer with a compiler-derived callback ABI and
/// either a noescape attribute or an audited runtime copying contract.
std::optional<DarwinBlockParameterContract>
darwinBlockParameterContract(const BinaryImage &Image, va_t ImportSlot,
                             unsigned Parameter);

/// A compiler-declared block parameter whose references and copies cannot
/// survive the imported call. Includes the complete fixed callback ABI; this
/// is not a read-only memory contract and never describes function pointers.
std::optional<SourceFunctionTypeHint>
darwinNonEscapingBlockSignature(const BinaryImage &Image, va_t ImportSlot,
                                unsigned Parameter);

struct DarwinFormatDeclaration {
  SourceFunctionTypeHint Signature;
  std::string Name;
  unsigned FormatParameter = 0;
  SourceCallTypeHint::FormatSyntax Syntax =
      SourceCallTypeHint::FormatSyntax::NSString;
};

/// Fixed prefix and language-specific format attribute of an exact C import.
/// This declaration alone cannot bind a variadic call's actual arguments.
std::optional<DarwinFormatDeclaration>
darwinRuntimeFormatDeclaration(const BinaryImage &Image, va_t ImportSlot);

std::optional<SourceCallTypeHint>
darwinFormattedSourceCallHint(const BinaryImage &Image, va_t ImportSlot,
                              va_t FormatAddress);

/// Address supplied by an exact data import with a known platform contract.
std::optional<SourceCallTypeHint>
darwinRuntimeGlobalAddressHint(const BinaryImage &Image, va_t ImportSlot);
} // namespace neverd

#endif
