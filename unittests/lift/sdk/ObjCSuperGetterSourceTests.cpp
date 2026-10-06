#include "../../../lib/sdk/capi/ObjCForwardedInitializerSources.h"
#include "../../../lib/sdk/capi/ObjCSuperGetterSources.h"
#include "../core/SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"

#include "llvm/BinaryFormat/MachO.h"

#include <cstring>

using namespace neverd;
using namespace neverd::sdk;

namespace {
struct SuperGetterFixture {
  static constexpr va_t Root = 0x1000;
  static constexpr va_t Getter = 0x1100;
  static constexpr va_t Metadata = 0x1200;
  static constexpr va_t SelectorSlot = 0x2100;
  BinaryImage Image;
  llvm::LLVMContext Context;
  PipelineResult Result;

  void word(va_t Address, uint32_t Value) {
    uint8_t Bytes[4];
    llvm::support::endian::write32le(Bytes, Value);
    ASSERT_TRUE(Image.writeVA(Address, Bytes, 4));
  }
  void pointer(va_t Address, uint64_t Value) {
    uint8_t Bytes[8];
    llvm::support::endian::write64le(Bytes, Value);
    ASSERT_TRUE(Image.writeVA(Address, Bytes, 8));
  }
  static uint32_t branch(va_t Site, va_t Target, bool Link = true) {
    return (Link ? 0x94000000u : 0x14000000u) |
           (uint32_t((int64_t(Target) - int64_t(Site)) / 4) & 0x03ffffff);
  }
  void exportTrie(const std::vector<uint8_t> &Trie) {
    auto &Bytes = Image.Segments.front().Data;
    auto *Header =
        reinterpret_cast<llvm::MachO::mach_header_64 *>(Bytes.data());
    auto *Command = reinterpret_cast<llvm::MachO::linkedit_data_command *>(
        Bytes.data() + sizeof(*Header) + Header->sizeofcmds);
    Command->cmd = llvm::MachO::LC_DYLD_EXPORTS_TRIE;
    Command->cmdsize = sizeof(*Command);
    Command->dataoff = 0x3200;
    Command->datasize = Trie.size();
    ++Header->ncmds;
    Header->sizeofcmds += sizeof(*Command);
    ASSERT_TRUE(Image.writeVA(0x3200, Trie.data(), Trie.size()));
  }
  SuperGetterFixture() {
    using namespace llvm::MachO;
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.Entry = Root;
    for (unsigned I = 0; I < 3; ++I) {
      Segment Segment;
      Segment.VA = Segment.FileOff = I ? 0x1000 + I * 0x1000 : 0;
      Segment.Size = Segment.FileSz = I ? 0x1000 : 0x2000;
      Segment.Flags = SegmentFlags::Readable |
                      (I ? SegmentFlags::Writable : SegmentFlags::Executable);
      Segment.Data.resize(Segment.Size);
      Image.Segments.push_back(std::move(Segment));
    }
    for (unsigned I = 0; I < 2; ++I) {
      Section Section;
      Section.VA = Section.FileOff = I ? 0x2000 : 0x1000;
      Section.Size = Section.FileSz = I ? 0x1000 : 0x280;
      Section.Flags = Image.Segments[I].Flags;
      Section.Type =
          I ? uint32_t(S_REGULAR) : uint32_t(S_ATTR_PURE_INSTRUCTIONS);
      Image.Sections.push_back(Section);
    }
    auto &Bytes = Image.Segments.front().Data;
    auto *Header = reinterpret_cast<mach_header_64 *>(Bytes.data());
    Header->magic = MH_MAGIC_64;
    Header->cputype = CPU_TYPE_ARM64;
    Header->filetype = MH_EXECUTE;
    Header->ncmds = 4;
    Header->sizeofcmds = 3 * sizeof(segment_command_64) +
                         2 * sizeof(section_64) + sizeof(symtab_command);
    size_t Offset = sizeof(mach_header_64);
    for (unsigned I = 0; I < 3; ++I) {
      auto *Command =
          reinterpret_cast<segment_command_64 *>(Bytes.data() + Offset);
      Command->cmd = LC_SEGMENT_64;
      Command->cmdsize = sizeof(*Command) + (I < 2 ? sizeof(section_64) : 0);
      const char *Name = I == 0 ? "__TEXT" : I == 1 ? "__DATA" : "__LINKEDIT";
      std::strcpy(Command->segname, Name);
      const auto &Segment = Image.Segments[I];
      Command->vmaddr = Segment.VA;
      Command->vmsize = Segment.Size;
      Command->fileoff = Segment.FileOff;
      Command->filesize = Segment.FileSz;
      Command->maxprot = Command->initprot =
          VM_PROT_READ | (I ? VM_PROT_WRITE : VM_PROT_EXECUTE);
      Command->nsects = I < 2;
      if (I < 2) {
        auto *Section = reinterpret_cast<section_64 *>(Command + 1);
        const auto &Source = Image.Sections[I];
        std::strcpy(Section->segname, Name);
        std::strcpy(Section->sectname, I ? "__data" : "__text");
        Section->addr = Source.VA;
        Section->size = Source.Size;
        Section->offset = Source.FileOff;
        Section->align = 2;
        Section->flags = Source.Type;
      }
      Offset += Command->cmdsize;
    }
    auto *Symbols = reinterpret_cast<symtab_command *>(Bytes.data() + Offset);
    Symbols->cmd = LC_SYMTAB;
    Symbols->cmdsize = sizeof(*Symbols);
    Symbols->symoff = 0x3000;
    Symbols->nsyms = 1;
    Symbols->stroff = 0x3100;
    Symbols->strsize = 32;
    auto *Symbol = reinterpret_cast<nlist_64 *>(Image.Segments[2].Data.data());
    Symbol->n_strx = 1;
    Symbol->n_type = N_SECT;
    Symbol->n_sect = 1;
    Symbol->n_value = Getter;
    std::strcpy(reinterpret_cast<char *>(Image.Segments[2].Data.data() + 0x101),
                "_shared_getter");
    for (const auto [Address, Name] : {std::pair{Root, "root"},
                                       {Getter, "shared"},
                                       {Metadata, "metadata"}}) {
      Image.Symbols.push_back({Name, Address, 0, true});
      Image.Exports.push_back({Name, 0, Address});
    }
    word(Root, 0xb0000002);
    word(Root + 4, 0x91040042);
    word(Root + 8, branch(Root + 8, Getter, false));
    const uint32_t Body[] = {0xd100c3ff,
                             0xa9014ff4,
                             0xa9027bfd,
                             0x910083fd,
                             0xaa0203f3,
                             0xaa0003f4,
                             branch(Getter + 24, Metadata),
                             0xa90003f4,
                             0xf9400261,
                             0x910003e0,
                             branch(Getter + 40, 0x1260),
                             0xa9427bfd,
                             0xa9414ff4,
                             0x9100c3ff,
                             0xd65f03c0};
    for (unsigned I = 0; I < std::size(Body); ++I)
      word(Getter + I * 4, Body[I]);
    const uint32_t Accessor[] = {0xa9bf7bfd,
                                 0x910003fd,
                                 0xb0000000,
                                 0x91008000,
                                 branch(Metadata + 16, 0x1240),
                                 0xd2800001,
                                 0xa8c17bfd,
                                 0xd65f03c0};
    for (unsigned I = 0; I < std::size(Accessor); ++I)
      word(Metadata + I * 4, Accessor[I]);
    for (const auto [Entry, Slot, Name] :
         {std::tuple{0x1240, 0x2f00, "_objc_opt_self"},
          std::tuple{0x1260, 0x2f08, "_objc_msgSendSuper2"}}) {
      word(Entry, 0xb0000010);
      word(Entry + 4, 0xf9400210 | (((Slot & 4095) / 8) << 10));
      word(Entry + 8, 0xd61f0200);
      Image.ImportPtrSlots[Slot] = Name;
      EXPECT_TRUE(Image.recordDyldBindSlot(Slot, Name, 0,
                                           "/usr/lib/libobjc.A.dylib", false));
    }
    pointer(0x2020, 0x2080);
    pointer(0x2040, 0x2200);
    pointer(0x20a0, 0x2280);
    pointer(0x2280, 1);
    pointer(0x2218, 0x2380);
    pointer(0x2298, 0x2380);
    const char Name[] = "Receiver";
    EXPECT_TRUE(Image.writeVA(0x2380, reinterpret_cast<const uint8_t *>(Name),
                              sizeof(Name)));
    ObjCClass Class;
    Class.Address = 0x2020;
    Class.Name = Name;
    Class.SuperclassName = "UIControl";
    Class.InheritanceStatus = "resolved";
    Image.ObjCClasses.push_back(Class);
    Image.ObjCSourceReferences[SelectorSlot] = {
        ObjCSourceReference::Kind::Selector,
        SelectorSlot,
        8,
        "isHighlighted",
        {}};
    ObjCMethod Method;
    Method.Status = "supported";
    Method.Implementation = Root;
    Method.ClassName = Class.Name;
    Method.ClassAddress = Class.Address;
    Method.Selector = "isHighlighted";
    Method.TypeEncoding = "B16@0:8";
    Method.TypeHint =
        parseObjCMethodEncoding(Method.Selector, Method.TypeEncoding);
    std::string Error;
    EXPECT_TRUE(assignDarwinObjCSourceABI(*Method.TypeHint, Image.Arch, Error));
    Image.ObjCMethods.push_back(Method);
    SourceFunctionTypeHint MetadataHint;
    MetadataHint.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    MetadataHint.ReturnType = NdType::makeInt(8);
    EXPECT_TRUE(assignDarwinScalarSourceABI(MetadataHint, Image.Arch, Error));
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {Root, Getter, Metadata};
    Options.SourceTypeHints.emplace(Metadata, MetadataHint);
    Result = Pipeline().run(Image, Context, Options);
    EXPECT_TRUE(Result.Success) << Result.Error;
  }
};

void makeCGRectGetter(SuperGetterFixture &F) {
  auto &Method = F.Image.ObjCMethods.front();
  Method.Selector = "frame";
  Method.TypeEncoding = "{CGRect={CGPoint=dd}{CGSize=dd}}16@0:8";
  Method.TypeHint =
      parseObjCMethodEncoding(Method.Selector, Method.TypeEncoding);
  std::string Error;
  ASSERT_TRUE(assignDarwinObjCSourceABI(*Method.TypeHint, F.Image.Arch, Error));
  F.Image.ObjCSourceReferences[F.SelectorSlot].Name = Method.Selector;
  const auto *Metadata =
      objc_super_getter_detail::uniqueEntry(F.Result.HighFuncs, F.Metadata);
  ASSERT_NE(Metadata, nullptr);
  ASSERT_TRUE(Metadata->SourceTypeHint);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  Options.OnlyFunctionEntries = {F.Root, F.Getter, F.Metadata};
  Options.SourceTypeHints.emplace(F.Metadata, *Metadata->SourceTypeHint);
  F.Result = Pipeline().run(F.Image, F.Context, Options);
  ASSERT_TRUE(F.Result.Success) << F.Result.Error;
}

void makeForwardedInitializer(SuperGetterFixture &F) {
  F.word(F.Root, 0x90000002);
  F.word(F.Root + 4, 0x91080042);
  const uint32_t Body[] = {0xd100c3ff,
                           0xa9014ff4,
                           0xa9027bfd,
                           0x910083fd,
                           0xaa0003f3,
                           0xd63f0040,
                           0xa90003f3,
                           0xb0000008,
                           0xf9408101,
                           0x910003e0,
                           F.branch(F.Getter + 40, 0x1260),
                           0xa9427bfd,
                           0xa9414ff4,
                           0x9100c3ff,
                           0xd65f03c0};
  for (unsigned I = 0; I < std::size(Body); ++I)
    F.word(F.Getter + I * 4, Body[I]);
  auto &Method = F.Image.ObjCMethods.front();
  Method.Selector = "init";
  Method.TypeEncoding = "@16@0:8";
  Method.TypeHint =
      parseObjCMethodEncoding(Method.Selector, Method.TypeEncoding);
  std::string Error;
  ASSERT_TRUE(assignDarwinObjCSourceABI(*Method.TypeHint, F.Image.Arch, Error));
  F.Image.ObjCSourceReferences[F.SelectorSlot].Name = "init";
  const auto *Metadata =
      objc_super_getter_detail::uniqueEntry(F.Result.HighFuncs, F.Metadata);
  ASSERT_NE(Metadata, nullptr);
  ASSERT_TRUE(Metadata->SourceTypeHint);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  Options.OnlyFunctionEntries = {F.Root, F.Getter, F.Metadata};
  Options.SourceTypeHints.emplace(F.Metadata, *Metadata->SourceTypeHint);
  F.Result = Pipeline().run(F.Image, F.Context, Options);
  ASSERT_TRUE(F.Result.Success) << F.Result.Error;
}

TEST(ObjCForwardedInitializerSources,
     KeepsCallerClassAccessorAndSelectorLoadInOneHelper) {
  SuperGetterFixture F;
  makeForwardedInitializer(F);
  PipelineOptions Seeded;
  EXPECT_EQ(seedObjCForwardedInitializerAccessorHints(F.Image, Seeded), 1U);
  EXPECT_TRUE(Seeded.SourceTypeHints.count(F.Metadata));
  const auto C =
      objc_forwarded_initializer_detail::prove(F.Image, F.Result, F.Root);
  ASSERT_TRUE(C);
  EXPECT_EQ(C->Accessor.Entry, F.Metadata);
  EXPECT_EQ(C->SelectorSlot, F.SelectorSlot);
  const auto Plan = discoverObjCForwardedInitializerSources(F.Image, F.Result);
  EXPECT_EQ(Plan.Callers, std::set<va_t>{F.Root});
  const auto *Root =
      objc_super_getter_detail::uniqueEntry(F.Result.HighFuncs, F.Root);
  ASSERT_NE(Root, nullptr);
  auto Projection = projectObjCForwardedInitializer(*Root, F.Image, Plan);
  ASSERT_TRUE(Projection.Projected);
  EXPECT_EQ(Projection.Dependencies, std::set<va_t>{F.Metadata});
  std::map<va_t, const HighFunc *> Functions;
  for (const auto &Function : F.Result.HighFuncs)
    Functions.emplace(Function.Entry, &Function);
  const auto Bound = [&](const HighExpr &Expression) {
    return objCForwardedInitializerSourceCallBound(
        Expression, F.Image, Plan, Projection.Function, Functions);
  };
  const auto &Call = *Projection.Function.Body.front().RetVal;
  EXPECT_TRUE(Bound(Call));
  EXPECT_TRUE(Bound(*Call.Operands[2]));
  EXPECT_TRUE(Bound(*Call.Operands[3]));
  const auto *Audit =
      objc_super_getter_detail::uniqueEntry(F.Result.FunctionAudits, F.Root);
  ASSERT_NE(Audit, nullptr);
  EXPECT_TRUE(sourceBodyLimitation(Projection.Function,
                                   *Projection.Function.SourceTypeHint, Audit,
                                   Bound)
                  .empty());
  std::set<std::string> Helpers;
  const auto Source =
      renderObjCForwardedInitializerHelpers(F.Image, Plan, {F.Root}, Helpers);
  EXPECT_NE(Source.find("sel_registerName(\"init\")"), std::string::npos);
  EXPECT_LT(Source.find("(*)(void))metadata)()"),
            Source.find("*(void **)selector_slot"));
  EXPECT_NE(Source.find("objc_msgSendSuper2)(&super, selector)"),
            std::string::npos);
  EXPECT_EQ(Helpers.size(), 2U);
}

TEST(ObjCForwardedInitializerSources, SharedBodyKeepsEachCallersClassAccessor) {
  SuperGetterFixture F;
  makeForwardedInitializer(F);
  constexpr va_t OtherRoot = 0x1020;
  constexpr va_t OtherAccessor = 0x1140;
  constexpr va_t OtherClass = 0x2400;
  F.word(OtherRoot, 0x90000002);
  F.word(OtherRoot + 4, 0x91050042);
  F.word(OtherRoot + 8, F.branch(OtherRoot + 8, F.Getter, false));
  const uint32_t Accessor[] = {0xa9bf7bfd,
                               0x910003fd,
                               0xb0000000,
                               0x91100000,
                               F.branch(OtherAccessor + 16, 0x1240),
                               0xd2800001,
                               0xa8c17bfd,
                               0xd65f03c0};
  for (unsigned I = 0; I < std::size(Accessor); ++I)
    F.word(OtherAccessor + I * 4, Accessor[I]);
  F.pointer(OtherClass, 0x2460);
  F.pointer(OtherClass + 0x20, 0x2600);
  F.pointer(0x2480, 0x2680);
  F.pointer(0x2680, 1);
  F.pointer(0x2618, 0x2780);
  F.pointer(0x2698, 0x2780);
  const char Name[] = "OtherReceiver";
  ASSERT_TRUE(F.Image.writeVA(0x2780, reinterpret_cast<const uint8_t *>(Name),
                              sizeof(Name)));
  ObjCClass Class;
  Class.Address = OtherClass;
  Class.Name = Name;
  Class.SuperclassName = "UIControl";
  Class.InheritanceStatus = "resolved";
  F.Image.ObjCClasses.push_back(Class);
  auto Method = F.Image.ObjCMethods.front();
  Method.Implementation = OtherRoot;
  Method.ClassAddress = OtherClass;
  Method.ClassName = Name;
  F.Image.ObjCMethods.push_back(Method);
  F.Image.Symbols.push_back({"other_root", OtherRoot, 0, true});
  F.Image.Symbols.push_back({"other_accessor", OtherAccessor, 0, true});
  F.Image.Exports.push_back({"other_root", 0, OtherRoot});
  F.Image.Exports.push_back({"other_accessor", 0, OtherAccessor});
  const auto *Metadata =
      objc_super_getter_detail::uniqueEntry(F.Result.HighFuncs, F.Metadata);
  ASSERT_NE(Metadata, nullptr);
  ASSERT_TRUE(Metadata->SourceTypeHint);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  Options.OnlyFunctionEntries = {F.Root, OtherRoot, F.Getter, F.Metadata,
                                 OtherAccessor};
  Options.SourceTypeHints.emplace(F.Metadata, *Metadata->SourceTypeHint);
  Options.SourceTypeHints.emplace(OtherAccessor, *Metadata->SourceTypeHint);
  F.Result = Pipeline().run(F.Image, F.Context, Options);
  ASSERT_TRUE(F.Result.Success) << F.Result.Error;
  EXPECT_TRUE(objc_super_getter_detail::completeLow(F.Result, OtherRoot, 3));
  EXPECT_TRUE(
      objc_super_getter_detail::completeLow(F.Result, OtherAccessor, 8));
  EXPECT_TRUE(objc_super_getter_detail::validatedClassAccessor(
      F.Image, F.Result, OtherAccessor));
  EXPECT_TRUE(
      objc_binding_detail::classObjectIdentities(F.Image).count(OtherClass));
  const auto Plan = discoverObjCForwardedInitializerSources(F.Image, F.Result);
  EXPECT_EQ(Plan.Callers, (std::set<va_t>{F.Root, OtherRoot}));
  const auto *First =
      objc_super_getter_detail::uniqueEntry(F.Result.HighFuncs, F.Root);
  const auto *Second =
      objc_super_getter_detail::uniqueEntry(F.Result.HighFuncs, OtherRoot);
  ASSERT_NE(First, nullptr);
  ASSERT_NE(Second, nullptr);
  auto A = projectObjCForwardedInitializer(*First, F.Image, Plan);
  auto B = projectObjCForwardedInitializer(*Second, F.Image, Plan);
  ASSERT_TRUE(A.Projected);
  ASSERT_TRUE(B.Projected);
  EXPECT_EQ(A.Dependencies, std::set<va_t>{F.Metadata});
  EXPECT_EQ(B.Dependencies, std::set<va_t>{OtherAccessor});
  EXPECT_EQ(A.Function.Body.front()
                .RetVal->Operands[3]
                ->SourceCallHint->TargetAddress,
            F.Metadata);
  EXPECT_EQ(B.Function.Body.front()
                .RetVal->Operands[3]
                ->SourceCallHint->TargetAddress,
            OtherAccessor);
  std::set<std::string> Helpers;
  const auto Source = renderObjCForwardedInitializerHelpers(
      F.Image, Plan, {F.Root, OtherRoot}, Helpers);
  EXPECT_EQ(Helpers.size(), 2U);
  EXPECT_EQ(Source.find("neverd_objc_forwarded_initializer_"),
            Source.rfind("neverd_objc_forwarded_initializer_"));
}

TEST(ObjCForwardedInitializerSources,
     DisconnectedAdjacentEntryCannotChangeTheMethodWrapper) {
  SuperGetterFixture F;
  makeForwardedInitializer(F);
  auto Low = std::find_if(F.Result.LowFuncs.begin(), F.Result.LowFuncs.end(),
                          [&](const auto &L) { return L.Entry == F.Root; });
  auto Audit = std::find_if(F.Result.FunctionAudits.begin(),
                            F.Result.FunctionAudits.end(),
                            [&](const auto &A) { return A.Entry == F.Root; });
  ASSERT_NE(Low, F.Result.LowFuncs.end());
  ASSERT_NE(Audit, F.Result.FunctionAudits.end());
  ASSERT_EQ(Low->Blocks.size(), 1U);
  auto Adjacent = Low->Blocks.front();
  Adjacent.Id = 1;
  Adjacent.StartAddr += 12;
  Adjacent.EndAddr += 12;
  for (auto &Op : Adjacent.Ops)
    Op.Addr += 12;
  for (auto &Boundary : Adjacent.InstructionBoundaries)
    Boundary.Address += 12;
  Low->Blocks.push_back(Adjacent);
  Audit->DecodedInstructions += 3;
  Audit->LiftedInstructions += 3;
  ASSERT_TRUE(
      objc_forwarded_initializer_detail::completeCallerLow(F.Result, F.Root));
  EXPECT_TRUE(
      objc_forwarded_initializer_detail::prove(F.Image, F.Result, F.Root));
  Low->Blocks.front().Succs.push_back(1);
  EXPECT_FALSE(
      objc_forwarded_initializer_detail::completeCallerLow(F.Result, F.Root));
  Low->Blocks.front().Succs.clear();
  Low->Blocks[1].Ops.front().Addr = F.Root + 4;
  EXPECT_FALSE(
      objc_forwarded_initializer_detail::completeCallerLow(F.Result, F.Root));
}

TEST(ObjCForwardedInitializerSources, RejectsChangedMachineAndClassEvidence) {
  for (unsigned Mutation = 0; Mutation < 11; ++Mutation) {
    SCOPED_TRACE(Mutation);
    SuperGetterFixture F;
    makeForwardedInitializer(F);
    ASSERT_TRUE(
        objc_forwarded_initializer_detail::prove(F.Image, F.Result, F.Root));
    if (Mutation == 0)
      F.word(F.Getter + 20, 0xd63f0060); // x3 rather than x2
    if (Mutation == 1)
      F.word(F.Getter + 32, 0xf9408501); // shifted selector load
    if (Mutation == 2)
      F.word(F.Getter + 48, 0xd503201f); // missing restore
    if (Mutation == 3)
      F.word(F.Root + 4, 0x91084042); // different accessor
    if (Mutation == 4)
      F.word(F.Root + 8, F.branch(F.Root + 8, F.Getter + 4, false));
    if (Mutation == 5)
      F.Image.ObjCSourceReferences[F.SelectorSlot].Name = "other";
    if (Mutation == 6)
      F.Image.ObjCMethods[0].ClassName = "Other";
    if (Mutation == 7)
      F.Image.ObjCMethods[0].Status = "ambiguous_dispatch";
    if (Mutation == 8)
      F.Image.DyldBindSlots[0x2f08].WeakImport = true;
    if (Mutation == 9)
      F.Result.FunctionAudits.clear();
    if (Mutation == 10)
      F.Image.ObjCMethods[0].ClassAddress += 8;
    EXPECT_FALSE(
        objc_forwarded_initializer_detail::prove(F.Image, F.Result, F.Root));
  }
}

TEST(ObjCSuperGetterSources, KeepsAccessorAndDynamicSelectorInOneHelper) {
  SuperGetterFixture F;
  ASSERT_TRUE(isMachOLocalFunctionRange(F.Image, F.Getter, 60));
  const auto Contract =
      objc_super_getter_detail::prove(F.Image, F.Result, F.Getter, F.Root);
  ASSERT_TRUE(Contract);
  const auto Plan = discoverObjCSuperGetterSources(F.Image, F.Result);
  ASSERT_EQ(Plan.Callers.size(), 1U);
  const auto *Root =
      objc_super_getter_detail::uniqueEntry(F.Result.HighFuncs, F.Root);
  ASSERT_NE(Root, nullptr);
  const auto Projection = projectObjCSuperGetter(*Root, F.Image, Plan);
  ASSERT_TRUE(Projection.Projected);
  EXPECT_EQ(Projection.Dependencies, std::set<va_t>{F.Metadata});
  std::set<std::string> Helpers;
  const auto Source =
      renderObjCSuperGetterHelpers(F.Image, Plan, {F.Root}, Helpers);
  EXPECT_NE(Source.find("(*)(void))metadata)()"), std::string::npos);
  EXPECT_LT(Source.find("(*)(void))metadata)()"),
            Source.find("*(void **)selector_slot"));
  EXPECT_NE(Source.find("objc_msgSendSuper2)(&super, selector)"),
            std::string::npos);
  EXPECT_EQ(Helpers.size(), 2U);
}

TEST(ObjCSuperGetterSources, RejectsChangedMachineAndCallerEvidence) {
  for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
    SCOPED_TRACE(Mutation);
    SuperGetterFixture F;
    ASSERT_TRUE(
        objc_super_getter_detail::prove(F.Image, F.Result, F.Getter, F.Root));
    if (Mutation == 0)
      F.word(F.Getter + 32, 0xf9400661); // shifted SEL load
    if (Mutation == 1)
      F.word(F.Getter + 48, 0xd503201f); // missing restore
    if (Mutation == 2)
      F.word(F.Metadata + 16, F.branch(F.Metadata + 16, 0x1260));
    if (Mutation == 3)
      F.word(F.Root + 8, F.branch(F.Root + 8, F.Getter + 4, false));
    if (Mutation == 4)
      F.word(F.Root + 8, F.branch(F.Root + 8, F.Getter));
    if (Mutation == 5)
      F.word(F.Root + 4, 0x91080042);
    if (Mutation == 6)
      F.Image.ObjCClasses[0].Name = "Other";
    if (Mutation == 7)
      F.Image.ObjCSourceReferences[F.SelectorSlot].Name = "other";
    if (Mutation == 8)
      F.Image.ObjCMethods[0].Status = "ambiguous_dispatch";
    if (Mutation == 9)
      F.Result.FunctionAudits.clear();
    if (Mutation == 10)
      F.Image.DyldBindSlots[0x2f08].WeakImport = true;
    if (Mutation == 11)
      F.Image.Segments[0].FileSz -= 4;
    EXPECT_FALSE(
        objc_super_getter_detail::prove(F.Image, F.Result, F.Getter, F.Root));
  }
}

