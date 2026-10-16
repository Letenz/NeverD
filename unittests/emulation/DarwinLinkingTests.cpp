//===- DarwinLinkingTests.cpp - Independent execution fixup fixtures -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Exact binding, rebase, malformed metadata and preparation bounds.
#include "DarwinTestImage.h"
#include "gtest/gtest.h"
#include "os/darwin/process/DarwinProcess.h"

#include "neverd/loader/MachO/MachOProgramLinking.h"

namespace neverd::emulation {
namespace {
using namespace llvm::MachO;
struct LinkingImage : darwin_test::Image {
  size_t Data, Symbols, Dynamic, Dependency, Fixups;
  static constexpr uint64_t Base = 0x100000000ULL;
  static constexpr size_t Bind = 33024, Rebase = 33088, Chain = 33152;
  explicit LinkingImage(bool Chained, bool X64 = false)
      : darwin_test::Image(X64) {
    u32(Thread, LC_MAIN);
    u32(Thread + 4, 24);
    u64(Thread + 8, 4096);
    u64(Thread + 16, 0);
    CommandsEnd = Thread + 24;
    u32(20, CommandsEnd - 32);
    Bytes.resize(49152, 0);
    u32(24, MH_DYLDLINK | MH_TWOLEVEL | MH_NOUNDEFS | MH_PIE);
    Data = command(LC_SEGMENT_64, 72);
    std::copy_n("__DATA", 6, Bytes.data() + Data + 8);
    u64(Data + 24, Base + 16384);
    u64(Data + 32, 16384);
    u64(Data + 40, 16384);
    u64(Data + 48, 16384);
    u32(Data + 56, 3);
    u32(Data + 60, 3);
    Dependency = command(LC_LOAD_DYLIB, 56);
    u32(Dependency + 8, 24);
    const char Library[] = "/usr/lib/libSystem.B.dylib";
    std::copy(std::begin(Library), std::end(Library),
              Bytes.begin() + Dependency + 24);
    Symbols = command(LC_SYMTAB, 24);
    u32(Symbols + 8, 32768);
    u32(Symbols + 12, 1);
    u32(Symbols + 16, 32784);
    u32(Symbols + 20, 8);
    u32(32768, 1);
    Bytes[32772] = 1; // External undefined symbol.
    Bytes[32775] = 1; // Ordinary dylib ordinal one.
    std::copy_n("\0_write\0", 8, Bytes.data() + 32784);
    Dynamic = command(LC_DYSYMTAB, 80);
    u32(Dynamic + 28, 1);
    if (!Chained) {
      Fixups = command(LC_DYLD_INFO_ONLY, 48);
      const uint8_t Binding[] = {0x11, 0x40, '_',  'w',  'r', 'i',  't',
                                 'e',  0,    0x51, 0x72, 0,   0x90, 0};
      std::copy(std::begin(Binding), std::end(Binding), Bytes.begin() + Bind);
      const uint8_t Rebasing[] = {0x11, 0x22, 8, 0x51, 0};
      std::copy(std::begin(Rebasing), std::end(Rebasing),
                Bytes.begin() + Rebase);
      u32(Fixups + 8, Rebase);
      u32(Fixups + 12, sizeof(Rebasing));
      u32(Fixups + 16, Bind);
      u32(Fixups + 20, sizeof(Binding));
      u64(16392, Base + 4096);
    } else {
      Fixups = command(LC_DYLD_CHAINED_FIXUPS, 16);
      u32(Fixups + 8, Chain);
      u32(Fixups + 12, 88);
      u32(Chain + 4, 32);
      u32(Chain + 8, 72);
      u32(Chain + 12, 76);
      u32(Chain + 16, 1);
      u32(Chain + 20, 1);
      u32(Chain + 32, 3);
      u32(Chain + 44, 16);
      u32(Chain + 48, 24);
      Bytes[Chain + 53] = 0x40; // 16384-byte chain page.
      Bytes[Chain + 54] = 2;    // Generic64 pointer format.
      u64(Chain + 56, 16384);
      Bytes[Chain + 68] = 1; // One page, start zero.
      u32(Chain + 72, 1);    // Import zero, provider one, name offset zero.
      std::copy_n("_write\0", 7, Bytes.data() + Chain + 76);
      u64(16384, (1ULL << 63) | (2ULL << 51));
      u64(16392, Base + 4096);
    }
  }
};
llvm::Expected<MachOProgramLinking> linking(const LinkingImage &I) {
  return readMachOProgramLinking(I.Bytes, 1 << 20,
                                 [] { return llvm::Error::success(); });
}
void refused(const LinkingImage &I, llvm::StringRef Reason) {
  SCOPED_TRACE(Reason.str());
  auto R = linking(I);
  ASSERT_FALSE(bool(R));
  const auto Message = llvm::toString(R.takeError());
  EXPECT_NE(Message.find(Reason.str()), std::string::npos) << Message;
}
TEST(DarwinLinking, ClassicAndChainedRetainExactOriginalFixupFacts) {
  for (bool Chain : {false, true})
    for (bool X64 : {false, true}) {
      LinkingImage I(Chain, X64);
      const auto Original = I.Bytes;
      auto R = linking(I);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Dependencies.size(), 1u);
      EXPECT_EQ(R->Dependencies[0].Command, uint32_t(LC_LOAD_DYLIB));
      EXPECT_EQ(R->Dependencies[0].Name, "/usr/lib/libSystem.B.dylib");
      ASSERT_EQ(R->Fixups.size(), 2u);
      const auto &B = R->Fixups[Chain ? 0 : 1];
      const auto &L = R->Fixups[Chain ? 1 : 0];
      EXPECT_TRUE(B.Bind);
      EXPECT_EQ(B.Address, I.Base + 16384);
      EXPECT_EQ(B.Symbol, "_write");
      EXPECT_EQ(B.LibraryOrdinal, 1);
      EXPECT_FALSE(B.WeakImport);
      EXPECT_EQ(B.Addend, 0);
      EXPECT_FALSE(L.Bind);
      EXPECT_EQ(L.Address, I.Base + 16392);
      EXPECT_EQ(L.Target, I.Base + 4096);
      EXPECT_GT(R->MetadataBytes, 0u);
      EXPECT_EQ(I.Bytes, Original);
    }
}
TEST(DarwinLinking, RejectsIncompleteMalformedAndAmbiguousClassicBindings) {
  LinkingImage Missing(false);
  Missing.u32(Missing.Fixups + 20, 13);
  refused(Missing, "unterminated");
  LinkingImage LEB(false);
  LEB.Bytes[LEB.Bind + 11] = 0x80;
  LEB.u32(LEB.Fixups + 20, 12);
  refused(LEB, "ULEB128");
  LinkingImage Opcode(false);
  Opcode.Bytes[Opcode.Bind + 12] = 0xd0;
  refused(Opcode, "opcode");
  LinkingImage Duplicate(false);
  Duplicate.Bytes[Duplicate.Bind + 13] = 0x72;
  Duplicate.Bytes[Duplicate.Bind + 14] = 0;
  Duplicate.Bytes[Duplicate.Bind + 15] = 0x90;
  Duplicate.u32(Duplicate.Fixups + 20, 17);
  refused(Duplicate, "duplicate");
  LinkingImage Ordinal(false);
  Ordinal.Bytes[Ordinal.Bind] = 0x12;
  refused(Ordinal, "ordinal");
  LinkingImage BindType(false);
  BindType.Bytes[BindType.Bind + 9] =
      0x11; // No SET_TYPE in an ordinary stream.
  refused(BindType, "pointer type");
  LinkingImage RebaseType(false);
  RebaseType.Bytes[RebaseType.Rebase] = 0x40; // ADD_ADDR_IMM_SCALED zero.
  refused(RebaseType, "pointer type");
  LinkingImage Lazy(false);
  Lazy.u32(Lazy.Fixups + 32, Lazy.Bind + 32);
  Lazy.u32(Lazy.Fixups + 36, 1);
  refused(Lazy, "weak/lazy");
  LinkingImage Unknown(false);
  Unknown.Bytes[Unknown.Bind + 2] = 'x';
  refused(Unknown, "undefined symbol");
}
TEST(DarwinLinking, FixupStorageAndTargetsRequireExactMappedAuthority) {
  for (bool Chain : {false, true}) {
    LinkingImage WritableText(Chain);
    WritableText.u32(WritableText.Data + 56, 7);
    refused(WritableText, "non-executable");
    LinkingImage Unmapped(Chain);
    Unmapped.u64(16392, 0x4000);
    refused(Unmapped, "local mapped");
    LinkingImage Truncated(Chain);
    Truncated.u64(Truncated.Data + 48, 4);
    refused(Truncated, "file storage");
    LinkingImage Relocations(Chain);
    Relocations.u32(Relocations.Dynamic + 64, 33400);
    Relocations.u32(Relocations.Dynamic + 68, 1);
    refused(Relocations, "relocations");
    LinkingImage Hidden(Chain);
    Hidden.u32(Hidden.Dynamic + 28, 0);
    refused(Hidden, "declared range");
  }
}
TEST(DarwinLinking, RejectsUnsupportedAndMalformedChainedRecords) {
  LinkingImage Ordinal(true);
  Ordinal.u64(16384, (1ULL << 63) | 1);
  refused(Ordinal, "pointer import");
  LinkingImage Authenticated(true);
  Authenticated.Bytes[Authenticated.Chain + 54] = 1;
  refused(Authenticated, "chained segment");
  LinkingImage Overlap(true);
  Overlap.u64(16384, (1ULL << 63) | (1ULL << 51));
  refused(Overlap, "aligned");
  LinkingImage Page(true);
  Page.u64(16384, (1ULL << 63) | (0xfffULL << 51));
  refused(Page, "page");
  LinkingImage Reserved(true);
  Reserved.u64(16384, (1ULL << 63) | (1ULL << 32));
  refused(Reserved, "pointer import");
  LinkingImage Both(true);
  Both.command(LC_DYLD_INFO_ONLY, 48);
  refused(Both, "mixed");
}
TEST(DarwinLinking, OffsetRebasesAndUnsignedPointerAddendsKeepTheirFullBits) {
  LinkingImage I(true);
  I.Bytes[I.Chain + 54] = 6;
  I.u64(16392, 4096);
  I.u64(16384, (1ULL << 63) | (2ULL << 51) | (0xffULL << 24));
  auto R = linking(I);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Fixups[0].Addend, 255);
  EXPECT_EQ(R->Fixups[1].Target, I.Base + 4096);
}
TEST(DarwinLinking, ChainedOrdinaryOrdinalsKeepBitsBelowTheSpecialRange) {
  for (unsigned Format : {1u, 2u, 3u}) {
    LinkingImage I(true);
    // Extend only this metadata fixture's command area. The execution reader
    // need not execute its entry bytes to identify a real provider ordinal128.
    unsigned Commands = llvm::support::endian::read32le(I.Bytes.data() + 16);
    for (unsigned N = 1; N < 128; ++N) {
      const auto At = I.CommandsEnd;
      I.CommandsEnd += 32;
      I.u32(At, LC_LOAD_DYLIB);
      I.u32(At + 4, 32);
      I.u32(At + 8, 24);
      std::copy_n("/x\0", 3, I.Bytes.data() + At + 24);
      ++Commands;
    }
    I.u32(16, Commands);
    I.u32(20, I.CommandsEnd - 32);
    I.Bytes[32775] = 128;
    I.u32(I.Chain + 20, Format);
    const unsigned Names = Format == 1 ? 76 : Format == 2 ? 80 : 88;
    I.u32(I.Chain + 12, Names);
    I.u32(I.Fixups + 12, Names + 8);
    I.u64(I.Chain + 72, 128);
    if (Format == 3)
      I.u64(I.Chain + 80, 0);
    std::copy_n("_write\0", 7, I.Bytes.data() + I.Chain + Names);
    auto R = linking(I);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Dependencies.size(), 128u);
    EXPECT_EQ(R->Fixups[0].LibraryOrdinal, 128);
    I.u32(I.Chain + 72, Format == 3 ? 0xffff : 0xff);
    refused(I, "special chained");
  }
  for (uint8_t Ordinal : {0u, 0xfeu, 0xffu}) {
    LinkingImage I(true);
    I.Bytes[32775] = Ordinal;
    refused(I, "ordinary library ordinal");
  }
}
TEST(DarwinLinking,
     PreparationDeadlineAndMetadataLimitsCannotPublishPartialFacts) {
  LinkingImage I(true);
  auto Full = linking(I);
  ASSERT_TRUE(bool(Full)) << llvm::toString(Full.takeError());
  auto Exact = readMachOProgramLinking(I.Bytes, Full->MetadataBytes,
                                       [] { return llvm::Error::success(); });
  ASSERT_TRUE(bool(Exact)) << llvm::toString(Exact.takeError());
  auto Under = readMachOProgramLinking(I.Bytes, Full->MetadataBytes - 1,
                                       [] { return llvm::Error::success(); });
  ASSERT_FALSE(bool(Under));
  EXPECT_NE(llvm::toString(Under.takeError()).find("byte limit"),
            std::string::npos);
  auto Small = readMachOProgramLinking(I.Bytes, 32,
                                       [] { return llvm::Error::success(); });
  ASSERT_FALSE(bool(Small));
  EXPECT_NE(llvm::toString(Small.takeError()).find("byte limit"),
            std::string::npos);
  unsigned Checks = 0;
  auto Expired =
      readMachOProgramLinking(I.Bytes, 1 << 20, [&]() -> llvm::Error {
        if (++Checks == 10)
          return llvm::make_error<darwin_model::PreparationTimeout>(
              GuestArchitecture::AArch64, LinkingImage::Base + 4096);
        return llvm::Error::success();
      });
  ASSERT_FALSE(bool(Expired));
  auto E = Expired.takeError();
  EXPECT_TRUE(E.isA<darwin_model::PreparationTimeout>());
  EXPECT_NE(llvm::toString(std::move(E)).find("deadline"), std::string::npos);
}
TEST(DarwinLinking,
     ProcessAdmissionRequiresTheProviderNamespaceAndExactSymbols) {
  const auto Load = [](const LinkingImage &I) {
    darwin_test::TemporaryImage File(I);
    return darwin_model::loadImage(File.path(), darwin_model::macOSProfile(),
                                   {});
  };
  for (bool Chain : {false, true}) {
    LinkingImage Good(Chain);
    Good.u32(Good.Data + 68, SG_READ_ONLY);
    auto R = Load(Good);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Imports.size(), 1u);
    EXPECT_EQ(R->Memory.Reserved.size(), 2u);
    EXPECT_GT(R->MetadataBytes, 0u);
    EXPECT_EQ(R->Plan.Regions[1].Permissions & Write, 0u);
    EXPECT_EQ(R->Memory.Maximum.back().Protection, 3u);
    const auto Reject = [&](const LinkingImage &I, llvm::StringRef Reason) {
      SCOPED_TRACE(Reason.str());
      auto Bad = Load(I);
      ASSERT_FALSE(bool(Bad));
      const auto Message = llvm::toString(Bad.takeError());
      EXPECT_NE(Message.find(Reason.str()), std::string::npos) << Message;
    };
    LinkingImage Flat(Chain);
    Flat.u32(24, MH_DYLDLINK | MH_PIE);
    Reject(Flat, "two-level");
    LinkingImage Provider(Chain);
    Provider.Bytes[Provider.Dependency + 24] = 'x';
    Reject(Provider, "provider");
    LinkingImage Weak(Chain);
    Weak.u32(Weak.Dependency, LC_LOAD_WEAK_DYLIB);
    Reject(Weak, "provider");
    LinkingImage Version(Chain);
    Version.u32(Version.Dependency + 20, 0x20000);
    Reject(Version, "provider");
    LinkingImage Exit(Chain);
    std::copy_n("_exit\0", 6, Exit.Bytes.data() + 32785);
    std::copy_n("_exit\0", 6,
                Exit.Bytes.data() + (Chain ? Exit.Chain + 76 : Exit.Bind + 2));
    if (!Chain) {
      std::move(Exit.Bytes.begin() + Exit.Bind + 9,
                Exit.Bytes.begin() + Exit.Bind + 14,
                Exit.Bytes.begin() + Exit.Bind + 8);
      Exit.Bytes[Exit.Bind + 13] = 0;
    }
    Reject(Exit, "libSystem import");
    auto Budget = ExecutionBudget::create(
        {100, 100, 1}, ExecutionBudget::Clock::now() - std::chrono::seconds(1));
    ASSERT_TRUE(bool(Budget));
    darwin_test::TemporaryImage File(Good);
    auto Expired = darwin_model::loadImage(
        File.path(), darwin_model::macOSProfile(), {}, Budget->get());
    ASSERT_FALSE(bool(Expired));
    auto E = Expired.takeError();
    EXPECT_TRUE(E.isA<darwin_model::PreparationTimeout>());
    EXPECT_NE(llvm::toString(std::move(E)).find("deadline"), std::string::npos);

    ProcessOptions Options;
    Options.Backend = ExecutionBackendKind::Unicorn;
    Options.Limits.TimeoutMicroseconds = 1;
    auto Timeout = darwin_model::runProcess(
        File.path(), darwin_model::macOSProfile(), Options);
    ASSERT_TRUE(bool(Timeout)) << llvm::toString(Timeout.takeError());
    EXPECT_EQ(Timeout->Stop, ProcessStopReason::Timeout);
    EXPECT_EQ(Timeout->SelectedBackend, ExecutionBackendKind::Unicorn);
    EXPECT_FALSE(Timeout->BackendSelectionReason.empty());
    EXPECT_EQ(Timeout->Entry, LinkingImage::Base + 4096);
    EXPECT_EQ(Timeout->PC, Timeout->Entry);
    EXPECT_EQ(Timeout->Instructions, 0u);
    EXPECT_EQ(Timeout->Events, 0u);
    EXPECT_FALSE(Timeout->ExitStatus);
    EXPECT_FALSE(Timeout->ReturnValue);
    EXPECT_FALSE(Timeout->LastCPUExit);
    EXPECT_TRUE(Timeout->Services.empty());
    EXPECT_TRUE(Timeout->NativeCalls.empty());

    // An expired deadline cannot turn malformed admission or a genuinely
    // unavailable transport into a successfully resolved Timeout result.
    auto WrongProfile = darwin_model::runProcess(
        File.path(), darwin_model::iOSProfile(false), Options);
    ASSERT_FALSE(bool(WrongProfile));
    EXPECT_NE(llvm::toString(WrongProfile.takeError()).find("platform"),
              std::string::npos);
#if defined(__APPLE__) || defined(_WIN32)
    Options.Backend = ExecutionBackendKind::KVM;
#else
    Options.Backend = ExecutionBackendKind::HVF;
#endif
    auto Unavailable = darwin_model::runProcess(
        File.path(), darwin_model::macOSProfile(), Options);
    ASSERT_FALSE(bool(Unavailable));
    auto BackendError = Unavailable.takeError();
    EXPECT_TRUE(BackendError.isA<BackendUnavailableError>());
    llvm::consumeError(std::move(BackendError));
  }
}
} // namespace
} // namespace neverd::emulation
