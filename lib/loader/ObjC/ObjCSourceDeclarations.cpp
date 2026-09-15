#include "neverd/loader/ObjC/ObjCSourceDeclarations.h"

#include "ObjCReceiverDeclarations.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCEncoding.h"

#include <map>
#include <set>

namespace neverd {
namespace {
bool mergeSignature(SourceFunctionTypeHint &A,
                    const SourceFunctionTypeHint &B) {
  if (A.Parameters.size() != B.Parameters.size())
    return false;
  for (size_t I = 0; I < A.Parameters.size(); ++I)
    if (!equalSourceTypes(A.Parameters[I].Type, B.Parameters[I].Type))
      return false;
  if (equalSourceTypes(A.ReturnType, B.ReturnType))
    return true;
  // A full-width integer result carries the same 64 bits regardless of
  // signedness. Comparisons and conversions retain their own IR semantics.
  // Narrow results, floating-point and pointer identities cannot use this.
  if (!A.ReturnType || !B.ReturnType || A.ReturnType->Kind != NdTypeKind::Int ||
      B.ReturnType->Kind != NdTypeKind::Int || A.ReturnType->Size != 8 ||
      B.ReturnType->Size != 8)
    return false;
  A.ReturnType = NdType::makeInt(8, false);
  return true;
}

using DeclarationIndex =
    std::map<std::string, std::optional<SourceFunctionTypeHint>, std::less<>>;

struct FrameworkDeclarations {
  std::string Modules;
  DeclarationIndex Selectors;
};
using FrameworkCatalog = std::map<std::string, FrameworkDeclarations>;

FrameworkCatalog buildFrameworkDeclarations(Arch Architecture) {
  FrameworkCatalog Result;
  auto Add = [&](DeclarationIndex &Index, const char *Selector,
                 const char *Encoding) {
    if (!Encoding)
      return;
    auto Hint = parseObjCMethodEncoding(Selector, Encoding);
    std::string Diagnostic;
    if (Hint) {
      Hint->Origin = SourceFunctionTypeHint::OriginKind::ObjCSDK;
      if (!assignDarwinObjCSourceABI(*Hint, Architecture, Diagnostic))
        Hint.reset();
    }
    auto [It, Inserted] = Index.try_emplace(Selector, Hint);
    // Negative evidence survives later valid declarations and their order.
    if (!Inserted && It->second &&
        (!Hint || !mergeSignature(*It->second, *Hint)))
      It->second.reset();
  };
  static constexpr struct {
    const char *Selector;
    const char *AArch64;
    const char *X64;
  } Foundation[] = {
#include "ObjCFoundationDeclarations.inc"
  };
  auto &Base = Result["Foundation"];
  Base.Modules =
      "/System/Library/Frameworks/Foundation.framework/Foundation|"
      "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation";
  for (const auto &D : Foundation)
    Add(Base.Selectors, D.Selector,
        Architecture == Arch::AArch64 ? D.AArch64 : D.X64);
  static constexpr struct {
    const char *Framework;
    const char *Modules;
    const char *Selector;
    const char *AArch64;
    const char *X64;
  } Frameworks[] = {
#include "ObjCFrameworkDeclarations.inc"
  };
  for (const auto &D : Frameworks) {
    auto &Framework = Result[D.Framework];
    Framework.Modules = D.Modules;
    Add(Framework.Selectors, D.Selector,
        Architecture == Arch::AArch64 ? D.AArch64 : D.X64);
  }
  return Result;
}

const FrameworkCatalog *frameworkDeclarations(Arch Architecture) {
  // Compiler facts are immutable. Activation and binary metadata are checked
  // on every query, so another image or reload cannot retain a framework.
  if (Architecture == Arch::AArch64) {
    static const auto Declarations = buildFrameworkDeclarations(Arch::AArch64);
    return &Declarations;
  }
  if (Architecture == Arch::X64) {
    static const auto Declarations = buildFrameworkDeclarations(Arch::X64);
    return &Declarations;
  }
  return nullptr;
}

bool usesFramework(const BinaryImage &Image,
                   const FrameworkDeclarations &Framework) {
  llvm::StringRef Modules(Framework.Modules);
  while (!Modules.empty()) {
    const auto [Module, Rest] = Modules.split('|');
    for (const auto &Needed : Image.DynInfo.NeededLibs)
      if (Needed == Module)
        return true;
    Modules = Rest;
  }
  return false;
}
} // namespace

static std::optional<SourceFunctionTypeHint>
selectorSourceTypeHint(const BinaryImage &Image, llvm::StringRef Selector,
                       const SourceFunctionTypeHint *FormatSignature) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64))
    return std::nullopt;
  std::optional<SourceFunctionTypeHint> Result;
  // The dynamic receiver class is unknown. No declaration is selected by
  // visitation order or by assuming that the receiver has a framework class.
  auto Include = [&](const auto &Method) {
    if (Method.Selector != Selector)
      return true;
    if (!Method.TypeHint)
      return false;
    auto Hint = *Method.TypeHint;
    std::string Diagnostic;
    if (!assignDarwinObjCSourceABI(Hint, Image.Arch, Diagnostic) ||
        (Result && !mergeSignature(*Result, Hint)))
      return false;
    if (!Result)
      Result = std::move(Hint);
    return true;
  };
  for (const auto &Method : Image.ObjCMethods)
    if (!Include(Method))
      return std::nullopt;
  for (const auto &Protocol : Image.ObjCProtocols)
    for (const auto &Method : Protocol.Methods)
      if (!Include(Method))
        return std::nullopt;
  for (const auto &Property : Image.ObjCProperties) {
    for (const auto &[Name, Hint] :
         {std::pair{&Property.Getter, &Property.GetterTypeHint},
          std::pair{&Property.Setter, &Property.SetterTypeHint}}) {
      if (Name->empty() || *Name != Selector)
        continue;
      if (!*Hint || (Result && !mergeSignature(*Result, **Hint)))
        return std::nullopt;
      if (!Result)
        Result = **Hint;
    }
  }
  if (const auto *Catalog = frameworkDeclarations(Image.Arch))
    for (const auto &[Name, Framework] : *Catalog) {
      if (!usesFramework(Image, Framework))
        continue;
      auto Found = Framework.Selectors.find(Selector.str());
      if (Found == Framework.Selectors.end())
        continue;
      // The format catalog currently belongs to Foundation. A negative
      // declaration from another framework must not inherit that contract.
      const auto *Declared = Found->second          ? &*Found->second
                             : Name == "Foundation" ? FormatSignature
                                                    : nullptr;
      if (!Declared || (Result && !mergeSignature(*Result, *Declared)))
        return std::nullopt;
      if (!Result)
        Result = *Declared;
      Result->Origin = SourceFunctionTypeHint::OriginKind::ObjCSDK;
    }
  return Result;
}

