//===- PDBIdentityTests.cpp - PE/PDB identity policy tests ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/debug/DebugInfoDiscovery.h"
#include "neverd/debug/PDBLoader.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFLoaderUtils.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/DebugInfo/CodeView/CVRecord.h"
#include "llvm/DebugInfo/CodeView/CodeView.h"
#include "llvm/DebugInfo/MSF/MSFCommon.h"
#include "llvm/DebugInfo/PDB/Native/RawConstants.h"
#include "llvm/Object/CVDebugRecord.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/BinaryByteStream.h"
#include "llvm/Support/BinaryStreamRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace neverd;

namespace {

PDBBuildIdentity makeIdentity(uint8_t Seed, uint32_t Age = 1) {
  PDBBuildIdentity Identity;
  for (size_t I = 0; I < Identity.Guid.size(); ++I)
    Identity.Guid[I] = static_cast<uint8_t>(Seed + I);
  Identity.Age = Age;
  return Identity;
}

std::vector<uint8_t> makeRSDS(const PDBBuildIdentity &Identity,
                              llvm::StringRef Path = "fixture.pdb") {
  llvm::codeview::PDB70DebugInfo Header{};
  Header.CVSignature = static_cast<uint32_t>(llvm::OMF::Signature::PDB70);
  std::copy(Identity.Guid.begin(), Identity.Guid.end(), Header.Signature);
  Header.Age = Identity.Age;

  std::vector<uint8_t> Bytes(sizeof(Header) + Path.size() + 1, 0);
  std::memcpy(Bytes.data(), &Header, sizeof(Header));
  std::memcpy(Bytes.data() + sizeof(Header), Path.data(), Path.size());
  return Bytes;
}

std::vector<uint8_t> makeMinimalCVSymbols(size_t Count) {
  const llvm::codeview::RecordPrefix Prefix(
      static_cast<uint16_t>(llvm::codeview::SymbolKind::S_END));
  std::vector<uint8_t> Bytes(Count * sizeof(Prefix));
  for (size_t I = 0; I < Count; ++I)
    std::memcpy(Bytes.data() + I * sizeof(Prefix), &Prefix, sizeof(Prefix));
  return Bytes;
}

std::filesystem::path safetyFixture(llvm::StringRef Name) {
  return std::filesystem::path(__FILE__).parent_path().parent_path() /
         "safety" / "fixtures" / "binaries" / Name.str();
}

llvm::Expected<BinaryImage> loadPEFixture(llvm::StringRef Name) {
  std::unique_ptr<Loader> ImageLoader = Loader::create(BinaryFormat::COFF);
  if (!ImageLoader)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "COFF loader is unavailable");
  return ImageLoader->load(safetyFixture(Name));
}

class ScratchDir {
public:
  ScratchDir() {
    llvm::SmallString<128> Created;
    std::error_code EC =
        llvm::sys::fs::createUniqueDirectory("neverd-pdb-identity", Created);
    EXPECT_FALSE(EC) << EC.message();
    Root = std::filesystem::path(Created.str().str());
  }

  ~ScratchDir() {
    if (!Root.empty())
      llvm::sys::fs::remove_directories(Root.string());
  }

  std::filesystem::path path(llvm::StringRef Name) const {
    return Root / Name.str();
  }

private:
  std::filesystem::path Root;
};

TEST(CodeViewRSDSParser, RecoversRawGuidAgeAndBoundedPath) {
  const PDBBuildIdentity Expected = makeIdentity(0x10, 7);
  std::vector<uint8_t> Bytes = makeRSDS(Expected, R"(C:\symbols\fixture.pdb)");

  auto Parsed = coff_loader::detail::parseCodeViewRSDS(Bytes);
  ASSERT_TRUE(static_cast<bool>(Parsed)) << llvm::toString(Parsed.takeError());
  EXPECT_EQ(Parsed->Identity, Expected);
  EXPECT_EQ(Parsed->Path, R"(C:\symbols\fixture.pdb)");
}

TEST(CodeViewRSDSParser, RejectsUnknownTruncatedAndUnterminatedRecords) {
  const PDBBuildIdentity Identity = makeIdentity(0x20);

  std::vector<uint8_t> Unknown = makeRSDS(Identity);
  Unknown[0] ^= 0xff;
  auto UnknownResult = coff_loader::detail::parseCodeViewRSDS(Unknown);
  EXPECT_FALSE(static_cast<bool>(UnknownResult));
  llvm::consumeError(UnknownResult.takeError());

  std::vector<uint8_t> Truncated(sizeof(llvm::codeview::PDB70DebugInfo) - 1, 0);
  auto TruncatedResult = coff_loader::detail::parseCodeViewRSDS(Truncated);
  EXPECT_FALSE(static_cast<bool>(TruncatedResult));
  llvm::consumeError(TruncatedResult.takeError());

  std::vector<uint8_t> Unterminated = makeRSDS(Identity);
  Unterminated.back() = 'x';
  auto UnterminatedResult =
      coff_loader::detail::parseCodeViewRSDS(Unterminated);
  EXPECT_FALSE(static_cast<bool>(UnterminatedResult));
  llvm::consumeError(UnterminatedResult.takeError());
}

