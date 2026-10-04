#include "SwiftMangledValueConstructorABI.h"

#include "SwiftFunctionSymbols.h"

#include "neverd/loader/ReadOnlyBytes.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Demangle/SwiftDemangle.h"

namespace neverd {
namespace {
using Node = llvm::SwiftDemangleNode;
bool shape(const Node &N, llvm::StringRef Kind, size_t Children) {
  return N.Kind == Kind && !N.Text && !N.Index && N.Children.size() == Children;
}
bool text(const Node &N, llvm::StringRef Kind, llvm::StringRef Value) {
  return N.Kind == Kind && N.Text && *N.Text == Value && !N.Index &&
         N.Children.empty();
}
bool identifier(const Node &N, llvm::StringRef Kind) {
  return N.Kind == Kind && N.Text && !N.Text->empty() &&
         N.Text->size() <= 128 && !N.Index && N.Children.empty() &&
         llvm::all_of(*N.Text,
                      [](char C) { return llvm::isAlnum(C) || C == '_'; });
}
bool nominal(const Node &N, llvm::StringRef Kind, llvm::StringRef Module,
             llvm::StringRef Name) {
  return shape(N, Kind, 2) && text(N.Children[0], "Module", Module) &&
         text(N.Children[1], "Identifier", Name);
}
const Node *functionType(const Node &Type) {
  return shape(Type, "Type", 1) && shape(Type.Children[0], "FunctionType", 2)
             ? &Type.Children[0]
             : nullptr;
}
const Node *functionPart(const Node &Function, size_t Index,
                         llvm::StringRef Kind) {
  const auto &Part = Function.Children[Index];
  return shape(Part, Kind, 1) && shape(Part.Children[0], "Type", 1)
             ? &Part.Children[0].Children[0]
             : nullptr;
}
bool optionalBoolCallback(const Node &N) {
  if (!shape(N, "BoundGenericEnum", 2) || !shape(N.Children[0], "Type", 1) ||
      !nominal(N.Children[0].Children[0], "Enum", "Swift", "Optional") ||
      !shape(N.Children[1], "TypeList", 1))
    return false;
  const auto *Function = functionType(N.Children[1].Children[0]);
  if (!Function)
    return false;
  const auto *Argument = functionPart(*Function, 0, "ArgumentTuple");
  const auto *Result = functionPart(*Function, 1, "ReturnType");
  return Argument && Result &&
         nominal(*Argument, "Structure", "Swift", "Bool") &&
         shape(*Result, "Tuple", 0);
}
} // namespace

std::optional<SwiftFixedRecordConstructorDeclaration>
swiftMangledFixedRecordConstructorDeclaration(const BinaryImage &Image,
                                              va_t Entry) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64) ||
      (Image.Arch == Arch::AArch64 && Entry % 4) ||
      !Image.hasAuthenticatedFunctionEntryAt(Entry) ||
      !readImmutableCodeBytes(Image, Entry,
                              Image.Arch == Arch::AArch64 ? 4 : 1))
    return std::nullopt;
  const auto *Only = uniqueSwiftFunctionSymbol(Image, Entry);
  if (!Only)
    return std::nullopt;
  for (const auto &S : Image.Symbols)
    if (&S != Only && (S.Addr == Entry || S.Name == Only->Name))
      return std::nullopt;
  for (const auto &E : Image.Exports)
    if ((E.Addr == Entry && E.Name != Only->Name) ||
        (E.Name == Only->Name && E.Addr != Entry))
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
  const auto Parsed = llvm::swiftDemangle(Name, Options);
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !shape(*Parsed.Root, "Global", 1) ||
      !shape(Parsed.Root->Children[0], "Allocator", 3))
    return std::nullopt;
  const auto &Constructor = Parsed.Root->Children[0];
  const auto &Owner = Constructor.Children[0];
  const auto &Labels = Constructor.Children[1];
  const auto *Function = functionType(Constructor.Children[2]);
  if (!shape(Owner, "Structure", 2) ||
      !identifier(Owner.Children[0], "Module") ||
      !identifier(Owner.Children[1], "Identifier") || !Function ||
      !shape(Labels, "LabelList", 4))
    return std::nullopt;
  const auto &Module = *Owner.Children[0].Text;
  const auto &TypeName = *Owner.Children[1].Text;
  if (Module == "Swift" || Module == "__C" || Module == "__C_Synthesized")
    return std::nullopt;
  const auto *Arguments = functionPart(*Function, 0, "ArgumentTuple");
  const auto *Result = functionPart(*Function, 1, "ReturnType");
  if (!Arguments || !Result || !shape(*Arguments, "Tuple", 4) ||
      !nominal(*Result, "Structure", Module, TypeName))
    return std::nullopt;

  // The bounded owner has an ordinary top-level spelling. Its symbol is only
  // a lookup key: registration, context, reflection, metadata and storage are
  // all independently authenticated by the shared metadata owner below.
  const std::string Mangled = std::to_string(Module.size()) + Module +
                              std::to_string(TypeName.size()) + TypeName + "V";
  const std::string DescriptorName = "_$s" + Mangled + "Mn";
  const Symbol *Descriptor = nullptr;
  for (const auto &S : Image.Symbols)
    if (S.Name == DescriptorName) {
      if (Descriptor || S.IsFunc)
        return std::nullopt;
      Descriptor = &S;
    }
  auto Storage = Descriptor ? swiftFixedRecordStorage(Image, Descriptor->Addr)
                            : std::nullopt;
  if (!Storage || Storage->MangledType != Mangled)
    return std::nullopt;
  for (unsigned I = 0; I != 4; ++I) {
    if (!text(Labels.Children[I], "Identifier", Storage->Fields[I].Name))
      return std::nullopt;
    const auto &Element = Arguments->Children[I];
    if (!shape(Element, "TupleElement", 1) ||
        !shape(Element.Children[0], "Type", 1))
      return std::nullopt;
    const auto &Argument = Element.Children[0].Children[0];
    if (I == 3) {
      if (!optionalBoolCallback(Argument))
        return std::nullopt;
    } else {
      const auto &Field = Storage->Fields[I];
      const bool CGFloat = Field.MangledType == "12CoreGraphics7CGFloatV";
      if ((!CGFloat && Field.MangledType != "Sd") ||
          !nominal(Argument, "Structure", CGFloat ? "CoreGraphics" : "Swift",
                   CGFloat ? "CGFloat" : "Double"))
        return std::nullopt;
    }
  }

  // The complete compiler-observed Swift shape has three independent FP
  // arguments and two ordinary opaque callback carriers, with no swiftself.
  // Storage's fourth field is instead one strong class reference. No callback
  // invocation, ARC operation or memory effect is authorized by this ABI.
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  const auto D = NdType::makeFloat(8), P = NdType::makePtr(NdType::makeVoid());
  Hint.Parameters = {{"first", D},
                     {"second", D},
                     {"third", D},
                     {"callback_code", P},
                     {"callback_context", P}};
  Hint.ReturnType = NdType::makeStruct({D, D, D, P});
  std::string Error;
  if (!assignDarwinSwiftSourceABI(Hint, Image.Arch, Error))
    return std::nullopt;
  return SwiftFixedRecordConstructorDeclaration{std::move(*Storage),
                                                std::move(Hint)};
}
} // namespace neverd
