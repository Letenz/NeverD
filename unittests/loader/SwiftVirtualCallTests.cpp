#include "gtest/gtest.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/DarwinRuntimeCalls.h"
#include "neverd/loader/Swift/SwiftVirtualCalls.h"

#include "llvm/Support/Endian.h"

using namespace neverd;

namespace {
struct Fixture {
  enum class GetterKind { CGFloat, Double, Bool };
  static constexpr va_t Entry = 0x1100;
  static constexpr va_t CallSite = 0x1170;
  static constexpr va_t IvarSlot = 0x2100;
  static constexpr va_t MaskSlot = 0x2180;

  BinaryImage Image;
  LowFunc Function;

  explicit Fixture(GetterKind Kind = GetterKind::CGFloat, bool Setter = false) {
    const bool Bool = Kind == GetterKind::Bool;
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Segment Text;
    Text.VA = 0x1000;
    Text.Size = Text.FileSz = 0x1000;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(0x1000);
    Text.Data[CallSite - Text.VA + 0] = Bool && Setter ? 0xc0 : 0xa0;
    Text.Data[CallSite - Text.VA + 1] = 0x02;
    Text.Data[CallSite - Text.VA + 2] = 0x3f;
    Text.Data[CallSite - Text.VA + 3] = 0xd6;
    Image.Segments.push_back(std::move(Text));
    Segment Data;
    Data.VA = 0x2000;
    Data.Size = Data.FileSz = 0x1000;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(0x1000);
    Image.Segments.push_back(std::move(Data));
    Image.ImportPtrSlots[MaskSlot] = "_swift_isaMask";
    Image.DyldBindSlots[MaskSlot] = {
        "_swift_isaMask", 0, "/usr/lib/swift/libswiftCore.dylib", false};
    Image.ObjCSourceReferences[IvarSlot] = {
        ObjCSourceReference::Kind::IvarOffset, IvarSlot, 8, "_animationView",
        "_TtC6Lottie23CompatibleAnimationView"};
    ObjCMethod Method;
    Method.Implementation = Entry;
    Method.ClassName = "_TtC6Lottie23CompatibleAnimationView";
    Method.Selector =
        Bool ? (Setter ? "setShouldRasterizeWhenIdle:"
                       : "shouldRasterizeWhenIdle")
        : Kind == GetterKind::Double
            ? (Setter ? "setCurrentTime:" : "currentTime")
            : (Setter ? "setCurrentProgress:" : "currentProgress");
    Method.TypeEncoding = Setter ? (Bool ? "v20@0:8B16" : "v24@0:8d16")
                                 : (Bool ? "B16@0:8" : "d16@0:8");
    SourceFunctionTypeHint Signature;
    Signature.ReturnType =
        Setter ? NdType::makeVoid()
               : (Bool ? NdType::makeInt(1, false) : NdType::makeFloat(8));
    Signature.Parameters = {{"self", NdType::makePtr(NdType::makeVoid())},
                            {"_cmd", NdType::makePtr(NdType::makeVoid())}};
    if (Setter)
      Signature.Parameters.push_back(
          {"value", Bool ? NdType::makeInt(1, false) : NdType::makeFloat(8)});
    std::string Diagnostic;
    EXPECT_TRUE(assignDarwinObjCSourceABI(Signature, Arch::AArch64, Diagnostic))
        << Diagnostic;
    Method.TypeHint = Signature;
    Image.ObjCMethods.push_back(Method);
    const std::string SymbolPrefix =
        Bool ? "_$s6Lottie23CompatibleAnimationViewC23shouldRasterizeWhenIdle"
        : Kind == GetterKind::Double
            ? "_$s6Lottie23CompatibleAnimationViewC11currentTime"
            : "_$s6Lottie23CompatibleAnimationViewC15currentProgress";
    const std::string SymbolSuffix =
        Bool                         ? (Setter ? "SbvsTo" : "SbvgTo")
        : Kind == GetterKind::Double ? (Setter ? "SdvsTo" : "SdvgTo")
                                     : (Setter ? "12CoreGraphics7CGFloatVvsTo"
                                               : "12CoreGraphics7CGFloatVvgTo");
    Image.Symbols.push_back({SymbolPrefix + SymbolSuffix, Entry, 0x80, true});

    Function.Entry = Entry;
    Function.Blocks.resize(1);
    Function.Blocks[0].StartAddr = Entry;
    auto Add = [&](va_t Address, NdOp Code, NdVar Output,
                   std::initializer_list<NdVar> Inputs) {
      LowOp Op;
      Op.Addr = Address;
      Op.Opcode = Code;
      Op.Output = Output;
      for (const auto &Input : Inputs)
        Op.addInput(Input);
      Function.Blocks[0].Ops.push_back(Op);
    };
    const auto X0 = NdVar::reg(a64reg::X0, 8);
    const auto X8 = NdVar::reg(a64reg::X8, 8);
    const auto X9 = NdVar::reg(a64reg::X9, 8);
    const auto X20 = NdVar::reg(a64reg::X20, 8);
    const auto X21 = NdVar::reg(a64reg::X21, 8);
    const auto X22 = NdVar::reg(a64reg::X22, 8);
    const auto Target = Bool && Setter ? X22 : X21;
    Add(0x1110, NdOp::COPY, X8, {NdVar::dataAddress(IvarSlot, 8)});
    Add(0x1114, NdOp::LOAD, X8, {X8});
    Add(0x111c, NdOp::INT_ADD, NdVar::tmp(TmpBase, 8), {X0, X8});
    Add(0x111c, NdOp::LOAD, X20, {NdVar::tmp(TmpBase, 8)});
    Add(0x1120, NdOp::LOAD, X8, {X20});
    Add(0x1124, NdOp::COPY, X9, {NdVar::dataAddress(MaskSlot, 8)});
    Add(0x1128, NdOp::LOAD, X9, {X9});
    Add(0x112c, NdOp::LOAD, X9, {X9});
    Add(0x1130, NdOp::INT_AND, X8, {X8, X9});
    Add(0x1138, NdOp::INT_ADD, NdVar::tmp(TmpBase, 8),
        {X8, NdVar::scalar((Bool                         ? 600
                            : Kind == GetterKind::Double ? 648
                                                         : 624) +
                               (Setter ? 8 : 0),
                           8)});
    Add(0x1138, NdOp::LOAD, Target, {NdVar::tmp(TmpBase, 8)});
    Add(0x1140, NdOp::CALL, X0, {NdVar::codeAddress(0x1300, 8)});
    Add(CallSite, NdOp::INDIR_CALL, X0, {Target});
  }
};

void addVoidVirtualMetadata(Fixture &F) {
  constexpr va_t Metadata = 0x2400, Descriptor = 0x4000;
  auto &Method = F.Image.ObjCMethods[0];
  Method.ClassAddress = Metadata;
  Section Data;
  Data.VA = 0x2000;
  Data.Size = Data.FileSz = 0x1000;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  F.Image.Sections.push_back(Data);
  Segment ReadOnly;
  ReadOnly.VA = Descriptor;
  ReadOnly.Size = ReadOnly.FileSz = 0x200;
  ReadOnly.Flags = SegmentFlags::Readable;
  ReadOnly.Data.resize(0x200);
  F.Image.Segments.push_back(ReadOnly);
  Section Constants;
  Constants.VA = Descriptor;
  Constants.Size = Constants.FileSz = 0x200;
  Constants.Flags = SegmentFlags::Readable;
  F.Image.Sections.push_back(Constants);
  auto &Mutable = F.Image.Segments[1].Data;
  const auto Put64 = [&](va_t Address, uint64_t Value) {
    llvm::support::endian::write64le(Mutable.data() + Address - 0x2000, Value);
  };
  Put64(Metadata + 32, 0x2502);
  Put64(0x2500 + 24, 0x2580);
  std::copy(Method.ClassName.begin(), Method.ClassName.end(),
            Mutable.begin() + 0x580);
  Put64(Metadata + 56, uint64_t(24) << 32 | 136);
  Put64(Metadata + 64, Descriptor);
  Put64(Metadata + 96, 0x1400);
  F.Image.DataPtrRelocSlots.insert(Metadata + 64);
  F.Image.DataPtrRelocTargetOwners[Metadata + 64] = Descriptor;
  F.Image.CodePtrRelocSlots.insert(Metadata + 96);
  auto &Bytes = F.Image.Segments[2].Data;
  const auto Put32 = [&](unsigned Offset, uint32_t Value) {
    llvm::support::endian::write32le(Bytes.data() + Offset, Value);
  };
  Put32(0, 0x80000050);
  Put32(4, 0xc0 - 4);
  Put32(8, 0x80 - 8);
  Put32(24, 3);
  Put32(28, 14);
  Put32(32, 1);
  Put32(44, 12);
  Put32(48, 1);
  Put32(52, 0x10);
  Put32(56, uint32_t(0x1400 - (Descriptor + 56)));
  Put32(0xc0, 0);
  Put32(0xc8, 0xe0 - 0xc8);
  const std::string Name = "AnimationViewBase", Module = "Lottie";
  std::copy(Name.begin(), Name.end(), Bytes.begin() + 0x80);
  std::copy(Module.begin(), Module.end(), Bytes.begin() + 0xe0);
  F.Image.Symbols.push_back(
      {"_$s6Lottie17AnimationViewBaseC6layoutyyF", 0x1400, 4, true});
}
} // namespace