TEST(CodeViewRSDSParser, RejectsZeroGuidAndZeroAge) {
  PDBBuildIdentity ZeroGuid;
  ZeroGuid.Age = 1;
  auto ZeroGuidResult =
      coff_loader::detail::parseCodeViewRSDS(makeRSDS(ZeroGuid));
  EXPECT_FALSE(static_cast<bool>(ZeroGuidResult));
  llvm::consumeError(ZeroGuidResult.takeError());

  PDBBuildIdentity ZeroAge = makeIdentity(0x30, 0);
  auto ZeroAgeResult =
      coff_loader::detail::parseCodeViewRSDS(makeRSDS(ZeroAge));
  EXPECT_FALSE(static_cast<bool>(ZeroAgeResult));
  llvm::consumeError(ZeroAgeResult.takeError());
}

TEST(CodeViewIdentityRegistry, IdenticalDuplicatesRemainUnique) {
  const PDBBuildIdentity Identity = makeIdentity(0x40, 2);
  coff_loader::detail::CodeViewIdentityRegistry Registry;
  Registry.observe({Identity, "z.pdb"});
  Registry.observe({Identity, "a.pdb"});

  ASSERT_EQ(Registry.state(), PDBIdentityState::Unique);
  ASSERT_TRUE(Registry.identity().has_value());
  EXPECT_EQ(*Registry.identity(), Identity);
  EXPECT_EQ(Registry.path(), "a.pdb");
}

TEST(CodeViewIdentityRegistry, EmptyRegistryHasNoIdentity) {
  coff_loader::detail::CodeViewIdentityRegistry Registry;
  EXPECT_EQ(Registry.state(), PDBIdentityState::Absent);
  EXPECT_FALSE(Registry.identity().has_value());
}

TEST(CodeViewIdentityRegistry, ConflictIsStickyAndOrderIndependent) {
  const coff_loader::detail::CodeViewRSDSRecord First{makeIdentity(0x50, 3),
                                                      "first.pdb"};
  const coff_loader::detail::CodeViewRSDSRecord Second{makeIdentity(0x60, 3),
                                                       "second.pdb"};
  for (const bool Reverse : {false, true}) {
    SCOPED_TRACE(Reverse);
    coff_loader::detail::CodeViewIdentityRegistry Registry;
    Registry.observe(Reverse ? Second : First);
    Registry.observe(Reverse ? First : Second);
    Registry.observe(Reverse ? Second : First);
    EXPECT_EQ(Registry.state(), PDBIdentityState::Ambiguous);
    EXPECT_FALSE(Registry.identity().has_value());
  }
}

TEST(CodeViewIdentityRegistry, SameGuidWithDifferentAgeIsAmbiguous) {
  const PDBBuildIdentity First = makeIdentity(0x68, 1);
  PDBBuildIdentity Second = First;
  Second.Age = 2;

  coff_loader::detail::CodeViewIdentityRegistry Registry;
  Registry.observe({First, "same.pdb"});
  Registry.observe({Second, "same.pdb"});
  EXPECT_EQ(Registry.state(), PDBIdentityState::Ambiguous);
  EXPECT_FALSE(Registry.identity().has_value());
}

TEST(CodeViewIdentityRegistry, MalformedObservationIsStickyInEitherOrder) {
  const coff_loader::detail::CodeViewRSDSRecord Valid{makeIdentity(0x70),
                                                      "valid.pdb"};
  for (const bool MalformedFirst : {false, true}) {
    SCOPED_TRACE(MalformedFirst);
    coff_loader::detail::CodeViewIdentityRegistry Registry;
    if (MalformedFirst)
      Registry.observeMalformed();
    Registry.observe(Valid);
    if (!MalformedFirst)
      Registry.observeMalformed();
    EXPECT_EQ(Registry.state(), PDBIdentityState::Ambiguous);
    EXPECT_FALSE(Registry.identity().has_value());
  }
}

TEST(COFFRawBackedRangeResolver, MapsOneFullRangeToItsExactFileOffset) {
  const coff_loader::detail::RawBackedSectionRange Section{
      /*RVA=*/0x1000, /*VirtualSize=*/0x80, /*FileOffset=*/0x20,
      /*RawSize=*/0x60};
  auto Offset = coff_loader::detail::resolveUniqueRawBackedFileOffset(
      llvm::ArrayRef(Section), /*FileSize=*/0x100, /*RVA=*/0x1010,
      /*Size=*/0x10);
  ASSERT_TRUE(static_cast<bool>(Offset)) << llvm::toString(Offset.takeError());
  EXPECT_EQ(*Offset, 0x30u);
}

TEST(COFFRawBackedRangeResolver, RejectsDirectoryCrossingRawTail) {
  const coff_loader::detail::RawBackedSectionRange Section{
      /*RVA=*/0x1000, /*VirtualSize=*/0x80, /*FileOffset=*/0x20,
      /*RawSize=*/0x20};
  auto Offset = coff_loader::detail::resolveUniqueRawBackedFileOffset(
      llvm::ArrayRef(Section), /*FileSize=*/0x100, /*RVA=*/0x1018,
      /*Size=*/0x10);
  EXPECT_FALSE(static_cast<bool>(Offset));
  llvm::consumeError(Offset.takeError());
}