std::optional<SourceFunctionTypeHint>
objcSelectorSourceTypeHint(const BinaryImage &Image, llvm::StringRef Selector) {
  return selectorSourceTypeHint(Image, Selector, nullptr);
}

std::optional<ObjCReceiverTypeHint>
objcMethodReceiverTypeHint(const BinaryImage &Image, va_t Entry) {
  if (!Entry || Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64))
    return std::nullopt;
  std::optional<ObjCReceiverTypeHint> Result;
  for (const auto &Method : Image.ObjCMethods) {
    if (Method.Implementation != Entry)
      continue;
    if (Method.ClassName.empty() || !Method.TypeHint ||
        (Result && (Result->ClassName != Method.ClassName ||
                    Result->IsClassMethod != Method.IsClassMethod)))
      return std::nullopt;
    auto Signature = *Method.TypeHint;
    std::string Diagnostic;
    if (!assignDarwinObjCSourceABI(Signature, Image.Arch, Diagnostic))
      return std::nullopt;
    Result =
        ObjCReceiverTypeHint{ObjCReceiverTypeHint::OriginKind::MethodEntry,
                             Entry, Method.ClassName, Method.IsClassMethod};
  }
  return Result;
}

namespace {
bool validReceiverRoot(const BinaryImage &Image,
                       const ObjCReceiverTypeHint &Receiver) {
  if (!Receiver.Address || Receiver.ClassName.empty() ||
      Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64))
    return false;
  switch (Receiver.Origin) {
  case ObjCReceiverTypeHint::OriginKind::MethodEntry: {
    const auto Expected = objcMethodReceiverTypeHint(Image, Receiver.Address);
    return Expected && Expected->ClassName == Receiver.ClassName &&
           Expected->IsClassMethod == Receiver.IsClassMethod;
  }
  case ObjCReceiverTypeHint::OriginKind::ClassReference: {
    const auto Found = Image.ObjCSourceReferences.find(Receiver.Address);
    return Receiver.IsClassMethod &&
           Found != Image.ObjCSourceReferences.end() &&
           Found->second.Address == Receiver.Address &&
           Found->second.TheKind == ObjCSourceReference::Kind::Class &&
           Found->second.Size == 8 && Found->second.Name == Receiver.ClassName;
  }
  }
  return false;
}