TEST(ObjCSuperGetterSources, RejectsExternalAndMalformedLinkage) {
  for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
    SCOPED_TRACE(Mutation);
    SuperGetterFixture F;
    ASSERT_TRUE(isMachOLocalFunctionRange(F.Image, F.Getter, 60));
    auto *Symbol = reinterpret_cast<llvm::MachO::nlist_64 *>(
        F.Image.Segments[2].Data.data());
    if (Mutation == 0)
      Symbol->n_type |= llvm::MachO::N_EXT;
    if (Mutation == 1)
      Symbol->n_type |= llvm::MachO::N_PEXT;
    if (Mutation == 2)
      Symbol->n_value += 4;
    if (Mutation == 3)
      Symbol->n_sect = 2;
    if (Mutation == 4)
      F.Image.Segments[2].FileOff += 1;
    if (Mutation == 5)
      F.Image.Segments[2].Data.clear();
    if (Mutation == 6)
      F.exportTrie({0, 1, 'x', 0, 5, 3, 0, 0x80, 0x22, 0});
    if (Mutation == 7)
      F.exportTrie({0, 1, 'x', 0, 0x7f});
    if (Mutation == 8) {
      // The stub lies elsewhere, but the resolver is an externally reachable
      // address inside this otherwise local function.
      F.exportTrie({0, 1, 'x', 0, 5, 5, 0x10, 0x80, 0x26, 0x80, 0x22, 0});
    }
    if (Mutation == 9) {
      auto *Header = reinterpret_cast<llvm::MachO::mach_header_64 *>(
          F.Image.Segments[0].Data.data());
      auto *Command = reinterpret_cast<llvm::MachO::symtab_command *>(
          F.Image.Segments[0].Data.data() + sizeof(*Header) +
          Header->sizeofcmds - sizeof(llvm::MachO::symtab_command));
      Command->nsyms = 2;
      Symbol[1] = Symbol[0];
    }
    if (Mutation == 10) {
      F.exportTrie({0, 1, 'x', 0, 5, 3, 0, 0x80, 0x22, 0});
      F.Image.Segments[0].VA += 0x10000;
    }
    if (Mutation == 11)
      F.Image.Sections[0].VA -= 4;
    EXPECT_FALSE(isMachOLocalFunctionRange(F.Image, F.Getter, 60));
  }
}

