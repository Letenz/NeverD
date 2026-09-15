#include "neverd/loader/ObjC/ObjCCallHints.h"

#include "../MachO/DarwinRuntimeImport.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/DarwinRuntimeCalls.h"
#include "neverd/loader/ObjC/ObjCFormattedCalls.h"
#include "neverd/loader/ObjC/ObjCSourceDeclarations.h"
#include "neverd/loader/Swift/SwiftRuntimeCalls.h"
#include "neverd/loader/Swift/SwiftStringCalls.h"
#include "neverd/object/SectionNames.h"

#include "llvm/Support/Endian.h"

#include <deque>
#include <optional>
#include <tuple>

namespace neverd {
namespace {

std::string importAt(const BinaryImage &Image, va_t Slot) {
  const auto Import = darwinRuntimeImport(Image, Slot);
  if (!Import)
    return {};
  llvm::StringRef Name(*Import);
  Name.consume_front("_");
  if (Name == "objc_msgSend" || Name == "objc_msgSendSuper2" ||
      objcRuntimeSourceCallHint(Image, Slot) ||
      darwinRuntimeSourceCallHint(Image, Slot) ||
      darwinRuntimeFormatDeclaration(Image, Slot) ||
      swiftStringSourceCallHint(Image, Slot) ||
      swiftRuntimeSourceCallHint(Image, Slot))
    return Name.str();
  return {};
}

const uint8_t *code(const BinaryImage &Image, va_t Address, size_t Size) {
  const auto *Section = Image.getSectionFor(Address);
  const auto *Segment = Image.getSegmentFor(Address);
  if (!Section || !Segment || !Section->isExecutable() ||
      !Segment->isExecutable() || Address > InvalidVA - Size ||
      !rangeInBounds(Address - Section->VA, Size, Section->FileSz) ||
      !rangeInBounds(Address - Segment->VA, Size, Segment->FileSz))
    return nullptr;
  return Image.readVA(Address, Size);
}

std::optional<va_t> addSigned(va_t Base, int64_t Offset) {
  if ((Offset < 0 && Base < uint64_t(-Offset)) ||
      (Offset >= 0 && Base > InvalidVA - uint64_t(Offset)))
    return std::nullopt;
  return Offset < 0 ? Base - uint64_t(-Offset) : Base + uint64_t(Offset);
}

std::optional<va_t> adrp(uint32_t Instruction, va_t PC, unsigned Register) {
  if ((Instruction & 0x9f00001f) != (0x90000000u | Register))
    return std::nullopt;
  const uint32_t Imm =
      ((Instruction >> 29) & 3) | (((Instruction >> 5) & 0x7ffff) << 2);
  const int64_t Signed = (Imm & 0x100000) ? int64_t(Imm) - 0x200000 : Imm;
  return addSigned(PC & ~va_t(4095), Signed * 4096);
}

std::optional<va_t> ldrSlot(uint32_t Instruction, va_t Page,
                            unsigned Register) {
  if ((Instruction & 0xffc003ff) != (0xf9400000u | (Register << 5) | Register))
    return std::nullopt;
  return addSigned(Page, ((Instruction >> 10) & 4095) * 8);
}

struct Dispatch {
  std::string Name;
  std::string Selector;
  va_t SelectorSlot = 0;
  va_t ImportSlot = 0;
};

std::optional<Dispatch> veneer(const BinaryImage &Image, va_t Address) {
  if (Image.Arch == Arch::X64) {
    const auto *Bytes = code(Image, Address, 6);
    if (!Bytes || Bytes[0] != 0xff || Bytes[1] != 0x25)
      return std::nullopt;
    auto Slot = addSigned(Address + 6,
                          int32_t(llvm::support::endian::read32le(Bytes + 2)));
    const auto Name = Slot ? importAt(Image, *Slot) : std::string();
    return Name.empty() ? std::nullopt
                        : std::optional<Dispatch>({Name, {}, 0, *Slot});
  }
  if (Image.Arch != Arch::AArch64)
    return std::nullopt;
  const auto *Bytes = code(Image, Address, 12);
  if (!Bytes)
    return std::nullopt;
  auto Word = [&](unsigned I) {
    return llvm::support::endian::read32le(Bytes + I * 4);
  };
  Dispatch Result;
  // ld's selector-specific stubs load x1, then perform the ordinary import
  // veneer. Symbols are deliberately irrelevant, so stripped stubs work too.
  if (auto Page = adrp(Word(0), Address, 1)) {
    auto Slot = ldrSlot(Word(1), *Page, 1);
    if (!Slot)
      return std::nullopt;
    auto Reference = Image.ObjCSourceReferences.find(*Slot);
    if (Reference == Image.ObjCSourceReferences.end() ||
        Reference->second.TheKind != ObjCSourceReference::Kind::Selector ||
        Reference->second.Size != 8)
      return std::nullopt;
    Result.Selector = Reference->second.Name;
    Result.SelectorSlot = *Slot;
    Address += 8;
    Bytes = code(Image, Address, 12);
    if (!Bytes)
      return std::nullopt;
  }
  auto Page = adrp(Word(0), Address, 16);
  auto Slot = Page ? ldrSlot(Word(1), *Page, 16) : std::nullopt;
  if (!Slot || Word(2) != 0xd61f0200u) // BR x16
    return std::nullopt;
  Result.Name = importAt(Image, *Slot);
  Result.ImportSlot = *Slot;
  // A selector-loading veneer only has a proven ABI for message dispatch.
  if (Result.SelectorSlot && Result.Name != "objc_msgSend" &&
      Result.Name != "objc_msgSendSuper2")
    return std::nullopt;
  return Result.Name.empty() ? std::nullopt
                             : std::optional<Dispatch>(std::move(Result));
}

struct Value {
  enum class Kind {
    Number,
    Selector,
    Import,
    InstanceSelf,
    ClassSelf,
    ClassReference
  };
  Kind TheKind = Kind::Number;
  uint64_t Number = 0;
  std::string Name;
  bool operator==(const Value &Other) const {
    return std::tie(TheKind, Number, Name) ==
           std::tie(Other.TheKind, Other.Number, Other.Name);
  }
};
using Key = std::tuple<VnodeSpace, uint64_t, uint16_t>;
Key key(const NdVar &V) { return {V.Space, V.Offset, V.Size}; }

std::optional<ObjCReceiverTypeHint> receiver(const Value &V) {
  if (V.TheKind != Value::Kind::InstanceSelf &&
      V.TheKind != Value::Kind::ClassSelf &&
      V.TheKind != Value::Kind::ClassReference)
    return std::nullopt;
  return ObjCReceiverTypeHint{
      V.TheKind == Value::Kind::ClassReference
          ? ObjCReceiverTypeHint::OriginKind::ClassReference
          : ObjCReceiverTypeHint::OriginKind::MethodEntry,
      V.Number, V.Name, V.TheKind != Value::Kind::InstanceSelf};
}
} // namespace

std::optional<SourceCallTypeHint>
objcRuntimeSourceCallHint(const BinaryImage &Image, va_t ImportSlot) {
  const auto Import = darwinRuntimeImport(Image, ImportSlot);
  if (!Import)
    return std::nullopt;
  llvm::StringRef Name(*Import);
  if (!Name.consume_front("_"))
    return std::nullopt;
  std::optional<unsigned> ArgumentRegister;
  llvm::StringRef Canonical = Name;
  for (llvm::StringRef Operation : {"objc_retain", "objc_release"}) {
    if (!Name.starts_with((Operation + "_x").str()))
      continue;
    const auto Suffix = Name.drop_front(Operation.size() + 2);
    unsigned Register;
    if (Image.Arch != Arch::AArch64 || Suffix.getAsInteger(10, Register) ||
        Suffix != std::to_string(Register) ||
        !(Register <= 15 || (Register >= 19 && Register <= 28)))
      return std::nullopt;
    Canonical = Operation;
    ArgumentRegister = Register;
  }
  SourceCallTypeHint Result;
  Result.CallKind = SourceCallTypeHint::Kind::ObjCRuntimeCall;
  Result.TargetAddress = ImportSlot;
  Result.TargetName = Canonical.str();
  auto &Signature = Result.Signature;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::ObjCRuntime;
  const auto Object = NdType::makePtr(NdType::makeVoid());
  const auto Slot = NdType::makePtr(Object);
  if (Canonical == "objc_retain" || Canonical == "objc_autorelease" ||
      Canonical == "objc_autoreleaseReturnValue" ||
      Canonical == "objc_retainAutorelease" ||
      Canonical == "objc_retainAutoreleaseReturnValue" ||
      Canonical == "objc_retainAutoreleasedReturnValue" ||
      Canonical == "objc_unsafeClaimAutoreleasedReturnValue" ||
      Canonical == "objc_retainBlock" || Canonical == "objc_alloc" ||
      Canonical == "objc_allocWithZone" || Canonical == "objc_alloc_init" ||
      Canonical == "objc_opt_new" || Canonical == "objc_opt_self" ||
      Canonical == "objc_opt_class") {
    Signature.ReturnType = Object;
    Signature.Parameters = {{"object", Object}};
  } else if (Canonical == "objc_release" ||
             Canonical == "objc_autoreleasePoolPop") {
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"object", Object}};
  } else if (Canonical == "objc_autoreleasePoolPush") {
    Signature.ReturnType = Object;
  } else if (Canonical == "objc_opt_isKindOfClass" ||
             Canonical == "objc_opt_respondsToSelector") {
    const auto Bind = Image.DyldBindSlots.find(ImportSlot);
    if (Bind == Image.DyldBindSlots.end() ||
        Bind->second.Module != "/usr/lib/libobjc.A.dylib")
      return std::nullopt;
    // These runtime queries return BOOL and preserve custom overrides.
    // https://github.com/apple-oss-distributions/objc4/blob/main/runtime/objc-internal.h
    // Carry its byte unchanged. ARM64 BOOL is unsigned and extends to W0;
    // x86_64 defines only AL, with signed/boolean conversions in the caller.
    Signature.ReturnType = NdType::makeInt(1, false);
    Signature.Parameters = {{"object", Object}, {"query", Object}};
  } else if (Canonical == "objc_storeStrong" || Canonical == "objc_storeWeak" ||
             Canonical == "objc_initWeak") {
    Signature.ReturnType =
        Canonical == "objc_storeStrong" ? NdType::makeVoid() : Object;
    Signature.Parameters = {{"slot", Slot}, {"object", Object}};
  } else if (Canonical == "objc_loadWeak" ||
             Canonical == "objc_loadWeakRetained" ||
             Canonical == "objc_destroyWeak") {
    Signature.ReturnType =
        Canonical == "objc_destroyWeak" ? NdType::makeVoid() : Object;
    Signature.Parameters = {{"slot", Slot}};
  } else if (Canonical == "objc_copyWeak" || Canonical == "objc_moveWeak") {
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"destination", Slot}, {"source", Slot}};
  } else if (Canonical == "objc_getAssociatedObject") {
    Signature.ReturnType = Object;
    Signature.Parameters = {{"object", Object}, {"key", Object}};
  } else if (Canonical == "objc_setAssociatedObject") {
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"object", Object},
                            {"key", Object},
                            {"value", Object},
                            {"policy", NdType::makeInt(8, false)}};
  } else if (Canonical == "objc_removeAssociatedObjects" ||
             Canonical == "objc_enumerationMutation") {
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"object", Object}};
  } else if (Canonical == "objc_setProperty_atomic" ||
             Canonical == "objc_setProperty_nonatomic" ||
             Canonical == "objc_setProperty_atomic_copy" ||
             Canonical == "objc_setProperty_nonatomic_copy") {
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"object", Object},
                            {"selector", Object},
                            {"value", Object},
                            {"offset", NdType::makeInt(8)}};
  } else {
    return std::nullopt;
  }
  std::string Diagnostic;
  if (!assignDarwinScalarSourceABI(Signature, Image.Arch, Diagnostic))
    return std::nullopt;
  if (ArgumentRegister)
    Signature.Parameters[0].Location.RegisterOffset =
        a64reg::X0 + *ArgumentRegister * 8;
  return Result;
}

