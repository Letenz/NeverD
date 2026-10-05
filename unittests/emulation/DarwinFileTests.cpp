//===- DarwinFileTests.cpp - Darwin descriptor and copy contracts ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinFileTestData.h"
#include "gtest/gtest.h"
#include "os/darwin/kernel/DarwinFiles.h"

#include "neverd/emulation/AddressSpace.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd::emulation::darwin_model {
namespace {
TEST(DarwinFileOptions, AdmissionCountsPathsTerminatorsFilesAndInputTogether) {
  DarwinFileOptions O;
  O.Files["/a"] = std::vector<uint8_t>(darwin_file_limits::Bytes - 4);
  O.StandardInput = {0xff};
  ASSERT_FALSE(bool(validateFileOptions(O)));
  O.Files["/b"] = {};
  auto TooLarge = validateFileOptions(O);
  ASSERT_TRUE(bool(TooLarge));
  llvm::consumeError(std::move(TooLarge));
  O = {};
  for (unsigned I = 0; I != 256; ++I)
    O.Files["/file" + std::to_string(I)] = {};
  ASSERT_FALSE(bool(validateFileOptions(O)));
  O.Files["/extra"] = {};
  auto TooMany = validateFileOptions(O);
  ASSERT_TRUE(bool(TooMany));
  llvm::consumeError(std::move(TooMany));
  O = {};
  O.Files['/' + std::string(256, 'a')] = {};
  auto LongName = validateFileOptions(O);
  ASSERT_TRUE(bool(LongName));
  llvm::consumeError(std::move(LongName));
}
class DarwinFileTest : public testing::TestWithParam<uint64_t> {
protected:
  static constexpr uint64_t Base = 0x100000;
  std::shared_ptr<AddressSpace> Space;
  std::optional<DarwinFileOptions> Options;
  std::unique_ptr<DarwinFiles> Files;
  ProcessResult Result{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn, "memory-only test"};
  uint64_t Page;
  void SetUp() override {
    Page = GetParam();
    auto Physical = PhysicalMemory::create(Page * 4);
    ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
    auto Created = AddressSpace::create(*Physical, Page * 4);
    ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
    Space = std::move(*Created);
    ASSERT_FALSE(
        bool(Space->map(Base, Page * 2, Read | Write | UserAccessible)));
    Options.emplace();
    Options->Files["/data"] = {'a', 'b', 0, 0xff, 'e', 'f'};
    Files = std::make_unique<DarwinFiles>(*Space, Options);
    path("/data");
  }
  void path(llvm::StringRef Text, uint64_t Address = Base) {
    std::string Data = Text.str() + '\0';
    ASSERT_FALSE(bool(Space->write(
        Address,
        llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(Data.data()),
                                Data.size()))));
  }
  std::optional<ServiceResult> invoke(ServiceKind Kind,
                                      std::array<uint64_t, 6> Args) {
    auto Out = Files->handle(Kind, {0, 0, Args, std::nullopt}, Result);
    EXPECT_TRUE(bool(Out)) << (Out ? "" : llvm::toString(Out.takeError()));
    return Out ? *Out : std::nullopt;
  }
  uint64_t ok(ServiceKind Kind, std::array<uint64_t, 6> Args) {
    auto Out = invoke(Kind, Args);
    EXPECT_TRUE(Out.has_value()) << Result.Diagnostic;
    if (!Out)
      return UINT64_MAX;
    EXPECT_FALSE(Out->Error) << Out->Value;
    return Out->Value;
  }
  void error(ServiceKind Kind, std::array<uint64_t, 6> Args, uint64_t Code) {
    auto Out = invoke(Kind, Args);
    ASSERT_TRUE(Out.has_value()) << Result.Diagnostic;
    EXPECT_TRUE(Out->Error);
    EXPECT_EQ(Out->Value, Code);
  }
};

TEST_P(DarwinFileTest, DupSharesOffsetsButOpenAndPreadDoNot) {
  auto A = ok(ServiceKind::Open, {Base});
  auto B = ok(ServiceKind::Dup, {A});
  auto C = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Read, {A, Base + Page, 2}), 2u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 2u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {C, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Pread, {B, Base + Page, 2, 2}), 2u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page, 2)), 0xff00u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 2u);
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  EXPECT_EQ(ok(ServiceKind::Read, {B, Base + Page, 1}), 1u);
  error(ServiceKind::Read, {A, Base + Page, 1}, 9);
  EXPECT_EQ(ok(ServiceKind::Open, {Base}), A);
}

