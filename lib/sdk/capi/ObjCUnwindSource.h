#ifndef NEVERD_SDK_CAPI_OBJCUNWINDSOURCE_H
#define NEVERD_SDK_CAPI_OBJCUNWINDSOURCE_H

#include "../../loader/MachO/DarwinRuntimeImport.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/loader/MachO/DarwinImportVeneer.h"
#include "neverd/loader/MachO/DarwinRuntimeCalls.h"
#include "neverd/loader/ObjC/ObjCCallHints.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace neverd::sdk {

// Typed void calls live in CallExpr. Untyped native results can instead
// occupy Val in an assignment or expression statement. Require the one
// storage location belonging to the statement, never an extra expression.
inline ExprPtr objcUnwindStatementCall(const HighStmt &Statement) {
  if (Statement.Kind == StmtKind::Call)
    return Statement.Dst || Statement.Val ? nullptr : Statement.CallExpr;
  if (Statement.Kind == StmtKind::Assign ||
      Statement.Kind == StmtKind::ExprStmt)
    return Statement.CallExpr ? nullptr : Statement.Val;
  return nullptr;
}

struct ObjCUnwindScalarSlice {
  uint8_t Offset = 0;
  uint8_t Bytes = 0;
  MedVar Root;
};
using ObjCUnwindScalarCopies =
    std::vector<std::pair<MedVar, ObjCUnwindScalarSlice>>;

inline bool objcUnwindSameScalarVariable(const MedVar &A, const MedVar &B) {
  return A == B && A.Size == B.Size && A.TheArch == B.TheArch &&
         A.RenameTag == B.RenameTag && A.RegOff == B.RegOff &&
         A.Provenance == B.Provenance && A.AddressOwnerVA == B.AddressOwnerVA;
}