TEST(ObjCSuperGetterSources, UnrelatedCallersKeepTheirOriginalNativeContract) {
  SuperGetterFixture F;
  F.word(0x1270, F.branch(0x1270, F.Getter));
  F.Image.CodeRefTargets.insert(F.Getter);
  HighFunc Other;
  Other.Entry = 0x1270;
  Other.Name = "unknown_indirect_caller";
  Other.ReturnType = NdType::makeInt(8);
  HighStmt Statement;
  Statement.Kind = StmtKind::Call;
  Statement.CallExpr = HighExpr::makeCall({}, F.Getter, {});
  Statement.CallExpr->IsIndirectCall = true;
  Other.Body.push_back(Statement);
  F.Result.HighFuncs.push_back(Other);
  const auto Plan = discoverObjCSuperGetterSources(F.Image, F.Result);
  EXPECT_EQ(Plan.Callers, (std::map<va_t, va_t>{{F.Root, F.Getter}}));
  EXPECT_FALSE(projectObjCSuperGetter(Other, F.Image, Plan).Projected);
  const auto *Original =
      objc_super_getter_detail::uniqueEntry(F.Result.HighFuncs, F.Getter);
  ASSERT_NE(Original, nullptr);
  EXPECT_FALSE(Original->SourceTypeHint);
  EXPECT_EQ(Original->Params.size(), 3U);
  EXPECT_FALSE(F.Result.HighFuncs.back().SourceTypeHint);
  EXPECT_TRUE(F.Result.HighFuncs.back().Body.front().CallExpr->IsIndirectCall);
}

