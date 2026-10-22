//===- HeaderLibrary.cpp - Type libraries of system headers ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/types/HeaderLibrary.h"

#include "neverd/types/TypeLayout.h"
#include "neverd/types/TypeParse.h"
#include "neverd/types/TypeRender.h"

#include "llvm/ADT/StringSet.h"

#include <array>
#include <cstddef>
#include <mutex>
#include <optional>

namespace neverd::types {
/// lib/types/headers/<File>.h, as the build embeds it, each in a unit of its
/// own (HeaderLibrarySource.cpp.in).
namespace header_sources {
#define NEVERD_HEADER_LIBRARY(Id, Name, File, Model, ABI, Spelling)            \
  extern const unsigned char File[];                                           \
  extern const std::size_t File##Size;                                         \
  extern const unsigned char File##Supplement[];                               \
  extern const std::size_t File##SupplementSize;
#include "neverd/types/HeaderLibraries.def"
} // namespace header_sources

namespace {

constexpr size_t LibraryCount = 0
#define NEVERD_HEADER_LIBRARY(Id, Name, File, Model, ABI, Spelling) +1
#include "neverd/types/HeaderLibraries.def"
    ;

/// A library read once, with what reading it met.
struct Read {
  std::once_flag Once;
  std::optional<TypeLibrary> Types;
  std::string Error;
};

std::array<Read, LibraryCount> &reads() {
  static std::array<Read, LibraryCount> All;
  return All;
}

/// The size \p Library gives the type named \p Name, as a member holds it.
std::optional<uint64_t> sizeOfNamed(const TypeLibrary &Library,
                                    llvm::StringRef Name) {
  const LocalType *Type = Library.find(Name);
  return Type ? Library.sizeOf(*makeNamed(Name.str(), Type->Kind))
              : std::nullopt;
}

uint32_t alignOfNamed(const TypeLibrary &Library, llvm::StringRef Name) {
  const LocalType *Type = Library.find(Name);
  return Type ? Library.alignOf(*makeNamed(Name.str(), Type->Kind)) : 0;
}

llvm::StringRef supplementSource(HeaderLibrary Library) {
  switch (Library) {
#define NEVERD_HEADER_LIBRARY(Id, Name, File, Model, ABI, Spelling)            \
  case HeaderLibrary::Id:                                                      \
    return llvm::StringRef(                                                    \
        reinterpret_cast<const char *>(header_sources::File##Supplement + 1),  \
        header_sources::File##SupplementSize);
#include "neverd/types/HeaderLibraries.def"
  }
  return {};
}

} // namespace

DataModel headerLibraryModel(HeaderLibrary Library) {
  switch (Library) {
#define NEVERD_HEADER_LIBRARY(Id, Name, File, Model, ABI, Spelling)            \
  case HeaderLibrary::Id:                                                      \
    return DataModel::Model;
#include "neverd/types/HeaderLibraries.def"
  }
  return DataModel::LLP64;
}

RecordABI headerLibraryRecordABI(HeaderLibrary Library) {
  switch (Library) {
#define NEVERD_HEADER_LIBRARY(Id, Name, File, Model, ABI, Spelling)            \
  case HeaderLibrary::Id:                                                      \
    return RecordABI::ABI;
#include "neverd/types/HeaderLibraries.def"
  }
  return RecordABI::Microsoft;
}

llvm::StringRef headerLibraryName(HeaderLibrary Library) {
  switch (Library) {
#define NEVERD_HEADER_LIBRARY(Id, Name, File, Model, ABI, Spelling)            \
  case HeaderLibrary::Id:                                                      \
    return Name;
#include "neverd/types/HeaderLibraries.def"
  }
  return "";
}

std::optional<HeaderLibrary> headerLibraryNamed(llvm::StringRef Name) {
#define NEVERD_HEADER_LIBRARY(Id, Spelled, File, Model, ABI, Spelling)         \
  if (Name == Spelled)                                                         \
    return HeaderLibrary::Id;
#include "neverd/types/HeaderLibraries.def"
  return std::nullopt;
}

std::optional<HeaderSystem> headerLibrarySystem(HeaderLibrary Library) {
  const auto system = [](llvm::StringRef Kind) -> std::optional<HeaderSystem> {
#define NEVERD_HEADER_SYSTEM(Id, Name, Platform, Test, Probe, Feature,         \
                             Configured)                                       \
  if (Kind == #Id)                                                             \
    return HeaderSystem{Name, Platform, {}, Test, Probe, Feature, Configured};
#include "neverd/types/HeaderLibrarySystems.def"
    return std::nullopt;
  };
  const auto arch = [](llvm::StringRef Arch) -> llvm::StringRef {
#define NEVERD_HEADER_SYSTEM_ARCH(Id, Test)                                    \
  if (Arch == #Id)                                                             \
    return Test;
#include "neverd/types/HeaderLibrarySystems.def"
    return {};
  };
#define NEVERD_HEADER_LIBRARY_IMAGE(Kind, Target, Image)                       \
  if (Library == HeaderLibrary::Image)                                         \
    if (std::optional<HeaderSystem> Found = system(#Kind)) {                   \
      Found->Arch = arch(#Target);                                             \
      if (!Found->Arch.empty())                                                \
        return Found;                                                          \
    }
#include "neverd/types/HeaderLibraryImages.def"
  return std::nullopt;
}

llvm::StringRef headerLibrarySource(HeaderLibrary Library) {
  switch (Library) {
#define NEVERD_HEADER_LIBRARY(Id, Name, File, Model, ABI, Spelling)            \
  case HeaderLibrary::Id:                                                      \
    return llvm::StringRef(                                                    \
        reinterpret_cast<const char *>(header_sources::File),                  \
        header_sources::File##Size);
#include "neverd/types/HeaderLibraries.def"
  }
  return "";
}

BuiltinSpelling headerLibrarySpelling(HeaderLibrary Library) {
  switch (Library) {
#define NEVERD_HEADER_LIBRARY(Id, Name, File, Model, ABI, Spelling)            \
  case HeaderLibrary::Id:                                                      \
    return BuiltinSpelling::Spelling;
#include "neverd/types/HeaderLibraries.def"
  }
  return BuiltinSpelling::Portable;
}

const TypeLibrary *headerLibrary(HeaderLibrary Library, std::string *Error) {
  Read &Entry = reads()[size_t(Library)];
  std::call_once(Entry.Once, [&] {
    TypeLibrary Types(headerLibraryModel(Library));
    Types.setRecordABI(headerLibraryRecordABI(Library));
    ParseOptions Options;
    Options.Origin = TypeOrigin::Header;
    const ParseResult Result =
        parseCTypes(headerLibrarySource(Library), Types, Options);
    if (!Result.ok()) {
      const SourceProblem &First = Result.Problems.front();
      Entry.Error = headerLibraryName(Library).str() + ":" +
                    std::to_string(First.Line) + ":" +
                    std::to_string(First.Column) + ": " + First.Message;
      return;
    }
    if (const auto Source = supplementSource(Library); !Source.empty()) {
      TypeLibrary Supplement(headerLibraryModel(Library));
      Supplement.setRecordABI(headerLibraryRecordABI(Library));
      const auto Parsed = parseCTypes(Source, Supplement, Options);
      if (!Parsed.ok()) {
        Entry.Error = headerLibraryName(Library).str() +
                      " supplement: " + Parsed.Problems.front().Message;
        return;
      }
      // Official declarations own every existing name. Compatibility sources
      // supply missing names only, with the same layout checks used when a
      // program's own debug types override a system library.
      mergeHeaderTypes(Types, Supplement);
    }
    Entry.Types = std::move(Types);
  });
  if (!Entry.Types && Error)
    *Error = Entry.Error;
  return Entry.Types ? &*Entry.Types : nullptr;
}

HeaderMerge mergeHeaderTypes(TypeLibrary &Library, const TypeLibrary &Headers) {
  HeaderMerge Merge;
  std::vector<std::string> Added;
  for (const LocalType *Type : Headers.types()) {
    if (Library.find(Type->Name))
      continue;
    LocalType Copy = *Type;
    Copy.Origin = TypeOrigin::Header;
    Library.define(std::move(Copy));
    Added.push_back(Type->Name);
  }
  // A header record was laid out with the headers' types it holds by value;
  // one that holds a type the program sizes or aligns otherwise goes, as does
  // any type that names one gone.  A typedef has no layout of its own: it names
  // the program's type, as C binds it.
  llvm::StringSet<> Gone;
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (const std::string &Name : Added) {
      if (Gone.contains(Name))
        continue;
      const LocalType *Type = Library.find(Name);
      const bool LaidOut = Type->Record && Type->Record->Defined;
      for (const std::string &Held : valueDependencyNames(*Type))
        if (Gone.contains(Held) ||
            (LaidOut &&
             (sizeOfNamed(Library, Held) != sizeOfNamed(Headers, Held) ||
              alignOfNamed(Library, Held) != alignOfNamed(Headers, Held)))) {
          Gone.insert(Name);
          Merge.Conflicts.push_back(Name);
          Changed = true;
          break;
        }
    }
  }
  // A record that goes stays declared: the types that point to it, which
  // need no layout of it, still name it.
  for (const std::string &Name : Merge.Conflicts) {
    const LocalType *Type = Library.find(Name);
    if (Type && Type->Record &&
        (Type->Kind == TypeKind::Struct || Type->Kind == TypeKind::Union)) {
      LocalType Declared = *Type;
      auto Record = std::make_shared<RecordDef>();
      Record->Union = Type->Record->Union;
      Record->Defined = false;
      Declared.Record = std::move(Record);
      Library.define(std::move(Declared));
      continue;
    }
    Library.remove(Name);
  }
  Merge.Types = Added.size() - Merge.Conflicts.size();
  for (const auto &Entry : Headers.prototypes())
    if (!Library.findPrototype(Entry.first())) {
      LocalType Routine;
      Routine.Function = Entry.second;
      bool Compatible = true;
      for (const std::string &Name : mentionedTypeNames(Routine))
        Compatible &= Library.find(Name) != nullptr;
      const auto SameLayout = [&](const TypeExprRef &Type) {
        return !Type || (Library.sizeOf(*Type) == Headers.sizeOf(*Type) &&
                         Library.alignOf(*Type) == Headers.alignOf(*Type));
      };
      Compatible &= SameLayout(Entry.second->Return);
      for (const Parameter &Param : Entry.second->Params)
        Compatible &= SameLayout(Param.Type);
      if (!Compatible)
        continue;
      Library.definePrototype(Entry.first().str(), Entry.second);
      ++Merge.Prototypes;
    }
  return Merge;
}

} // namespace neverd::types
