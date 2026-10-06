#ifndef NEVERD_LOADER_SWIFT_SWIFTMANGLEDCLASSMETHODABI_H
#define NEVERD_LOADER_SWIFT_SWIFTMANGLEDCLASSMETHODABI_H

#include "SwiftFunctionSymbols.h"

#include "neverd/ir/SourceABI.h"

#include "llvm/Demangle/SwiftDemangle.h"

#include <algorithm>

namespace neverd {
struct SwiftClassMethodDeclaration {
  std::string Module;
  std::string ClassName;
  SourceFunctionTypeHint Signature;
};

// An instance method with two Objective-C object arguments and no result uses
// x0/x1 for the objects and swiftself (x20) for its receiver on arm64. Accept
// only the complete mangled type tree; native body and callers remain subject
// to the ordinary source proof.
inline std::optional<SwiftClassMethodDeclaration>
swiftMangledObjCObjectPairVoidMethodDeclaration(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      !Image.isCodeAddress(Entry))
    return std::nullopt;
  const Symbol *Only = uniqueSwiftFunctionSymbol(Image, Entry, SymbolIndex);
  if (!Only)
    return std::nullopt;
  llvm::StringRef Name(Only->Name);
  Name.consume_front("_");
  if (!Name.starts_with("$s"))
    return std::nullopt;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 1024;
  Options.MaxNodes = 128;
  Options.MaxDepth = 24;
  Options.MaxMemoryBytes = 65536;
  Options.MaxOperations = 10000;
  const auto Parsed = llvm::swiftDemangle(Name.str(), Options);
  using Node = llvm::SwiftDemangleNode;
  const auto Shape = [](const Node &N, llvm::StringRef Kind, size_t Children) {
    return N.Kind == Kind && !N.Text && !N.Index &&
           N.Children.size() == Children;
  };
  const auto Text = [](const Node &N, llvm::StringRef Kind,
                       llvm::StringRef Value) {
    return N.Kind == Kind && N.Text && *N.Text == Value && !N.Index &&
           N.Children.empty();
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 1) ||
      !Shape(Parsed.Root->Children[0], "Function", 4))
    return std::nullopt;
  const auto &Function = Parsed.Root->Children[0];
  const auto &Owner = Function.Children[0];
  const auto &Labels = Function.Children[2];
  const auto &Type = Function.Children[3];
  if (!Shape(Owner, "Class", 2) || Owner.Children[0].Kind != "Module" ||
      !Owner.Children[0].Text || Owner.Children[0].Text->empty() ||
      Owner.Children[0].Index || !Owner.Children[0].Children.empty() ||
      Owner.Children[1].Kind != "Identifier" || !Owner.Children[1].Text ||
      Owner.Children[1].Text->empty() || Owner.Children[1].Index ||
      !Owner.Children[1].Children.empty() ||
      Function.Children[1].Kind != "Identifier" || !Function.Children[1].Text ||
      Function.Children[1].Text->empty() || Function.Children[1].Index ||
      !Function.Children[1].Children.empty() ||
      !Shape(Labels, "LabelList", 2) ||
      !Shape(Labels.Children[0], "FirstElementMarker", 0) ||
      Labels.Children[1].Kind != "Identifier" || !Labels.Children[1].Text ||
      Labels.Children[1].Text->empty() || Labels.Children[1].Index ||
      !Labels.Children[1].Children.empty() || !Shape(Type, "Type", 1) ||
      !Shape(Type.Children[0], "FunctionType", 2) ||
      !Shape(Type.Children[0].Children[0], "ArgumentTuple", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0].Children[0], "Tuple",
             2) ||
      !Shape(Type.Children[0].Children[1], "ReturnType", 1) ||
      !Shape(Type.Children[0].Children[1].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[1].Children[0].Children[0], "Tuple", 0))
    return std::nullopt;
  for (const auto &Argument :
       Type.Children[0].Children[0].Children[0].Children[0].Children) {
    if (!Shape(Argument, "TupleElement", 1) ||
        !Shape(Argument.Children[0], "Type", 1) ||
        !Shape(Argument.Children[0].Children[0], "Class", 2) ||
        !Text(Argument.Children[0].Children[0].Children[0], "Module", "__C") ||
        Argument.Children[0].Children[0].Children[1].Kind != "Identifier" ||
        !Argument.Children[0].Children[0].Children[1].Text ||
        Argument.Children[0].Children[0].Children[1].Text->empty() ||
        Argument.Children[0].Children[0].Children[1].Index ||
        !Argument.Children[0].Children[0].Children[1].Children.empty())
      return std::nullopt;
  }

  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = NdType::makeVoid();
  Hint.Parameters = {{"first", NdType::makePtr(NdType::makeVoid())},
                     {"second", NdType::makePtr(NdType::makeVoid())},
                     {"self", NdType::makePtr(NdType::makeVoid())}};
  Hint.Parameters[2].TheRole = SourceParameterTypeHint::Role::SwiftContext;
  std::string Error;
  if (!assignDarwinSwiftSourceABI(Hint, Image.Arch, Error))
    return std::nullopt;
  return SwiftClassMethodDeclaration{*Owner.Children[0].Text,
                                     *Owner.Children[1].Text, std::move(Hint)};
}

