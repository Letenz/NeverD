#include "../../lib/loader/Swift/SwiftBooleanSourceBinding.h"
#include "../lift/core/ImmutableNativeCallFixture.h"
#include "gtest/gtest.h"

#include "neverd/ir/low/SourceFrameAnalysis.h"
#include "neverd/loader/Swift/SwiftOpaqueValueEffects.h"

#include <functional>

using namespace neverd;
namespace {
constexpr va_t Entry = 0x1000, Copy = 0x1200, Destroy = 0x1240;
constexpr va_t Metadata = 0x2800, Equality = 0x2808;
constexpr const char *Provider = "/usr/lib/swift/libswiftCore.dylib";
struct Fixture : immutable_native_call_test::Fixture {
  SourceCallTypeHint CopyBinding, DestroyBinding, EqualBinding;
  const SourceCallOccurrenceKey CopySite{0x1018, 1, NdOp::CALL, Copy};
  const SourceCallOccurrenceKey EqualSite{0x1024, 1, NdOp::CALL, 0x1100};
  const SourceCallOccurrenceKey DestroySite{0x1030, 1, NdOp::CALL, Destroy};
  Fixture() {
    Image.DynInfo.NeededLibs = {Provider};
    Image.ImportPtrSlots.clear();
    Image.DyldBindSlots.clear();
    Image.Segments[1].VA = Metadata;
    Image.Sections[0].VA = Metadata;
    Image.ImportPtrSlots[Metadata] = "_$ss11AnyHashableVN";
    Image.DyldBindSlots[Metadata] = {Image.ImportPtrSlots[Metadata], 0,
                                     Provider, false};
    Image.ImportPtrSlots[Equality] =
        SwiftBooleanAnyHashableEqualityImport.str();
    Image.DyldBindSlots[Equality] = {Image.ImportPtrSlots[Equality], 0,
                                     Provider, false};
    Image.Segments[1].Size = Image.Segments[1].FileSz = 16;
    Image.Segments[1].Data.resize(16);
    Image.Sections[0].Size = Image.Sections[0].FileSz = 16;
    Image.Symbols = {{"_caller", Entry, 72, true},
                     {"_equal_stub", 0x1100, 12, true},
                     {"_copy", Copy, 60, true},
                     {"_destroy", Destroy, 52, true}};
    const uint32_t Caller[] = {
        0xd10143ff, 0xa9034ff4, 0xa9047bfd, 0x910103fd, 0xaa0003f3, 0x910023e1,
        0x9400007a, 0x910023e0, 0xaa1303e1, 0x94000037, 0xaa0003f4, 0x910023e0,
        0x94000084, 0xaa1403e0, 0xa9447bfd, 0xa9434ff4, 0x910143ff, 0xd65f03c0};
    for (size_t I = 0; I < std::size(Caller); ++I)
      word(I, Caller[I]);
    word(0x100 / 4 + 1, 0xf9440610);
    const uint32_t CopyWords[] = {
        0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0xaa0103f3, 0xaa0003e1,
        0xb0000002, 0xf9440042, 0xf85f8048, 0xf9400908, 0xaa1303e0,
        0xd63f0100, 0xaa1303e0, 0xa9417bfd, 0xa8c24ff4, 0xd65f03c0};
    const uint32_t DestroyWords[] = {
        0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0xaa0003f3, 0xb0000001,
        0xf9440021, 0xf85f8028, 0xf9400508, 0xd63f0100, 0xaa1303e0,
        0xa9417bfd, 0xa8c24ff4, 0xd65f03c0};
    for (size_t I = 0; I < std::size(CopyWords); ++I)
      word((Copy - Entry) / 4 + I, CopyWords[I]);
    for (size_t I = 0; I < std::size(DestroyWords); ++I)
      word((Destroy - Entry) / 4 + I, DestroyWords[I]);
    CopyBinding.TargetAddress = Copy;
    CopyBinding.Signature.Origin =
        SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    CopyBinding.Signature.ReturnType = NdType::makeInt(8);
    CopyBinding.Signature.Parameters = {
        {"source", NdType::makePtr(NdType::makeVoid())},
        {"destination", NdType::makePtr(NdType::makeVoid())}};
    DestroyBinding = CopyBinding;
    DestroyBinding.TargetAddress = Destroy;
    DestroyBinding.Signature.Parameters.resize(1);
    std::string Error;
    EXPECT_TRUE(assignDarwinScalarSourceABI(CopyBinding.Signature,
                                            Arch::AArch64, Error));
    EXPECT_TRUE(assignDarwinScalarSourceABI(DestroyBinding.Signature,
                                            Arch::AArch64, Error));
    EqualBinding.CallKind = SourceCallTypeHint::Kind::SwiftBooleanProjection;
    EqualBinding.TargetName =
        SwiftBooleanAnyHashableEqualityImport.drop_front().str();
    EqualBinding.TargetAddress = Equality;
    EqualBinding.Signature =
        *swiftBooleanNormalizedSignature(SwiftBooleanAnyHashableEqualityImport);
    EqualBinding.BooleanResult =
        SourceCallTypeHint::BooleanResultProjection{Entry, EqualSite};
    rerun();
  }
  void rerun() {
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {Entry, Copy, Destroy, 0x1100};
    Options.SourceTypeHints.emplace(Copy, CopyBinding.Signature);
    Options.SourceTypeHints.emplace(Destroy, DestroyBinding.Signature);
    Result = Pipeline().run(Image, Context, Options);
  }
  const LowFunc *function(va_t Address) const {
    for (const auto &F : Result.LowFuncs)
      if (F.Entry == Address)
        return &F;
    return nullptr;
  }
  std::optional<SourceFrameEffects> effects(bool Copying) {
    return swiftOpaqueValueCallEffects(Image, *function(Entry),
                                       Copying ? CopySite : DestroySite,
                                       Copying ? CopyBinding : DestroyBinding,
                                       function(Copying ? Copy : Destroy));
  }
  std::optional<SourceFrameEffects> equality() {
    return swiftOpaqueValueCallEffects(Image, *function(Entry), EqualSite,
                                       EqualBinding);
  }
};
} // namespace