TEST(COFFRawBackedRangeResolver, RejectsPayloadCrossingVirtualBoundary) {
  const coff_loader::detail::RawBackedSectionRange Section{
      /*RVA=*/0x1000, /*VirtualSize=*/0x20, /*FileOffset=*/0x20,
      /*RawSize=*/0x60};
  auto Offset = coff_loader::detail::resolveUniqueRawBackedFileOffset(
      llvm::ArrayRef(Section), /*FileSize=*/0x100, /*RVA=*/0x1018,
      /*Size=*/0x10);
  EXPECT_FALSE(static_cast<bool>(Offset));
  llvm::consumeError(Offset.takeError());
}

TEST(COFFRawBackedRangeResolver, RejectsTruncatedDeclaredRawSection) {
  const coff_loader::detail::RawBackedSectionRange Section{
      /*RVA=*/0x1000, /*VirtualSize=*/0x20, /*FileOffset=*/0x50,
      /*RawSize=*/0x20};
  auto Offset = coff_loader::detail::resolveUniqueRawBackedFileOffset(
      llvm::ArrayRef(Section), /*FileSize=*/0x60, /*RVA=*/0x1000,
      /*Size=*/0x10);
  EXPECT_FALSE(static_cast<bool>(Offset));
  llvm::consumeError(Offset.takeError());
}

TEST(COFFRawBackedRangeResolver, RejectsOverlappingVirtualOwners) {
  const std::array<coff_loader::detail::RawBackedSectionRange, 2> Sections{{
      {/*RVA=*/0x1000, /*VirtualSize=*/0x80, /*FileOffset=*/0x20,
       /*RawSize=*/0x80},
      {/*RVA=*/0x1040, /*VirtualSize=*/0x80, /*FileOffset=*/0xa0,
       /*RawSize=*/0x80},
  }};
  auto Offset = coff_loader::detail::resolveUniqueRawBackedFileOffset(
      Sections, /*FileSize=*/0x140, /*RVA=*/0x1050, /*Size=*/0x10);
  EXPECT_FALSE(static_cast<bool>(Offset));
  llvm::consumeError(Offset.takeError());
}

TEST(CodeViewPayloadResolver, RequiresRVAAndRawToNameTheSameFileOffset) {
  const coff_loader::detail::RawBackedSectionRange Section{
      /*RVA=*/0x1000, /*VirtualSize=*/0x80, /*FileOffset=*/0x20,
      /*RawSize=*/0x80};
  std::vector<uint8_t> File(0xa0, 0x5a);

  auto SameContentDifferentOffset = coff_loader::detail::resolveCodeViewPayload(
      File, llvm::ArrayRef(Section), /*RVA=*/0x1010,
      /*RawFileOffset=*/0x40, /*Size=*/0x10);
  EXPECT_FALSE(static_cast<bool>(SameContentDifferentOffset));
  llvm::consumeError(SameContentDifferentOffset.takeError());

  std::fill(File.begin() + 0x40, File.begin() + 0x50, 0xa5);
  auto ConflictingContent = coff_loader::detail::resolveCodeViewPayload(
      File, llvm::ArrayRef(Section), /*RVA=*/0x1010,
      /*RawFileOffset=*/0x40, /*Size=*/0x10);
  EXPECT_FALSE(static_cast<bool>(ConflictingContent));
  llvm::consumeError(ConflictingContent.takeError());

  auto ExactOffset = coff_loader::detail::resolveCodeViewPayload(
      File, llvm::ArrayRef(Section), /*RVA=*/0x1010,
      /*RawFileOffset=*/0x30, /*Size=*/0x10);
  ASSERT_TRUE(static_cast<bool>(ExactOffset))
      << llvm::toString(ExactOffset.takeError());
  EXPECT_EQ(ExactOffset->data(), File.data() + 0x30);
}

TEST(PDBSymbolRecordIndex, AcceptsOnlyExactRecordStarts) {
  std::vector<uint8_t> Bytes = makeMinimalCVSymbols(2);
  llvm::BinaryByteStream Stream(Bytes, llvm::endianness::little);
  llvm::codeview::CVSymbolArray Records{llvm::BinaryStreamRef(Stream)};

  auto Indexed = pdb_loader_detail::indexSymbolRecords(Records);
  ASSERT_TRUE(static_cast<bool>(Indexed))
      << llvm::toString(Indexed.takeError());
  ASSERT_EQ(Indexed->size(), 2u);
  EXPECT_EQ((*Indexed)[0].Offset, 0u);
  EXPECT_EQ((*Indexed)[1].Offset, sizeof(llvm::codeview::RecordPrefix));
  EXPECT_NE(pdb_loader_detail::findSymbolAtExactOffset(*Indexed, 0), nullptr);
  EXPECT_EQ(pdb_loader_detail::findSymbolAtExactOffset(*Indexed, 1), nullptr);
}