const ObjCIvar *receiverIvar(const BinaryImage &Image, std::string ClassName,
                             uint64_t Key, bool BySlot) {
  const ObjCIvar *Result = nullptr;
  std::set<std::string> Visited;
  size_t Remaining = 4096;
  while (!ClassName.empty()) {
    if (Visited.size() >= 64 || !Visited.insert(ClassName).second)
      return nullptr;
    const ObjCClass *Class = nullptr;
    for (const auto &Candidate : Image.ObjCClasses)
      if (Candidate.Name == ClassName) {
        if (Class)
          return nullptr;
        Class = &Candidate;
      }
    if (!Class)
      break; // No external layout is invented beyond the recorded lineage.
    if (Class->IvarStatus != "recovered" ||
        (Class->RootClass ? Class->InheritanceStatus != "root" ||
                                !Class->SuperclassName.empty()
                          : Class->InheritanceStatus != "resolved" ||
                                Class->SuperclassName.empty()))
      return nullptr;
    for (const auto &Ivar : Class->Ivars) {
      if (!Remaining--)
        return nullptr;
      const bool Match = BySlot ? Ivar.OffsetAddress == Key
                                : Key < uint64_t(Ivar.Offset) + Ivar.Size &&
                                      Ivar.Offset < Key + 8;
      if (!Match)
        continue;
      if (Result || !Ivar.MetadataAddress || !Ivar.OffsetAddress ||
          Ivar.Size != 8 || (!BySlot && Ivar.Offset != Key) ||
          Ivar.Offset < Class->InstanceStart ||
          !rangeInBounds(Ivar.Offset, Ivar.Size, Class->InstanceSize))
        return nullptr;
      const auto Ref = Image.ObjCSourceReferences.find(Ivar.OffsetAddress);
      if (Ref == Image.ObjCSourceReferences.end() ||
          Ref->second.Address != Ivar.OffsetAddress ||
          Ref->second.TheKind != ObjCSourceReference::Kind::IvarOffset ||
          (Ref->second.Size != 4 && Ref->second.Size != 8) ||
          Ref->second.Name != Ivar.Name ||
          Ref->second.ClassName != Class->Name ||
          !objcEncodedObjectClass(Ivar.TypeEncoding))
        return nullptr;
      Result = &Ivar;
    }
    ClassName = Class->SuperclassName;
  }
  return Result;
}

struct ReceiverType {
  std::string ClassName;
  bool IsClassMethod = false;
};

std::optional<ReceiverType> receiverType(const BinaryImage &Image,
                                         const ObjCReceiverTypeHint &Receiver) {
  if (Receiver.IvarLoads.size() > 8 || !validReceiverRoot(Image, Receiver))
    return std::nullopt;
  ReceiverType Result{Receiver.ClassName, Receiver.IsClassMethod};
  for (const auto &Access : Receiver.IvarLoads) {
    if (Result.IsClassMethod)
      return std::nullopt;
    const auto *Ivar =
        receiverIvar(Image, Result.ClassName, Access.OffsetSlot, true);
    if (!Ivar || (Access.ByteOffset && *Access.ByteOffset != Ivar->Offset) ||
        Image.ObjCSourceReferences.at(Access.OffsetSlot).Size !=
            Access.OffsetWidth)
      return std::nullopt;
    const auto ClassName = objcEncodedObjectClass(Ivar->TypeEncoding);
    if (!ClassName)
      return std::nullopt;
    Result.ClassName = *ClassName;
  }
  return Result;
}

std::optional<ObjCReceiverTypeHint>
fieldReceiver(const BinaryImage &Image, const ObjCReceiverTypeHint &Receiver,
              uint64_t Key, bool BySlot) {
  const auto Type = receiverType(Image, Receiver);
  if (!Type || Type->IsClassMethod || Receiver.IvarLoads.size() >= 8 ||
      (!BySlot && Key > UINT32_MAX))
    return std::nullopt;
  const auto *Ivar = receiverIvar(Image, Type->ClassName, Key, BySlot);
  if (!Ivar)
    return std::nullopt;
  auto Result = Receiver;
  Result.IvarLoads.push_back(
      {Ivar->OffsetAddress,
       BySlot ? std::nullopt : std::optional<uint32_t>(Key),
       Image.ObjCSourceReferences.at(Ivar->OffsetAddress).Size});
  return Result;
}
} // namespace

