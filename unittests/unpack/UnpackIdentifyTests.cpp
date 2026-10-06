//===- UnpackIdentifyTests.cpp - Protector evidence in input images -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "UnpackTestSupport.h"

#include "neverd/unpack/Unpack.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::unpack {
namespace {
using namespace test;

struct Fixture {
  const char *Name, *Packed, *Original;
};
constexpr Fixture Fixtures[] = {
#define NEVERD_UNPACK_TEST_FIXTURE(Name, Packed, Original)                     \
  {#Name, Packed, Original},
#include "UnpackCases.def"
#undef NEVERD_UNPACK_TEST_FIXTURE
};

const std::vector<PackerEvidence> AllEvidence = {
    PackerEvidence::UPXSectionNames, PackerEvidence::UPXPackHeader,
    PackerEvidence::UPXEntryStub};

/// Replace every occurrence of \p From, which must be present.
void replaceAll(std::vector<uint8_t> &Bytes, llvm::StringRef From,
                llvm::StringRef To) {
  ASSERT_EQ(From.size(), To.size());
  unsigned Count = 0;
  for (auto At = Bytes.begin(); (At = std::search(At, Bytes.end(), From.begin(),
                                                  From.end())) != Bytes.end();
       ++Count)
    At = std::copy(To.begin(), To.end(), At);
  ASSERT_NE(Count, 0u);
}

TEST(UnpackIdentify, OriginalImagesCarryNoEvidence) {
  for (const char *Name : {Plain, RuntimeOriginal}) {
    auto Packer = identifyPacker(readFile(fixture(Name)));
    ASSERT_TRUE(bool(Packer)) << llvm::toString(Packer.takeError());
    EXPECT_EQ(Packer->Kind, PackerKind::Unidentified) << Name;
    EXPECT_TRUE(Packer->Evidence.empty()) << Name;
  }
}

TEST(UnpackIdentify, EveryPackedFixtureShowsEachObservation) {
  for (const auto &F : Fixtures) {
    auto Packer = identifyPacker(readFile(fixture(F.Packed)));
    ASSERT_TRUE(bool(Packer)) << llvm::toString(Packer.takeError());
    EXPECT_EQ(Packer->Kind, PackerKind::UPX) << F.Name;
    EXPECT_EQ(Packer->Evidence, AllEvidence) << F.Name;
  }
}

TEST(UnpackIdentify, RenamedSectionsLeaveTwoObservations) {
  auto Bytes = readFile(fixture(PlainPacked));
  // The stub never reads its section names; the image still runs.
  replaceAll(Bytes, PackedSection, RenamedSection);
  auto Packer = identifyPacker(Bytes);
  ASSERT_TRUE(bool(Packer)) << llvm::toString(Packer.takeError());
  EXPECT_EQ(Packer->Kind, PackerKind::UPX);
  EXPECT_EQ(Packer->Evidence, (std::vector{PackerEvidence::UPXPackHeader,
                                           PackerEvidence::UPXEntryStub}));
}

TEST(UnpackIdentify, OneObservationDoesNotNameThePacker) {
  auto Bytes = readFile(fixture(PlainPacked));
  replaceAll(Bytes, PackedSection, RenamedSection);
  replaceAll(Bytes, PackMagic, RenamedSection);
  auto Packer = identifyPacker(Bytes);
  ASSERT_TRUE(bool(Packer)) << llvm::toString(Packer.takeError());
  EXPECT_EQ(Packer->Kind, PackerKind::Unidentified);
  EXPECT_EQ(Packer->Evidence, std::vector{PackerEvidence::UPXEntryStub});
}

TEST(UnpackIdentify, PackHeaderIsBoundByItsChecksum) {
  auto Bytes = readFile(fixture(PlainPacked));
  const llvm::StringRef Magic(PackMagic);
  auto At = std::search(Bytes.begin(), Bytes.end(), Magic.begin(), Magic.end());
  ASSERT_NE(At, Bytes.end());
  // The byte after the magic is covered by the checksum but not by any
  // other check, so only the checksum can reject this header.
  At[Magic.size()] ^= 1;
  auto Packer = identifyPacker(Bytes);
  ASSERT_TRUE(bool(Packer)) << llvm::toString(Packer.takeError());
  EXPECT_EQ(Packer->Kind, PackerKind::UPX);
  EXPECT_EQ(Packer->Evidence, (std::vector{PackerEvidence::UPXSectionNames,
                                           PackerEvidence::UPXEntryStub}));
}

TEST(UnpackIdentify, MalformedImagesAreErrors) {
  auto Bytes = readFile(fixture(PlainPacked));
  auto Truncated = Bytes;
  Truncated.resize(TruncatedBytes);
  auto NotPE = Bytes;
  NotPE[0] ^= 1;
  for (const auto &Input : {Truncated, NotPE, std::vector<uint8_t>{}}) {
    auto Packer = identifyPacker(Input);
    EXPECT_FALSE(bool(Packer));
    llvm::consumeError(Packer.takeError());
  }
}

TEST(UnpackIdentify, ContainerWithoutAFormatModuleIsANamedError) {
  // Another container is recognized as unsupported; it is not parsed as the
  // nearest known one.
  std::vector<uint8_t> Other(TruncatedBytes, 0);
  std::copy(std::begin(ELFMagic), std::end(ELFMagic), Other.begin());
  auto Packer = identifyPacker(Other);
  ASSERT_FALSE(bool(Packer));
  EXPECT_NE(llvm::toString(Packer.takeError()).find(SupportedFormat),
            std::string::npos);
}

TEST(UnpackIdentify, StubEvidenceIsBoundToItsInstructionSet) {
  // The protector module knows this stub for one instruction set. The same
  // bytes in an image for another one are not evidence of that stub.
  auto Bytes = readFile(fixture(PlainPacked));
  const Image Layout = readImage(Bytes);
  ASSERT_FALSE(HasFailure());
  llvm::support::endian::write16le(Bytes.data() + Layout.MachineOffset,
                                   MachineARM64);
  auto Packer = identifyPacker(Bytes);
  ASSERT_TRUE(bool(Packer)) << llvm::toString(Packer.takeError());
  EXPECT_EQ(Packer->Kind, PackerKind::Unidentified);
  EXPECT_EQ(Packer->Evidence, std::vector{PackerEvidence::UPXSectionNames});
}

TEST(UnpackIdentify, SectionOutsideTheImageIsAnError) {
  auto Bytes = readFile(fixture(Plain));
  const Image Layout = readImage(Bytes);
  ASSERT_FALSE(HasFailure());
  const uint64_t Last = Layout.SectionTableOffset +
                        (Layout.Sections.size() - 1) * SectionHeaderBytes;
  llvm::support::endian::write32le(Bytes.data() + Last + SectionAddressOffset,
                                   OutsideImageAddress);
  auto Packer = identifyPacker(Bytes);
  EXPECT_FALSE(bool(Packer));
  llvm::consumeError(Packer.takeError());
}

TEST(UnpackIdentify, NamesAreStable) {
#define NEVERD_UNPACK_TEST_NAME(Expression, Text)                              \
  EXPECT_STREQ(Expression, Text);
#include "UnpackCases.def"
#undef NEVERD_UNPACK_TEST_NAME
}
} // namespace
} // namespace neverd::unpack