TEST(PDBSymbolRecordIndex, TrailingMalformedRecordRejectsPartialIndex) {
  std::vector<uint8_t> Bytes = makeMinimalCVSymbols(1);
  Bytes.push_back(0xff);
  llvm::BinaryByteStream Stream(Bytes, llvm::endianness::little);
  llvm::codeview::CVSymbolArray Records{llvm::BinaryStreamRef(Stream)};

  auto Indexed = pdb_loader_detail::indexSymbolRecords(Records);
  EXPECT_FALSE(static_cast<bool>(Indexed));
  llvm::consumeError(Indexed.takeError());
}

TEST(PDBFunctionNameRegistry,
     ConflictingNamesAtOneAddressAreStickyAndOrderIndependent) {
  for (const bool Reverse : {false, true}) {
    SCOPED_TRACE(Reverse);
    pdb_loader_detail::FunctionNameRegistry Registry;
    Registry.observe(0x140001000, Reverse ? "guarded_free" : "leaks_memory");
    Registry.observe(0x140001000, Reverse ? "leaks_memory" : "guarded_free");
    Registry.observe(0x140001000, Reverse ? "guarded_free" : "leaks_memory");

    EXPECT_EQ(Registry.state(0x140001000),
              pdb_loader_detail::FunctionNameState::Ambiguous);
    EXPECT_FALSE(Registry.name(0x140001000).has_value());
  }
}

TEST(PDBFunctionNameRegistry, IdenticalNamesRemainUnique) {
  pdb_loader_detail::FunctionNameRegistry Registry;
  Registry.observe(0x140001000, "leaks_memory");
  Registry.observe(0x140001000, "leaks_memory");

  EXPECT_EQ(Registry.state(0x140001000),
            pdb_loader_detail::FunctionNameState::Unique);
  ASSERT_TRUE(Registry.name(0x140001000).has_value());
  EXPECT_EQ(*Registry.name(0x140001000), "leaks_memory");
}

TEST(PDBIdentityIntegration, MatchingFixtureAuthenticatesNamesButNotExtents) {
  auto ImageOr = loadPEFixture("safety_cases_pe_x64.exe");
  ASSERT_TRUE(static_cast<bool>(ImageOr))
      << llvm::toString(ImageOr.takeError());
  ASSERT_EQ(ImageOr->DynInfo.CodeViewPDBIdentityState,
            PDBIdentityState::Unique);
  ASSERT_TRUE(ImageOr->DynInfo.CodeViewPDBIdentity.has_value());

  auto ContextOr =
      PDBDebugContext::load(safetyFixture("safety_cases_pe_x64.pdb"), *ImageOr);
  ASSERT_TRUE(static_cast<bool>(ContextOr))
      << llvm::toString(ContextOr.takeError());
  ASSERT_NE(*ContextOr, nullptr);
  EXPECT_TRUE((*ContextOr)->hasAuthenticatedImageIdentity());
  EXPECT_FALSE((*ContextOr)->hasExactObjectMetadataPrerequisites());
  EXPECT_FALSE((*ContextOr)->hasAuthenticatedFunctionSignatures());
  EXPECT_FALSE((*ContextOr)->hasAuthenticatedObjectExtents());
  EXPECT_TRUE((*ContextOr)->hasInfo());
  const std::vector<FunctionSym> Functions = (*ContextOr)->allFunctions();
  ASSERT_FALSE(Functions.empty());
  for (const FunctionSym &Function : Functions) {
    EXPECT_EQ(Function.Size, 0u)
        << "Phase A must not publish unauthenticated PDB code extents";
    EXPECT_FALSE(Function.ReturnType)
        << "Phase A must not consume an unvalidated PDB type graph";
  }
}

TEST(PDBIdentityIntegration, CrossArchitectureFixtureIsRejectedByGuidAndAge) {
  auto ImageOr = loadPEFixture("safety_cases_pe_x64.exe");
  ASSERT_TRUE(static_cast<bool>(ImageOr))
      << llvm::toString(ImageOr.takeError());

  auto ContextOr = PDBDebugContext::load(
      safetyFixture("safety_cases_pe_arm64.pdb"), *ImageOr);
  ASSERT_FALSE(static_cast<bool>(ContextOr));
  const std::string Error = llvm::toString(ContextOr.takeError());
  EXPECT_NE(Error.find("GUID/age does not match"), std::string::npos) << Error;
}

TEST(PDBIdentityIntegration, EitherGuidOrAgeMismatchRejectsTheCompanion) {
  auto ImageOr = loadPEFixture("safety_cases_pe_x64.exe");
  ASSERT_TRUE(static_cast<bool>(ImageOr))
      << llvm::toString(ImageOr.takeError());
  ASSERT_TRUE(ImageOr->DynInfo.CodeViewPDBIdentity.has_value());

  for (const bool ChangeAge : {false, true}) {
    SCOPED_TRACE(ChangeAge);
    BinaryImage Mutated = *ImageOr;
    if (ChangeAge)
      ++Mutated.DynInfo.CodeViewPDBIdentity->Age;
    else
      Mutated.DynInfo.CodeViewPDBIdentity->Guid.front() ^= 1;

    auto ContextOr = PDBDebugContext::load(
        safetyFixture("safety_cases_pe_x64.pdb"), Mutated);
    ASSERT_FALSE(static_cast<bool>(ContextOr));
    const std::string Error = llvm::toString(ContextOr.takeError());
    EXPECT_NE(Error.find("GUID/age does not match"), std::string::npos)
        << Error;
  }
}