TEST(ObjCSuperGetterSources, SelectorsShareCodeWithoutSharingTheirCells) {
  SuperGetterFixture F;
  constexpr va_t SecondRoot = 0x1020;
  constexpr va_t SecondSlot = 0x2108;
  F.word(SecondRoot, 0xb0000002);
  F.word(SecondRoot + 4, 0x91042042);
  F.word(SecondRoot + 8, F.branch(SecondRoot + 8, F.Getter, false));
  F.Image.ObjCSourceReferences[SecondSlot] = {
      ObjCSourceReference::Kind::Selector, SecondSlot, 8, "isSelected", {}};
  auto Method = F.Image.ObjCMethods.front();
  Method.Implementation = SecondRoot;
  Method.Selector = "isSelected";
  Method.TypeHint =
      parseObjCMethodEncoding(Method.Selector, Method.TypeEncoding);
  std::string Error;
  ASSERT_TRUE(assignDarwinObjCSourceABI(*Method.TypeHint, F.Image.Arch, Error));
  F.Image.ObjCMethods.push_back(Method);
  F.Image.Symbols.push_back({"second_root", SecondRoot, 0, true});
  F.Image.Exports.push_back({"second_root", 0, SecondRoot});
  const auto *Metadata =
      objc_super_getter_detail::uniqueEntry(F.Result.HighFuncs, F.Metadata);
  ASSERT_NE(Metadata, nullptr);
  ASSERT_TRUE(Metadata->SourceTypeHint);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  Options.OnlyFunctionEntries = {F.Root, SecondRoot, F.Getter, F.Metadata};
  Options.SourceTypeHints.emplace(F.Metadata, *Metadata->SourceTypeHint);
  F.Result = Pipeline().run(F.Image, F.Context, Options);
  ASSERT_TRUE(F.Result.Success);
  const auto Plan = discoverObjCSuperGetterSources(F.Image, F.Result);
  ASSERT_EQ(Plan.Callers.size(), 2U);
  const auto *First =
      objc_super_getter_detail::uniqueEntry(F.Result.HighFuncs, F.Root);
  const auto *Second =
      objc_super_getter_detail::uniqueEntry(F.Result.HighFuncs, SecondRoot);
  ASSERT_NE(First, nullptr);
  ASSERT_NE(Second, nullptr);
  auto A = projectObjCSuperGetter(*First, F.Image, Plan);
  auto B = projectObjCSuperGetter(*Second, F.Image, Plan);
  ASSERT_TRUE(A.Projected);
  ASSERT_TRUE(B.Projected);
  std::set<std::string> Helpers;
  renderObjCSuperGetterHelpers(F.Image, Plan, {F.Root, SecondRoot}, Helpers);
  EXPECT_EQ(Helpers.size(), 3U);
  std::map<va_t, const HighFunc *> Functions;
  for (const auto &Function : F.Result.HighFuncs)
    Functions.emplace(Function.Entry, &Function);
  auto &Call = *A.Function.Body.front().RetVal;
  EXPECT_TRUE(objCSuperGetterSourceCallBound(Call, F.Image, Plan, A.Function,
                                             Functions));
  Call.Operands[2] = B.Function.Body.front().RetVal->Operands[2];
  EXPECT_FALSE(objCSuperGetterSourceCallBound(Call, F.Image, Plan, A.Function,
                                              Functions));
}