inline std::optional<SourceFunctionTypeHint>
swiftMangledObjCObjectPairVoidMethodSourceABI(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  auto Declaration = swiftMangledObjCObjectPairVoidMethodDeclaration(
      Image, Entry, SymbolIndex);
  return Declaration ? std::optional<SourceFunctionTypeHint>(
                           std::move(Declaration->Signature))
                     : std::nullopt;
}
inline std::optional<SwiftClassMethodDeclaration>
swiftMangledZeroArgClassMethodDeclaration(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      !Image.isCodeAddress(Entry))
    return std::nullopt;
  const Symbol *Only = uniqueSwiftFunctionSymbol(Image, Entry, SymbolIndex);
  if (!Only)
    return std::nullopt;
  llvm::StringRef Name(Only->Name);
  Name.consume_front("_");
  if (!Name.starts_with("$s"))
    return std::nullopt;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 1024;
  Options.MaxNodes = 128;
  Options.MaxDepth = 24;
  Options.MaxMemoryBytes = 65536;
  Options.MaxOperations = 10000;
  const auto Parsed = llvm::swiftDemangle(Name.str(), Options);
  using Node = llvm::SwiftDemangleNode;
  const auto Shape = [](const Node &N, llvm::StringRef Kind, size_t Children) {
    return N.Kind == Kind && !N.Text && !N.Index &&
           N.Children.size() == Children;
  };
  const auto Text = [](const Node &N, llvm::StringRef Kind,
                       llvm::StringRef Value) {
    return N.Kind == Kind && N.Text && *N.Text == Value && !N.Index &&
           N.Children.empty();
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 1) ||
      !Shape(Parsed.Root->Children[0], "Function", 3))
    return std::nullopt;
  const auto &Function = Parsed.Root->Children[0];
  const auto &Owner = Function.Children[0];
  const auto &Type = Function.Children[2];
  const auto Identifier = [](const Node &N) {
    return N.Kind == "Identifier" && N.Text && !N.Text->empty() && !N.Index &&
           N.Children.empty();
  };
  const auto &Member = Function.Children[1];
  const bool NamedMember =
      Identifier(Member) ||
      (Shape(Member, "PrivateDeclName", 2) && Identifier(Member.Children[0]) &&
       Identifier(Member.Children[1]));
  if (!Shape(Owner, "Class", 2) || Owner.Children[0].Kind != "Module" ||
      !Owner.Children[0].Text || Owner.Children[0].Text->empty() ||
      Owner.Children[0].Index || !Owner.Children[0].Children.empty() ||
      Owner.Children[1].Kind != "Identifier" || !Owner.Children[1].Text ||
      Owner.Children[1].Text->empty() || Owner.Children[1].Index ||
      !Owner.Children[1].Children.empty() || !NamedMember ||
      !Shape(Type, "Type", 1) || !Shape(Type.Children[0], "FunctionType", 2) ||
      !Shape(Type.Children[0].Children[0], "ArgumentTuple", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0].Children[0], "Tuple",
             0) ||
      !Shape(Type.Children[0].Children[1], "ReturnType", 1) ||
      !Shape(Type.Children[0].Children[1].Children[0], "Type", 1))
    return std::nullopt;
  const auto &Result = Type.Children[0].Children[1].Children[0].Children[0];
  const bool VoidResult = Shape(Result, "Tuple", 0);
  const bool BoolResult = Shape(Result, "Structure", 2) &&
                          Text(Result.Children[0], "Module", "Swift") &&
                          Text(Result.Children[1], "Identifier", "Bool");
  if (!VoidResult && !BoolResult)
    return std::nullopt;

  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = VoidResult ? NdType::makeVoid() : NdType::makeInt(1, false);
  Hint.Parameters = {{"self", NdType::makePtr(NdType::makeVoid())}};
  Hint.Parameters[0].TheRole = SourceParameterTypeHint::Role::SwiftContext;
  std::string Error;
  if (!assignDarwinSwiftSourceABI(Hint, Image.Arch, Error))
    return std::nullopt;
  return SwiftClassMethodDeclaration{*Owner.Children[0].Text,
                                     *Owner.Children[1].Text, std::move(Hint)};
}