TEST(SwiftOpaqueValueEffects,
     AuthenticatedTypedLifetimeDoesNotInitializePadding) {
  Fixture F;
  ASSERT_TRUE(F.Result.Success) << F.Result.Error;
  auto C = F.effects(true), D = F.effects(false), E = F.equality();
  ASSERT_TRUE(C);
  ASSERT_TRUE(D);
  ASSERT_TRUE(E);
  using Action = SourceFrameValueEffect::Action;
  EXPECT_EQ(C->OpaqueValueParameters.at(0).TheAction, Action::Read);
  EXPECT_EQ(C->OpaqueValueParameters.at(1).TheAction, Action::Initialize);
  EXPECT_EQ(D->OpaqueValueParameters.at(0).TheAction, Action::Destroy);
  EXPECT_EQ(E->OpaqueValueParameters.at(0).TheAction, Action::Read);
  EXPECT_EQ(E->OpaqueValueParameters.at(1), C->OpaqueValueParameters.at(0));
  EXPECT_EQ(C->OpaqueValueParameters.at(1).Bytes, 40U);
  EXPECT_TRUE(C->InitializedFrameParameters.empty());
  EXPECT_FALSE(C->InitializesIndirectResult);
  EXPECT_EQ(C->ReturnFrameOrExternal, (SourceFrameReturnAlias{1, 40}));
  EXPECT_EQ(D->ReturnFrameOrExternal, (SourceFrameReturnAlias{0, 40}));
  NativeSourceCalls Calls;
  for (const auto &[Site, Binding, Effects] :
       {std::tuple{F.CopySite, &F.CopyBinding, *C},
        std::tuple{F.EqualSite, &F.EqualBinding, *E},
        std::tuple{F.DestroySite, &F.DestroyBinding, *D}}) {
    auto &Contract = Calls[Site];
    Contract.Signature = &Binding->Signature;
    static_cast<SourceFrameEffects &>(Contract) = Effects;
  }
  std::set<uint64_t> Observed;
  EXPECT_TRUE(restoresNativeSourceState(*F.function(Entry), Arch::AArch64,
                                        Calls, &Observed));
  EXPECT_TRUE(Observed.count(0));
  // The shared IR owner still owns all lifetime/path/padding obligations.
  for (const auto &Site : {F.CopySite, F.EqualSite, F.DestroySite}) {
    auto Bad = Calls;
    Bad[Site].OpaqueValueParameters.clear();
    EXPECT_FALSE(
        restoresNativeSourceState(*F.function(Entry), Arch::AArch64, Bad));
  }
}

