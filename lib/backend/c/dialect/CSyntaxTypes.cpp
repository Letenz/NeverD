//===- CSyntaxTypes.cpp - The C types of emitted C ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Interned C types with the conversions C applies to them: integer
/// promotion and the usual arithmetic conversions (C17 6.3.1), with C23's
/// rule that bit-precise integers are never promoted.
///
//===----------------------------------------------------------------------===//

#include "CSyntaxTree.h"

#include "llvm/ADT/StringExtras.h"

using namespace neverd;
using namespace neverd::csyntax;

CDataModel CDataModel::forTarget(Arch TheArch, BinaryFormat Format) {
  CDataModel M;
  const bool Windows = Format == BinaryFormat::COFF;
  const bool Apple = Format == BinaryFormat::MachO;
  switch (TheArch) {
  case Arch::X86:
  case Arch::ARM:
    M.PointerBits = 32;
    M.LongBits = 32;
    break;
  default:
    M.PointerBits = 64;
    M.LongBits = Windows ? 32 : 64;
    break;
  }
  switch (TheArch) {
  case Arch::X64:
  case Arch::X86:
    M.LongDoubleBits = Windows ? 64 : 80;
    break;
  case Arch::AArch64:
    M.LongDoubleBits = Windows || Apple ? 64 : 128;
    break;
  default:
    M.LongDoubleBits = 64;
    break;
  }
  // AAPCS makes plain char unsigned; Windows and Apple keep it signed.
  M.CharSigned = !((TheArch == Arch::ARM || TheArch == Arch::AArch64) &&
                   !Windows && !Apple);
  M.WCharBits = Windows ? 16 : 32;
  M.WCharSigned = !Windows;
  return M;
}

TypeContext::TypeContext(const CDataModel &DataModel) : Model(DataModel) {
  CType V;
  V.TheKind = CType::Kind::Void;
  V.Spelling = "void";
  Void = intern(V);
  CType B;
  B.TheKind = CType::Kind::Bool;
  B.Bits = 8;
  B.Spelling = "_Bool";
  Bool = intern(B);
  Int = integer(32, true, "int");
  UnsignedInt = integer(32, false, "unsigned int");
  Char = integer(8, Model.CharSigned, "char");
  Size = integer(Model.PointerBits, false, "size_t");
  Ptrdiff = integer(Model.PointerBits, true, "ptrdiff_t");
  Double = floating(64, "double");
  CType U;
  U.TheKind = CType::Kind::Named;
  Unknown = intern(U);
}

const CType *TypeContext::intern(CType T) {
  std::string Key;
  llvm::raw_string_ostream OS(Key);
  OS << static_cast<int>(T.TheKind) << '|' << T.Bits << '|' << T.Signed
     << T.BitPrecise << T.Const << T.Volatile << '|' << T.AddressSpace << '|'
     << static_cast<const void *>(T.Inner) << '|'
     << (T.Count ? std::to_string(*T.Count) : "-") << '|';
  for (const CType *P : T.Params)
    OS << static_cast<const void *>(P) << ',';
  OS << '|' << T.Variadic << T.Prototyped << '|' << T.Spelling << '|'
     << static_cast<const void *>(T.Record);
  auto [It, Inserted] = Interned.try_emplace(Key, nullptr);
  if (Inserted) {
    Types.push_back(std::move(T));
    It->second = &Types.back();
  }
  return It->second;
}

const CType *TypeContext::integer(unsigned Bits, bool Signed,
                                  llvm::StringRef Spelling, bool BitPrecise) {
  CType T;
  T.TheKind = CType::Kind::Integer;
  T.Bits = Bits;
  T.Signed = Signed;
  T.BitPrecise = BitPrecise;
  T.Spelling = Spelling.str();
  return intern(std::move(T));
}

const CType *TypeContext::floating(unsigned Bits, llvm::StringRef Spelling) {
  CType T;
  T.TheKind = CType::Kind::Floating;
  T.Bits = Bits;
  T.Signed = true;
  T.Spelling = Spelling.str();
  return intern(std::move(T));
}

const CType *TypeContext::named(llvm::StringRef Spelling, unsigned Bits) {
  CType T;
  T.TheKind = CType::Kind::Named;
  T.Bits = Bits;
  T.Spelling = Spelling.str();
  return intern(std::move(T));
}

const CType *TypeContext::pointerTo(const CType *Pointee) {
  CType T;
  T.TheKind = CType::Kind::Pointer;
  T.Bits = Model.PointerBits;
  T.Inner = Pointee;
  return intern(std::move(T));
}