inline std::optional<SourceFunctionTypeHint>
swiftMangledZeroArgClassMethodSourceABI(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  auto Declaration =
      swiftMangledZeroArgClassMethodDeclaration(Image, Entry, SymbolIndex);
  return Declaration ? std::optional<SourceFunctionTypeHint>(
                           std::move(Declaration->Signature))
                     : std::nullopt;
}

// Compiler-observed arm64 instance methods taking CGContext and CGRect use
// x0, d0..d3, and swiftself. The complete type tree owns this declaration;
// neither its member name nor an Objective-C thunk is an ABI certificate.
inline std::optional<SwiftClassMethodDeclaration>
swiftMangledCGContextCGRectClassMethodDeclaration(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      !Image.isCodeAddress(Entry))
    return std::nullopt;
  const Symbol *Only = uniqueSwiftFunctionSymbol(Image, Entry, SymbolIndex);
  if (!Only)
    return std::nullopt;
  llvm::StringRef Name(Only->Name);
  Name.consume_front("_");
  if (!Name.starts_with("$s"))
    return std::nullopt;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 1024;
  Options.MaxNodes = 128;
  Options.MaxDepth = 24;
  Options.MaxMemoryBytes = 65536;
  Options.MaxOperations = 10000;
  const auto Parsed = llvm::swiftDemangle(Name.str(), Options);
  using Node = llvm::SwiftDemangleNode;
  const auto Shape = [](const Node &N, llvm::StringRef Kind, size_t Children) {
    return N.Kind == Kind && !N.Text && !N.Index &&
           N.Children.size() == Children;
  };
  const auto Text = [](const Node &N, llvm::StringRef Kind,
                       llvm::StringRef Value) {
    return N.Kind == Kind && N.Text && *N.Text == Value && !N.Index &&
           N.Children.empty();
  };
  const auto Identifier = [](const Node &N) {
    return N.Kind == "Identifier" && N.Text && !N.Text->empty() && !N.Index &&
           N.Children.empty();
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 1) ||
      !Shape(Parsed.Root->Children[0], "Function", 4))
    return std::nullopt;
  const auto &Function = Parsed.Root->Children[0];
  const auto &Owner = Function.Children[0];
  const auto &Member = Function.Children[1];
  const auto &Labels = Function.Children[2];
  const auto &Type = Function.Children[3];
  if (!Shape(Owner, "Class", 2) || Owner.Children[0].Kind != "Module" ||
      !Owner.Children[0].Text || Owner.Children[0].Text->empty() ||
      Owner.Children[0].Index || !Owner.Children[0].Children.empty() ||
      !Identifier(Owner.Children[1]) ||
      !(Identifier(Member) ||
        (Shape(Member, "PrivateDeclName", 2) &&
         Identifier(Member.Children[0]) && Identifier(Member.Children[1]))) ||
      !Shape(Labels, "LabelList", 2) ||
      !std::all_of(Labels.Children.begin(), Labels.Children.end(),
                   [&](const auto &N) {
                     return Identifier(N) || Shape(N, "FirstElementMarker", 0);
                   }) ||
      !Shape(Type, "Type", 1) || !Shape(Type.Children[0], "FunctionType", 2) ||
      !Shape(Type.Children[0].Children[0], "ArgumentTuple", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0].Children[0], "Tuple",
             2) ||
      !Shape(Type.Children[0].Children[1], "ReturnType", 1) ||
      !Shape(Type.Children[0].Children[1].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[1].Children[0].Children[0], "Tuple", 0))
    return std::nullopt;
  const auto &Arguments =
      Type.Children[0].Children[0].Children[0].Children[0].Children;
  const auto Argument = [&](const Node &N, llvm::StringRef Kind,
                            llvm::StringRef Nominal) {
    return Shape(N, "TupleElement", 1) && Shape(N.Children[0], "Type", 1) &&
           Shape(N.Children[0].Children[0], Kind, 2) &&
           Text(N.Children[0].Children[0].Children[0], "Module", "__C") &&
           Text(N.Children[0].Children[0].Children[1], "Identifier", Nominal);
  };
  if (!Argument(Arguments[0], "TypeAlias", "CGContextRef") ||
      !Argument(Arguments[1], "Structure", "CGRect"))
    return std::nullopt;
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = NdType::makeVoid();
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  const auto Double = NdType::makeFloat(8);
  Hint.Parameters = {
      {"context", Pointer},
      {"rect", NdType::makeStruct({Double, Double, Double, Double})},
      {"self", Pointer}};
  Hint.Parameters[2].TheRole = SourceParameterTypeHint::Role::SwiftContext;
  std::string Error;
  if (!assignDarwinSwiftSourceABI(Hint, Image.Arch, Error))
    return std::nullopt;
  return SwiftClassMethodDeclaration{*Owner.Children[0].Text,
                                     *Owner.Children[1].Text, std::move(Hint)};
}