bool objcSelectorStubOverwritesCommand(const BinaryImage &Image, va_t Address) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64)
    return false;
  const auto *Section = Image.getSectionFor(Address);
  if (!Section || Section->Name != section_names::macho::ObjCStubs)
    return false;
  const auto Target = veneer(Image, Address);
  return Target && Target->Name == "objc_msgSend" &&
         Target->SelectorSlot != 0 && !Target->Selector.empty();
}

std::map<va_t, SourceCallTypeHint>
buildObjCSourceCallHints(const BinaryImage &Image, const LowFunc &Function) {
  std::map<va_t, SourceCallTypeHint> Result;
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64))
    return Result;
  const auto &TRI = getTargetRegInfo(Image.Arch);
  const size_t Count = Function.Blocks.size();
  if (Count > 16384)
    return {};
  std::map<int, size_t> BlockIndices;
  for (size_t I = 0; I < Function.Blocks.size(); ++I)
    if (!BlockIndices.emplace(Function.Blocks[I].Id, I).second)
      return {};
  // Only reciprocal ordinary edges carry machine-state facts. Explicit
  // entry roles and exceptional entries contribute an unknown incoming state.
  // Unvisited predecessors are the dataflow bottom, not an unknown value;
  // later backedges can invalidate provisional facts before publication.
  std::vector<std::vector<size_t>> Parents(Count), Children(Count);
  std::vector<bool> Roots(Count), ExceptionalRoots(Count), Queued(Count);
  std::map<va_t, unsigned> CallOccurrences;
  for (size_t I = 0; I < Count; ++I) {
    const auto &Block = Function.Blocks[I];
    Roots[I] = Block.StartAddr == Function.Entry || Block.Preds.empty() ||
               Function.ModuleAnalysisRoots.count(Block.StartAddr) ||
               Function.OrdinaryModuleAnalysisRoots.count(Block.StartAddr) ||
               !Block.ExceptionalPreds.empty();
    ExceptionalRoots[I] = !Block.ExceptionalPreds.empty();
    for (int Parent : Block.Preds) {
      const auto Found = BlockIndices.find(Parent);
      if (Found == BlockIndices.end() ||
          !Function.Blocks[Found->second].hasSucc(Block.Id))
        return {};
      Parents[I].push_back(Found->second);
    }
    for (int Child : Block.Succs) {
      const auto Found = BlockIndices.find(Child);
      if (Found == BlockIndices.end() ||
          !llvm::is_contained(Function.Blocks[Found->second].Preds, Block.Id))
        return {};
      Children[I].push_back(Found->second);
    }
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
        ++CallOccurrences[Op.Addr];
  }
  for (const auto &Block : Function.Blocks)
    for (const auto &Edge : Block.ExceptionalSuccs) {
      if (Edge.BlockId < 0)
        continue; // An unwind outside this function has no local successor.
      const auto Found = BlockIndices.find(Edge.BlockId);
      if (Found == BlockIndices.end())
        return {};
      Roots[Found->second] = true;
      ExceptionalRoots[Found->second] = true;
    }
  using Facts = std::map<Key, Value>;
  using Hints = std::map<va_t, SourceCallTypeHint>;
  Facts EntryFacts;
  if (llvm::count_if(Function.Blocks, [&](const auto &Block) {
        return Block.StartAddr == Function.Entry;
      }) == 1)
    if (const auto Receiver = objcMethodReceiverTypeHint(Image, Function.Entry))
      EntryFacts.emplace(key(NdVar::reg(TRI.IntParamRegs[0], 8)),
                         Value{Receiver->IsClassMethod
                                   ? Value::Kind::ClassSelf
                                   : Value::Kind::InstanceSelf,
                               Receiver->Address, Receiver->ClassName});
  std::map<std::tuple<Value::Kind, uint64_t, std::string, std::string>,
           ObjCReceiverDeclaration>
      ReceiverDeclarations;
  auto Transfer = [&](size_t Index, Facts Values,
                      Hints &BlockHints) -> std::optional<Facts> {
    const auto &Block = Function.Blocks[Index];
    va_t PreviousAddress = InvalidVA;
    auto Read = [&](const NdVar &V) -> std::optional<Value> {
      if (V.Space == VnodeSpace::CONST)
        return Value{Value::Kind::Number, V.Offset, {}};
      auto It = Values.find(key(V));
      return It == Values.end() ? std::nullopt
                                : std::optional<Value>(It->second);
    };
    auto Clobber = [&](bool KnownABI) {
      for (auto It = Values.begin(); It != Values.end();) {
        const auto &[Space, Offset, Size] = It->first;
        if (!KnownABI || Space != VnodeSpace::REG ||
            !TRI.isCallPreserved(Offset, Size))
          It = Values.erase(It);
        else
          ++It;
      }
    };
    for (const auto &Op : Block.Ops) {
      if (Op.Addr != PreviousAddress) {
        for (auto It = Values.begin(); It != Values.end();)
          if (std::get<0>(It->first) == VnodeSpace::TEMP)
            It = Values.erase(It);
          else
            ++It;
        PreviousAddress = Op.Addr;
      }
      if (Op.Opcode == NdOp::INTRINSIC) {
        Clobber(false);
        continue;
      }
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        if (CallOccurrences.at(Op.Addr) != 1) {
          Clobber(false);
          continue;
        }
        std::optional<Dispatch> Target;
        auto V = Op.NumInputs ? Read(Op.Inputs[0]) : std::nullopt;
        if (V && V->TheKind == Value::Kind::Import)
          Target = Dispatch{V->Name, {}, 0, V->Number};
        else if (V && V->TheKind == Value::Kind::Number) {
          if (Op.Opcode == NdOp::INDIR_CALL &&
              Op.Inputs[0].Space == VnodeSpace::CONST) {
            auto Name = importAt(Image, V->Number);
            if (!Name.empty())
              Target = Dispatch{Name, {}, 0, V->Number};
          } else if (Op.Opcode == NdOp::CALL) {
            Target = veneer(Image, V->Number);
          }
        }
        if (Target) {
          auto Runtime = objcRuntimeSourceCallHint(Image, Target->ImportSlot);
          if (!Runtime)
            Runtime = swiftRuntimeSourceCallHint(Image, Target->ImportSlot);
          if (!Runtime)
            Runtime = darwinRuntimeSourceCallHint(Image, Target->ImportSlot);
          if (!Runtime)
            Runtime = swiftStringSourceCallHint(Image, Target->ImportSlot);
          if (!Runtime) {
            const auto Declaration =
                darwinRuntimeFormatDeclaration(Image, Target->ImportSlot);
            if (Declaration) {
              const auto &Location =
                  Declaration->Signature
                      .Parameters[Declaration->FormatParameter]
                      .Location;
              const auto Format =
                  Location.Kind == SourceABICarrierKind::IntegerRegister
                      ? Read(NdVar::reg(Location.RegisterOffset, 8))
                      : std::nullopt;
              if (Format && Format->TheKind == Value::Kind::Number)
                Runtime = darwinFormattedSourceCallHint(
                    Image, Target->ImportSlot, Format->Number);
            }
          }
          if (Runtime) {
            BlockHints.emplace(Op.Addr, std::move(*Runtime));
            Clobber(true);
            continue;
          }
        }
        // A known import whose format contract is unresolved is still that
        // C routine. Selector-shaped register contents cannot turn it into
        // Objective-C message dispatch.
        if (Target && Target->Name != "objc_msgSend" &&
            Target->Name != "objc_msgSendSuper2") {
          Clobber(false);
          continue;
        }
        if (Target && Target->Selector.empty()) {
          NdVar Selector;
          Selector.Space = VnodeSpace::REG;
          Selector.Offset = TRI.IntParamRegs[1];
          Selector.Size = 8;
          auto Name = Read(Selector);
          if (Name && Name->TheKind == Value::Kind::Selector)
            Target->Selector = Name->Name;
        }
        if (Target && !Target->Selector.empty()) {
          std::optional<ObjCReceiverTypeHint> Receiver;
          ObjCReceiverDeclaration Declaration;
          const auto Self = Read(NdVar::reg(TRI.IntParamRegs[0], 8));
          if (Self && Target->Name == "objc_msgSend")
            Receiver = receiver(*Self);
          if (Receiver) {
            auto [It, Inserted] = ReceiverDeclarations.try_emplace(std::tuple{
                Self->TheKind, Self->Number, Self->Name, Target->Selector});
            if (Inserted)
              It->second = objcReceiverSourceTypeHint(Image, Target->Selector,
                                                      *Receiver);
            Declaration = It->second;
          }
          const bool Qualified = Declaration.HasDeclaration &&
                                 !Declaration.RequiresGlobalAgreement;
          auto Signature =
              Qualified ? Declaration.Signature
                        : objcSelectorSourceTypeHint(Image, Target->Selector);
          if (Signature) {
            SourceCallTypeHint Hint;
            Hint.CallKind = Target->Name == "objc_msgSendSuper2"
                                ? SourceCallTypeHint::Kind::ObjCSuper2
                                : SourceCallTypeHint::Kind::ObjCMessage;
            Hint.Signature = std::move(*Signature);
            Hint.TargetAddress =
                V && V->TheKind == Value::Kind::Number ? V->Number : 0;
            Hint.TargetName = Target->Name;
            Hint.Selector = Target->Selector;
            Hint.SelectorReferenceAddress = Target->SelectorSlot;
            if (Qualified)
              Hint.Receiver = std::move(Receiver);
            BlockHints.emplace(Op.Addr, std::move(Hint));
          } else if (Target->Name == "objc_msgSend") {
            const auto Declaration =
                objcSelectorFormatDeclaration(Image, Target->Selector);
            if (Declaration) {
              const auto &Location =
                  Declaration->Signature
                      .Parameters[Declaration->FormatParameter]
                      .Location;
              auto Format =
                  Location.Kind == SourceABICarrierKind::IntegerRegister
                      ? Read(NdVar::reg(Location.RegisterOffset, 8))
                      : std::nullopt;
              auto Hint = Format && Format->TheKind == Value::Kind::Number
                              ? objcFormattedSourceCallHint(
                                    Image, Target->Selector, Format->Number)
                              : std::nullopt;
              if (Hint) {
                Hint->TargetAddress =
                    V && V->TheKind == Value::Kind::Number ? V->Number : 0;
                Hint->SelectorReferenceAddress = Target->SelectorSlot;
                BlockHints.emplace(Op.Addr, std::move(*Hint));
              }
            }
          }
        }
        // Only a bound Darwin ABI establishes which physical views survive.
        // Unknown calls may use another convention and invalidate every fact.
        Clobber(BlockHints.count(Op.Addr) != 0);
        continue;
      }
      std::optional<Value> Out;
      if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
        Out = Read(Op.Inputs[0]);
      else if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
               Op.NumInputs == 2) {
        auto A = Read(Op.Inputs[0]);
        auto B = Read(Op.Inputs[1]);
        if (A && B && A->TheKind == Value::Kind::Number &&
            B->TheKind == Value::Kind::Number)
          Out = Value{Value::Kind::Number,
                      Op.Opcode == NdOp::INT_ADD ? A->Number + B->Number
                                                 : A->Number - B->Number,
                      {}};
      } else if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1 &&
                 Op.Output.Size == 8) {
        auto Address = Read(Op.Inputs[0]);
        if (Address && Address->TheKind == Value::Kind::Number) {
          auto Ref = Image.ObjCSourceReferences.find(Address->Number);
          if (Ref != Image.ObjCSourceReferences.end() &&
              Ref->second.TheKind == ObjCSourceReference::Kind::Selector &&
              Ref->second.Size == 8)
            Out = Value{Value::Kind::Selector, 0, Ref->second.Name};
          else if (Ref != Image.ObjCSourceReferences.end() &&
                   Ref->second.TheKind == ObjCSourceReference::Kind::Class &&
                   Ref->second.Size == 8 &&
                   Ref->second.Address == Address->Number &&
                   !Ref->second.Name.empty())
            Out = Value{Value::Kind::ClassReference, Address->Number,
                        Ref->second.Name};
          else if (auto Name = importAt(Image, Address->Number); !Name.empty())
            Out = Value{Value::Kind::Import, Address->Number, std::move(Name)};
        }
      }
      if (!Op.Output.Size)
        continue;
      // Kill all overlapping physical aliases, not just the queried width.
      for (auto It = Values.begin(); It != Values.end();)
        if (std::get<0>(It->first) == Op.Output.Space &&
            std::get<1>(It->first) < Op.Output.Offset + Op.Output.Size &&
            Op.Output.Offset < std::get<1>(It->first) + std::get<2>(It->first))
          It = Values.erase(It);
        else
          ++It;
      if (Out && Op.Output.Size <= 8) {
        if (Out->TheKind == Value::Kind::Number) {
          if (Op.Output.Size < 8)
            Out->Number &= (uint64_t(1) << (Op.Output.Size * 8)) - 1;
          Values.emplace(key(Op.Output), std::move(*Out));
        } else if (Op.Output.Size == 8)
          Values.emplace(key(Op.Output), std::move(*Out));
        if (Values.size() > 4096)
          return std::nullopt;
      }
    }
    for (auto It = Values.begin(); It != Values.end();)
      if (std::get<0>(It->first) == VnodeSpace::TEMP)
        It = Values.erase(It);
      else
        ++It;
    return Values;
  };
  std::vector<std::optional<Facts>> Exits(Count);
  std::vector<Hints> Bindings(Count);
  std::deque<size_t> Work;
  for (size_t I = 0; I < Count; ++I)
    if (Roots[I]) {
      Work.push_back(I);
      Queued[I] = true;
    }
  size_t Remaining = 1048576;
  while (!Work.empty()) {
    const auto Index = Work.front();
    Work.pop_front();
    Queued[Index] = false;
    const auto Cost = std::max(size_t(1), Function.Blocks[Index].Ops.size());
    if (Cost > Remaining)
      return {}; // Never publish a partially converged proof.
    Remaining -= Cost;
    Facts Incoming;
    bool Initialized = Roots[Index];
    if (Function.Blocks[Index].StartAddr == Function.Entry &&
        !ExceptionalRoots[Index])
      Incoming = EntryFacts;
    for (size_t Parent : Parents[Index]) {
      if (!Exits[Parent])
        continue;
      if (!Initialized) {
        Incoming = *Exits[Parent];
        Initialized = true;
      } else {
        for (auto It = Incoming.begin(); It != Incoming.end();) {
          const auto Found = Exits[Parent]->find(It->first);
          if (Found == Exits[Parent]->end() || !(It->second == Found->second))
            It = Incoming.erase(It);
          else
            ++It;
        }
      }
    }
    if (!Initialized)
      continue;
    Bindings[Index].clear();
    auto Out = Transfer(Index, std::move(Incoming), Bindings[Index]);
    if (!Out)
      return {};
    if (Exits[Index] && *Exits[Index] == *Out)
      continue;
    Exits[Index] = std::move(Out);
    for (size_t Child : Children[Index])
      if (!Queued[Child]) {
        Queued[Child] = true;
        Work.push_back(Child);
      }
  }
  for (auto &Block : Bindings)
    for (auto &[Address, Hint] : Block)
      if (CallOccurrences[Address] == 1)
        Result.emplace(Address, std::move(Hint));
  return Result;
}
} // namespace neverd