TEST(PDBIdentityIntegration, ImageWithoutUniqueRSDSCannotUsePDBNames) {
  BinaryImage Image;
  Image.Format = BinaryFormat::COFF;
  Image.Arch = Arch::X64;

  auto ContextOr =
      PDBDebugContext::load(safetyFixture("safety_cases_pe_x64.pdb"), Image);
  ASSERT_FALSE(static_cast<bool>(ContextOr));
  const std::string Error = llvm::toString(ContextOr.takeError());
  EXPECT_NE(Error.find("no unique CodeView identity"), std::string::npos)
      << Error;
}

TEST(PDBIdentityIntegration, MatchingIdentityStillRequiresMachineAgreement) {
  auto ImageOr = loadPEFixture("safety_cases_pe_x64.exe");
  ASSERT_TRUE(static_cast<bool>(ImageOr))
      << llvm::toString(ImageOr.takeError());
  ImageOr->Arch = Arch::AArch64;

  auto ContextOr =
      PDBDebugContext::load(safetyFixture("safety_cases_pe_x64.pdb"), *ImageOr);
  ASSERT_FALSE(static_cast<bool>(ContextOr));
  const std::string Error = llvm::toString(ContextOr.takeError());
  EXPECT_NE(Error.find("machine does not match"), std::string::npos) << Error;
}

TEST(PDBIdentityIntegration, MatchingIdentityStillRequiresSectionAgreement) {
  auto ImageOr = loadPEFixture("safety_cases_pe_x64.exe");
  ASSERT_TRUE(static_cast<bool>(ImageOr))
      << llvm::toString(ImageOr.takeError());
  ASSERT_FALSE(ImageOr->Sections.empty());
  ++ImageOr->Sections.front().Size;

  auto ContextOr =
      PDBDebugContext::load(safetyFixture("safety_cases_pe_x64.pdb"), *ImageOr);
  ASSERT_FALSE(static_cast<bool>(ContextOr));
  const std::string Error = llvm::toString(ContextOr.takeError());
  EXPECT_NE(Error.find("section table does not match"), std::string::npos)
      << Error;
}

TEST(PDBIdentityDiscovery, ExplicitMismatchedPDBReportsIdentityError) {
  auto ImageOr = loadPEFixture("safety_cases_pe_x64.exe");
  ASSERT_TRUE(static_cast<bool>(ImageOr))
      << llvm::toString(ImageOr.takeError());

  DebugInfoRequest Request;
  Request.PDBPath = safetyFixture("safety_cases_pe_arm64.pdb");
  DebugInfoResult Result = loadDebugInfo(
      safetyFixture("safety_cases_pe_x64.exe"), *ImageOr, Request);
  EXPECT_FALSE(static_cast<bool>(Result));
  EXPECT_NE(Result.Error.find("GUID/age does not match"), std::string::npos)
      << Result.Error;
}

PDBBuildIdentity makeNB10Identity(uint32_t Signature, uint32_t Age = 1) {
  PDBBuildIdentity Identity;
  Identity.Kind = PDBIdentityKind::NB10;
  Identity.Signature = Signature;
  Identity.Age = Age;
  return Identity;
}

std::vector<uint8_t> makeNB10(const PDBBuildIdentity &Identity,
                              llvm::StringRef Path = "legacy.pdb") {
  llvm::codeview::PDB20DebugInfo Header{};
  Header.CVSignature = static_cast<uint32_t>(llvm::OMF::Signature::PDB20);
  Header.Offset = 0;
  Header.Signature = Identity.Signature;
  Header.Age = Identity.Age;
  std::vector<uint8_t> Bytes(sizeof(Header) + Path.size() + 1, 0);
  std::memcpy(Bytes.data(), &Header, sizeof(Header));
  std::memcpy(Bytes.data() + sizeof(Header), Path.data(), Path.size());
  return Bytes;
}

void appendU16(std::vector<uint8_t> &Out, uint16_t Value) {
  const uint8_t Bytes[2] = {static_cast<uint8_t>(Value),
                            static_cast<uint8_t>(Value >> 8)};
  Out.insert(Out.end(), Bytes, Bytes + 2);
}

void appendU32(std::vector<uint8_t> &Out, uint32_t Value) {
  const uint8_t Bytes[4] = {
      static_cast<uint8_t>(Value), static_cast<uint8_t>(Value >> 8),
      static_cast<uint8_t>(Value >> 16), static_cast<uint8_t>(Value >> 24)};
  Out.insert(Out.end(), Bytes, Bytes + 4);
}

uint32_t pagesFor(uint32_t Bytes, uint32_t BlockSize) {
  return Bytes == 0 ? 0 : (Bytes + BlockSize - 1) / BlockSize;
}

