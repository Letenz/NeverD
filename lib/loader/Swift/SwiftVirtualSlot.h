#ifndef NEVERD_LOADER_SWIFT_SWIFTVIRTUALSLOT_H
#define NEVERD_LOADER_SWIFT_SWIFTVIRTUALSLOT_H

#include "../ObjC/ObjCRuntimeData.h"
#include "SwiftFunctionSymbols.h"

#include "neverd/loader/ReadOnlyBytes.h"

#include "llvm/Demangle/SwiftDemangle.h"
#include "llvm/Support/Endian.h"

namespace neverd::swift_virtual_detail {

// A wrapper's void return does not describe a virtual callee inside it.
// Authenticate this slot's own declaration through a non-generic class's
// fixed vtable descriptor. Dispatch still reads the live receiver's table;
// its initial implementation supplies type evidence, not a replacement call.
inline bool isVoidClassVirtualSlot(const BinaryImage &Image,
                                   const ObjCMethod &Method, uint32_t Slot) {
  const va_t Metadata = Method.ClassAddress;
  objc::RuntimeData Data(Image);
  if (!Metadata || Metadata % 8 || Slot % 8 || Slot < 80 || Slot > 4096 ||
      Metadata > InvalidVA - 4104 || !Data.supportsPlainObjectPointers() ||
      !Data.bytes(Metadata, 80))
    return false;
  const auto RuntimeName = Data.className(Metadata);
  const auto Descriptor = readInitialImagePointer(Image, Metadata + 64);
  const auto ClassSize = Data.u32(Metadata + 56);
  const auto AddressPoint = Data.u32(Metadata + 60);
  if (!RuntimeName || *RuntimeName != Method.ClassName || !Descriptor ||
      !ClassSize || !AddressPoint || *AddressPoint > *ClassSize ||
      *ClassSize - *AddressPoint < Slot + 8 || *Descriptor > InvalidVA - 52)
    return false;
  const auto Header = readImmutableImageBytes(Image, *Descriptor, 52);
  if (!Header)
    return false;
  const auto Word = [&](unsigned Offset) {
    return llvm::support::endian::read32le(Header->data() + Offset);
  };
  const uint32_t Flags = Word(0);
  // Unique class context, ordinary metadata initialization, no generic or
  // resilient-superclass prefix. An override table follows the vtable and
  // does not change the offsets of this class's own method declarations.
  if ((Flags & 0x80000000u) == 0 || (Flags & ~0xc0000050u) != 0 ||
      (Flags & 0xffffu) != 0x50u || Word(24) > 512 || Word(28) > 512 ||
      uint64_t(Word(24)) * 8 != *AddressPoint ||
      uint64_t(Word(24) + Word(28)) * 8 != *ClassSize)
    return false;
  const uint32_t Start = Word(44), Count = Word(48);
  if (!Count || Count > 512 || Start > Word(28) || Count > Word(28) - Start ||
      Slot / 8 < Start || Slot / 8 - Start >= Count ||
      !readImmutableImageBytes(Image, *Descriptor, 52 + uint64_t(Count) * 8))
    return false;
  const auto Relative = [](va_t Field, uint32_t Bits) -> std::optional<va_t> {
    const int64_t Delta = int32_t(Bits);
    if (!Delta || (Delta < 0 ? Field < uint64_t(-Delta)
                             : Field > InvalidVA - uint64_t(Delta)))
      return std::nullopt;
    return Delta < 0 ? Field - uint64_t(-Delta) : Field + uint64_t(Delta);
  };
  // Only a direct, non-nested module context is supported here.
  const auto Parent =
      (Word(4) & 1) ? std::nullopt : Relative(*Descriptor + 4, Word(4));
  const auto NameAddress = Relative(*Descriptor + 8, Word(8));
  if (!Parent || *Parent > InvalidVA - 12 || !NameAddress)
    return false;
  const auto ParentBytes = readImmutableImageBytes(Image, *Parent, 12);
  if (!ParentBytes ||
      (llvm::support::endian::read32le(ParentBytes->data()) & ~0x40u) != 0 ||
      llvm::support::endian::read32le(ParentBytes->data() + 4) != 0)
    return false;
  const auto ModuleAddress = Relative(
      *Parent + 8, llvm::support::endian::read32le(ParentBytes->data() + 8));
  const auto Module =
      ModuleAddress ? Data.string(*ModuleAddress) : std::nullopt;
  const auto Name = Data.string(*NameAddress);
  if (!Module || !Name)
    return false;
  const uint64_t RecordOffset = 52 + uint64_t(Slot / 8 - Start) * 8;
  if (*Descriptor > InvalidVA - RecordOffset - 8)
    return false;
  const va_t Record = *Descriptor + RecordOffset;
  const auto Bytes = readImmutableImageBytes(Image, Record, 8);
  // Ordinary synchronous instance method. Getters, constructors, coroutines,
  // async functions and dynamically replaced methods need their own ABI.
  if (!Bytes || llvm::support::endian::read32le(Bytes->data()) != 0x10)
    return false;
  const auto Target =
      Relative(Record + 4, llvm::support::endian::read32le(Bytes->data() + 4));
  const auto Initial = Data.localPointer(Metadata + Slot);
  if (!Target || !Initial || *Initial != *Target ||
      !Image.CodePtrRelocSlots.count(Metadata + Slot) ||
      Image.DataPtrRelocSlots.count(Metadata + Slot) ||
      !Image.isCodeAddress(*Target))
    return false;
  const auto *Symbol = uniqueSwiftFunctionSymbol(Image, *Target);
  if (!Symbol)
    return false;
  llvm::StringRef Mangled(Symbol->Name);
  Mangled.consume_front("_");
  if (!Mangled.starts_with("$s"))
    return false;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 1024;
  Options.MaxNodes = 128;
  Options.MaxDepth = 24;
  Options.MaxMemoryBytes = 65536;
  Options.MaxOperations = 10000;
  const auto Parsed = llvm::swiftDemangle(Mangled.str(), Options);
  using Node = llvm::SwiftDemangleNode;
  const auto Shape = [](const Node &N, llvm::StringRef Kind, size_t Count) {
    return N.Kind == Kind && !N.Text && !N.Index && N.Children.size() == Count;
  };
  const auto Text = [](const Node &N, llvm::StringRef Kind,
                       llvm::StringRef Value) {
    return N.Kind == Kind && N.Text && *N.Text == Value && !N.Index &&
           N.Children.empty();
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 1) ||
      !Shape(Parsed.Root->Children[0], "Function", 3))
    return false;
  const auto &Function = Parsed.Root->Children[0];
  const auto &Context = Function.Children[0];
  if (!Shape(Context, "Class", 2) ||
      !Text(Context.Children[0], "Module", *Module) ||
      !Text(Context.Children[1], "Identifier", *Name) ||
      Function.Children[1].Kind != "Identifier" || !Function.Children[1].Text ||
      Function.Children[1].Text->empty() || Function.Children[1].Index ||
      !Function.Children[1].Children.empty() ||
      !Shape(Function.Children[2], "Type", 1) ||
      !Shape(Function.Children[2].Children[0], "FunctionType", 2))
    return false;
  const auto &Type = Function.Children[2].Children[0];
  const auto Empty = [&](const Node &N, llvm::StringRef Kind) {
    return Shape(N, Kind, 1) && Shape(N.Children[0], "Type", 1) &&
           Shape(N.Children[0].Children[0], "Tuple", 0);
  };
  return Empty(Type.Children[0], "ArgumentTuple") &&
         Empty(Type.Children[1], "ReturnType");
}
} // namespace neverd::swift_virtual_detail

#endif
