//===- SymbolSpellingSwift.cpp - Swift symbol names -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Swift names, read from the bundled Swift demangler's node tree.  A name
/// reads as a declaration path with its argument labels, without types:
/// `Demo.Box.update(with:)`, `Demo.Box.count.getter`,
/// `dispatch thunk of Demo.P.name.getter`.  Only the node kinds
/// SwiftSymbolNodes.def lists are read; a symbol holding another kind where
/// they are read keeps its mangled name.
///
//===----------------------------------------------------------------------===//

#include "SymbolSpellingDetail.h"

#include "llvm/Demangle/SwiftDemangle.h"

#include <optional>
#include <vector>

using namespace neverd;
using namespace neverd::symbol_spelling;

namespace {

using Node = llvm::SwiftDemangleNode;

/// A declaration path read two ways: as Swift spells it and as the words of
/// a C identifier stem.
struct Spelling {
  std::string Readable;
  std::vector<std::string> Words;
};

bool isContextKind(llvm::StringRef Kind) {
#define NEVERD_SWIFT_CONTEXT(ContextKind)                                      \
  if (Kind == ContextKind)                                                     \
    return true;
#include "neverd/loader/SwiftSymbolNodes.def"
  return false;
}

const Node *childOfKind(const Node &Parent, llvm::StringRef Kind) {
  for (const Node &Child : Parent.Children)
    if (Child.Kind == Kind)
      return &Child;
  return nullptr;
}

/// The name a declaration node spells: an Identifier, or the name a private
/// declaration keeps after its file discriminator.
std::optional<std::string> declarationName(const Node &N) {
  if (N.Kind == "Identifier" && N.Text)
    return *N.Text;
  if (N.Kind == "PrivateDeclName" && !N.Children.empty())
    return declarationName(N.Children.back());
  return std::nullopt;
}

std::optional<Spelling> spellContext(const Node &N) {
  if (N.Kind == "Module" && N.Text)
    return Spelling{*N.Text, {*N.Text}};
  if (N.Kind == "Type" && N.Children.size() == 1)
    return spellContext(N.Children.front());
  // An extension's members read under the type it extends.
  if (N.Kind == "Extension" && N.Children.size() >= 2)
    return spellContext(N.Children[1]);
  if (!isContextKind(N.Kind) || N.Children.size() != 2)
    return std::nullopt;
  auto Context = spellContext(N.Children[0]);
  const auto Name = declarationName(N.Children[1]);
  if (!Context || !Name)
    return std::nullopt;
  Context->Readable += "." + *Name;
  Context->Words.push_back(*Name);
  return Context;
}

/// The argument labels of a function-like declaration, `x:y:` or `_:` for
/// an unlabeled argument.  A declaration without a label list takes one
/// unlabeled argument per element of its argument tuple.
std::optional<std::vector<std::string>> argumentLabels(const Node &Entity) {
  std::vector<std::string> Labels;
  if (const Node *List = childOfKind(Entity, "LabelList")) {
    for (const Node &Label : List->Children) {
      if (Label.Kind != "Identifier" && Label.Kind != "FirstElementMarker")
        return std::nullopt;
      Labels.push_back(Label.Text && !Label.Text->empty() ? *Label.Text : "_");
    }
    if (!List->Children.empty())
      return Labels;
  }
  const Node *Type = childOfKind(Entity, "Type");
  const Node *Function =
      Type && Type->Children.size() == 1 ? &Type->Children.front() : nullptr;
  if (!Function || Function->Kind != "FunctionType")
    return Labels;
  const Node *Arguments = childOfKind(*Function, "ArgumentTuple");
  if (!Arguments || Arguments->Children.size() != 1)
    return std::nullopt;
  const Node &ArgumentType = Arguments->Children.front();
  if (ArgumentType.Kind != "Type" || ArgumentType.Children.size() != 1)
    return std::nullopt;
  const Node &Tuple = ArgumentType.Children.front();
  const size_t Count = Tuple.Kind == "Tuple" ? Tuple.Children.size() : 1;
  Labels.assign(Count, "_");
  return Labels;
}

/// \p Base followed by `(labels)` in the readable spelling, and by the named
/// labels in the stem.
std::optional<Spelling> withArguments(Spelling Base, const Node &Entity) {
  const auto Labels = argumentLabels(Entity);
  if (!Labels)
    return std::nullopt;
  Base.Readable += "(";
  for (const std::string &Label : *Labels) {
    Base.Readable += Label + ":";
    if (Label != "_")
      Base.Words.push_back(Label);
  }
  Base.Readable += ")";
  return Base;
}

std::optional<Spelling> spellEntity(const Node &N) {
  if ((N.Kind == "Function" || N.Kind == "Variable") &&
      N.Children.size() >= 2) {
    auto Context = spellContext(N.Children[0]);
    const auto Name = declarationName(N.Children[1]);
    if (!Context || !Name)
      return std::nullopt;
    Context->Readable += "." + *Name;
    Context->Words.push_back(*Name);
    if (N.Kind == "Variable")
      return Context;
    return withArguments(std::move(*Context), N);
  }
  if (N.Kind == "Subscript" && !N.Children.empty()) {
    auto Context = spellContext(N.Children[0]);
    if (!Context)
      return std::nullopt;
    Context->Readable += ".subscript";
    Context->Words.push_back("subscript");
    return withArguments(std::move(*Context), N);
  }
#define NEVERD_SWIFT_MEMBER(NodeKind, Spelled, Word, TakesArguments)           \
  if (N.Kind == NodeKind && !N.Children.empty()) {                             \
    auto Context = spellContext(N.Children[0]);                                \
    if (!Context)                                                              \
      return std::nullopt;                                                     \
    Context->Readable += "." Spelled;                                          \
    Context->Words.push_back(Word);                                            \
    if (!TakesArguments)                                                       \
      return Context;                                                          \
    return withArguments(std::move(*Context), N);                              \
  }
#define NEVERD_SWIFT_ACCESSOR(NodeKind, Spelled, Word)                         \
  if (N.Kind == NodeKind && N.Children.size() == 1) {                          \
    auto Storage = spellEntity(N.Children[0]);                                 \
    if (!Storage)                                                              \
      return std::nullopt;                                                     \
    Storage->Readable += "." Spelled;                                          \
    Storage->Words.push_back(Word);                                            \
    return Storage;                                                            \
  }
#define NEVERD_SWIFT_THUNK(NodeKind, Spelled, Word)                            \
  if (N.Kind == NodeKind && N.Children.size() == 1) {                          \
    auto Entity = spellEntity(N.Children[0]);                                  \
    if (!Entity)                                                               \
      return std::nullopt;                                                     \
    Entity->Readable.insert(0, Spelled);                                       \
    Entity->Words.push_back(Word);                                             \
    return Entity;                                                             \
  }
#define NEVERD_SWIFT_TYPE_THUNK(NodeKind, Spelled, Word)                       \
  if (N.Kind == NodeKind && N.Children.size() == 1) {                          \
    auto Type = spellContext(N.Children[0]);                                   \
    if (!Type)                                                                 \
      return std::nullopt;                                                     \
    Type->Readable.insert(0, Spelled);                                         \
    Type->Words.push_back(Word);                                               \
    return Type;                                                               \
  }
#include "neverd/loader/SwiftSymbolNodes.def"
  return std::nullopt;
}

/// The marker \p N is, as its readable prefix and its stem word.
std::optional<std::pair<llvm::StringRef, llvm::StringRef>>
marker(const Node &N) {
#define NEVERD_SWIFT_MARKER(NodeKind, Spelled, Word)                           \
  if (N.Kind == NodeKind && N.Children.empty())                                \
    return std::make_pair(llvm::StringRef(Spelled), llvm::StringRef(Word));
#include "neverd/loader/SwiftSymbolNodes.def"
  return std::nullopt;
}

std::optional<Spelling> spellSymbol(llvm::StringRef Name) {
  const llvm::SwiftDemangleResult Parsed = llvm::swiftDemangle(Name);
  if (!Parsed.Root || Parsed.Root->Kind != "Global" ||
      Parsed.Root->Children.empty())
    return std::nullopt;
  const std::vector<Node> &Children = Parsed.Root->Children;
  auto Entity = spellEntity(Children.back());
  if (!Entity)
    return std::nullopt;
  for (size_t I = Children.size() - 1; I-- > 0;) {
    const auto Marker = marker(Children[I]);
    if (!Marker)
      return std::nullopt;
    Entity->Readable.insert(0, Marker->first.str());
    Entity->Words.push_back(Marker->second.str());
  }
  return Entity;
}

} // namespace

bool symbol_spelling::claimsSwiftName(llvm::StringRef) { return true; }

std::string symbol_spelling::readableSwiftName(llvm::StringRef Name) {
  const auto Spelled = spellSymbol(Name);
  return Spelled ? Spelled->Readable : std::string();
}

std::string symbol_spelling::stemOfSwiftName(llvm::StringRef Name) {
  const auto Spelled = spellSymbol(Name);
  return Spelled ? joinStemWords(Spelled->Words) : std::string();
}