bool objcReceiverTypeHintValid(const BinaryImage &Image,
                               const ObjCReceiverTypeHint &Receiver) {
  return receiverType(Image, Receiver).has_value();
}

std::optional<ObjCReceiverTypeHint>
objcReceiverIvarTypeHint(const BinaryImage &Image,
                         const ObjCReceiverTypeHint &Receiver,
                         va_t OffsetSlot) {
  return fieldReceiver(Image, Receiver, OffsetSlot, true);
}

std::optional<ObjCReceiverTypeHint>
objcReceiverFieldTypeHint(const BinaryImage &Image,
                          const ObjCReceiverTypeHint &Receiver,
                          uint64_t Offset) {
  return fieldReceiver(Image, Receiver, Offset, false);
}

ObjCReceiverDeclaration
objcReceiverSourceTypeHint(const BinaryImage &Image, llvm::StringRef Selector,
                           const ObjCReceiverTypeHint &Receiver) {
  ObjCReceiverDeclaration Result;
  const auto Type = receiverType(Image, Receiver);
  if (!Type)
    return {true, std::nullopt};
  bool Complete = true;
  bool KnownScope = true;
  auto Include = [&](const std::optional<SourceFunctionTypeHint> &Signature) {
    Result.HasDeclaration = true;
    if (!Signature) {
      Complete = false;
      return;
    }
    auto Hint = *Signature;
    std::string Diagnostic;
    if (!assignDarwinObjCSourceABI(Hint, Image.Arch, Diagnostic) ||
        (Result.Signature && !mergeSignature(*Result.Signature, Hint))) {
      Complete = false;
      return;
    }
    if (!Result.Signature)
      Result.Signature = Hint;
    if (Hint.Origin == SourceFunctionTypeHint::OriginKind::ObjCSDK)
      Result.Signature->Origin = Hint.Origin;
  };
  // Class and protocol namespaces can share names (notably NSObject).
  using Owner = std::pair<bool, std::string>;
  std::set<Owner> Active, Visited;
  auto Visit = [&](auto &&Self, const Owner &Key) -> bool {
    if (Active.count(Key) || Visited.size() + Active.size() >= 256)
      return false;
    if (Visited.count(Key))
      return true;
    Active.insert(Key);
    const auto &[Protocol, Name] = Key;
    const auto SDK = objc::sdkReceiverDeclarations(
        Image, Name, Protocol, Type->IsClassMethod, Selector);
    Complete &= SDK.Complete;
    std::optional<std::string> Superclass = SDK.Superclass;
    std::set<Owner> Parents;
    for (const auto &Parent : SDK.Protocols)
      Parents.emplace(true, Parent);
    for (const auto &Member : SDK.Members)
      Include(Member.Signature);
    bool Present = SDK.Present;
    if (Protocol) {
      for (const auto &Declaration : Image.ObjCProtocols) {
        if (Declaration.Name != Name)
          continue;
        Present = true;
        Complete &= Declaration.Status == "recovered";
        for (const auto &Method : Declaration.Methods)
          if (Method.IsClassMethod == Type->IsClassMethod &&
              Method.Selector == Selector)
            Include(Method.TypeHint);
        for (va_t Address : Declaration.AdoptedProtocols) {
          std::optional<std::string> ParentName;
          for (const auto &Parent : Image.ObjCProtocols)
            if (Parent.Address == Address) {
              if (ParentName && *ParentName != Parent.Name)
                return false;
              ParentName = Parent.Name;
            }
          if (!ParentName || ParentName->empty())
            return false;
          Parents.emplace(true, *ParentName);
        }
      }
    } else {
      for (const auto &Class : Image.ObjCClasses) {
        if (Class.Name != Name)
          continue;
        Present = true;
        const bool Root = Class.RootClass &&
                          Class.InheritanceStatus == "root" &&
                          Class.SuperclassName.empty();
        if (!Root && (Class.InheritanceStatus != "resolved" ||
                      Class.SuperclassName.empty())) {
          Complete = false;
          continue;
        }
        if (Superclass && *Superclass != Class.SuperclassName)
          return false;
        Superclass = Class.SuperclassName;
      }
      for (const auto &Method : Image.ObjCMethods)
        if (Method.ClassName == Name &&
            Method.IsClassMethod == Type->IsClassMethod &&
            Method.Selector == Selector)
          Include(Method.TypeHint);
      if (!Superclass)
        KnownScope = false;
      else if (!Superclass->empty())
        Parents.emplace(false, *Superclass);
    }
    for (const auto &Property : Image.ObjCProperties) {
      const bool IsProtocol =
          Property.Owner == ObjCProperty::OwnerKind::Protocol;
      if (IsProtocol != Protocol ||
          Property.IsClassProperty != Type->IsClassMethod ||
          (Protocol ? Property.OwnerName : Property.ClassName) != Name)
        continue;
      if (!Property.Getter.empty() && Property.Getter == Selector)
        Include(Property.GetterTypeHint);
      if (!Property.Setter.empty() && Property.Setter == Selector)
        Include(Property.SetterTypeHint);
    }
    KnownScope &= Present;
    for (const auto &Parent : Parents)
      if (!Self(Self, Parent))
        return false;
    Active.erase(Key);
    Visited.insert(Key);
    return true;
  };
  // Entry self is a base-class constraint. Known subclass declarations still
  // participate, whereas loading an exact class object fixes class dispatch.
  std::set<std::string> Classes{Type->ClassName};
  std::vector<std::string> Work{Type->ClassName};
  if (Receiver.Origin == ObjCReceiverTypeHint::OriginKind::MethodEntry ||
      !Receiver.IvarLoads.empty())
    while (!Work.empty()) {
      auto Name = std::move(Work.back());
      Work.pop_back();
      auto Children = objc::sdkReceiverSubclasses(Image, Name);
      for (const auto &Class : Image.ObjCClasses)
        if (Class.SuperclassName == Name)
          Children.push_back(Class.Name);
      for (const auto &Child : Children) {
        if (Child.empty() || Classes.size() >= 256)
          return {true, std::nullopt};
        if (Classes.insert(Child).second)
          Work.push_back(Child);
      }
    }
  for (const auto &Class : Classes)
    if (!Visit(Visit, {false, Class}))
      return {true, std::nullopt};
  if (!Complete)
    Result.Signature.reset();
  else if (!KnownScope) {
    Result.Signature.reset();
    Result.RequiresGlobalAgreement = true;
  }
  return Result;
}