TEST_P(DarwinFileTest, PerDescriptorFlagsDup2AndCaptureSinkIdentity) {
  auto A = ok(ServiceKind::Open, {Base, 0x1000000});
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Dup2, {A, A}), A);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 1}), 1u);
  auto B = ok(ServiceKind::Dup, {A});
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 2, 3}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 2, 2}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 67, 20}), 20u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {20, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 0, 20}), 21u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {21, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), 0u);
  EXPECT_EQ(ok(ServiceKind::Dup2, {1, A}), A);
  EXPECT_EQ(Files->outputSink(A), 1u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {1}), 0u);
  EXPECT_FALSE(Files->outputSink(1));
  EXPECT_EQ(Files->outputSink(A), 1u);
  EXPECT_EQ(ok(ServiceKind::Dup2, {A, 2}), 2u);
  EXPECT_EQ(Files->outputSink(2), 1u);
  error(ServiceKind::Dup2, {A, UINT64_MAX}, 9);
  error(ServiceKind::Fcntl, {A, 0, UINT64_MAX}, 22);
}

TEST_P(DarwinFileTest, ReadErrorsEOFAndSeekOverflowPreserveCursor) {
  auto FD = ok(ServiceKind::Open, {Base});
  error(ServiceKind::Read, {99, 0, 0x80000000}, 22);
  error(ServiceKind::Pread, {99, 0, UINT64_MAX, UINT64_MAX}, 22);
  error(ServiceKind::Read, {99, 0, 1}, 9);
  error(ServiceKind::Read, {FD, 0, 1}, 14);
  error(ServiceKind::Read, {FD, UINT64_MAX, 1}, 14);
  error(ServiceKind::Pread, {FD, Base + Page, 0, UINT64_MAX}, 22);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 1, 2}), 7u);
  EXPECT_EQ(ok(ServiceKind::Read, {FD, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Read, {FD, UINT64_MAX, 0}), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, INT64_MAX, 0}), uint64_t(INT64_MAX));
  error(ServiceKind::Lseek, {FD, 1, 1}, 84);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), uint64_t(INT64_MAX));
  error(ServiceKind::Lseek, {FD, uint64_t(INT64_MIN), 1}, 22);
  error(ServiceKind::Lseek, {FD, 0, 99}, 22);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, uint64_t(-1), 2}), 5u);
  EXPECT_EQ(ok(ServiceKind::Read, {FD, Base + Page, 10}), 1u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page, 1)), 'f');
}

TEST_P(DarwinFileTest, PartialCopyoutIsRefusedBeforeBytesOrOffsetChange) {
  auto FD = ok(ServiceKind::Open, {Base});
  const uint64_t End = Base + Page * 2 - 2;
  ASSERT_FALSE(bool(Space->writeInteger(End, 0x7777, 2)));
  EXPECT_FALSE(invoke(ServiceKind::Read, {FD, End, 4}));
  EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(End, 2)), 0x7777u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 0u);
  ASSERT_FALSE(bool(Space->protect(Base + Page, Page, Read | UserAccessible)));
  error(ServiceKind::Read, {FD, Base + Page, 1}, 14);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 0u);
}

TEST_P(DarwinFileTest, FiniteInputSharesItsCursorAndClosedFDZeroIsReusable) {
  EXPECT_FALSE(invoke(ServiceKind::Read, {0, Base + Page, 1}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileInput);
  Options->StandardInput = {0, 0xff, 'x'};
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  auto Copy = ok(ServiceKind::Dup, {0});
  EXPECT_EQ(ok(ServiceKind::Read, {Copy, Base + Page, 2}), 2u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page, 2)), 0xff00u);
  error(ServiceKind::Pread, {Copy, Base + Page, 1, 0}, 29);
  error(ServiceKind::Lseek, {Copy, 0, 0}, 29);
  EXPECT_EQ(ok(ServiceKind::Close, {0}), 0u);
  EXPECT_EQ(ok(ServiceKind::Open, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Read, {Copy, Base + Page, 3}), 1u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page, 1)), 'x');
  EXPECT_EQ(ok(ServiceKind::Read, {Copy, 0, 1}), 0u);
  Options->StandardInput = std::vector<uint8_t>();
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  EXPECT_EQ(ok(ServiceKind::Read, {0, 0, 1}), 0u);
}