const CType *TypeContext::arrayOf(const CType *Element,
                                  std::optional<uint64_t> Count) {
  CType T;
  T.TheKind = CType::Kind::Array;
  T.Inner = Element;
  T.Count = Count;
  return intern(std::move(T));
}

const CType *TypeContext::function(const CType *Return,
                                   std::vector<const CType *> Params,
                                   bool Variadic, bool Prototyped) {
  CType T;
  T.TheKind = CType::Kind::Function;
  T.Inner = Return;
  T.Params = std::move(Params);
  T.Variadic = Variadic;
  T.Prototyped = Prototyped;
  return intern(std::move(T));
}

RecordDef *TypeContext::recordDef(llvm::StringRef Spelling) {
  RecordDef *&Def = RecordsByName[Spelling];
  if (!Def) {
    Records.emplace_back();
    Def = &Records.back();
    Def->Spelling = Spelling.str();
  }
  return Def;
}

const CType *TypeContext::record(llvm::StringRef Spelling) {
  CType T;
  T.TheKind = CType::Kind::Record;
  T.Spelling = Spelling.str();
  T.Record = recordDef(Spelling);
  return intern(std::move(T));
}

const CType *TypeContext::qualified(const CType *T, bool Const, bool Volatile,
                                    unsigned AddressSpace) {
  if ((!Const || T->Const) && (!Volatile || T->Volatile) &&
      (!AddressSpace || T->AddressSpace == AddressSpace))
    return T;
  CType Q = *T;
  Q.Const |= Const;
  Q.Volatile |= Volatile;
  if (AddressSpace)
    Q.AddressSpace = AddressSpace;
  return intern(std::move(Q));
}

const CType *TypeContext::unqualified(const CType *T) {
  if (!T->Const && !T->Volatile && !T->AddressSpace)
    return T;
  CType Q = *T;
  Q.Const = Q.Volatile = false;
  Q.AddressSpace = 0;
  return intern(std::move(Q));
}

const CType *TypeContext::spelled(const CType *T, llvm::StringRef Spelling) {
  if (T->TheKind == CType::Kind::Record || T->Spelling == Spelling)
    return T;
  CType S = *T;
  S.Spelling = Spelling.str();
  return intern(std::move(S));
}

const CType *TypeContext::promoted(const CType *T) {
  T = unqualified(T);
  if (T->TheKind == CType::Kind::Bool)
    return Int;
  if (T->TheKind == CType::Kind::Integer && !T->BitPrecise && T->Bits < 32)
    return Int;
  return T;
}

const CType *TypeContext::usualArithmetic(const CType *A, const CType *B) {
  A = unqualified(A);
  B = unqualified(B);
  if (A->isFloating() || B->isFloating()) {
    if (!B->isFloating())
      return A;
    if (!A->isFloating())
      return B;
    return A->Bits >= B->Bits ? A : B;
  }
  A = promoted(A);
  B = promoted(B);
  if (A->Bits == B->Bits && A->Signed == B->Signed)
    return A->BitPrecise && !B->BitPrecise ? B : A;
  if (A->Signed == B->Signed)
    return A->Bits > B->Bits ? A : B;
  const CType *U = A->Signed ? B : A;
  const CType *S = A->Signed ? A : B;
  return U->Bits >= S->Bits ? U : S;
}

std::optional<uint64_t> TypeContext::sizeOf(const CType *T) const {
  switch (T->TheKind) {
  case CType::Kind::Void:
    // GNU C sizes void as 1 for its byte arithmetic.
    return 1;
  case CType::Kind::Bool:
    return 1;
  case CType::Kind::Integer:
    return (T->Bits + 7) / 8 <= 8 ? llvm::PowerOf2Ceil((T->Bits + 7) / 8)
                                  : llvm::alignTo((T->Bits + 7) / 8, 16);
  case CType::Kind::Floating:
    if (T->Bits == 80)
      return Model.PointerBits == 32 ? 12 : 16;
    return T->Bits / 8;
  case CType::Kind::Pointer:
    return Model.PointerBits / 8;
  case CType::Kind::Array:
    if (!T->Count)
      return std::nullopt;
    if (auto Element = sizeOf(T->Inner))
      return *Element * *T->Count;
    return std::nullopt;
  case CType::Kind::Named:
    if (T->Bits)
      return T->Bits / 8;
    return std::nullopt;
  case CType::Kind::Function:
  case CType::Kind::Record:
    return std::nullopt;
  }
  return std::nullopt;
}