TEST(ObjCSuperGetterSources, BoundCallsRequireCurrentProjectionAndProvider) {
  SuperGetterFixture F;
  const auto Plan = discoverObjCSuperGetterSources(F.Image, F.Result);
  ASSERT_EQ(Plan.Callers.size(), 1U);
  const auto *Root =
      objc_super_getter_detail::uniqueEntry(F.Result.HighFuncs, F.Root);
  ASSERT_NE(Root, nullptr);
  auto Projection = projectObjCSuperGetter(*Root, F.Image, Plan);
  ASSERT_TRUE(Projection.Projected);
  std::map<va_t, const HighFunc *> Functions;
  for (const auto &Function : F.Result.HighFuncs)
    Functions.emplace(Function.Entry, &Function);
  auto &Call = *Projection.Function.Body.front().RetVal;
  const auto Bound = [&](const HighExpr &Expression) {
    return objCSuperGetterSourceCallBound(Expression, F.Image, Plan,
                                          Projection.Function, Functions);
  };
  EXPECT_TRUE(Bound(Call));
  EXPECT_TRUE(Bound(*Call.Operands[2]));
  EXPECT_TRUE(Bound(*Call.Operands[3]));
  const auto *Audit =
      objc_super_getter_detail::uniqueEntry(F.Result.FunctionAudits, F.Root);
  ASSERT_NE(Audit, nullptr);
  EXPECT_TRUE(sourceBodyLimitation(Projection.Function,
                                   *Projection.Function.SourceTypeHint, Audit,
                                   Bound)
                  .empty());
  auto Hint = std::make_shared<SourceCallTypeHint>(*Call.SourceCallHint);
  Call.SourceCallHint = Hint;
  Hint->DoesNotReturn = true;
  EXPECT_FALSE(Bound(Call));
  Hint->DoesNotReturn = false;
  Call.Operands[0]->Var.Id = 1;
  EXPECT_FALSE(Bound(Call));
  Call.Operands[0]->Var.Id = 0;
  Projection.Function.Body.push_back(Projection.Function.Body.front());
  EXPECT_FALSE(Bound(Call));
  Projection.Function.Body.pop_back();
  Functions.erase(F.Metadata);
  EXPECT_FALSE(Bound(Call));
  F.Image.ObjCSourceReferences[F.SelectorSlot].Name = "changed";
  std::set<std::string> Helpers;
  EXPECT_THROW(renderObjCSuperGetterHelpers(F.Image, Plan, {F.Root}, Helpers),
               std::runtime_error);
  EXPECT_TRUE(Helpers.empty());
}
} // namespace