std::vector<uint8_t> writeJgPdb(llvm::ArrayRef<std::vector<uint8_t>> Streams,
                                uint32_t BlockSize = 1024) {
  std::vector<uint32_t> PageCounts;
  PageCounts.reserve(Streams.size());
  uint32_t RootSize = 4 + static_cast<uint32_t>(Streams.size()) * 8;
  for (const std::vector<uint8_t> &Stream : Streams) {
    const uint32_t Count = pagesFor(static_cast<uint32_t>(Stream.size()), BlockSize);
    PageCounts.push_back(Count);
    RootSize += Count * 2;
  }
  const uint32_t RootPages = pagesFor(RootSize, BlockSize);
  EXPECT_LE(60u + RootPages * 2, BlockSize);

  uint16_t NextPage = 1;
  std::vector<std::vector<uint16_t>> StreamPages;
  for (uint32_t Count : PageCounts) {
    std::vector<uint16_t> Pages;
    for (uint32_t I = 0; I < Count; ++I)
      Pages.push_back(NextPage++);
    StreamPages.push_back(std::move(Pages));
  }
  std::vector<uint16_t> RootPageList;
  for (uint32_t I = 0; I < RootPages; ++I)
    RootPageList.push_back(NextPage++);
  const uint16_t NumFilePages = NextPage;

  std::vector<uint8_t> Root;
  appendU16(Root, static_cast<uint16_t>(Streams.size()));
  appendU16(Root, 0);
  for (const std::vector<uint8_t> &Stream : Streams) {
    appendU32(Root, static_cast<uint32_t>(Stream.size()));
    appendU32(Root, 0);
  }
  for (const std::vector<uint16_t> &Pages : StreamPages)
    for (uint16_t Page : Pages)
      appendU16(Root, Page);

  std::vector<uint8_t> File(static_cast<size_t>(NumFilePages) * BlockSize, 0);
  std::memcpy(File.data(), llvm::msf::MagicJG, sizeof(llvm::msf::MagicJG));
  std::memcpy(File.data() + 44, &BlockSize, 4);
  const uint16_t FreePage = 1;
  std::memcpy(File.data() + 48, &FreePage, 2);
  std::memcpy(File.data() + 50, &NumFilePages, 2);
  std::memcpy(File.data() + 52, &RootSize, 4);
  for (size_t I = 0; I < RootPageList.size(); ++I)
    std::memcpy(File.data() + 60 + I * 2, &RootPageList[I], 2);

  for (size_t S = 0; S < Streams.size(); ++S) {
    const std::vector<uint8_t> &Stream = Streams[S];
    for (size_t I = 0; I < StreamPages[S].size(); ++I) {
      const size_t Off = static_cast<size_t>(StreamPages[S][I]) * BlockSize;
      const size_t Chunk = std::min<size_t>(BlockSize, Stream.size() - I * BlockSize);
      std::memcpy(File.data() + Off, Stream.data() + I * BlockSize, Chunk);
    }
  }
  for (size_t I = 0; I < RootPageList.size(); ++I) {
    const size_t Off = static_cast<size_t>(RootPageList[I]) * BlockSize;
    const size_t Chunk = std::min<size_t>(BlockSize, Root.size() - I * BlockSize);
    std::memcpy(File.data() + Off, Root.data() + I * BlockSize, Chunk);
  }
  return File;
}

std::vector<uint8_t> makeLegacyProcStream(llvm::StringRef Name, uint32_t Offset,
                                          uint16_t Segment, uint32_t Size) {
  std::vector<uint8_t> Payload;
  appendU32(Payload, 0);
  appendU32(Payload, 0);
  appendU32(Payload, 0);
  appendU32(Payload, Size);
  appendU32(Payload, 0);
  appendU32(Payload, 0);
  appendU32(Payload, 0);
  appendU32(Payload, Offset);
  appendU16(Payload, Segment);
  Payload.push_back(0);
  Payload.push_back(static_cast<uint8_t>(Name.size()));
  Payload.insert(Payload.end(), Name.begin(), Name.end());

  std::vector<uint8_t> Stream;
  appendU32(Stream, 2);
  appendU16(Stream, static_cast<uint16_t>(Payload.size() + 2));
  appendU16(Stream, static_cast<uint16_t>(llvm::codeview::SymbolKind::S_GPROC32_ST));
  Stream.insert(Stream.end(), Payload.begin(), Payload.end());
  return Stream;
}

std::vector<uint8_t> makeLegacySectionHdr(llvm::StringRef Name, uint32_t VA,
                                          uint32_t VSize, uint32_t RawSize,
                                          uint32_t RawOff, uint32_t Chars) {
  std::vector<uint8_t> Hdr(40, 0);
  std::memcpy(Hdr.data(), Name.data(), std::min<size_t>(Name.size(), 8));
  std::memcpy(Hdr.data() + 8, &VSize, 4);
  std::memcpy(Hdr.data() + 12, &VA, 4);
  std::memcpy(Hdr.data() + 16, &RawSize, 4);
  std::memcpy(Hdr.data() + 20, &RawOff, 4);
  std::memcpy(Hdr.data() + 36, &Chars, 4);
  return Hdr;
}