// Only contiguous bytes of the original exception (or self) can certify a
// reconstructed pointer. Calls, loads, arithmetic, extensions and unknown
// byte values cannot supply any part of this identity.
inline std::optional<ObjCUnwindScalarSlice>
objcUnwindScalarSlice(const ExprPtr &Expression,
                      const ObjCUnwindScalarCopies &Copies, bool Exception,
                      size_t &Budget, unsigned Depth = 0) {
  if (!Expression || !Budget || Depth > 32 || !Expression->Type ||
      !Expression->Type->Size || Expression->Type->Size > 8 ||
      (Expression->Type->Kind != NdTypeKind::Int &&
       Expression->Type->Kind != NdTypeKind::Ptr) ||
      Expression->IntrinsicId != Intrinsic::None ||
      !Expression->IntrinsicOutputs.empty() || Expression->SourceCallHint ||
      Expression->MemoryOrdering != NdMemoryOrdering::None ||
      Expression->MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return std::nullopt;
  --Budget;
  const auto Bytes = static_cast<uint8_t>(Expression->Type->Size);
  if (Expression->Kind == ExprKind::Var && Expression->Operands.empty() &&
      Expression->Var.Size == Bytes) {
    if (Bytes == 8 && (Exception ? Expression->Var.Kind == MedVar::EHException
                                 : Expression->Var.Kind == MedVar::Param &&
                                       Expression->Var.Id == 0))
      return ObjCUnwindScalarSlice{0, 8, Expression->Var};
    for (const auto &[Variable, Slice] : Copies)
      if (objcUnwindSameScalarVariable(Expression->Var, Variable) &&
          Slice.Bytes == Bytes)
        return Slice;
    return std::nullopt;
  }
  if (Expression->Kind == ExprKind::Cast && Expression->Operands.size() == 1 &&
      Expression->CastTo &&
      equalSourceTypes(Expression->Type, Expression->CastTo)) {
    auto Slice = objcUnwindScalarSlice(Expression->Operands[0], Copies,
                                       Exception, Budget, Depth + 1);
    if (!Slice || Bytes > Slice->Bytes ||
        (Expression->Type->Kind == NdTypeKind::Ptr && Bytes != 8))
      return std::nullopt;
    Slice->Bytes = Bytes;
    return Slice;
  }
  if (Expression->Kind != ExprKind::BinOp || Expression->Operands.size() != 2)
    return std::nullopt;
  auto Left = objcUnwindScalarSlice(Expression->Operands[0], Copies, Exception,
                                    Budget, Depth + 1);
  const auto &Right = Expression->Operands[1];
  if (!Left || !Right)
    return std::nullopt;
  if (Expression->Op == NdOp::SUBBYTES && Right->Kind == ExprKind::Const &&
      Right->Operands.empty() && Right->Type &&
      Right->Type->Kind == NdTypeKind::Int && Right->Type->Size &&
      Right->Type->Size <= 8 && Right->IntrinsicId == Intrinsic::None &&
      Right->IntrinsicOutputs.empty() && !Right->SourceCallHint &&
      Right->MemoryOrdering == NdMemoryOrdering::None &&
      Right->MemoryAddressSpace == NdMemoryAddressSpace::Default &&
      (Right->ConstProvenance == ConstantAddressProvenance::Unknown ||
       Right->ConstProvenance == ConstantAddressProvenance::Scalar) &&
      Right->AddressOwnerVA == InvalidVA && Right->ConstVal <= Left->Bytes &&
      Bytes <= Left->Bytes - Right->ConstVal)
    return ObjCUnwindScalarSlice{
        static_cast<uint8_t>(Left->Offset + Right->ConstVal), Bytes,
        Left->Root};
  if (Expression->Op != NdOp::CONCAT)
    return std::nullopt;
  const auto Low =
      objcUnwindScalarSlice(Right, Copies, Exception, Budget, Depth + 1);
  if (!Low || !objcUnwindSameScalarVariable(Low->Root, Left->Root) ||
      Low->Offset + Low->Bytes != Left->Offset ||
      Low->Bytes + Left->Bytes != Bytes)
    return std::nullopt;
  return ObjCUnwindScalarSlice{Low->Offset, Bytes, Low->Root};
}

inline bool objcUnwindImportProvider(const BinaryImage &Image, va_t Slot,
                                     llvm::StringRef Providers) {
  const auto Bind = Image.DyldBindSlots.find(Slot);
  return Bind != Image.DyldBindSlots.end() &&
         darwinExportModuleMatches(Providers, Bind->second.Module) &&
         std::find(Image.DynInfo.NeededLibs.begin(),
                   Image.DynInfo.NeededLibs.end(),
                   Bind->second.Module) != Image.DynInfo.NeededLibs.end();
}

inline bool objcUnwindRuntimeTargetIs(const BinaryImage &Image, va_t Target,
                                      llvm::StringRef Name,
                                      llvm::StringRef Providers) {
  const auto Slot = darwinImportVeneerSlot(Image, Target);
  const auto Import = Slot ? darwinRuntimeImport(Image, *Slot) : std::nullopt;
  return Import && *Import == Name &&
         objcUnwindImportProvider(Image, *Slot, Providers);
}

// A symbol spelling or a cached hint cannot remove an ordinary fallthrough
// edge into an exceptional suffix. Reauthenticate the imported runtime call.
inline bool objcUnwindNoReturnTarget(const BinaryImage &Image, va_t Target) {
  const auto Slot = darwinImportVeneerSlot(Image, Target);
  if (!Slot)
    return false;
  const auto Runtime = darwinRuntimeSourceCallHint(Image, *Slot);
  const auto ObjC = objcRuntimeSourceCallHint(Image, *Slot);
  return (Runtime && Runtime->DoesNotReturn &&
          Runtime->TargetName == "__stack_chk_fail" &&
          objcUnwindImportProvider(Image, *Slot,
                                   "/usr/lib/libSystem.B.dylib|/usr/lib/system/"
                                   "libsystem_c.dylib")) ||
         (ObjC && ObjC->DoesNotReturn &&
          ObjC->TargetName == "objc_exception_throw" &&
          objcUnwindImportProvider(Image, *Slot, "/usr/lib/libobjc.A.dylib"));
}

} // namespace neverd::sdk
#endif