TEST(ObjCSuperGetterSources,
     KeepsFourCGRectResultsInTheCurrentSelectorContract) {
  SuperGetterFixture F;
  makeCGRectGetter(F);
  const auto Contract =
      objc_super_getter_detail::prove(F.Image, F.Result, F.Getter, F.Root);
  ASSERT_TRUE(Contract);
  ASSERT_EQ(Contract->MessageSignature.ReturnComponents.size(), 4U);
  const auto Plan = discoverObjCSuperGetterSources(F.Image, F.Result);
  const auto *Root =
      objc_super_getter_detail::uniqueEntry(F.Result.HighFuncs, F.Root);
  ASSERT_NE(Root, nullptr);
  auto Projection = projectObjCSuperGetter(*Root, F.Image, Plan);
  ASSERT_TRUE(Projection.Projected);
  std::map<va_t, const HighFunc *> Functions;
  for (const auto &Function : F.Result.HighFuncs)
    Functions.emplace(Function.Entry, &Function);
  const auto Bound = [&](const HighExpr &Expression) {
    return objCSuperGetterSourceCallBound(Expression, F.Image, Plan,
                                          Projection.Function, Functions);
  };
  const auto &Call = *Projection.Function.Body.front().RetVal;
  EXPECT_TRUE(Bound(Call));
  EXPECT_TRUE(Bound(*Call.Operands[2]));
  EXPECT_TRUE(Bound(*Call.Operands[3]));
  const auto *Audit =
      objc_super_getter_detail::uniqueEntry(F.Result.FunctionAudits, F.Root);
  ASSERT_NE(Audit, nullptr);
  EXPECT_TRUE(sourceBodyLimitation(Projection.Function,
                                   *Projection.Function.SourceTypeHint, Audit,
                                   Bound)
                  .empty());
}