TEST(SwiftOpaqueValueEffects,
     EveryMachineWordAndCurrentLowDefinitionMustMatch) {
  Fixture F;
  for (const auto &[Target, Count] :
       {std::pair{Copy, 15U}, std::pair{Destroy, 13U}}) {
    ASSERT_TRUE(F.effects(Target == Copy));
    for (unsigned I = 0; I < Count; ++I) {
      auto &Byte = F.Image.Segments[0].Data[Target - Entry + I * 4];
      Byte ^= 1;
      EXPECT_FALSE(F.effects(Target == Copy))
          << std::hex << Target << " word " << I;
      Byte ^= 1;
    }
    const auto *Good = F.function(Target);
    auto Bad = *Good;
    Bad.Blocks.front().Ops.front().Inputs[0] = NdVar::cst(0, 8);
    EXPECT_FALSE(swiftOpaqueValueCallEffects(
        F.Image, *F.function(Entry),
        Target == Copy ? F.CopySite : F.DestroySite,
        Target == Copy ? F.CopyBinding : F.DestroyBinding, &Bad));
    Bad = *Good;
    Bad.Blocks.front().Succs.push_back(Bad.Blocks.front().Id);
    EXPECT_FALSE(swiftOpaqueValueCallEffects(
        F.Image, *F.function(Entry),
        Target == Copy ? F.CopySite : F.DestroySite,
        Target == Copy ? F.CopyBinding : F.DestroyBinding, &Bad));
    Bad = *Good;
    --Bad.DecodedInstructionCount;
    EXPECT_FALSE(swiftOpaqueValueCallEffects(
        F.Image, *F.function(Entry),
        Target == Copy ? F.CopySite : F.DestroySite,
        Target == Copy ? F.CopyBinding : F.DestroyBinding, &Bad));
  }
  auto Caller = *F.function(Entry);
  Caller.Blocks.front().Ops.front().Inputs[0] = NdVar::cst(0, 8);
  EXPECT_FALSE(swiftOpaqueValueCallEffects(F.Image, Caller, F.CopySite,
                                           F.CopyBinding, F.function(Copy)));
  EXPECT_FALSE(swiftOpaqueValueCallEffects(F.Image, Caller, F.EqualSite,
                                           F.EqualBinding));
  auto Site = F.CopySite;
  ++Site.Sequence;
  EXPECT_FALSE(swiftOpaqueValueCallEffects(F.Image, *F.function(Entry), Site,
                                           F.CopyBinding, F.function(Copy)));
  EXPECT_FALSE(swiftOpaqueValueCallEffects(F.Image, *F.function(Entry),
                                           F.CopySite, F.CopyBinding));
  EXPECT_FALSE(swiftOpaqueValueCallEffects(F.Image, *F.function(Entry),
                                           F.CopySite, F.CopyBinding,
                                           F.function(Destroy)));
}

TEST(SwiftOpaqueValueEffects,
     StrongIdentityStorageAndCompleteABICannotBeSubstituted) {
  Fixture F;
  const std::vector<std::function<void(BinaryImage &, va_t)>> Mutations = {
      [](auto &I, auto S) { I.DyldBindSlots[S].WeakImport = true; },
      [](auto &I, auto S) { I.DyldBindSlots[S].Addend = 8; },
      [](auto &I, auto S) { I.DyldBindSlots[S].Module = "other"; },
      [](auto &I, auto S) { I.DyldBindSlots[S].Name = "_$sOtherN"; },
      [](auto &I, auto S) { I.ImportPtrSlots[S] = "_$sOtherN"; },
      [](auto &I, auto S) { I.DyldBindSlots.erase(S); },
      [](auto &I, auto S) { I.ConflictingImportStorageSlots.insert(S); },
      [](auto &I, auto) { I.DynInfo.NeededLibs.clear(); },
      [](auto &I, auto) { I.DynInfo.NeededLibs.push_back(Provider); },
      [](auto &I, auto) {
        I.Segments[1].Flags = I.Segments[1].Flags | SegmentFlags::Writable;
      },
      [](auto &I, auto) {
        I.Sections[0].Type = llvm::MachO::S_THREAD_LOCAL_VARIABLES;
      },
      [](auto &I, auto) { I.IsRelocatable = true; },
      [](auto &I, auto) { I.Arch = Arch::X64; },
      [](auto &I, auto) { I.Bits = Bitness::Bits32; },
      [](auto &I, auto) { I.Format = BinaryFormat::ELF; },
      [](auto &I, auto) { I.MachOChainedFixupsAmbiguous = true; },
  };
  for (va_t Slot : {Metadata, Equality})
    for (size_t Index = 0; Index < Mutations.size(); ++Index) {
      auto Image = F.Image;
      Mutations[Index](Image, Slot);
      EXPECT_FALSE(swiftOpaqueValueCallEffects(
          Image, *F.function(Entry),
          Slot == Metadata ? F.CopySite : F.EqualSite,
          Slot == Metadata ? F.CopyBinding : F.EqualBinding,
          Slot == Metadata ? F.function(Copy) : nullptr))
          << Index << ":" << Slot;
    }
  const std::vector<std::function<void(SourceCallTypeHint &)>> ABIs = {
      [](auto &B) { B.WeakImport = true; },
      [](auto &B) { B.DoesNotReturn = true; },
      [](auto &B) { B.Signature.HasExplicitABI = false; },
      [](auto &B) { B.Signature.Architecture = Arch::X64; },
      [](auto &B) { B.Signature.Parameters[0].Location.ValueBytes = 4; },
      [](auto &B) { B.Signature.Parameters[0].Type = NdType::makeInt(4); },
      [](auto &B) { B.Signature.Parameters[0].Location.RegisterOffset = 16; },
      [](auto &B) {
        B.Signature.Parameters[0].TheRole =
            SourceParameterTypeHint::Role::SwiftContext;
      },
      [](auto &B) {
        B.Signature.Parameters.push_back(B.Signature.Parameters[0]);
      },
      [](auto &B) { B.Signature.ReturnType = NdType::makeFloat(8); },
      [](auto &B) { B.Signature.ReturnLocation.ValueBytes = 4; },
      [](auto &B) { B.TargetAddress += 4; },
  };
  for (size_t Index = 0; Index < ABIs.size(); ++Index)
    for (bool Native : {true, false}) {
      auto Binding = Native ? F.CopyBinding : F.EqualBinding;
      ABIs[Index](Binding);
      EXPECT_FALSE(swiftOpaqueValueCallEffects(
          F.Image, *F.function(Entry), Native ? F.CopySite : F.EqualSite,
          Binding, Native ? F.function(Copy) : nullptr))
          << Index;
    }
  auto Binding = F.EqualBinding;
  Binding.BooleanResult.reset();
  EXPECT_FALSE(swiftOpaqueValueCallEffects(F.Image, *F.function(Entry),
                                           F.EqualSite, Binding));
  Binding = F.EqualBinding;
  Binding.BooleanResult->FunctionEntry += 4;
  EXPECT_FALSE(swiftOpaqueValueCallEffects(F.Image, *F.function(Entry),
                                           F.EqualSite, Binding));
}

