//===- ModuleBindingTests.cpp - Resource declarations and atomic supply
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Resource declarations and atomic supply.
///

#include "gtest/gtest.h"

#include "neverd/emulation/ModuleResolver.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Support/FileSystem.h"

#include <algorithm>
#include <cstring>
#include <fstream>

using namespace neverd;
using namespace neverd::emulation;
using namespace neverd::modules;
namespace {
constexpr uint64_t Page = 4096, Original = 0x401000;
llvm::Error failure() {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "injected preparation failure");
}
class ModuleBindingFiles : public testing::Test {
protected:
  std::filesystem::path Root, File, Map;
  std::shared_ptr<ModuleSet> Sources;
  std::shared_ptr<AddressSpace> Space;
  std::unique_ptr<ModuleBindings> Bindings;
  void SetUp() override {
    llvm::SmallString<128> Directory;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-binding-test", Directory));
    Root = std::u8string(Directory.begin(), Directory.end());
    File = Root / "input.elf";
    Map = Root / "input.map";
    using ELF = llvm::object::ELF64LE;
    ELF::Ehdr H{};
    std::memcpy(H.e_ident, "\177ELF", 4);
    H.e_ident[llvm::ELF::EI_CLASS] = llvm::ELF::ELFCLASS64;
    H.e_ident[llvm::ELF::EI_DATA] = llvm::ELF::ELFDATA2LSB;
    H.e_ident[llvm::ELF::EI_VERSION] = llvm::ELF::EV_CURRENT;
    H.e_type = llvm::ELF::ET_EXEC;
    H.e_machine = llvm::ELF::EM_X86_64;
    H.e_version = llvm::ELF::EV_CURRENT;
    H.e_entry = Original;
    H.e_ehsize = sizeof(H);
    H.e_phoff = sizeof(H);
    H.e_phentsize = sizeof(ELF::Phdr);
    H.e_phnum = 1;
    H.e_shentsize = sizeof(ELF::Shdr);
    ELF::Phdr P{};
    P.p_type = llvm::ELF::PT_LOAD;
    P.p_flags = llvm::ELF::PF_R | llvm::ELF::PF_X;
    P.p_offset = Page;
    P.p_vaddr = Original;
    P.p_filesz = 6;
    P.p_memsz = 6;
    P.p_align = Page;
    std::vector<uint8_t> Bytes(Page + 6);
    std::memcpy(Bytes.data(), &H, sizeof(H));
    std::memcpy(Bytes.data() + sizeof(H), &P, sizeof(P));
    const uint8_t Code[] = {0xB8, 42, 0, 0, 0, 0xC3};
    std::memcpy(Bytes.data() + Page, Code, sizeof(Code));
    std::ofstream OS(File, std::ios::binary);
    OS.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    OS.close();
    std::ofstream(Map)
        << "VMA LMA Size Align Out In Symbol\n"
           "0000000000401000 0000000000401000 00000006 16 .text\n"
           "0000000000401000 0000000000401000 00000006 1 probe.o:(.text)\n"
           "0000000000401000 0000000000401000 00000006 1 probe\n";
    ModuleOptions O;
    O.DefaultLoad = LoadPolicy::Lazy;
    O.Modules.push_back(
        {"resource", File, DebugInput{DebugKind::Map, Map}, {}});
    Sources = llvm::cantFail(ModuleSet::create(std::move(O)));
    Space = llvm::cantFail(AddressSpace::create(
        llvm::cantFail(PhysicalMemory::create(16 * Page)), 16 * Page));
    Bindings = llvm::cantFail(ModuleBindings::create(Sources));
  }
  void TearDown() override {
    Bindings.reset();
    Sources.reset();
    Space.reset();
    std::error_code EC;
    std::filesystem::remove_all(Root, EC);
  }
  ModuleBinding binding(std::string ID = "one", uint64_t Base = 0x10000,
                        std::shared_ptr<AddressSpace> AS = {}) {
    const auto Guest = ID + ".module";
    return {std::move(ID),   "resource", Guest, "kernel",
            AS ? AS : Space, Base,       Page};
  }
  static llvm::Expected<ModuleMappingPlan> build(const ModuleBinding &B,
                                                 const ModuleImage &Source) {
    // This fixture contains no imports or absolute-address operands. Real
    // relocation and OS loader decisions belong to an environment adapter.
    const auto &Image = Source.image();
    auto Plan = ImageMappingPlan::create(Image, 0, Page, 16 * Page, false,
                                         ImagePagePadding::Zero,
                                         ImageByteSource::OriginalFile);
    if (!Plan)
      return Plan.takeError();
    const uint64_t Base = B.Base.value_or(Plan->Base);
    for (auto &R : Plan->Regions)
      R.Address = Base + (R.Address - Plan->Base);
    Plan->Entry = Base + (Plan->Entry - Plan->Base);
    Plan->Base = Base;
    ModuleExport E{"probe", 7, Plan->Entry, SymbolKind::Function};
    return ModuleMappingPlan{std::move(*Plan), Page, {E}};
  }
  ModuleInstance preexisting(std::string ID = "bootstrap",
                             uint64_t Base = 0x10000) {
    ModuleInstance I;
    I.Binding = binding(std::move(ID), Base);
    I.Source = llvm::cantFail(Sources->prepare("resource"));
    I.Base = Base;
    I.Size = Page;
    I.Entry = Base;
    I.Exports = {{"probe", 7, Base, SymbolKind::Function}};
    I.EnvironmentOwned = true;
    I.Role = "fixture-owner";
    return I;
  }
};
TEST_F(ModuleBindingFiles, AdoptionPinsPreexistingObservationWithoutRemapping) {
  auto I = preexisting();
  I.Exports.push_back({"opaque-data", 8, 0x20000, SymbolKind::Data, true});
  llvm::cantFail(Space->map(I.Base, Page, Read | Execute));
  const auto Generation = Space->mappingGeneration();
  const auto View = llvm::cantFail(Space->pinBacking(I.Base, Page));
  auto Empty = llvm::cantFail(ModuleSet::create({}));
  Bindings = llvm::cantFail(ModuleBindings::create(std::move(Empty)));
  Sources.reset();
  std::filesystem::remove(File);
  std::filesystem::remove(Map);
  unsigned Validations = 0;
  auto Proof = [&](llvm::ArrayRef<ModuleInstance> Batch) {
    ++Validations;
    EXPECT_EQ(Batch.size(), 1u);
    EXPECT_EQ(Batch[0].Source, I.Source);
    return llvm::Error::success();
  };
  ASSERT_EQ(llvm::toString(Bindings->adopt({I}, Proof)), "");
  EXPECT_EQ(Validations, 1u);
  EXPECT_EQ(Bindings->revision(), 1u);
  EXPECT_EQ(**Bindings->lookupSymbol("bootstrap", "probe"), I.Base);
  EXPECT_EQ((**Bindings->lookupExport("bootstrap", uint32_t(7))).Address,
            I.Base);
  EXPECT_EQ((**Bindings->lookupExport("bootstrap", "opaque-data")).Address,
            0x20000u);
  EXPECT_FALSE(*Space->canAccess(0x20000, 1, Read));
  EXPECT_TRUE(Bindings->find("bootstrap")->EnvironmentOwned);
  EXPECT_EQ(Space->mappingGeneration(), Generation);
  EXPECT_EQ(Space->physicalMemory()->allocatedBytes(), Page);
  EXPECT_TRUE(
      View.describesSameBytes(llvm::cantFail(Space->pinBacking(I.Base, Page))));
  EXPECT_FALSE(*Space->canAccess(I.Base, 1, Write));
  EXPECT_NE(llvm::toString(Bindings->adopt({I}, Proof)), "");
  EXPECT_EQ(Validations, 1u);
}
TEST_F(ModuleBindingFiles, AdoptionRejectsInvalidOwnershipAndPendingRequests) {
  const auto I = preexisting();
  const auto Accept = [](llvm::ArrayRef<ModuleInstance>) {
    return llvm::Error::success();
  };
  EXPECT_NE(llvm::toString(Bindings->adopt({I}, {})), "");
  EXPECT_NE(
      llvm::toString(Bindings->adopt({I}, [](auto) { return failure(); })), "");
  std::vector<ModuleInstance> Bad;
  auto B = I;
  B.Source.reset();
  Bad.push_back(B);
  B = I;
  B.EnvironmentOwned = false;
  Bad.push_back(B);
  B = I;
  B.Role.clear();
  Bad.push_back(B);
  B = I;
  B.Binding.Size = 2 * Page;
  Bad.push_back(B);
  B = I;
  B.Entry = B.Base + B.Size;
  Bad.push_back(B);
  B = I;
  B.Exports[0].Address = B.Base + B.Size;
  Bad.push_back(B);
  B = I;
  B.Exports.push_back(B.Exports[0]);
  Bad.push_back(B);
  for (const auto &Invalid : Bad)
    EXPECT_NE(llvm::toString(Bindings->adopt({Invalid}, Accept)), "");
  B = preexisting("other", I.Base);
  EXPECT_NE(llvm::toString(Bindings->adopt({I, B}, Accept)), "");
  EXPECT_TRUE(Bindings->inspect().empty());
  EXPECT_EQ(Bindings->revision(), 0u);
  auto R = llvm::cantFail(Bindings->requestDependency(Space, "owner.module"));
  EXPECT_NE(llvm::toString(Bindings->adopt({I}, Accept)), "");
  EXPECT_EQ(Bindings->pendingRequest()->Token, R.Token);
}
TEST_F(ModuleBindingFiles, LazyDeclarationIsVisibleWithoutFilePreparation) {
  ASSERT_EQ(llvm::toString(Bindings->declare(binding())), "");
  EXPECT_EQ(Bindings->inspect().size(), 1u);
  EXPECT_FALSE(Bindings->find("one")->mapped());
  EXPECT_EQ(Bindings->find("one")->Load, LoadPolicy::Lazy);
  std::filesystem::remove(File);
  EXPECT_EQ(llvm::toString(Bindings->prepareEager(Space, build)), "");
  EXPECT_FALSE(Sources->prepared("resource"));
  EXPECT_EQ(Space->mappedBytes(), 0u);
}
TEST_F(ModuleBindingFiles, SameFileHasIndependentBasesAndAddressSpaces) {
  const auto A = binding(), B = binding("two", 0x20000);
  auto Other =
      llvm::cantFail(AddressSpace::create(Space->physicalMemory(), 16 * Page));
  const auto C = binding("three", 0x10000, Other);
  ASSERT_EQ(llvm::toString(Bindings->materialize({A, B}, build)), "");
  ASSERT_EQ(llvm::toString(Bindings->materialize({C}, build)), "");
  EXPECT_EQ(Bindings->find("one")->Source, Bindings->find("three")->Source);
  EXPECT_EQ(**Bindings->lookupSymbol("one", "probe"), 0x10000u);
  EXPECT_EQ(**Bindings->lookupSymbol("two", "probe"), 0x20000u);
  EXPECT_EQ(**Bindings->lookupSymbol("three", "probe"), 0x10000u);
  EXPECT_EQ((**Bindings->lookupExport("two", "probe")).Address, 0x20000u);
  EXPECT_EQ((**Bindings->lookupExport("one", uint32_t(7))).Address, 0x10000u);
  EXPECT_FALSE(*Bindings->lookupExport("one", "Probe"));
  EXPECT_FALSE(*Bindings->lookupSymbol("one", "absent"));
  EXPECT_EQ(*Space->readInteger(0x10001, 1), 42u);
  EXPECT_EQ(*Other->readInteger(0x10001, 1), 42u);
  EXPECT_NE(llvm::toString(Space->writeInteger(0x10001, 3, 1)), "");
}
TEST_F(ModuleBindingFiles, GuestNamesAreScopedButVirtualRangesCannotOverlap) {
  auto A = binding();
  ASSERT_EQ(llvm::toString(Bindings->declare(A)), "");
  auto B = binding("two", 0x20000);
  B.GuestName = A.GuestName;
  EXPECT_NE(llvm::toString(Bindings->declare(B)), "");
  B.Scope = "another";
  EXPECT_EQ(llvm::toString(Bindings->declare(B)), "");
  auto C = binding("three", 0x10000);
  C.Scope = "third";
  EXPECT_NE(llvm::toString(Bindings->declare(C)), "");
  EXPECT_EQ(Bindings->inspect().size(), 2u);
}
TEST_F(ModuleBindingFiles, EagerAndLazyPoliciesUseOneMappingPath) {
  ASSERT_EQ(
      llvm::toString(Sources->add({"startup", File, {}, LoadPolicy::Eager})),
      "");
  auto Eager = binding("startup");
  Eager.ModuleID = "startup";
  auto Lazy = binding("later", 0x20000);
  ASSERT_EQ(llvm::toString(Bindings->declare(Eager)), "");
  ASSERT_EQ(llvm::toString(Bindings->declare(Lazy)), "");
  ASSERT_EQ(llvm::toString(Bindings->prepareEager(Space, build)), "");
  EXPECT_TRUE(Bindings->find("startup")->mapped());
  EXPECT_FALSE(Bindings->find("later")->mapped());
  EXPECT_FALSE(Sources->prepared("resource"));
  const auto Request = llvm::cantFail(Bindings->requestModule("later"));
  ASSERT_EQ(llvm::toString(Bindings->supply(Request.Token, {Lazy}, build)), "");
  EXPECT_TRUE(Bindings->find("later")->mapped());
  EXPECT_EQ(Space->mappedBytes(), 2 * Page);
}
TEST_F(ModuleBindingFiles, DeclaredRangeRequestRetainsAccessAndPC) {
  const auto B = binding();
  ASSERT_EQ(llvm::toString(Bindings->declare(B)), "");
  const auto R = llvm::cantFail(
      Bindings->requestAddress(Space, 0x777, 0x10000, 6, ModuleAccess::Fetch));
  EXPECT_EQ(R.Kind, ModuleRequestKind::DeclaredModule);
  EXPECT_EQ(R.InstanceID, "one");
  EXPECT_EQ(R.ModuleID, "resource");
  EXPECT_EQ(R.PC, 0x777u);
  EXPECT_EQ(R.Size, 6u);
  ASSERT_EQ(llvm::toString(Bindings->supply(R.Token, {B}, build)), "");
  EXPECT_FALSE(Bindings->pendingRequest());
  auto Again =
      Bindings->requestAddress(Space, 0x777, 0x10000, 1, ModuleAccess::Write);
  ASSERT_FALSE(bool(Again));
  llvm::consumeError(Again.takeError());
}
TEST_F(ModuleBindingFiles, UnknownAddressDoesNotInventModuleIdentity) {
  const auto R = llvm::cantFail(
      Bindings->requestAddress(Space, 0x777, 0x90000, 4, ModuleAccess::Read));
  EXPECT_EQ(R.Kind, ModuleRequestKind::UnmappedCandidate);
  EXPECT_FALSE(R.InstanceID);
  EXPECT_FALSE(R.ModuleID);
  EXPECT_NE(llvm::toString(Bindings->supply(R.Token, {binding()}, build)), "");
  EXPECT_EQ(Bindings->pendingRequest()->Token, R.Token);
  EXPECT_TRUE(Bindings->inspect().empty());
  ASSERT_EQ(llvm::toString(Bindings->supply(
                R.Token, {binding("supplied", 0x90000)}, build)),
            "");
  EXPECT_TRUE(Bindings->find("supplied")->mapped());
}
TEST_F(ModuleBindingFiles, SupplyMustMatchSpaceIdentityAndFinalPermission) {
  auto R = llvm::cantFail(
      Bindings->requestAddress(Space, 1, 0x10000, 4, ModuleAccess::Write));
  auto Other =
      llvm::cantFail(AddressSpace::create(Space->physicalMemory(), 16 * Page));
  EXPECT_NE(llvm::toString(Bindings->supply(
                R.Token, {binding("other", 0x10000, Other)}, build)),
            "");
  EXPECT_NE(llvm::toString(Bindings->supply(R.Token, {binding()}, build)), "");
  EXPECT_EQ(Space->mappedBytes(), 0u);
  EXPECT_EQ(Other->mappedBytes(), 0u);
  EXPECT_TRUE(Bindings->pendingRequest());
  ASSERT_EQ(llvm::toString(Bindings->cancel(R.Token)), "");
  ASSERT_EQ(llvm::toString(Bindings->declare(binding())), "");
  R = llvm::cantFail(Bindings->requestModule("one"));
  auto Changed = binding();
  Changed.ModuleID = "another-resource";
  EXPECT_NE(llvm::toString(Bindings->supply(R.Token, {Changed}, build)), "");
}
TEST_F(ModuleBindingFiles, TokensAreOwnedVersionedAndConsumedExactlyOnce) {
  auto Other = llvm::cantFail(ModuleBindings::create(Sources));
  const auto R = llvm::cantFail(
      Bindings->requestAddress(Space, 1, 0x10000, 1, ModuleAccess::Read));
  const auto Foreign = llvm::cantFail(
      Other->requestAddress(Space, 1, 0x20000, 1, ModuleAccess::Read));
  unsigned Calls = 0;
  auto Builder = [&](const ModuleBinding &B, const ModuleImage &I) {
    ++Calls;
    return build(B, I);
  };
  EXPECT_NE(
      llvm::toString(Bindings->supply(Foreign.Token, {binding()}, Builder)),
      "");
  auto Stale = R.Token;
  ++Stale.Version;
  EXPECT_NE(llvm::toString(Bindings->supply(Stale, {binding()}, Builder)), "");
  EXPECT_EQ(Calls, 0u);
  EXPECT_FALSE(Sources->prepared("resource"));
  ASSERT_EQ(llvm::toString(Bindings->supply(R.Token, {binding()}, Builder)),
            "");
  EXPECT_EQ(Calls, 1u);
  EXPECT_NE(llvm::toString(Bindings->supply(R.Token, {binding()}, Builder)),
            "");
  EXPECT_EQ(Calls, 1u);
  EXPECT_TRUE(Other->pendingRequest());
}
TEST_F(ModuleBindingFiles, CancelledTokenDoesNotApplyToNextRequest) {
  const auto R =
      llvm::cantFail(Bindings->requestDependency(Space, "one.module"));
  ASSERT_EQ(llvm::toString(Bindings->cancel(R.Token)), "");
  const auto Next =
      llvm::cantFail(Bindings->requestDependency(Space, "one.module"));
  EXPECT_NE(R.Token, Next.Token);
  EXPECT_NE(llvm::toString(Bindings->cancel(R.Token)), "");
  EXPECT_NE(llvm::toString(Bindings->supply(R.Token, {binding()}, build)), "");
  EXPECT_FALSE(Sources->prepared("resource"));
  ASSERT_EQ(llvm::toString(Bindings->supply(Next.Token, {binding()}, build)),
            "");
}
TEST_F(ModuleBindingFiles, DependencySupplyRejectsUnrelatedIdentity) {
  const auto R =
      llvm::cantFail(Bindings->requestDependency(Space, "helper.module", 0x42));
  EXPECT_EQ(R.Kind, ModuleRequestKind::MissingDependency);
  EXPECT_EQ(R.Dependency, "helper.module");
  EXPECT_NE(llvm::toString(Bindings->supply(R.Token, {binding()}, build)), "");
  EXPECT_FALSE(Sources->prepared("resource"));
  ASSERT_EQ(
      llvm::toString(Bindings->supply(R.Token, {binding("helper")}, build)),
      "");
}
TEST_F(ModuleBindingFiles, FailedBuilderBatchPublishesNoMappingsOrExports) {
  const auto Generation = Space->mappingGeneration();
  const auto Revision = Bindings->revision();
  unsigned Calls = 0;
  auto Builder =
      [&](const ModuleBinding &B,
          const ModuleImage &I) -> llvm::Expected<ModuleMappingPlan> {
    if (++Calls == 2)
      return failure();
    return build(B, I);
  };
  EXPECT_NE(llvm::toString(Bindings->materialize(
                {binding(), binding("two", 0x20000)}, Builder)),
            "");
  EXPECT_EQ(Calls, 2u);
  EXPECT_TRUE(Bindings->inspect().empty());
  EXPECT_EQ(Bindings->revision(), Revision);
  EXPECT_EQ(Space->mappedBytes(), 0u);
  EXPECT_EQ(Space->physicalMemory()->allocatedBytes(), 0u);
  EXPECT_EQ(Space->mappingGeneration(), Generation);
  auto Query = Bindings->lookupExport("one", "probe");
  ASSERT_FALSE(bool(Query));
  llvm::consumeError(Query.takeError());
  ASSERT_EQ(llvm::toString(Bindings->materialize(
                {binding(), binding("two", 0x20000)}, build)),
            "");
}
TEST_F(ModuleBindingFiles, FailedSourcePreparationRetainsPendingRequest) {
  const auto B = binding();
  ASSERT_EQ(llvm::toString(Bindings->declare(B)), "");
  const auto R = llvm::cantFail(Bindings->requestModule("one"));
  const auto Backup = Root / "saved.elf";
  std::filesystem::rename(File, Backup);
  EXPECT_NE(llvm::toString(Bindings->supply(R.Token, {B}, build)), "");
  EXPECT_EQ(Bindings->pendingRequest()->Token, R.Token);
  EXPECT_FALSE(Bindings->find("one")->mapped());
  std::filesystem::rename(Backup, File);
  ASSERT_EQ(llvm::toString(Bindings->supply(R.Token, {B}, build)), "");
}
TEST_F(ModuleBindingFiles, ConflictLeavesExistingBytesAliasesAndBudgets) {
  ASSERT_EQ(llvm::toString(Space->map(0x20000, Page, Read | Write)), "");
  ASSERT_EQ(llvm::toString(Space->writeInteger(0x20000, 73, 4)), "");
  ASSERT_EQ(llvm::toString(Space->mapAlias(0x30000, 0x20000, Page, Read)), "");
  const auto Generation = Space->mappingGeneration();
  EXPECT_NE(llvm::toString(Bindings->materialize(
                {binding(), binding("two", 0x20000)}, build)),
            "");
  EXPECT_EQ(Space->mappingGeneration(), Generation);
  EXPECT_EQ(Space->mappedBytes(), 2 * Page);
  EXPECT_EQ(Space->physicalMemory()->allocatedBytes(), Page);
  EXPECT_EQ(*Space->readInteger(0x30000, 4), 73u);
  EXPECT_TRUE(Bindings->inspect().empty());
}
TEST_F(ModuleBindingFiles,
       MalformedPlansAndExportConflictsAreRejectedBeforeCommit) {
  const auto Generation = Space->mappingGeneration();
  for (unsigned Case = 0; Case < 7; ++Case) {
    auto Builder =
        [&](const ModuleBinding &B,
            const ModuleImage &I) -> llvm::Expected<ModuleMappingPlan> {
      auto P = llvm::cantFail(build(B, I));
      switch (Case) {
      case 0:
        ++P.Image.Base;
        break;
      case 1:
        ++P.Image.MappedBytes;
        break;
      case 2:
        P.Image.Entry = 0x90000;
        break;
      case 3:
        P.Exports.push_back(P.Exports.front());
        break;
      case 4:
        P.Exports.front().Address = 0x90000;
        break;
      case 5:
        P.Image.Regions.front().Permissions = Read;
        break;
      case 6:
        P.Image.Regions.front().Bytes.resize(Page - 1);
        --P.Image.MappedBytes;
        break;
      }
      return P;
    };
    EXPECT_NE(llvm::toString(Bindings->materialize({binding()}, Builder)), "")
        << Case;
    EXPECT_EQ(Space->mappingGeneration(), Generation);
    EXPECT_EQ(Space->physicalMemory()->allocatedBytes(), 0u);
    EXPECT_TRUE(Bindings->inspect().empty());
  }
}
TEST_F(ModuleBindingFiles, NoBaseDeclarationsAreValidatedAtMaterialization) {
  auto A = binding();
  A.Base.reset();
  A.Size.reset();
  ASSERT_EQ(llvm::toString(Bindings->declare(A)), "");
  ASSERT_EQ(llvm::toString(Bindings->declare(binding("two", Original))), "");
  EXPECT_NE(llvm::toString(Bindings->materialize({A}, build)), "");
  EXPECT_EQ(Space->mappedBytes(), 0u);
  EXPECT_FALSE(Bindings->find("one")->mapped());
}
TEST_F(ModuleBindingFiles, BuilderCannotMutateItsBindingOwnerRecursively) {
  const auto B = binding();
  auto Builder = [&](const ModuleBinding &Input, const ModuleImage &I) {
    EXPECT_NE(llvm::toString(Bindings->declare(binding("nested", 0x20000))),
              "");
    auto R = Bindings->requestDependency(Space, "nested");
    EXPECT_FALSE(bool(R));
    if (!R)
      llvm::consumeError(R.takeError());
    return build(Input, I);
  };
  ASSERT_EQ(llvm::toString(Bindings->materialize({B}, Builder)), "");
  EXPECT_EQ(Bindings->inspect().size(), 1u);
}
TEST_F(ModuleBindingFiles, ResolverAndManualSupplyShareTheCommitPath) {
  const auto B = binding();
  ASSERT_EQ(llvm::toString(Bindings->declare(B)), "");
  const auto R = llvm::cantFail(Bindings->requestModule("one"));
  unsigned Calls = 0;
  auto Resolver =
      [&](const ModuleRequest &Input) -> llvm::Expected<ModuleResolution> {
    ++Calls;
    EXPECT_EQ(Input.Token, R.Token);
    EXPECT_EQ(Space->mappedBytes(), 0u);
    return ModuleResolution{ModuleResolutionKind::Found, {B}};
  };
  EXPECT_EQ(*resolveModuleRequest(*Bindings, Resolver, build),
            ModuleResolutionKind::Found);
  EXPECT_EQ(Calls, 1u);
  EXPECT_TRUE(Bindings->find("one")->mapped());
  EXPECT_FALSE(Bindings->pendingRequest());
}
TEST_F(ModuleBindingFiles,
       ResolverCanWaitFailAndRegisterOmittedResourceWithoutResuming) {
  const auto R = llvm::cantFail(
      Bindings->requestAddress(Space, 1, 0x90000, 1, ModuleAccess::Read));
  for (const auto Kind :
       {ModuleResolutionKind::NeedInput, ModuleResolutionKind::NotFound}) {
    EXPECT_EQ(
        *resolveModuleRequest(
            *Bindings,
            [&](const ModuleRequest &) -> llvm::Expected<ModuleResolution> {
              return ModuleResolution{Kind, {}};
            },
            build),
        Kind);
    EXPECT_EQ(Bindings->pendingRequest()->Token, R.Token);
  }
  auto Error = resolveModuleRequest(
      *Bindings,
      [&](const ModuleRequest &) -> llvm::Expected<ModuleResolution> {
        return failure();
      },
      build);
  ASSERT_FALSE(bool(Error));
  llvm::consumeError(Error.takeError());
  auto Resolver =
      [&](const ModuleRequest &) -> llvm::Expected<ModuleResolution> {
    if (auto E = Sources->add({"omitted", File, {}, LoadPolicy::Lazy}))
      return std::move(E);
    auto B = binding("late", 0x90000);
    B.ModuleID = "omitted";
    return ModuleResolution{ModuleResolutionKind::Found, {B}};
  };
  EXPECT_EQ(*resolveModuleRequest(*Bindings, Resolver, build),
            ModuleResolutionKind::Found);
  EXPECT_TRUE(Bindings->find("late")->mapped());
}
TEST_F(ModuleBindingFiles, ResolverCannotCancelSupplyOrReenterDuringCallback) {
  const auto R =
      llvm::cantFail(Bindings->requestDependency(Space, "one.module"));
  auto Resolver =
      [&](const ModuleRequest &) -> llvm::Expected<ModuleResolution> {
    EXPECT_NE(llvm::toString(Bindings->cancel(R.Token)), "");
    EXPECT_NE(llvm::toString(Bindings->supply(R.Token, {binding()}, build)),
              "");
    auto Recursive = resolveModuleRequest(
        *Bindings,
        [](const ModuleRequest &) -> llvm::Expected<ModuleResolution> {
          ADD_FAILURE() << "recursive resolver ran";
          return ModuleResolution{};
        },
        build);
    EXPECT_FALSE(bool(Recursive));
    if (!Recursive)
      llvm::consumeError(Recursive.takeError());
    return ModuleResolution{};
  };
  EXPECT_EQ(*resolveModuleRequest(*Bindings, Resolver, build),
            ModuleResolutionKind::NeedInput);
  EXPECT_EQ(Bindings->pendingRequest()->Token, R.Token);
  EXPECT_FALSE(Sources->prepared("resource"));
}
TEST_F(ModuleBindingFiles, PreparedSourceBytesSurviveHostReplacement) {
  auto Source = llvm::cantFail(Sources->prepare("resource"));
  std::ofstream(File, std::ios::binary | std::ios::trunc)
      << "changed host input";
  ASSERT_EQ(llvm::toString(Bindings->materialize({binding()}, build)), "");
  EXPECT_EQ(Bindings->find("one")->Source, Source);
  EXPECT_EQ(*Space->readInteger(0x10001, 1), 42u);
}
TEST_F(ModuleBindingFiles,
       CrossPageSupplyValidatesTheWholeAccessAcrossModules) {
  const auto R = llvm::cantFail(
      Bindings->requestAddress(Space, 1, 0x10FFC, 8, ModuleAccess::Read));
  const auto A = binding(), B = binding("next", 0x11000);
  EXPECT_NE(llvm::toString(Bindings->supply(R.Token, {A}, build)), "");
  EXPECT_EQ(Space->mappedBytes(), 0u);
  ASSERT_EQ(llvm::toString(Bindings->supply(R.Token, {A, B}, build)), "");
  EXPECT_TRUE(*Space->canAccess(0x10FFC, 8, Read));
}
TEST_F(ModuleBindingFiles, CrossPageSupplyCanRetainAnExistingPrefix) {
  ASSERT_EQ(llvm::toString(Space->map(0x10000, Page, Read | Write)), "");
  ASSERT_EQ(llvm::toString(Space->writeInteger(0x10FFC, 73, 4)), "");
  const auto R = llvm::cantFail(
      Bindings->requestAddress(Space, 1, 0x10FFC, 8, ModuleAccess::Read));
  ASSERT_EQ(llvm::toString(
                Bindings->supply(R.Token, {binding("next", 0x11000)}, build)),
            "");
  EXPECT_EQ(*Space->readInteger(0x10FFC, 4), 73u);
  EXPECT_TRUE(*Space->canAccess(0x10FFC, 8, Read));
}
TEST_F(ModuleBindingFiles,
       PhysicalCapacityFailureDoesNotPublishModuleMetadata) {
  Space = llvm::cantFail(AddressSpace::create(
      llvm::cantFail(PhysicalMemory::create(Page)), 16 * Page));
  const auto Generation = Space->mappingGeneration();
  EXPECT_NE(llvm::toString(Bindings->materialize(
                {binding(), binding("next", 0x11000)}, build)),
            "");
  EXPECT_EQ(Space->mappingGeneration(), Generation);
  EXPECT_EQ(Space->physicalMemory()->allocatedBytes(), 0u);
  EXPECT_TRUE(Bindings->inspect().empty());
}
TEST_F(ModuleBindingFiles, FullyMappedMemoryIsNotAResourceRequest) {
  ASSERT_EQ(llvm::toString(Space->map(0x10000, Page, Read)), "");
  auto R = Bindings->requestAddress(Space, 1, 0x10000, 1, ModuleAccess::Write);
  ASSERT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  EXPECT_FALSE(Bindings->pendingRequest());
}
class TestPublication final : public ModulePublication {
public:
  TestPublication(std::vector<std::string> &Target,
                  std::vector<std::string> Next, unsigned &Commits,
                  unsigned &Abandoned, uint64_t GuestAddress = 0)
      : Target(Target), Next(std::move(Next)), Commits(Commits),
        Abandoned(Abandoned) {
    if (GuestAddress) {
      GuestBytes = {42};
      Patches.push_back({GuestAddress, GuestBytes});
    }
  }
  llvm::ArrayRef<BackingPatch> patches() const override { return Patches; }
  ~TestPublication() override {
    if (!Committed)
      ++Abandoned;
  }
  void commit() noexcept override {
    Target.swap(Next);
    ++Commits;
    Committed = true;
  }

private:
  std::vector<std::string> &Target;
  std::vector<std::string> Next;
  unsigned &Commits, &Abandoned;
  std::vector<uint8_t> GuestBytes;
  std::vector<BackingPatch> Patches;
  bool Committed = false;
};
TEST_F(ModuleBindingFiles, AdoptionPublishesMetadataAndPatchesAtomically) {
  const auto I = preexisting();
  llvm::cantFail(Space->map(I.Base, Page, Read));
  const auto Generation = Space->mappingGeneration();
  std::vector<std::string> Published{"original"};
  unsigned Commits = 0, Abandoned = 0, Mode = 0;
  auto Prepare = [&](llvm::ArrayRef<ModuleInstance> Batch)
      -> llvm::Expected<std::unique_ptr<ModulePublication>> {
    if (!Mode)
      return failure();
    EXPECT_EQ(Batch.size(), 1u);
    return std::make_unique<TestPublication>(
        Published, std::vector<std::string>{Batch[0].Binding.ID}, Commits,
        Abandoned, Mode == 1 ? 0x30000 : I.Base);
  };
  auto Proof = [](auto) { return llvm::Error::success(); };
  Bindings = llvm::cantFail(ModuleBindings::create(Sources, 256, Prepare));
  EXPECT_NE(llvm::toString(Bindings->adopt({I}, Proof)), "");
  Mode = 1;
  EXPECT_NE(llvm::toString(Bindings->adopt({I}, Proof)), "");
  EXPECT_EQ(Abandoned, 1u);
  EXPECT_EQ(Commits, 0u);
  EXPECT_TRUE(Bindings->inspect().empty());
  EXPECT_EQ(Bindings->revision(), 0u);
  EXPECT_EQ(Published, std::vector<std::string>{"original"});
  EXPECT_EQ(*Space->readInteger(I.Base, 1), 0u);
  EXPECT_EQ(Space->mappingGeneration(), Generation);
  Mode = 2;
  ASSERT_EQ(llvm::toString(Bindings->adopt({I}, Proof)), "");
  EXPECT_EQ(Published, std::vector<std::string>{"bootstrap"});
  EXPECT_EQ(Commits, 1u);
  EXPECT_EQ(Bindings->revision(), 1u);
  EXPECT_EQ(*Space->readInteger(I.Base, 1), 42u);
  EXPECT_FALSE(*Space->canAccess(I.Base, 1, Write));
  EXPECT_EQ(Space->physicalMemory()->allocatedBytes(), Page);
}
TEST_F(ModuleBindingFiles, MetadataRefreshDoesNotBuildOrFinalizeAnEmptyBatch) {
  llvm::cantFail(Space->map(0x30000, Page, Read));
  std::filesystem::remove(File);
  unsigned Builds = 0, Finalizations = 0, Commits = 0, Abandoned = 0;
  std::vector<std::string> Published;
  Bindings = llvm::cantFail(ModuleBindings::create(
      Sources, 256,
      [&](auto) -> llvm::Expected<std::unique_ptr<ModulePublication>> {
        return std::make_unique<TestPublication>(
            Published, std::vector<std::string>{"updated"}, Commits, Abandoned,
            0x30000);
      },
      [&](auto, auto) {
        ++Finalizations;
        return failure();
      }));
  auto Builder = [&](const auto &B, const auto &I) {
    ++Builds;
    return build(B, I);
  };
  ASSERT_EQ(llvm::toString(Bindings->refreshPublication(Space, Builder)), "");
  EXPECT_EQ(Builds, 0u);
  EXPECT_EQ(Finalizations, 0u);
  EXPECT_EQ(Commits, 1u);
  EXPECT_EQ(*Space->readInteger(0x30000, 1), 42u);
  EXPECT_FALSE(*Space->canAccess(0x30000, 1, Write));
  EXPECT_TRUE(Bindings->inspect().empty());
  EXPECT_FALSE(Sources->prepared("resource"));
  EXPECT_EQ(Bindings->revision(), 1u);
}
TEST_F(ModuleBindingFiles,
       MetadataRefreshExpandsOnlyADeclaredRequiredDependency) {
  std::vector<std::string> Published;
  unsigned Commits = 0, Abandoned = 0;
  Bindings = llvm::cantFail(ModuleBindings::create(
      Sources, 256,
      [&](llvm::ArrayRef<ModuleInstance> Preview)
          -> llvm::Expected<std::unique_ptr<ModulePublication>> {
        const auto Found =
            std::find_if(Preview.begin(), Preview.end(), [](const auto &I) {
              return I.Binding.ID == "one" && I.mapped();
            });
        if (Found == Preview.end())
          return llvm::make_error<ModuleDependencyError>("one.module", "one");
        return std::make_unique<TestPublication>(
            Published, std::vector<std::string>{"one"}, Commits, Abandoned);
      }));
  llvm::cantFail(Bindings->declare(binding()));
  llvm::cantFail(Bindings->declare(binding("unused", 0x20000)));
  EXPECT_FALSE(Sources->prepared("resource"));
  ASSERT_EQ(llvm::toString(Bindings->refreshPublication(Space, build)), "");
  EXPECT_TRUE(Bindings->find("one")->mapped());
  EXPECT_FALSE(Bindings->find("unused")->mapped());
  EXPECT_EQ(Published, std::vector<std::string>{"one"});
  EXPECT_EQ(Commits, 1u);
  EXPECT_EQ(Space->mappedBytes(), Page);
  EXPECT_EQ(Bindings->revision(), 3u);
}
TEST_F(ModuleBindingFiles,
       MetadataRefreshFailureRetainsThePendingDependencyAndRam) {
  Bindings = llvm::cantFail(ModuleBindings::create(
      Sources, 256,
      [](auto) -> llvm::Expected<std::unique_ptr<ModulePublication>> {
        return llvm::make_error<ModuleDependencyError>("one.module", "one");
      }));
  llvm::cantFail(Bindings->declare(binding()));
  std::filesystem::remove(File);
  auto Error = Bindings->refreshPublication(Space, build);
  ASSERT_TRUE(bool(Error));
  EXPECT_TRUE(Error.isA<ModuleDependencyError>());
  llvm::consumeError(std::move(Error));
  EXPECT_EQ(Bindings->revision(), 1u);
  EXPECT_FALSE(Bindings->find("one")->mapped());
  EXPECT_EQ(Space->mappedBytes(), 0u);
  const auto R = llvm::cantFail(
      Bindings->requestDependency(Space, "one.module", 0, "one"));
  EXPECT_NE(llvm::toString(Bindings->refreshPublication(Space, build)), "");
  EXPECT_EQ(Bindings->pendingRequest()->Token, R.Token);
}
TEST_F(ModuleBindingFiles, EnvironmentMetadataSharesTheMemoryCommitBoundary) {
  std::vector<std::string> Published{"original"};
  unsigned Commits = 0, Abandoned = 0, Mode = 0;
  auto Prepare = [&](llvm::ArrayRef<ModuleInstance> Preview)
      -> llvm::Expected<std::unique_ptr<ModulePublication>> {
    if (Mode == 0)
      return failure();
    std::vector<std::string> Names;
    for (const auto &I : Preview)
      if (I.mapped())
        Names.push_back(I.Binding.ID);
    return std::make_unique<TestPublication>(Published, std::move(Names),
                                             Commits, Abandoned);
  };
  Bindings = llvm::cantFail(ModuleBindings::create(Sources, 256, Prepare));
  const auto R =
      llvm::cantFail(Bindings->requestDependency(Space, "one.module"));
  EXPECT_NE(llvm::toString(Bindings->supply(R.Token, {binding()}, build)), "");
  EXPECT_EQ(Published, std::vector<std::string>{"original"});
  EXPECT_EQ(Space->mappedBytes(), 0u);
  Mode = 1;
  ASSERT_EQ(llvm::toString(Space->map(0x10000, Page, Read)), "");
  EXPECT_NE(llvm::toString(Bindings->supply(R.Token, {binding()}, build)), "");
  EXPECT_EQ(Commits, 0u);
  EXPECT_EQ(Abandoned, 1u);
  EXPECT_TRUE(Bindings->inspect().empty());
  EXPECT_EQ(Published, std::vector<std::string>{"original"});
  EXPECT_EQ(Bindings->pendingRequest()->Token, R.Token);
  ASSERT_EQ(llvm::toString(Space->unmap(0x10000, Page)), "");
  ASSERT_EQ(llvm::toString(Bindings->supply(R.Token, {binding()}, build)), "");
  EXPECT_EQ(Published, std::vector<std::string>{"one"});
  EXPECT_EQ(Commits, 1u);
  EXPECT_EQ(Abandoned, 1u);
  EXPECT_TRUE(Bindings->find("one")->mapped());
  EXPECT_EQ(Space->mappedBytes(), Page);
}
TEST_F(ModuleBindingFiles, NullEnvironmentPublicationFailsBeforeMapping) {
  Bindings = llvm::cantFail(ModuleBindings::create(
      Sources, 256,
      [](llvm::ArrayRef<ModuleInstance>)
          -> llvm::Expected<std::unique_ptr<ModulePublication>> {
        return std::unique_ptr<ModulePublication>();
      }));
  EXPECT_NE(llvm::toString(Bindings->materialize({binding()}, build)), "");
  EXPECT_EQ(Space->mappedBytes(), 0u);
  EXPECT_TRUE(Bindings->inspect().empty());
}
TEST_F(ModuleBindingFiles, RuntimeWritesArePrivateToEachInstance) {
  const auto A = binding(), B = binding("second", 0x20000);
  auto Builder =
      [&](const ModuleBinding &B,
          const ModuleImage &I) -> llvm::Expected<ModuleMappingPlan> {
    auto P = build(B, I);
    if (!P)
      return P.takeError();
    P->Image.Regions.front().Permissions |= Write;
    return std::move(*P);
  };
  ASSERT_EQ(llvm::toString(Bindings->materialize({A, B}, Builder)), "");
  ASSERT_EQ(llvm::toString(Space->writeInteger(0x10001, 73, 1)), "");
  EXPECT_EQ(*Space->readInteger(0x10001, 1), 73u);
  EXPECT_EQ(*Space->readInteger(0x20001, 1), 42u);
  EXPECT_EQ(Bindings->find("one")->Source->bytes()[Page + 1], 42u);
}
TEST_F(ModuleBindingFiles, GuestMetadataPatchesCommitWithModuleAndHostTables) {
  ASSERT_EQ(llvm::toString(Space->map(0x30000, Page, Read | Write)), "");
  ASSERT_EQ(llvm::toString(Space->mapAlias(0x40000, 0x30000, Page, Read)), "");
  ASSERT_EQ(llvm::toString(Space->writeInteger(0x30000, 73, 1)), "");
  std::vector<std::string> Published{"original"};
  unsigned Commits = 0, Abandoned = 0;
  auto Prepare = [&](llvm::ArrayRef<ModuleInstance>)
      -> llvm::Expected<std::unique_ptr<ModulePublication>> {
    return std::make_unique<TestPublication>(Published,
                                             std::vector<std::string>{"one"},
                                             Commits, Abandoned, 0x30000);
  };
  Bindings = llvm::cantFail(ModuleBindings::create(Sources, 256, Prepare));
  ASSERT_EQ(llvm::toString(Space->map(0x10000, Page, Read)), "");
  EXPECT_NE(llvm::toString(Bindings->materialize({binding()}, build)), "");
  EXPECT_EQ(*Space->readInteger(0x40000, 1), 73u);
  EXPECT_EQ(Published, std::vector<std::string>{"original"});
  EXPECT_EQ(Commits, 0u);
  EXPECT_EQ(Abandoned, 1u);
  ASSERT_EQ(llvm::toString(Space->unmap(0x10000, Page)), "");
  ASSERT_EQ(llvm::toString(Bindings->materialize({binding()}, build)), "");
  EXPECT_EQ(*Space->readInteger(0x40000, 1), 42u);
  EXPECT_EQ(Published, std::vector<std::string>{"one"});
  EXPECT_EQ(Commits, 1u);
  EXPECT_TRUE(Bindings->find("one")->mapped());
}
TEST_F(ModuleBindingFiles,
       ExportAddressesDoNotMakeOpaqueRuntimeBytesAccessible) {
  auto Builder =
      [&](const ModuleBinding &B,
          const ModuleImage &I) -> llvm::Expected<ModuleMappingPlan> {
    auto P = build(B, I);
    if (!P)
      return P.takeError();
    P->Image.Entry = 0;
    P->Image.Regions.front().Permissions = 0;
    P->Exports.push_back({"opaque_global", 8, *B.Base + 8, SymbolKind::Data});
    return std::move(*P);
  };
  ASSERT_EQ(llvm::toString(Bindings->materialize({binding()}, Builder)), "");
  auto Code = Bindings->lookupExport("one", "probe");
  ASSERT_TRUE(bool(Code));
  ASSERT_TRUE(Code->has_value());
  EXPECT_EQ((**Code).Address, 0x10000u);
  auto Data = Bindings->lookupExport("one", "opaque_global");
  ASSERT_TRUE(bool(Data));
  ASSERT_TRUE(Data->has_value());
  EXPECT_EQ((**Data).Address, 0x10008u);
  EXPECT_FALSE(*Space->canAccess(0x10000, 1, Execute));
  EXPECT_FALSE(*Space->canAccess(0x10008, 1, Read));
}
TEST_F(ModuleBindingFiles, DeviceMemoryIsNotClassifiedAsAMissingModule) {
  GuestMMIOCallbacks Device;
  Device.Validate = [](uint64_t, uint64_t, bool) {
    return llvm::Error::success();
  };
  Device.Read = [](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
    return 7;
  };
  Device.Write = [](uint64_t, unsigned, uint64_t) {
    return llvm::Error::success();
  };
  ASSERT_EQ(llvm::toString(Space->mapMMIO(0x10000, Page, std::move(Device))),
            "");
  auto R = Bindings->requestAddress(Space, 1, 0x10000, 4, ModuleAccess::Read);
  ASSERT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  EXPECT_FALSE(Bindings->pendingRequest());
}
TEST_F(ModuleBindingFiles, ResourceSupplyRequiresEveryRequestedPermissionBit) {
  auto Request = Bindings->requestAddress(Space, 1, 0x10000, 8,
                                          ModuleAccess::Write, Read | Write);
  ASSERT_TRUE(bool(Request));
  EXPECT_EQ(Request->RequiredPermissions, Read | Write);
  unsigned Rights = Write;
  auto Builder =
      [&](const ModuleBinding &B,
          const ModuleImage &I) -> llvm::Expected<ModuleMappingPlan> {
    auto Plan = build(B, I);
    if (!Plan)
      return Plan.takeError();
    Plan->Image.Entry = 0;
    Plan->Image.Regions.front().Permissions = Rights;
    return std::move(*Plan);
  };
  EXPECT_NE(
      llvm::toString(Bindings->supply(Request->Token, {binding()}, Builder)),
      "");
  EXPECT_EQ(Space->mappedBytes(), 0u);
  EXPECT_EQ(Bindings->pendingRequest()->Token, Request->Token);
  Rights = Read | Write;
  ASSERT_EQ(
      llvm::toString(Bindings->supply(Request->Token, {binding()}, Builder)),
      "");
  EXPECT_TRUE(*Space->canAccess(0x10000, 8, Read | Write));
}
TEST_F(ModuleBindingFiles, ResourceSupplyPreservesRequiredPrivilegeMetadata) {
  auto Request = Bindings->requestAddress(
      Space, 1, 0x10000, 8, ModuleAccess::Read, Read | UserAccessible);
  ASSERT_TRUE(bool(Request));
  EXPECT_NE(
      llvm::toString(Bindings->supply(Request->Token, {binding()}, build)), "");
  EXPECT_EQ(Space->mappedBytes(), 0u);
  auto Builder =
      [&](const ModuleBinding &B,
          const ModuleImage &I) -> llvm::Expected<ModuleMappingPlan> {
    auto Plan = build(B, I);
    if (!Plan)
      return Plan.takeError();
    Plan->Image.Regions.front().Permissions |= UserAccessible;
    return std::move(*Plan);
  };
  ASSERT_EQ(
      llvm::toString(Bindings->supply(Request->Token, {binding()}, Builder)),
      "");
  EXPECT_TRUE(*Space->canAccess(0x10000, 8, Read | UserAccessible));
}
TEST_F(ModuleBindingFiles,
       FinalizerSeesAllDraftsBeforeAnyRamOrInstancePublication) {
  unsigned Calls = 0;
  auto Finalize =
      [&](llvm::ArrayRef<ModuleInstance> Existing,
          llvm::ArrayRef<ModuleMappingCandidate> Drafts) -> llvm::Error {
    ++Calls;
    EXPECT_TRUE(Existing.empty());
    EXPECT_EQ(Drafts.size(), 2u);
    EXPECT_EQ(Space->mappedBytes(), 0u);
    for (const auto &D : Drafts)
      D.Plan.Image.Regions.front().Bytes[1] = 73;
    return llvm::Error::success();
  };
  Bindings = llvm::cantFail(ModuleBindings::create(Sources, 256, {}, Finalize));
  ASSERT_EQ(llvm::toString(Bindings->materialize(
                {binding(), binding("two", 0x20000)}, build)),
            "");
  EXPECT_EQ(Calls, 1u);
  EXPECT_EQ(*Space->readInteger(0x10001, 1), 73u);
  EXPECT_EQ(*Space->readInteger(0x20001, 1), 73u);
  EXPECT_EQ(Sources->prepared("resource")->bytes()[Page + 1], 42u);
}
TEST_F(ModuleBindingFiles,
       FailedFinalizationDiscardsDraftWritesAndKeepsTheToken) {
  auto Finalize =
      [&](llvm::ArrayRef<ModuleInstance>,
          llvm::ArrayRef<ModuleMappingCandidate> Drafts) -> llvm::Error {
    Drafts.front().Plan.Image.Regions.front().Bytes[1] = 73;
    return failure();
  };
  Bindings = llvm::cantFail(ModuleBindings::create(Sources, 256, {}, Finalize));
  auto R = Bindings->requestAddress(Space, 1, 0x10000, 1, ModuleAccess::Fetch);
  ASSERT_TRUE(bool(R));
  const auto Revision = Bindings->revision();
  EXPECT_NE(llvm::toString(Bindings->supply(R->Token, {binding()}, build)), "");
  EXPECT_EQ(Space->mappedBytes(), 0u);
  EXPECT_TRUE(Bindings->inspect().empty());
  EXPECT_EQ(Bindings->pendingRequest()->Token, R->Token);
  EXPECT_EQ(Bindings->revision(), Revision);
  EXPECT_EQ(Sources->prepared("resource")->bytes()[Page + 1], 42u);
}
TEST_F(ModuleBindingFiles,
       DeclaredDependencyExpandsOneAtomicBatchAndSupportsForwarding) {
  unsigned Calls = 0;
  auto Finalize =
      [&](llvm::ArrayRef<ModuleInstance>,
          llvm::ArrayRef<ModuleMappingCandidate> Drafts) -> llvm::Error {
    ++Calls;
    EXPECT_EQ(Space->mappedBytes(), 0u);
    if (Drafts.size() == 1)
      return llvm::make_error<ModuleDependencyError>("two.module", "two");
    Drafts.front().Plan.Exports.push_back(
        {"forward", 8, 0x20000, SymbolKind::Function, true});
    return llvm::Error::success();
  };
  Bindings = llvm::cantFail(ModuleBindings::create(Sources, 256, {}, Finalize));
  ASSERT_EQ(llvm::toString(Bindings->declare(binding("two", 0x20000))), "");
  ASSERT_EQ(llvm::toString(Bindings->materialize({binding()}, build)), "");
  EXPECT_EQ(Calls, 2u);
  EXPECT_EQ(Space->mappedBytes(), 2 * Page);
  EXPECT_TRUE(Bindings->find("one")->mapped());
  EXPECT_TRUE(Bindings->find("two")->mapped());
  EXPECT_EQ(Bindings->find("two")->Load, LoadPolicy::Lazy);
  auto E = Bindings->lookupExport("one", "forward");
  ASSERT_TRUE(bool(E));
  ASSERT_TRUE(*E);
  EXPECT_EQ((**E).Address, 0x20000u);
  EXPECT_TRUE((**E).Forwarded);
}
TEST_F(ModuleBindingFiles, UndeclaredDependencyDoesNotInventAFileOrBinding) {
  auto Finalize = [&](llvm::ArrayRef<ModuleInstance>,
                      llvm::ArrayRef<ModuleMappingCandidate>) -> llvm::Error {
    return llvm::make_error<ModuleDependencyError>("not-provided.module",
                                                   "not-declared");
  };
  Bindings = llvm::cantFail(ModuleBindings::create(Sources, 256, {}, Finalize));
  auto E = Bindings->materialize({binding()}, build);
  bool Missing = false;
  auto Other =
      llvm::handleErrors(std::move(E), [&](const ModuleDependencyError &D) {
        Missing = true;
        EXPECT_EQ(D.Dependency, "not-provided.module");
      });
  EXPECT_TRUE(Missing);
  EXPECT_EQ(llvm::toString(std::move(Other)), "");
  EXPECT_EQ(Space->mappedBytes(), 0u);
  EXPECT_TRUE(Bindings->inspect().empty());
}
struct TestModulePreparation final : ModuleMappingPreparation {
  const void *typeID() const override {
    static char ID = 0;
    return &ID;
  }
};
TEST_F(ModuleBindingFiles, UnconsumedPreparationCannotPublishAnUnlinkedImage) {
  for (bool WithFinalizer : {false, true}) {
    ModuleMappingFinalizer Finalize;
    if (WithFinalizer)
      Finalize = [](llvm::ArrayRef<ModuleInstance>,
                    llvm::ArrayRef<ModuleMappingCandidate>) {
        return llvm::Error::success();
      };
    Bindings =
        llvm::cantFail(ModuleBindings::create(Sources, 256, {}, Finalize));
    auto Builder =
        [&](const ModuleBinding &B,
            const ModuleImage &I) -> llvm::Expected<ModuleMappingPlan> {
      auto P = build(B, I);
      if (!P)
        return P.takeError();
      P->Preparation = std::make_shared<TestModulePreparation>();
      return std::move(*P);
    };
    EXPECT_NE(llvm::toString(Bindings->materialize({binding()}, Builder)), "");
    EXPECT_EQ(Space->mappedBytes(), 0u);
    EXPECT_TRUE(Bindings->inspect().empty());
  }
}
TEST_F(ModuleBindingFiles,
       ForwardedMetadataRequiresAnEnvironmentAndMappedTarget) {
  auto Builder =
      [&](const ModuleBinding &B,
          const ModuleImage &I) -> llvm::Expected<ModuleMappingPlan> {
    auto P = build(B, I);
    if (!P)
      return P.takeError();
    P->Exports.push_back({"forward", 8, 0x20000, SymbolKind::Function, true});
    return std::move(*P);
  };
  EXPECT_NE(llvm::toString(Bindings->materialize({binding()}, Builder)), "");
  auto Finalize = [](llvm::ArrayRef<ModuleInstance>,
                     llvm::ArrayRef<ModuleMappingCandidate>) {
    return llvm::Error::success();
  };
  Bindings = llvm::cantFail(ModuleBindings::create(Sources, 256, {}, Finalize));
  EXPECT_NE(llvm::toString(Bindings->materialize({binding()}, Builder)), "");
  EXPECT_EQ(Space->mappedBytes(), 0u);
}
TEST_F(ModuleBindingFiles, OversizedBatchFailsBeforeFinalizationOrPublication) {
  unsigned Finalizations = 0;
  auto Finalize = [&](llvm::ArrayRef<ModuleInstance>,
                      llvm::ArrayRef<ModuleMappingCandidate>) {
    ++Finalizations;
    return llvm::Error::success();
  };
  Bindings = llvm::cantFail(ModuleBindings::create(Sources, 256, {}, Finalize));
  std::vector<ModuleBinding> Batch;
  for (unsigned N = 0; N < 17; ++N)
    Batch.push_back(
        binding("module" + std::to_string(N), 0x10000 + N * 0x10000));
  auto Error = llvm::toString(Bindings->materialize(Batch, build));
  EXPECT_NE(Error.find("batch mapped-byte budget"), std::string::npos);
  EXPECT_EQ(Finalizations, 0u);
  EXPECT_EQ(Space->mappedBytes(), 0u);
  EXPECT_TRUE(Bindings->inspect().empty());
}
} // namespace