TEST_P(DarwinFileTest, PathErrorsAndResourceExhaustionDoNotLeakDescriptors) {
  Options->Files["/nested/file"] = {'x'};
  path("/nested/file");
  EXPECT_EQ(ok(ServiceKind::Open, {Base}), 3u);
  EXPECT_EQ(ok(ServiceKind::Close, {3}), 0u);
  path("/nested");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileDirectory);
  for (unsigned I = 0; I != 4; ++I)
    error(ServiceKind::Open, {0}, 14);
  path("");
  error(ServiceKind::Open, {Base}, 2);
  path("/missing");
  error(ServiceKind::Open, {Base}, 2);
  path("/data/child");
  error(ServiceKind::Open, {Base}, 20);
  path("/data/" + std::string(256, 'a'));
  error(ServiceKind::Open, {Base}, 20);
  path("/missing/" + std::string(256, 'a'));
  error(ServiceKind::Open, {Base}, 2);
  path('/' + std::string(256, 'a'));
  error(ServiceKind::Open, {Base}, 63);
  path(std::string(1024, 'a'));
  error(ServiceKind::Open, {Base}, 63);
  path("/data", Base + Page * 2 - 6);
  EXPECT_EQ(ok(ServiceKind::Open, {Base + Page * 2 - 6}), 3u);
  Options->DescriptorLimit = 4;
  error(ServiceKind::Open, {0}, 24);
  error(ServiceKind::Dup, {3}, 24);
  EXPECT_EQ(ok(ServiceKind::Close, {3}), 0u);
  path("/data");
  EXPECT_EQ(ok(ServiceKind::Open, {Base}), 3u);
}

TEST_P(DarwinFileTest, UnsupportedOperationsDoNotPretendToBeMissingFiles) {
  for (auto Text : {"relative", "/./data", "/data/../data", "//data", "/"}) {
    path(Text);
    EXPECT_FALSE(invoke(ServiceKind::Open, {Base}));
    EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
  }
  path("/data");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 1}));
  EXPECT_EQ(ok(ServiceKind::Open, {Base}), 3u);
  EXPECT_FALSE(invoke(ServiceKind::Fcntl, {3, 999}));
  EXPECT_FALSE(invoke(ServiceKind::Lseek, {3, 0, 3}));
  Options.reset();
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileInputs);
  Options.emplace();
  error(ServiceKind::Open, {Base}, 2);
}

TEST_P(DarwinFileTest, Stat64PreservesAllFieldsPaddingAndDescriptorOffsets) {
  Options->Metadata["/data"] = darwin_test::metadata(6);
  // Independently specified little-endian LP64 record, including the rdev,
  // alignment gap and all reserved bytes. Also checked against the native SDK.
  const auto Expected = llvm::fromHex(
      "85ffffffa48103001032547698badcfeefcdab8998badcfe0000000000000000"
      "01000000000000800100000000000000ffffffffffffff7fffc99a3b00000000"
      "fdffffffffffffff0400000000000000fbffffffffffffff0600000000000000"
      "060000000000000008000000000000000010000034120000efcdab8900000000"
      "00000000000000000000000000000000");
  const uint64_t Buffer = Base + Page - 7;
  auto FD = ok(ServiceKind::Open, {Base});
  auto Copy = ok(ServiceKind::Dup, {FD});
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 2, 0}), 2u);
  EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
  for (auto Kind :
       {ServiceKind::Stat64, ServiceKind::Lstat64, ServiceKind::Fstat64}) {
    ASSERT_FALSE(bool(Space->writeInteger(Buffer - 1, 0xaa, 1)));
    ASSERT_FALSE(bool(Space->writeInteger(Buffer + 144, 0xbb, 1)));
    EXPECT_EQ(ok(Kind, {Kind == ServiceKind::Fstat64 ? Copy : Base, Buffer}),
              0u);
    std::array<uint8_t, 144> Bytes;
    ASSERT_FALSE(bool(Space->read(Buffer, Bytes)));
    EXPECT_EQ(llvm::toHex(llvm::ArrayRef<uint8_t>(Bytes)),
              llvm::toHex(Expected));
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer - 1, 1)), 0xaau);
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer + 144, 1)), 0xbbu);
    EXPECT_EQ(ok(ServiceKind::Lseek, {Copy, 0, 1}), 2u);
  }
  EXPECT_EQ(ok(ServiceKind::Open, {Base}), FD);
}