TEST(SwiftOpaqueValueEffects,
     FreshReliftingAndNamesCannotReplaceBodyOrEntryProof) {
  Fixture F;
  ASSERT_TRUE(F.effects(true));
  ASSERT_TRUE(F.effects(false));
  // Neither a mangled helper name nor a particular parameter spelling supplies
  // its type, physical carrier, or value witness operation.
  for (auto &Symbol : F.Image.Symbols)
    Symbol.Name = "private_renamed_" + std::to_string(Symbol.Addr);
  F.CopyBinding.Signature.Parameters[0].Name = "renamed_input";
  F.CopyBinding.Signature.Parameters[1].Name = "renamed_destination";
  EXPECT_TRUE(F.effects(true));
  EXPECT_TRUE(F.effects(false));
  for (const auto &[Address, Word] :
       {std::pair{Copy + 44, 0xaa0103e0U}, std::pair{Copy + 32, 0xf9400d08U},
        std::pair{Destroy + 28, 0xf9400908U},
        std::pair{Destroy + 36, 0xaa0103e0U}}) {
    const auto Original = llvm::support::endian::read32le(
        F.Image.Segments[0].Data.data() + Address - Entry);
    F.word((Address - Entry) / 4, Word);
    F.rerun();
    EXPECT_FALSE(F.effects(Address < Destroy));
    F.word((Address - Entry) / 4, Original);
    F.rerun();
    ASSERT_TRUE(F.effects(Address < Destroy));
  }
  for (bool Interior : {false, true}) {
    auto Image = F.Image;
    if (Interior)
      Image.Symbols.push_back({"interior_entry", Copy + 4, 56, true});
    else
      std::erase_if(Image.Symbols,
                    [](const auto &S) { return S.Addr == Copy; });
    EXPECT_FALSE(swiftOpaqueValueCallEffects(Image, *F.function(Entry),
                                             F.CopySite, F.CopyBinding,
                                             F.function(Copy)));
  }
  // A decoder constant rewritten into a saved data-address annotation is not
  // enough for the strict machine replay, even if its numeric payload agrees.
  auto Callee = *F.function(Copy);
  for (auto &Block : Callee.Blocks)
    for (auto &Op : Block.Ops)
      if (Op.Addr == Copy + 20)
        Op.Inputs[0] = NdVar::dataAddress(Op.Inputs[0].Offset, 8, Metadata);
  EXPECT_FALSE(swiftOpaqueValueCallEffects(F.Image, *F.function(Entry),
                                           F.CopySite, F.CopyBinding, &Callee));
}
