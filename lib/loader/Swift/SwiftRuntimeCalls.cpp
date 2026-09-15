#include "neverd/loader/Swift/SwiftRuntimeCalls.h"

#include "../MachO/DarwinRuntimeImport.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <iterator>

namespace neverd {
namespace {
struct SwiftCDeclaration {
  const char *Name;
  const char *Signature;
  bool DoesNotReturn;
};
#include "SwiftCDeclarations.inc"

bool declaredCABI(llvm::StringRef Name, SourceCallTypeHint &Hint) {
  const auto *Found = std::lower_bound(
      std::begin(SwiftCDeclarations), std::end(SwiftCDeclarations), Name,
      [](const SwiftCDeclaration &Row, llvm::StringRef Value) {
        return llvm::StringRef(Row.Name) < Value;
      });
  if (Found == std::end(SwiftCDeclarations) || Name != Found->Name)
    return false;
  const auto Type = [](char Encoding) -> TypeRef {
    switch (Encoding) {
    case 'v':
      return NdType::makeVoid();
    case 'p':
      return NdType::makePtr(NdType::makeVoid());
    case 'z':
      return NdType::makeInt(8, false);
    default:
      return {};
    }
  };
  llvm::StringRef Encoding(Found->Signature);
  if (Encoding.empty() || Encoding.size() > 17)
    return false;
  auto &Signature = Hint.Signature;
  Signature.ReturnType = Type(Encoding.front());
  if (!Signature.ReturnType)
    return false;
  Signature.Parameters.clear();
  for (char Code : Encoding.drop_front()) {
    const auto Parameter = Type(Code);
    if (!Parameter || Parameter->Kind == NdTypeKind::Void)
      return false;
    Signature.Parameters.push_back(
        {"arg" + std::to_string(Signature.Parameters.size()), Parameter});
  }
  Hint.DoesNotReturn = Found->DoesNotReturn;
  return !Hint.DoesNotReturn || Signature.ReturnType->Kind == NdTypeKind::Void;
}
} // namespace

std::optional<SourceCallTypeHint>
swiftRuntimeSourceCallHint(const BinaryImage &Image, va_t ImportSlot) {
  const auto Import = darwinRuntimeImport(Image, ImportSlot);
  if (!Import)
    return std::nullopt;
  llvm::StringRef Name(*Import);
  if (!Name.consume_front("_"))
    return std::nullopt;

  SourceCallTypeHint Result;
  Result.CallKind = SourceCallTypeHint::Kind::SwiftRuntimeCall;
  Result.TargetAddress = ImportSlot;
  Result.TargetName = Name.str();
  auto &Signature = Result.Signature;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::SwiftRuntime;
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  const bool ReportInFile =
      Name == "_swift_stdlib_reportFatalErrorInFile" ||
      Name == "_swift_stdlib_reportUnimplementedInitializerInFile";
  const bool ReportInitializer =
      Name == "_swift_stdlib_reportUnimplementedInitializer" ||
      Name == "_swift_stdlib_reportUnimplementedInitializerInFile";
  if (ReportInFile || ReportInitializer ||
      Name == "_swift_stdlib_reportFatalError") {
    // SwiftShims/AssertionReporting.h declares ordinary C calls. Reporting
    // returns; the compiler emits a separate trap. Each string is consumed
    // through a bounded precision and copied into the diagnostic message.
    const auto Bytes = NdType::makePtr(NdType::makeInt(1, false));
    const auto Length = NdType::makeInt(4, true);
    const auto Unsigned = NdType::makeInt(4, false);
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"first", Bytes},
                            {"first_length", Length},
                            {"second", Bytes},
                            {"second_length", Length}};
    Result.BorrowedByteInputs = {{0, 1}, {2, 3}};
    if (ReportInFile) {
      Signature.Parameters.push_back({"file", Bytes});
      Signature.Parameters.push_back({"file_length", Length});
      Signature.Parameters.push_back({"line", Unsigned});
      Result.BorrowedByteInputs.push_back({4, 5});
      if (ReportInitializer)
        Signature.Parameters.push_back({"column", Unsigned});
    }
    Signature.Parameters.push_back({"flags", Unsigned});
    std::string Diagnostic;
    if (!assignDarwinScalarSourceABI(Signature, Image.Arch, Diagnostic))
      return std::nullopt;
    return Result;
  }
  // These entries have C_CC declarations in the Swift runtime ABI. In
  // particular, Direct refcount entries use SwiftDirectRR_CC and must not be
  // accepted by prefix matching. Keep calls and their memory effects intact.
  // https://github.com/swiftlang/swift/blob/main/include/swift/Runtime/RuntimeFunctions.def
  if (Name == "swift_retain" || Name == "swift_nonatomic_retain" ||
      Name == "swift_unknownObjectRetain" ||
      Name == "swift_nonatomic_unknownObjectRetain" ||
      Name == "swift_bridgeObjectRetain" ||
      Name == "swift_nonatomic_bridgeObjectRetain" ||
      Name == "swift_getObjectType") {
    Signature.ReturnType = Pointer;
    Signature.Parameters = {{"object", Pointer}};
  } else if (Name == "swift_release" || Name == "swift_nonatomic_release" ||
             Name == "swift_unknownObjectRelease" ||
             Name == "swift_nonatomic_unknownObjectRelease" ||
             Name == "swift_bridgeObjectRelease" ||
             Name == "swift_nonatomic_bridgeObjectRelease") {
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"object", Pointer}};
  } else if (Name == "swift_weakInit" || Name == "swift_weakAssign" ||
             Name == "swift_unknownObjectWeakInit" ||
             Name == "swift_unknownObjectWeakAssign") {
    Signature.ReturnType = Pointer;
    Signature.Parameters = {{"reference", Pointer}, {"object", Pointer}};
  } else if (Name == "swift_weakLoadStrong" || Name == "swift_weakTakeStrong" ||
             Name == "swift_unknownObjectWeakLoadStrong" ||
             Name == "swift_unknownObjectWeakTakeStrong") {
    Signature.ReturnType = Pointer;
    Signature.Parameters = {{"reference", Pointer}};
  } else if (Name == "swift_weakDestroy" ||
             Name == "swift_unknownObjectWeakDestroy") {
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"reference", Pointer}};
  } else if (Name == "swift_once") {
    // Runtime/Once.h uses C_CC, including the context argument passed to the
    // callback. A source binding preserves the runtime call and its predicate;
    // it does not prove ownership or permit eager/omitted initialization.
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"predicate", Pointer},
                            {"function", NdType::makePtr(NdType::makeFunc(
                                             NdType::makeVoid(), {Pointer}))},
                            {"context", Pointer}};
  } else if (Name == "swift_beginAccess") {
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"address", Pointer},
                            {"scratch", Pointer},
                            {"flags", NdType::makeInt(8, false)},
                            {"pc", Pointer}};
  } else if (Name == "swift_endAccess") {
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"scratch", Pointer}};
  } else if (!declaredCABI(Name, Result)) {
    return std::nullopt;
  }
  std::string Diagnostic;
  if (!assignDarwinScalarSourceABI(Signature, Image.Arch, Diagnostic))
    return std::nullopt;
  return Result;
}

} // namespace neverd