TEST(SwiftVirtualCalls, ExactMaskedIsaGetterBindsSwiftContext) {
  Fixture F;
  const auto Hints = buildSwiftVirtualCallHints(F.Image, F.Function);
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Hint = Hints.at(Fixture::CallSite);
  ASSERT_TRUE(Hint.Virtual);
  EXPECT_EQ(Hint.Virtual->MethodEntry, Fixture::Entry);
  EXPECT_EQ(Hint.Virtual->IsaMaskImport, Fixture::MaskSlot);
  EXPECT_EQ(Hint.Virtual->VtableByteOffset, 624U);
  EXPECT_TRUE(isSwiftVirtualSourceCallHint(F.Image, Hint));
  EXPECT_EQ(Hint.Signature.Parameters.at(0).TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  auto Global = darwinRuntimeGlobalAddressHint(F.Image, Fixture::MaskSlot);
  ASSERT_TRUE(Global);
  EXPECT_EQ(Global->TargetName, "swift_isaMask");
}

TEST(SwiftVirtualCalls, ExactMaskedIsaBoolGetterBindsSwiftContext) {
  Fixture F(Fixture::GetterKind::Bool);
  const auto Hints = buildSwiftVirtualCallHints(F.Image, F.Function);
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Hint = Hints.at(Fixture::CallSite);
  ASSERT_TRUE(Hint.Virtual);
  EXPECT_EQ(Hint.Virtual->VtableByteOffset, 600U);
  ASSERT_TRUE(Hint.Signature.ReturnType);
  EXPECT_EQ(Hint.Signature.ReturnType->Kind, NdTypeKind::Int);
  EXPECT_EQ(Hint.Signature.ReturnType->Size, 1U);
  EXPECT_TRUE(isSwiftVirtualSourceCallHint(F.Image, Hint));

  F.Image.ObjCMethods[0].TypeEncoding = "d16@0:8";
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
}

TEST(SwiftVirtualCalls, ExactMaskedIsaDoubleGetterBindsSwiftContext) {
  Fixture F(Fixture::GetterKind::Double);
  const auto Hints = buildSwiftVirtualCallHints(F.Image, F.Function);
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Hint = Hints.at(Fixture::CallSite);
  ASSERT_TRUE(Hint.Virtual);
  EXPECT_EQ(Hint.Virtual->VtableByteOffset, 648U);
  ASSERT_TRUE(Hint.Signature.ReturnType);
  EXPECT_EQ(Hint.Signature.ReturnType->Kind, NdTypeKind::Float);
  EXPECT_EQ(Hint.Signature.ReturnType->Size, 8U);
  EXPECT_TRUE(isSwiftVirtualSourceCallHint(F.Image, Hint));

  F.Image.Symbols[0].Name += "Other";
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
}

TEST(SwiftVirtualCalls, ExactMaskedIsaSettersBindValueAndSwiftContext) {
  const std::pair<Fixture::GetterKind, uint32_t> Cases[] = {
      {Fixture::GetterKind::CGFloat, 632},
      {Fixture::GetterKind::Double, 656},
      {Fixture::GetterKind::Bool, 608},
  };
  for (const auto &[Kind, Slot] : Cases) {
    Fixture F(Kind, true);
    const auto Hints = buildSwiftVirtualCallHints(F.Image, F.Function);
    ASSERT_EQ(Hints.size(), 1U);
    const auto &Hint = Hints.at(Fixture::CallSite);
    ASSERT_TRUE(Hint.Virtual);
    EXPECT_EQ(Hint.Virtual->VtableByteOffset, Slot);
    ASSERT_TRUE(Hint.Signature.ReturnType);
    EXPECT_EQ(Hint.Signature.ReturnType->Kind, NdTypeKind::Void);
    ASSERT_EQ(Hint.Signature.Parameters.size(), 2U);
    EXPECT_EQ(Hint.Signature.Parameters[0].Type->Kind,
              Kind == Fixture::GetterKind::Bool ? NdTypeKind::Int
                                                : NdTypeKind::Float);
    EXPECT_EQ(Hint.Signature.Parameters[1].TheRole,
              SourceParameterTypeHint::Role::SwiftContext);
    EXPECT_TRUE(isSwiftVirtualSourceCallHint(F.Image, Hint));
  }

  Fixture F(Fixture::GetterKind::Bool, true);
  F.Image.Segments[0].Data[Fixture::CallSite - 0x1000] = 0xa0;
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
}

TEST(SwiftVirtualCalls, ExactVoidMethodWindowRejectsOrdinaryArguments) {
  Fixture F;
  auto &Method = F.Image.ObjCMethods[0];
  Method.Selector = "stop";
  Method.TypeEncoding = "v16@0:8";
  SourceFunctionTypeHint Signature;
  Signature.ReturnType = NdType::makeVoid();
  Signature.Parameters = {{"self", NdType::makePtr(NdType::makeVoid())},
                          {"_cmd", NdType::makePtr(NdType::makeVoid())}};
  std::string Diagnostic;
  ASSERT_TRUE(assignDarwinObjCSourceABI(Signature, Arch::AArch64, Diagnostic))
      << Diagnostic;
  Method.TypeHint = Signature;
  F.Image.Symbols[0].Name = "_$s6Lottie23CompatibleAnimationViewC4stopyyFTo";
  F.Image.Imports.push_back({"/usr/lib/libobjc.A.dylib", "_objc_retain", 0, 0});
  F.Image.ImportStubIndices[0x1300] = 0;
  auto &Ops = F.Function.Blocks[0].Ops;
  for (auto &Op : Ops)
    if (Op.Opcode == NdOp::INT_ADD && Op.Addr == 0x1138)
      Op.Inputs[1] = NdVar::scalar(280, 8);
  LowOp Saved;
  Saved.Addr = 0x1144;
  Saved.Opcode = NdOp::COPY;
  Saved.Output = NdVar::reg(a64reg::X19, 8);
  Saved.addInput(NdVar::reg(a64reg::X0, 8));
  LowOp Link;
  Link.Addr = Fixture::CallSite;
  Link.Opcode = NdOp::COPY;
  Link.Output = NdVar::reg(a64reg::X30, 8);
  Link.addInput(NdVar::scalar(Fixture::CallSite + 4, 8));
  Ops.insert(Ops.end() - 1, Saved);
  Ops.insert(Ops.end() - 1, Link);

  const auto Hints = buildSwiftVirtualCallHints(F.Image, F.Function);
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Hint = Hints.at(Fixture::CallSite);
  ASSERT_TRUE(Hint.Virtual);
  EXPECT_EQ(Hint.Virtual->VtableByteOffset, 280U);
  EXPECT_EQ(Hint.Signature.ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(Hint.Signature.Parameters.size(), 1U);
  EXPECT_EQ(Hint.Signature.Parameters[0].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_TRUE(isSwiftVirtualSourceCallHint(F.Image, Hint));

  LowOp ExtraArgument;
  ExtraArgument.Addr = 0x1148;
  ExtraArgument.Opcode = NdOp::COPY;
  ExtraArgument.Output = NdVar::reg(a64reg::X0, 8);
  ExtraArgument.addInput(NdVar::scalar(0, 8));
  ExtraArgument.Inputs[0].Provenance = ConstantAddressProvenance::Unknown;
  Ops.insert(Ops.end() - 2, ExtraArgument);
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());

  LowOp SecondZero = ExtraArgument;
  SecondZero.Addr = 0x114c;
  SecondZero.Output = NdVar::reg(a64reg::X1, 8);
  Ops.insert(Ops.end() - 2, SecondZero);
  const auto NilArgumentHints = buildSwiftVirtualCallHints(F.Image, F.Function);
  ASSERT_EQ(NilArgumentHints.size(), 1U);
  const auto &NilHint = NilArgumentHints.at(Fixture::CallSite);
  ASSERT_TRUE(NilHint.Virtual);
  EXPECT_EQ(NilHint.Virtual->ZeroArgumentWords, 2U);
  ASSERT_EQ(NilHint.Signature.Parameters.size(), 3U);
  EXPECT_EQ(NilHint.Signature.Parameters[0].Type->Kind, NdTypeKind::Int);
  EXPECT_EQ(NilHint.Signature.Parameters[1].Type->Kind, NdTypeKind::Int);
  EXPECT_EQ(NilHint.Signature.Parameters[2].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_TRUE(isSwiftVirtualSourceCallHint(F.Image, NilHint));

  Ops[Ops.size() - 3].Inputs[0] = NdVar::scalar(1, 8);
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
}

TEST(SwiftVirtualCalls, RetainedSelfVoidMethodUsesSwiftContext) {
  Fixture F;
  auto &Method = F.Image.ObjCMethods[0];
  Method.ClassName = "_TtC6Lottie17AnimationViewBase";
  Method.Selector = "layoutSubviews";
  Method.TypeEncoding = "v16@0:8";
  Method.TypeHint->ReturnType = NdType::makeVoid();
  F.Image.Symbols[0].Name =
      "_$s6Lottie17AnimationViewBaseC14layoutSubviewsyyFTo";
  addVoidVirtualMetadata(F);
  F.Image.Segments[0].Data[Fixture::CallSite - 0x1000 + 0] = 0x00;
  F.Image.Segments[0].Data[Fixture::CallSite - 0x1000 + 1] = 0x01;
  F.Image.Segments[0].Data[Fixture::CallSite - 0x1000 - 4 + 0] = 0xf4;
  F.Image.Segments[0].Data[Fixture::CallSite - 0x1000 - 4 + 1] = 0x03;
  F.Image.Segments[0].Data[Fixture::CallSite - 0x1000 - 4 + 2] = 0x13;
  F.Image.Segments[0].Data[Fixture::CallSite - 0x1000 - 4 + 3] = 0xaa;
  F.Image.Imports.push_back({"/usr/lib/libobjc.A.dylib", "_objc_retain", 0, 0});
  F.Image.ImportStubIndices[0x1300] = 0;

  auto &Ops = F.Function.Blocks[0].Ops;
  Ops.clear();
  const auto X0 = NdVar::reg(a64reg::X0, 8);
  const auto X8 = NdVar::reg(a64reg::X8, 8);
  const auto X9 = NdVar::reg(a64reg::X9, 8);
  const auto X19 = NdVar::reg(a64reg::X19, 8);
  const auto X20 = NdVar::reg(a64reg::X20, 8);
  auto Add = [&](va_t Address, NdOp Code, NdVar Output,
                 std::initializer_list<NdVar> Inputs) {
    LowOp Op;
    Op.Addr = Address;
    Op.Opcode = Code;
    Op.Output = Output;
    for (const auto &Input : Inputs)
      Op.addInput(Input);
    Ops.push_back(Op);
  };
  Add(0x1110, NdOp::COPY, X19, {X0});
  Add(0x1114, NdOp::COPY, X0, {X19});
  Add(0x1118, NdOp::CALL, X0, {NdVar::codeAddress(0x1300, 8)});
  Add(0x111c, NdOp::COPY, X19, {X0});
  Add(0x1120, NdOp::LOAD, X8, {X19});
  Add(0x1124, NdOp::COPY, X9, {NdVar::dataAddress(Fixture::MaskSlot, 8)});
  Add(0x1128, NdOp::LOAD, X9, {X9});
  Add(0x112c, NdOp::LOAD, X9, {X9});
  Add(0x1130, NdOp::INT_AND, X8, {X8, X9});
  Add(0x1138, NdOp::INT_ADD, NdVar::tmp(TmpBase, 8),
      {X8, NdVar::scalar(96, 8)});
  Add(0x1138, NdOp::LOAD, X8, {NdVar::tmp(TmpBase, 8)});
  Add(0x1168, NdOp::COPY, X20, {X19});
  Add(Fixture::CallSite, NdOp::COPY, NdVar::reg(a64reg::X30, 8),
      {NdVar::scalar(Fixture::CallSite + 4, 8)});
  Add(Fixture::CallSite, NdOp::INDIR_CALL, X0, {X8});

  const auto Hints = buildSwiftVirtualCallHints(F.Image, F.Function);
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Hint = Hints.at(Fixture::CallSite);
  ASSERT_TRUE(Hint.Virtual);
  EXPECT_TRUE(Hint.Virtual->DirectSelf);
  EXPECT_EQ(Hint.Virtual->VtableByteOffset, 96U);
  EXPECT_EQ(Hint.Signature.ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(Hint.Signature.Parameters.size(), 1U);
  EXPECT_EQ(Hint.Signature.Parameters[0].TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  EXPECT_TRUE(isSwiftVirtualSourceCallHint(F.Image, Hint));

  const auto OriginalImage = F.Image;
  for (unsigned Mutation = 0; Mutation < 28; ++Mutation) {
    SCOPED_TRACE(Mutation);
    const auto Put32 = [&](unsigned Offset, uint32_t Value) {
      llvm::support::endian::write32le(F.Image.Segments[2].Data.data() + Offset,
                                       Value);
    };
    switch (Mutation) {
    case 0:
      Put32(0, 0x800000d0);
      break; // Generic context.
    case 1:
      Put32(0, 0xa0000050);
      break; // Resilient superclass.
    case 2:
      Put32(0, 0x80010050);
      break; // Extra initialization prefix.
    case 3:
      Put32(44, 13);
      break; // Slot is outside this vtable.
    case 4:
      Put32(48, 0);
      break;
    case 5:
      Put32(52, 0x11);
      break; // Constructor, not an ordinary method.
    case 6:
      Put32(56, uint32_t(0x1404 - (0x4000 + 56)));
      break;
    case 7:
      F.Image.Symbols.back().Name = "_$s6Lottie5OtherC6layoutyyF";
      break;
    case 8:
      // The enclosing ObjC method remains void; its callee takes a Bool.
      F.Image.Symbols.back().Name =
          "_$s6Lottie17AnimationViewBaseC6layoutyySbF";
      break;
    case 9:
      F.Image.Symbols.push_back(F.Image.Symbols.back());
      break;
    case 10:
      F.Image.DataPtrRelocSlots.erase(0x2440);
      break;
    case 11:
      F.Image.ImportPtrSlots[0x2460] = "_external";
      break;
    case 12:
      F.Image.DataPtrRelocSlots.insert(0x2460);
      break;
    case 13:
      F.Image.MachOHasChainedFixups = true;
      break;
    case 14:
      F.Image.ConflictingImportStorageSlots.insert(0x2440);
      break;
    case 15:
      F.Image.Segments[2].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
      F.Image.Sections[1].Flags = F.Image.Segments[2].Flags;
      break;
    case 16:
      Put32(48, 512);
      break;
    case 17:
      F.Image.Segments[2].Data[0x80] = 'X';
      break;
    case 18:
      Put32(4, 0xbd);
      break; // Indirect parent context.
    case 19:
      Put32(28, UINT32_MAX);
      break;
    case 20:
      F.Image.ObjCMethods[0].ClassAddress = 0;
      break;
    case 21:
      F.Image.Segments[1].Data[0x580] = 'X';
      break;
    case 22:
      F.Image.Symbols.back().Name = "_$s6Lottie17AnimationViewBaseC6layoutSiyF";
      break;
    case 23:
      F.Image.Symbols.back().Name =
          "_$s6Lottie17AnimationViewBaseC6layoutyyYaF";
      break;
    case 24:
      Put32(52, 0);
      break; // Static method has no swiftself.
    case 25:
      // Same carrier shape as WMF's sizeThatFits(_:apply:) call inside
      // layoutSubviews: two floating arguments, a Bool and a CGSize result.
      F.Image.Symbols.back().Name =
          "_$s6Lottie17AnimationViewBaseC6layout_5applySo6CGSizeVAG_SbtF";
      break;
    case 26:
      Put32(0xc4, 4); // A module context cannot have a parent.
      break;
    case 27:
      Put32(48, 2);
      F.Image.Sections[1].Size = F.Image.Sections[1].FileSz = 60;
      break; // The selected record exists but the declared vtable is truncated.
    }
    EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
    EXPECT_FALSE(isSwiftVirtualSourceCallHint(F.Image, Hint));
    F.Image = OriginalImage;
  }

  F.Image.Segments[0].Data[Fixture::CallSite - 0x1000 - 4] = 0x00;
  EXPECT_FALSE(isSwiftVirtualSourceCallHint(F.Image, Hint));
  F.Image.Segments[0].Data[Fixture::CallSite - 0x1000 - 4] = 0xf4;

  F.Image.DyldBindSlots[Fixture::MaskSlot].Module =
      "/usr/lib/libSystem.B.dylib";
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
  F.Image.DyldBindSlots[Fixture::MaskSlot].Module =
      "/usr/lib/swift/libswiftCore.dylib";
  F.Image.Imports[0].Name = "_objc_release";
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
  F.Image.Imports[0].Name = "_objc_retain";
  Ops[Ops.size() - 3].Inputs[0] = X0;
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
  Ops[Ops.size() - 3].Inputs[0] = X19;
  Ops[0].Inputs[0] = NdVar::reg(a64reg::X1, 8);
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
  Ops[0].Inputs[0] = X0;
  F.Image.Imports[0].Name = "_objc_retain_x19";
  Ops[1].Inputs[0] = NdVar::reg(a64reg::X1, 8);
  EXPECT_EQ(buildSwiftVirtualCallHints(F.Image, F.Function).size(), 1U);
  F.Image.Imports[0].Name = "_objc_retain";
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
}

TEST(SwiftVirtualCalls, RejectsAlteredCodeImportAndReceiverEvidence) {
  Fixture F;
  F.Image.Segments[0].Data[Fixture::CallSite - 0x1000] = 0x00;
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
  F = Fixture();
  F.Image.DyldBindSlots[Fixture::MaskSlot].Module =
      "/usr/lib/libSystem.B.dylib";
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
  F = Fixture();
  F.Image.ObjCSourceReferences[Fixture::IvarSlot].ClassName = "Other";
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
  F = Fixture();
  F.Image.ObjCMethods[0].TypeEncoding = "q16@0:8";
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
}