TEST(ObjCSuperGetterSources,
     CGRectPublicationRejectsChangedCarriersAndOccurrence) {
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    SuperGetterFixture F;
    makeCGRectGetter(F);
    const auto Plan = discoverObjCSuperGetterSources(F.Image, F.Result);
    const auto *Root =
        objc_super_getter_detail::uniqueEntry(F.Result.HighFuncs, F.Root);
    ASSERT_NE(Root, nullptr);
    auto P = projectObjCSuperGetter(*Root, F.Image, Plan);
    ASSERT_TRUE(P.Projected);
    std::map<va_t, const HighFunc *> Functions;
    for (const auto &Function : F.Result.HighFuncs)
      Functions.emplace(Function.Entry, &Function);
    const auto Bound = [&](const HighExpr &Expression) {
      return objCSuperGetterSourceCallBound(Expression, F.Image, Plan,
                                            P.Function, Functions);
    };
    auto &Call = *P.Function.Body.front().RetVal;
    ASSERT_TRUE(Bound(Call));
    switch (Mutation) {
    case 0:
      P.Function.SourceTypeHint->ReturnComponents[3].RegisterOffset += 16;
      break;
    case 1:
      P.Function.ReturnType = NdType::makeFloat(8);
      break;
    case 2:
      P.Function.Body.front().Addr += 4;
      break;
    case 3:
      P.Function.DoesNotReturn = true;
      break;
    case 4:
      P.Function.StructuredExceptionRegions = 1;
      break;
    case 5: {
      auto Hint = std::make_shared<SourceCallTypeHint>(*Call.SourceCallHint);
      Hint->Signature.ReturnComponents[3].ValueBytes = 4;
      Call.SourceCallHint = Hint;
      break;
    }
    case 6:
      Call.Operands[0]->Var.RegOff += 8;
      break;
    case 7:
      F.Image.DyldBindSlots[0x2f08].WeakImport = true;
      break;
    case 8:
      F.word(F.Getter + 44, 0xd503201f);
      break;
    case 9:
      Functions.erase(F.Metadata);
      break;
    }
    EXPECT_FALSE(Bound(Call));
  }
}