std::vector<uint8_t> makeLegacyDbi(uint32_t Age, uint16_t ModuleStream,
                                   uint32_t ModuleSymBytes,
                                   uint16_t SectionStream) {
  std::vector<uint8_t> Modi(64, 0);
  std::memcpy(Modi.data() + 34, &ModuleStream, 2);
  std::memcpy(Modi.data() + 36, &ModuleSymBytes, 4);
  Modi.insert(Modi.end(), {'m', 'o', 'd', 0, 'o', 'b', 'j', 0});

  std::vector<uint8_t> Dbg;
  for (int I = 0; I < 5; ++I)
    appendU16(Dbg, 0xFFFF);
  appendU16(Dbg, SectionStream);

  std::vector<uint8_t> Dbi(64, 0);
  const int32_t Sig = -1;
  const uint32_t Ver = llvm::pdb::PdbDbiV60;
  std::memcpy(Dbi.data() + 0, &Sig, 4);
  std::memcpy(Dbi.data() + 4, &Ver, 4);
  std::memcpy(Dbi.data() + 8, &Age, 4);
  const uint32_t ModiSize = static_cast<uint32_t>(Modi.size());
  std::memcpy(Dbi.data() + 24, &ModiSize, 4);
  const uint32_t DbgSize = static_cast<uint32_t>(Dbg.size());
  std::memcpy(Dbi.data() + 48, &DbgSize, 4);
  Dbi.insert(Dbi.end(), Modi.begin(), Modi.end());
  Dbi.insert(Dbi.end(), Dbg.begin(), Dbg.end());
  return Dbi;
}

std::vector<uint8_t> makeLegacyInfo(uint32_t Signature, uint32_t Age) {
  std::vector<uint8_t> Info;
  appendU32(Info, llvm::pdb::PdbImplVC98);
  appendU32(Info, Signature);
  appendU32(Info, Age);
  return Info;
}

TEST(PDBIdentityDiscovery, AutoSearchSkipsMismatchAndUsesLaterMatchingPDB) {
  auto ImageOr = loadPEFixture("safety_cases_pe_x64.exe");
  ASSERT_TRUE(static_cast<bool>(ImageOr))
      << llvm::toString(ImageOr.takeError());

  ScratchDir Dir;
  const std::filesystem::path Binary = Dir.path("probe.exe");
  const std::filesystem::path WrongRecordedPDB = Dir.path("recorded.pdb");
  ImageOr->DynInfo.PDBPath = WrongRecordedPDB.string();
  ASSERT_TRUE(std::filesystem::copy_file(
      safetyFixture("safety_cases_pe_x64.exe"), Binary));
  ASSERT_TRUE(std::filesystem::copy_file(
      safetyFixture("safety_cases_pe_arm64.pdb"), WrongRecordedPDB));
  ASSERT_TRUE(std::filesystem::copy_file(
      safetyFixture("safety_cases_pe_x64.pdb"), Dir.path("probe.pdb")));

  DebugInfoResult Result = loadDebugInfo(Binary, *ImageOr);
  ASSERT_TRUE(static_cast<bool>(Result)) << Result.Error;
  EXPECT_EQ(Result.Kind, DebugInfoKind::PDB);
  EXPECT_EQ(Result.Path.filename().string(), "probe.pdb");
}

TEST(CodeViewNB10Parser, RecoversSignatureAgeAndBoundedPath) {
  const PDBBuildIdentity Expected = makeNB10Identity(0x5CBFE727, 3);
  std::vector<uint8_t> Bytes = makeNB10(Expected, R"(C:\symbols\legacy.pdb)");

  auto Parsed = coff_loader::detail::parseCodeViewNB10(Bytes);
  ASSERT_TRUE(static_cast<bool>(Parsed)) << llvm::toString(Parsed.takeError());
  EXPECT_EQ(Parsed->Identity, Expected);
  EXPECT_EQ(Parsed->Path, R"(C:\symbols\legacy.pdb)");
}

TEST(CodeViewNB10Parser, RejectsRSDSAndZeroAge) {
  auto AsRSDS = coff_loader::detail::parseCodeViewNB10(
      makeRSDS(makeIdentity(0x11)));
  EXPECT_FALSE(static_cast<bool>(AsRSDS));
  llvm::consumeError(AsRSDS.takeError());

  auto ZeroAge = coff_loader::detail::parseCodeViewNB10(
      makeNB10(makeNB10Identity(0x11, 0)));
  EXPECT_FALSE(static_cast<bool>(ZeroAge));
  llvm::consumeError(ZeroAge.takeError());
}

TEST(CodeViewIdentityDispatch, AcceptsBothRSDSAndNB10) {
  auto RSDS = coff_loader::detail::parseCodeViewIdentity(
      makeRSDS(makeIdentity(0x21, 2)));
  ASSERT_TRUE(static_cast<bool>(RSDS)) << llvm::toString(RSDS.takeError());
  EXPECT_EQ(RSDS->Identity.Kind, PDBIdentityKind::RSDS);

  auto NB10 = coff_loader::detail::parseCodeViewIdentity(
      makeNB10(makeNB10Identity(0x22, 2)));
  ASSERT_TRUE(static_cast<bool>(NB10)) << llvm::toString(NB10.takeError());
  EXPECT_EQ(NB10->Identity.Kind, PDBIdentityKind::NB10);
}