TEST_P(DarwinFileTest, Stat64FailureOrderAndPartialOutputHaveNoSideEffects) {
  Options->Metadata["/data"] = darwin_test::metadata(6);
  auto FD = ok(ServiceKind::Open, {Base});
  error(ServiceKind::Fstat64, {99, 0}, 9);
  error(ServiceKind::Stat64, {0, 0}, 14);
  path("/absent");
  error(ServiceKind::Stat64, {Base, 0}, 2);
  path("/data/child");
  error(ServiceKind::Lstat64, {Base, 0}, 20);
  path("/data");
  Options->DescriptorLimit = 4;
  error(ServiceKind::Open, {Base}, 24);
  EXPECT_EQ(ok(ServiceKind::Stat64, {Base, Base + Page}), 0u);
  for (auto Kind :
       {ServiceKind::Stat64, ServiceKind::Lstat64, ServiceKind::Fstat64}) {
    const uint64_t Source = Kind == ServiceKind::Fstat64 ? FD : Base;
    error(Kind, {Source, 0}, 14);
    error(Kind, {Source, UINT64_MAX}, 14);
    const auto End = Base + Page * 2 - 8;
    ASSERT_FALSE(bool(Space->writeInteger(End, 0xaabbccddeeff0011, 8)));
    EXPECT_FALSE(invoke(Kind, {Source, End}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::FilePartialStatus);
    EXPECT_EQ(llvm::cantFail(Space->readInteger(End, 8)), 0xaabbccddeeff0011u);
  }
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 0u);
  ASSERT_FALSE(bool(Space->protect(Base + Page, Page, Read | UserAccessible)));
  error(ServiceKind::Fstat64, {FD, Base + Page}, 14);
}

TEST_P(DarwinFileTest, UnknownMetadataAndDirectoriesAreNotFabricated) {
  auto FD = ok(ServiceKind::Open, {Base});
  for (auto Kind :
       {ServiceKind::Stat64, ServiceKind::Lstat64, ServiceKind::Fstat64}) {
    EXPECT_FALSE(
        invoke(Kind, {Kind == ServiceKind::Fstat64 ? FD : Base, Base + Page}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::FileMetadata);
  }
  for (auto FD : {0u, 1u, 2u}) {
    EXPECT_FALSE(invoke(ServiceKind::Fstat64, {FD, Base + Page}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::FileMetadata);
  }
  path("/");
  EXPECT_FALSE(invoke(ServiceKind::Stat64, {Base, Base + Page}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileDirectory);
  Options.reset();
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  EXPECT_FALSE(invoke(ServiceKind::Lstat64, {Base, Base + Page}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileInputs);
}

TEST(DarwinFileOptions,
     MetadataAdmissionRejectsUnknownPathsAndIncoherentValues) {
  DarwinFileOptions O;
  O.Files["/data"] = std::vector<uint8_t>(10);
  O.Metadata["/missing"] = darwin_test::metadata();
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::FileMetadataPath);
  O.Metadata.clear();
  auto &M = O.Metadata["/data"];
  for (unsigned Case = 0; Case != 8; ++Case) {
    M = darwin_test::metadata();
    switch (Case) {
    case 0:
      M.Mode = 0040644;
      break;
    case 1:
      M.Size = 9;
      break;
    case 2:
      M.BlockSize = uint32_t(INT32_MAX) + 1;
      break;
    case 3:
      M.Blocks = uint64_t(INT64_MAX) + 1;
      break;
    case 4:
      M.AccessTime.Nanoseconds = -1;
      break;
    case 5:
      M.ModificationTime.Nanoseconds = 1000000000;
      break;
    case 6:
      M.ChangeTime.Nanoseconds = -1;
      break;
    case 7:
      M.BirthTime.Nanoseconds = 1000000000;
      break;
    }
    EXPECT_EQ(llvm::toString(validateFileOptions(O)),
              diagnostic::FileMetadataOption);
  }
  M = darwin_test::metadata();
  ASSERT_FALSE(bool(validateFileOptions(O)));
}

INSTANTIATE_TEST_SUITE_P(OSPages, DarwinFileTest, testing::Values(4096, 16384));
} // namespace
} // namespace neverd::emulation::darwin_model