inline std::optional<SourceFunctionTypeHint>
swiftMangledCGContextCGRectClassMethodSourceABI(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  auto Declaration = swiftMangledCGContextCGRectClassMethodDeclaration(
      Image, Entry, SymbolIndex);
  return Declaration ? std::optional<SourceFunctionTypeHint>(
                           std::move(Declaration->Signature))
                     : std::nullopt;
}

// Complete Swift 6.1.2 declarations for ordinary and generic class receivers
// use two double input/result lanes and swiftself for CGPoint -> CGPoint.
// Match the exact imported nominal type, excluding thunks, inout, async,
// throwing and generic method signatures. No body or call effect is implied.
inline std::optional<SwiftClassMethodDeclaration>
swiftMangledCGPointClassMethodDeclaration(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64) ||
      !Image.isCodeAddress(Entry))
    return std::nullopt;
  const auto *Only =
      consistentSwiftFunctionDeclarationSymbol(Image, Entry, SymbolIndex);
  if (!Only || Only->IsBoundaryGuess || Only->Origin != NameOrigin::Stated)
    return std::nullopt;
  llvm::StringRef Name(Only->Name);
  Name.consume_front("_");
  if (!Name.starts_with("$s"))
    return std::nullopt;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 1024;
  Options.MaxNodes = 128;
  Options.MaxDepth = 24;
  Options.MaxMemoryBytes = 65536;
  Options.MaxOperations = 10000;
  const auto Parsed = llvm::swiftDemangle(Name.str(), Options);
  using Node = llvm::SwiftDemangleNode;
  const auto Shape = [](const Node &N, llvm::StringRef Kind, size_t Children) {
    return N.Kind == Kind && !N.Text && !N.Index &&
           N.Children.size() == Children;
  };
  const auto Text = [](const Node &N, llvm::StringRef Kind,
                       llvm::StringRef Value) {
    return N.Kind == Kind && N.Text && *N.Text == Value && !N.Index &&
           N.Children.empty();
  };
  const auto Identifier = [](const Node &N, llvm::StringRef Kind) {
    return N.Kind == Kind && N.Text && !N.Text->empty() && !N.Index &&
           N.Children.empty();
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 1) ||
      !Shape(Parsed.Root->Children[0], "Function", 4))
    return std::nullopt;
  const auto &Function = Parsed.Root->Children[0];
  const auto &Owner = Function.Children[0];
  const auto &Member = Function.Children[1];
  const auto &Labels = Function.Children[2];
  const auto &Type = Function.Children[3];
  if (!Shape(Owner, "Class", 2) || !Identifier(Owner.Children[0], "Module") ||
      !Identifier(Owner.Children[1], "Identifier") ||
      !(Identifier(Member, "Identifier") ||
        (Shape(Member, "PrivateDeclName", 2) &&
         Identifier(Member.Children[0], "Identifier") &&
         Identifier(Member.Children[1], "Identifier"))) ||
      Labels.Kind != "LabelList" || Labels.Text || Labels.Index ||
      Labels.Children.size() > 1 ||
      (!Labels.Children.empty() &&
       !Identifier(Labels.Children[0], "Identifier") &&
       !Shape(Labels.Children[0], "FirstElementMarker", 0)) ||
      !Shape(Type, "Type", 1) || !Shape(Type.Children[0], "FunctionType", 2) ||
      !Shape(Type.Children[0].Children[0], "ArgumentTuple", 1) ||
      !Shape(Type.Children[0].Children[1], "ReturnType", 1))
    return std::nullopt;
  const auto Point = [&](const Node &N) {
    return Shape(N, "Type", 1) && Shape(N.Children[0], "Structure", 2) &&
           Text(N.Children[0].Children[0], "Module", "__C") &&
           Text(N.Children[0].Children[1], "Identifier", "CGPoint");
  };
  const auto &Argument = Type.Children[0].Children[0].Children[0];
  const bool NamedPoint =
      Shape(Argument, "Type", 1) && Shape(Argument.Children[0], "Tuple", 1) &&
      Shape(Argument.Children[0].Children[0], "TupleElement", 1) &&
      Point(Argument.Children[0].Children[0].Children[0]);
  if ((!Point(Argument) && !NamedPoint) ||
      !Point(Type.Children[0].Children[1].Children[0]))
    return std::nullopt;
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  const auto Double = NdType::makeFloat(8);
  Hint.ReturnType = NdType::makeStruct({Double, Double});
  Hint.Parameters = {{"point", Hint.ReturnType},
                     {"self", NdType::makePtr(NdType::makeVoid())}};
  Hint.Parameters[1].TheRole = SourceParameterTypeHint::Role::SwiftContext;
  std::string Error;
  if (!assignDarwinSwiftSourceABI(Hint, Image.Arch, Error))
    return std::nullopt;
  return SwiftClassMethodDeclaration{*Owner.Children[0].Text,
                                     *Owner.Children[1].Text, std::move(Hint)};
}