TEST(PDB20IdentityIntegration, MatchingNB10LoadsLengthPrefixedProcName) {
  constexpr uint32_t Signature = 0x11223344;
  constexpr uint32_t Age = 1;
  constexpr uint32_t Chars = 0x60000020;
  const std::vector<uint8_t> Module =
      makeLegacyProcStream("legacy_target", 0x100, 1, 0x20);
  const std::vector<uint8_t> Sections =
      makeLegacySectionHdr(".text", 0x1000, 0x1000, 0x200, 0x400, Chars);
  const std::vector<uint8_t> Dbi =
      makeLegacyDbi(Age, /*ModuleStream=*/5,
                    static_cast<uint32_t>(Module.size()), /*SectionStream=*/4);
  std::vector<std::vector<uint8_t>> Streams(6);
  Streams[1] = makeLegacyInfo(Signature, Age);
  Streams[3] = Dbi;
  Streams[4] = Sections;
  Streams[5] = Module;
  const std::vector<uint8_t> PdbBytes = writeJgPdb(Streams);

  ScratchDir Dir;
  const std::filesystem::path PdbPath = Dir.path("legacy.pdb");
  {
    std::error_code EC;
    llvm::raw_fd_ostream Out(PdbPath.string(), EC, llvm::sys::fs::OF_None);
    ASSERT_FALSE(EC) << EC.message();
    Out.write(reinterpret_cast<const char *>(PdbBytes.data()),
              static_cast<size_t>(PdbBytes.size()));
  }

  BinaryImage Image;
  Image.Format = BinaryFormat::COFF;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Base = 0x400000;
  Image.IsRelocatable = false;
  Image.DynInfo.CodeViewPDBIdentityState = PDBIdentityState::Unique;
  Image.DynInfo.CodeViewPDBIdentity = makeNB10Identity(Signature, Age);
  Section Text;
  Text.Name = ".text";
  Text.VA = 0x401000;
  Text.Size = 0x1000;
  Text.FileOff = 0x400;
  Text.FileSz = 0x200;
  Text.Type = Chars;
  Image.Sections.push_back(Text);

  auto ContextOr = PDBDebugContext::load(PdbPath, Image);
  ASSERT_TRUE(static_cast<bool>(ContextOr))
      << llvm::toString(ContextOr.takeError());
  EXPECT_TRUE((*ContextOr)->hasAuthenticatedImageIdentity());
  auto Fn = (*ContextOr)->resolveFunction(0x401100);
  ASSERT_TRUE(Fn.has_value());
  EXPECT_EQ(Fn->Name, "legacy_target");
  EXPECT_EQ(Fn->Addr, 0x401100u);
  EXPECT_EQ(Fn->Size, 0u);
}

TEST(PDB20IdentityIntegration, SignatureOrAgeMismatchRejectsTheCompanion) {
  constexpr uint32_t Signature = 0x55667788;
  constexpr uint32_t Age = 2;
  constexpr uint32_t Chars = 0x60000020;
  const std::vector<uint8_t> Module =
      makeLegacyProcStream("legacy_target", 0x100, 1, 0x20);
  std::vector<std::vector<uint8_t>> Streams(6);
  Streams[1] = makeLegacyInfo(Signature, Age);
  Streams[3] = makeLegacyDbi(Age, 5, static_cast<uint32_t>(Module.size()), 4);
  Streams[4] =
      makeLegacySectionHdr(".text", 0x1000, 0x1000, 0x200, 0x400, Chars);
  Streams[5] = Module;
  const std::vector<uint8_t> PdbBytes = writeJgPdb(Streams);

  ScratchDir Dir;
  const std::filesystem::path PdbPath = Dir.path("legacy.pdb");
  {
    std::error_code EC;
    llvm::raw_fd_ostream Out(PdbPath.string(), EC, llvm::sys::fs::OF_None);
    ASSERT_FALSE(EC) << EC.message();
    Out.write(reinterpret_cast<const char *>(PdbBytes.data()),
              static_cast<size_t>(PdbBytes.size()));
  }

  BinaryImage Image;
  Image.Format = BinaryFormat::COFF;
  Image.Arch = Arch::X86;
  Image.Base = 0x400000;
  Image.DynInfo.CodeViewPDBIdentityState = PDBIdentityState::Unique;
  Image.DynInfo.CodeViewPDBIdentity = makeNB10Identity(Signature ^ 1, Age);
  Section Text;
  Text.Name = ".text";
  Text.VA = 0x401000;
  Text.Size = 0x1000;
  Text.FileOff = 0x400;
  Text.FileSz = 0x200;
  Text.Type = Chars;
  Image.Sections.push_back(Text);

  auto ContextOr = PDBDebugContext::load(PdbPath, Image);
  ASSERT_FALSE(static_cast<bool>(ContextOr));
  const std::string Error = llvm::toString(ContextOr.takeError());
  EXPECT_NE(Error.find("signature/age does not match"), std::string::npos)
      << Error;
}

} // namespace