std::optional<ObjCFormatDeclaration>
objcSelectorFormatDeclaration(const BinaryImage &Image,
                              llvm::StringRef Selector) {
  const auto *Catalog = frameworkDeclarations(Image.Arch);
  if (!Catalog)
    return std::nullopt;
  const auto &Foundation = Catalog->at("Foundation");
  if (!usesFramework(Image, Foundation))
    return std::nullopt;
  const auto Ordinary = Foundation.Selectors.find(Selector.str());
  if (Ordinary == Foundation.Selectors.end() || Ordinary->second)
    return std::nullopt;
  static constexpr struct {
    const char *Selector;
    const char *AArch64;
    const char *X64;
    unsigned FormatParameter;
    unsigned FixedCount;
  } Formats[] = {
#include "ObjCFormatDeclarations.inc"
  };
  std::optional<ObjCFormatDeclaration> Result;
  for (const auto &D : Formats) {
    if (D.Selector != Selector)
      continue;
    // More than one declaration must never be resolved by visitation order.
    if (Result)
      return std::nullopt;
    auto Signature = parseObjCMethodEncoding(
        Selector, Image.Arch == Arch::AArch64 ? D.AArch64 : D.X64);
    std::string Diagnostic;
    if (!Signature || Signature->Parameters.size() != D.FixedCount ||
        D.FormatParameter >= D.FixedCount ||
        Signature->Parameters[D.FormatParameter].Type->Kind != NdTypeKind::Ptr)
      return std::nullopt;
    Signature->Origin = SourceFunctionTypeHint::OriginKind::ObjCSDK;
    if (!assignDarwinObjCSourceABI(*Signature, Image.Arch, Diagnostic))
      return std::nullopt;
    Signature = selectorSourceTypeHint(Image, Selector, &*Signature);
    if (!Signature)
      return std::nullopt;
    Result = ObjCFormatDeclaration{std::move(*Signature), D.FormatParameter};
  }
  return Result;
}
} // namespace neverd