inline std::optional<SourceFunctionTypeHint>
swiftMangledCGPointClassMethodSourceABI(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  auto Declaration =
      swiftMangledCGPointClassMethodDeclaration(Image, Entry, SymbolIndex);
  return Declaration ? std::optional<SourceFunctionTypeHint>(
                           std::move(Declaration->Signature))
                     : std::nullopt;
}

// Receiver discovery and both publication replays consume the same complete
// entry declaration, including the logical index of swiftself.
inline std::optional<SwiftClassMethodDeclaration>
swiftMangledReceiverClassMethodDeclaration(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  if (auto Declaration =
          swiftMangledZeroArgClassMethodDeclaration(Image, Entry, SymbolIndex))
    return Declaration;
  if (auto Declaration = swiftMangledCGContextCGRectClassMethodDeclaration(
          Image, Entry, SymbolIndex))
    return Declaration;
  return swiftMangledCGPointClassMethodDeclaration(Image, Entry, SymbolIndex);
}

inline std::optional<SourceFunctionTypeHint>
swiftMangledReceiverClassMethodSourceABI(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  auto Declaration =
      swiftMangledReceiverClassMethodDeclaration(Image, Entry, SymbolIndex);
  return Declaration ? std::optional<SourceFunctionTypeHint>(
                           std::move(Declaration->Signature))
                     : std::nullopt;
}

} // namespace neverd
#endif