TEST(ObjCSuperGetterSources,
     MixedReturnsKeepSeparateHelpersForTheSameMachineBody) {
  SuperGetterFixture F;
  makeCGRectGetter(F);
  constexpr va_t SecondRoot = 0x1020;
  constexpr va_t SecondSlot = 0x2108;
  F.word(SecondRoot, 0xb0000002);
  F.word(SecondRoot + 4, 0x91042042);
  F.word(SecondRoot + 8, F.branch(SecondRoot + 8, F.Getter, false));
  F.Image.ObjCSourceReferences[SecondSlot] = {
      ObjCSourceReference::Kind::Selector, SecondSlot, 8, "isHighlighted", {}};
  auto Method = F.Image.ObjCMethods.front();
  Method.Implementation = SecondRoot;
  Method.Selector = "isHighlighted";
  Method.TypeEncoding = "B16@0:8";
  Method.TypeHint =
      parseObjCMethodEncoding(Method.Selector, Method.TypeEncoding);
  std::string Error;
  ASSERT_TRUE(assignDarwinObjCSourceABI(*Method.TypeHint, F.Image.Arch, Error));
  F.Image.ObjCMethods.push_back(Method);
  F.Image.Symbols.push_back({"second_root", SecondRoot, 0, true});
  F.Image.Exports.push_back({"second_root", 0, SecondRoot});
  const auto *Metadata =
      objc_super_getter_detail::uniqueEntry(F.Result.HighFuncs, F.Metadata);
  ASSERT_NE(Metadata, nullptr);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  Options.OnlyFunctionEntries = {F.Root, SecondRoot, F.Getter, F.Metadata};
  Options.SourceTypeHints.emplace(F.Metadata, *Metadata->SourceTypeHint);
  F.Result = Pipeline().run(F.Image, F.Context, Options);
  ASSERT_TRUE(F.Result.Success);
  const auto Plan = discoverObjCSuperGetterSources(F.Image, F.Result);
  ASSERT_EQ(Plan.Callers.size(), 2U);
  std::set<std::string> Helpers;
  const auto Source = renderObjCSuperGetterHelpers(
      F.Image, Plan, {F.Root, SecondRoot}, Helpers);
  EXPECT_TRUE(Helpers.count("neverd_objc_super_getter_1100"));
  EXPECT_TRUE(Helpers.count("neverd_objc_super_getter_1100_cgrect"));
  EXPECT_EQ(Helpers.size(), 4U);
  EXPECT_NE(Source.find("_1100_cgrect("), std::string::npos);
}

TEST(ObjCSuperGetterSources, CGRectHelperMatchesNativeDispatchAtO0AndO2) {
  SuperGetterFixture F;
  makeCGRectGetter(F);
  const auto Plan = discoverObjCSuperGetterSources(F.Image, F.Result);
  const auto *Root =
      objc_super_getter_detail::uniqueEntry(F.Result.HighFuncs, F.Root);
  ASSERT_NE(Root, nullptr);
  auto P = projectObjCSuperGetter(*Root, F.Image, Plan);
  ASSERT_TRUE(P.Projected);
  P.Function.Name = "projected_root";
  std::string Emitted;
  llvm::raw_string_ostream OS(Emitted);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  Options.Image = &F.Image;
  ASSERT_TRUE(HighCEmitter().emit({P.Function}, OS, Options));
  std::set<std::string> Helpers;
  auto Source = renderObjCSuperGetterHelpers(F.Image, Plan, {F.Root}, Helpers);
  const std::string Header = "#include <objc/runtime.h>";
  const auto Position = Source.find(Header);
  ASSERT_NE(Position, std::string::npos);
  Source.replace(Position, Header.size(),
                 "void *sel_registerName(const char *name);");
  const auto Record = typeToC(P.Function.ReturnType);
  const auto Name = objc_super_getter_detail::helperName(
      *validatedObjCSuperGetter(F.Image, Plan, F.Root));
  std::string Program = "#include <string.h>\n" + Emitted + Source;
  Program += R"C(
static void *slot;
static unsigned metadata_calls, dispatch_calls, bad;
static const uint64_t words[4] = {0x3fe0000000000000ULL, 0x8000000000000000ULL,
                                 0x7ff0000000000000ULL, 0x7ff8000000000011ULL};
void *sel_registerName(const char *name) { (void)name; return (void *)0x2220; }
uint64_t metadata(void) {
  ++metadata_calls;
  slot = (void *)0x2220;
  return 0x3330;
}
)C";
  Program += Record + R"C( test_super(void *p, void *selector) {
  void *receiver, *current_class;
  memcpy(&receiver, p, 8);
  memcpy(&current_class, (const unsigned char *)p + 8, 8);
  if (receiver != (void *)0x1110 || current_class != (void *)0x3330 ||
      selector != (void *)0x2220 || metadata_calls != dispatch_calls + 1)
    ++bad;
  ++dispatch_calls;
)C" + Record +
             R"C( result;
  memcpy(&result, words, 32);
  return result;
}
#if defined(__aarch64__) && defined(__APPLE__)
__asm__(".text\n.p2align 2\n.globl _objc_msgSendSuper2\n"
        "_objc_msgSendSuper2:\nb _test_super\n"
        ".globl _original_getter\n_original_getter:\n"
        "sub sp, sp, #48\nstp x20, x19, [sp, #16]\n"
        "stp x29, x30, [sp, #32]\nadd x29, sp, #32\n"
        "mov x19, x2\nmov x20, x0\nbl _metadata\n"
        "stp x20, x0, [sp]\nldr x1, [x19]\nmov x0, sp\n"
        "bl _objc_msgSendSuper2\nldp x29, x30, [sp, #32]\n"
        "ldp x20, x19, [sp, #16]\nadd sp, sp, #48\nret\n");
)C" + Record +
             R"C( original_getter(void *, void *, void **);
#else
)C" + Record +
             R"C( portable_super(void *, void *)
#if defined(__APPLE__)
__asm__("_objc_msgSendSuper2");
#else
__asm__("objc_msgSendSuper2");
#endif
)C" + Record +
             R"C( portable_super(void *p, void *s) { return test_super(p, s); }
#endif
int main(void) {
  slot = (void *)0x4440;
)C" + Record +
             " generated = " + Name +
             R"C(((void *)0x1110, 0, &slot, (void *)metadata);
  if (bad || metadata_calls != 1 || dispatch_calls != 1 || memcmp(&generated, words, 32)) return 1;
#if defined(__aarch64__) && defined(__APPLE__)
  slot = (void *)0x4440;
)C" + Record +
             R"C( original = original_getter((void *)0x1110, 0, &slot);
  if (bad || metadata_calls != 2 || dispatch_calls != 2 || memcmp(&original, &generated, 32)) return 2;
#endif
  return 0;
}
)C";
  source_call_execution_test::compileAndRun(Program, {"-O0"});
  source_call_execution_test::compileAndRun(Program, {"-O2"});
}
