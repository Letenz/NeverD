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
  EXPECT_EQ(ok(ServiceKind::Open, {Base}), 3u);
  EXPECT_EQ(ok(ServiceKind::Close, {3}), 0u);
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
  for (auto Text : {"relative", "./data", "../data"}) {
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
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMetadata);
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

TEST(DarwinFileOptions, DirectoryAdmissionIncludesImplicitPathsAndCWD) {
  DarwinFileOptions O;
  O.Files["/tree/data"] = {};
  O.Directories = {"/tree", "/tree/empty/deep"};
  O.WorkingDirectory = "/tree/empty";
  auto M = darwin_test::metadata(128);
  M.Mode = 0040755;
  O.Metadata["/"] = M;
  O.Metadata["/tree/empty"] = M;
  ASSERT_FALSE(bool(validateFileOptions(O)));
  for (auto Bad : {"", "relative", "/tree/data", "/absent", "/tree/../tree"}) {
    O.WorkingDirectory = Bad;
    auto E = validateFileOptions(O);
    EXPECT_TRUE(bool(E)) << Bad;
    llvm::consumeError(std::move(E));
  }
  O.WorkingDirectory = "/";
  for (auto Bad : {"/tree/data", "/tree/data/child", "/tree/", "relative"}) {
    O.Directories.insert(Bad);
    EXPECT_EQ(llvm::toString(validateFileOptions(O)),
              diagnostic::FileOptionPath);
    O.Directories.erase(Bad);
  }
  O.Metadata["/"].Size = uint64_t(INT64_MAX) + 1;
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::FileMetadataOption);
  O.Metadata["/"] = darwin_test::metadata();
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::FileMetadataOption);
  O = {};
  for (unsigned I = 0; I != 255; ++I)
    O.Directories.insert("/dir" + std::to_string(I));
  O.Files["/data"] = {};
  ASSERT_FALSE(bool(validateFileOptions(O)));
  O.Directories.insert("/extra");
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::FileOptionsLimit);
  O = {};
  O.Files["/data"] = std::vector<uint8_t>(darwin_file_limits::Bytes - 8);
  O.WorkingDirectory = "/"; // Six path bytes plus two CWD bytes fit exactly.
  ASSERT_FALSE(bool(validateFileOptions(O)));
  O.Directories.insert("/");
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::FileOptionsLimit);
}

TEST_P(DarwinFileTest, RelativeLookupWalksAncestorsAndPreservesErrorOrder) {
  Options->Directories.insert("/empty");
  path("/empty");
  auto Dir = ok(ServiceKind::Open, {Base, value::OpenDirectory});
  auto Dup = ok(ServiceKind::Dup, {Dir});
  path("/data");
  auto File = ok(ServiceKind::Open, {Base});
  error(ServiceKind::Open, {Base, value::OpenDirectory}, 20);
  for (auto Text : {"/data/.", "/data/..", "/data/", "/data//child"}) {
    path(Text);
    error(ServiceKind::Open, {Base}, 20);
  }
  path("/absent/../data");
  error(ServiceKind::Open, {Base}, 2);
  for (auto Text : {"//./data", "/empty/../../data", "/../data"}) {
    path(Text);
    auto FD = ok(ServiceKind::OpenAt, {999, Base});
    EXPECT_EQ(ok(ServiceKind::Read, {FD, Base + Page, 1}), 1u);
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page, 1)), 'a');
    ok(ServiceKind::Close, {FD});
  }
  path("../data");
  auto FD = ok(ServiceKind::OpenAt, {Dup, Base});
  ok(ServiceKind::Close, {FD});
  error(ServiceKind::OpenAt, {999, Base}, 9);
  error(ServiceKind::OpenAt, {File, Base}, 20);
  EXPECT_FALSE(invoke(ServiceKind::OpenAt, {0, Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileDirectoryKind);
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileWorkingDirectory);
  path("");
  error(ServiceKind::OpenAt, {999, Base}, 9);
  error(ServiceKind::OpenAt, {File, Base}, 20);
  error(ServiceKind::OpenAt, {Dir, Base}, 2);
  error(ServiceKind::Open, {Base}, 2);
  error(ServiceKind::OpenAt, {999, 0}, 14);
  Options->DescriptorLimit = 6;
  error(ServiceKind::OpenAt, {999, 0}, 24);
}

TEST_P(DarwinFileTest,
       WorkingDirectorySurvivesFailuresClosureAndFDReplacement) {
  Options->Files["/tree/data"] = {'t'};
  Options->Directories.insert("/empty");
  Options->WorkingDirectory = "/tree";
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  path("data");
  auto File = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Read, {File, Base + Page, 1}), 1u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page, 1)), 't');
  error(ServiceKind::Fchdir, {File}, 20);
  error(ServiceKind::Fchdir, {999}, 9);
  path("/missing");
  error(ServiceKind::Chdir, {Base}, 2);
  path("/data");
  error(ServiceKind::Chdir, {Base}, 20);
  path(".");
  auto Dir = ok(ServiceKind::Open, {Base});
  ok(ServiceKind::Fchdir, {Dir});
  ok(ServiceKind::Close, {Dir});
  path("/data");
  EXPECT_EQ(ok(ServiceKind::Open, {Base}), Dir);
  path("data");
  auto StillTree = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Read, {StillTree, Base + Page, 1}), 1u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page, 1)), 't');
  path("/empty");
  auto Empty = ok(ServiceKind::Open, {Base});
  ok(ServiceKind::Fchdir, {Empty});
  ok(ServiceKind::Dup2, {File, Empty});
  path("data");
  error(ServiceKind::Open, {Base}, 2);
  path("..");
  ok(ServiceKind::Chdir, {Base});
  path("data");
  auto RootFile = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Read, {RootFile, Base + Page, 1}), 1u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page, 1)), 'a');
}

TEST_P(DarwinFileTest, DirectoryMetadataReadAndSeekUseExplicitObservations) {
  Options->Directories.insert("/empty");
  auto M = darwin_test::metadata(128);
  M.Mode = 0040700;
  Options->Metadata["/empty"] = M;
  path("/empty");
  auto Dir = ok(ServiceKind::Open, {Base, value::OpenDirectory});
  EXPECT_EQ(ok(ServiceKind::Fcntl, {Dir, value::GetFileFlags}), 0u);
  for (auto Kind : {ServiceKind::Read, ServiceKind::Pread})
    for (unsigned Count : {0, 1})
      error(Kind, {Dir, 0, Count}, 21);
  error(ServiceKind::Pread, {Dir, 0, 0, UINT64_MAX}, 22);
  EXPECT_EQ(std::get<uint32_t>(Files->mappingSource(Dir)), 22u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Dir, 7, 0}), 7u);
  auto Dup = ok(ServiceKind::Dup, {Dir});
  EXPECT_EQ(ok(ServiceKind::Lseek, {Dup, 0, 1}), 7u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Dir, 0, 2}), 128u);
  error(ServiceKind::Lseek, {Dir, UINT64_MAX, 0}, 22);
  error(ServiceKind::Lseek, {Dir, uint64_t(-129), 1}, 22);
  error(ServiceKind::Lseek, {Dir, 0, 99}, 22);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Dir, INT64_MAX, 0}), uint64_t(INT64_MAX));
  error(ServiceKind::Lseek, {Dir, 1, 1}, 84);
  for (unsigned Whence : {3, 4})
    EXPECT_FALSE(invoke(ServiceKind::Lseek, {Dir, 0, Whence}));
  EXPECT_EQ(ok(ServiceKind::Lseek, {Dir, 0, 1}), uint64_t(INT64_MAX));
  EXPECT_EQ(ok(ServiceKind::FstatAt64, {Dir, 0, Base + Page, value::AtFDOnly}),
            0u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page + 4, 2)), 0040700u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page + 96, 8)), 128u);
  path("/");
  auto Root = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Lseek, {Root, 7, 0}), 7u);
  EXPECT_FALSE(invoke(ServiceKind::Lseek, {Root, 0, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMetadata);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Root, 0, 1}), 7u);
}

TEST_P(DarwinFileTest,
       FstatAtSharesPathOwnerAndGetPathCopiesCanonicalIdentity) {
  Options->Metadata["/data"] = darwin_test::metadata(6);
  Options->Directories.insert("/empty");
  path("/empty");
  auto Dir = ok(ServiceKind::Open, {Base});
  path("..//./data");
  auto File = ok(ServiceKind::OpenAt, {Dir, Base});
  auto Dup = ok(ServiceKind::Dup, {File});
  ok(ServiceKind::Close, {File});
  const auto Buffer = Base + Page;
  for (auto Flags : {0u, 0x20u, 0x800u, 0x820u}) {
    EXPECT_EQ(ok(ServiceKind::FstatAt64, {Dir, Base, Buffer, Flags}), 0u);
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer + 96, 8)), 6u);
  }
  EXPECT_EQ(ok(ServiceKind::FstatAt64, {Dup, 0, Buffer, 0x400}), 0u);
  error(ServiceKind::FstatAt64, {999, 0, 0, 1}, 22);
  error(ServiceKind::FstatAt64, {999, 0, 0}, 14);
  error(ServiceKind::FstatAt64, {999, 0, 0, 0x400}, 9);
  EXPECT_FALSE(invoke(ServiceKind::FstatAt64, {Dir, Base, Buffer, 0x200}));
  path("");
  error(ServiceKind::FstatAt64, {999, Base, 0}, 9);
  error(ServiceKind::FstatAt64, {Dup, Base, 0}, 20);
  path("/data");
  EXPECT_EQ(ok(ServiceKind::FstatAt64, {999, Base, Buffer}), 0u);
  ASSERT_FALSE(bool(Space->writeInteger(Buffer, UINT64_MAX, 8)));
  EXPECT_EQ(ok(ServiceKind::Fcntl, {Dup, 50, Buffer}), 0u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer, 8)), 0xffff00617461642fu);
  error(ServiceKind::Fcntl, {Dup, 50, 0}, 14);
  error(ServiceKind::Fcntl, {999, 50, 0}, 9);
  const auto End = Base + Page * 2 - 2;
  ASSERT_FALSE(bool(Space->writeInteger(End, 0xaaaa, 2)));
  EXPECT_FALSE(invoke(ServiceKind::Fcntl, {Dup, 50, End}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FilePartialPath);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(End, 2)), 0xaaaau);
  EXPECT_FALSE(invoke(ServiceKind::Fcntl, {0, 50, Buffer}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FilePathIdentity);
}

TEST_P(DarwinFileTest,
       DirectoryRecordsPreserveLayoutCookiesAndIndependentOpens) {
  Options->Directories.insert("/empty");
  Options->DirectoryContents["/"] = darwin_test::directoryContents();
  Options->DirectoryContents["/"].Entries[2].SeekOffset = UINT64_MAX;
  path("/");
  const auto A = ok(ServiceKind::Open, {Base});
  const auto B = ok(ServiceKind::Dup, {A});
  const auto C = ok(ServiceKind::Open, {Base});
  const auto Buffer = Base + 256, Position = Base + 2048;
  error(ServiceKind::GetDirEntries64, {A, Buffer, 63, Position}, 22);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {A, Buffer, 64, Position}), 64u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Position, 8)), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 22u);
  // Independent explicit wire bytes, including native tail padding.
  const auto Expected = llvm::fromHex(
      "2900000000000000000000000000000020000100042e00000000000000000000"
      "2900000000000000000000000000000020000200042e2e000000000000000000");
  std::array<uint8_t, 64> Records;
  ASSERT_FALSE(bool(Space->read(Buffer, Records)));
  EXPECT_EQ(llvm::toHex(Records), llvm::toHex(Expected));
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {B, Buffer, 32, Position}), 32u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer + 8, 8)), UINT64_MAX);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Position, 8)), 22u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}),
            7u); // Cookies need not increase.
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {B, Buffer, 32, Position}), 32u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer, 8)),
            0xfedcba9876543210ULL);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 99u);
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {B, 0, 1, Position}), 0u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Position, 8)), 99u);
  error(ServiceKind::GetDirEntries64, {B, 0, 0, Position}, 22);
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {C, Buffer, 128, Position}), 128u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Position, 8)), 0u);
  ok(ServiceKind::Lseek, {B, 0, 0});
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {B, Buffer, 128, Position}), 128u);
  ok(ServiceKind::Lseek, {B, 88, 0});
  EXPECT_FALSE(
      invoke(ServiceKind::GetDirEntries64, {B, Buffer, 128, Position}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryPosition);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 88u);
}

TEST_P(DarwinFileTest, DirectoryEOFAndExtendedFlagsRespectOriginalBufferEnd) {
  Options->Directories.insert("/empty");
  Options->DirectoryContents["/"] = darwin_test::directoryContents();
  path("/");
  auto FD = ok(ServiceKind::Open, {Base});
  const auto Buffer = Base + 256, Position = Base + 2048;
  for (uint64_t Count : {1023, 1024, 4096}) {
    ok(ServiceKind::Lseek, {FD, 0, 0});
    ASSERT_FALSE(bool(Space->writeInteger(Buffer + Count - 4, 0xaaaaaaaa, 4)));
    EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {FD, Buffer, Count, Position}),
              128u);
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer + Count - 4, 4)),
              Count < 1024 ? 0xaaaaaaaau : 1u);
  }
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {FD, 0, 64, Position}), 0u);
  error(ServiceKind::GetDirEntries64, {FD, 0, 1024, Position}, 14);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Position, 8)), 99u);
  // Count is capped for record payload, but its unsigned suffix address wraps.
  ok(ServiceKind::Lseek, {FD, 0, 0});
  ASSERT_FALSE(bool(Space->writeInteger(Buffer - 5, 0xaaaaaaaa, 4)));
  EXPECT_EQ(
      ok(ServiceKind::GetDirEntries64, {FD, Buffer, UINT64_MAX, Position}),
      128u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer - 5, 4)), 1u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 99u);
  // Extended output must explicitly clear EOF when a whole-record batch is
  // shorter than the remaining snapshot. The next call resumes at its cookie.
  for (unsigned I = 0; I != 4; ++I) {
    std::string Name(255, 'x');
    Name.back() = 'a' + I;
    Options->Files['/' + Name] = {};
    Options->DirectoryContents["/"].Entries.push_back(
        {Name, 50 + I, 8, 100 + I, 0});
  }
  ok(ServiceKind::Lseek, {FD, 0, 0});
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {FD, Buffer, 1024, Position}),
            968u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer + 1020, 4)), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 102u);
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {FD, Buffer, 1024, Position}),
            280u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Position, 8)), 102u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer + 1020, 4)), 1u);
}

TEST_P(DarwinFileTest, DirectoryCopyPhasesRetainEarlierEffectsAndAliasInOrder) {
  Options->Directories.insert("/empty");
  Options->DirectoryContents["/"] = darwin_test::directoryContents();
  path("/");
  auto FD = ok(ServiceKind::Open, {Base});
  const auto Buffer = Base + 256, Position = Base + 2048;
  error(ServiceKind::GetDirEntries64, {999, 0, 0, 0}, 9);
  error(ServiceKind::GetDirEntries64, {FD, 0, 128, Position}, 14);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 0u);
  error(ServiceKind::GetDirEntries64, {FD, Buffer, 1024, 0}, 14);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer, 8)), 41u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 99u);
  // A failing suffix occurs after data, offset advancement and position copy.
  ok(ServiceKind::Lseek, {FD, 0, 0});
  ASSERT_FALSE(bool(Space->writeInteger(Position, 0xaaaaaaaa, 8)));
  error(ServiceKind::GetDirEntries64, {FD, Buffer, Page * 4, Position}, 14);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Position, 8)), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 99u);
  // With overlapping outputs, position overwrites data and flags overwrite it
  // last.
  ok(ServiceKind::Lseek, {FD, 0, 0});
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {FD, Buffer, 128, Buffer}), 128u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer, 8)), 0u);
  const auto Suffix = Buffer + 1020;
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {FD, Buffer, 1024, Suffix}), 0u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Suffix, 8)), 1u);
  const auto End = Base + Page * 2 - 4;
  ok(ServiceKind::Lseek, {FD, 0, 0});
  ASSERT_FALSE(bool(Space->writeInteger(End, 0xbbbbbbbb, 4)));
  EXPECT_FALSE(invoke(ServiceKind::GetDirEntries64, {FD, End, 128, Position}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryPartialData);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(End, 4)), 0xbbbbbbbbu);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::GetDirEntries64, {FD, Buffer, 128, End}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryPartialPosition);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 99u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(End, 4)), 0xbbbbbbbbu);
  // Position succeeded before an individually partial extended-flag copy.
  const auto PartialCount = Base + Page * 2 - 2 - Buffer + 4;
  ok(ServiceKind::Lseek, {FD, 0, 0});
  EXPECT_FALSE(invoke(ServiceKind::GetDirEntries64,
                      {FD, Buffer, PartialCount, Position}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryPartialFlags);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Position, 8)), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 99u);
}

TEST_P(DarwinFileTest, DirectoryLongRecordsAndMissingObservationsStayExplicit) {
  auto File = ok(ServiceKind::Open, {Base});
  error(ServiceKind::GetDirEntries64, {File, 0, 0, 0}, 22);
  path("/");
  auto Dir = ok(ServiceKind::Open, {Base});
  EXPECT_FALSE(invoke(ServiceKind::GetDirEntries64,
                      {Dir, Base + 256, 1024, Base + 2048}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryContents);
  Options->Directories.insert("/empty");
  const std::string Name(255, 'x');
  Options->Files['/' + Name] = {};
  auto &C = Options->DirectoryContents["/"];
  C = darwin_test::directoryContents();
  C.Entries.push_back({Name, 43, 8, 100, 0});
  EXPECT_EQ(
      ok(ServiceKind::GetDirEntries64, {Dir, Base + 256, 128, Base + 2048}),
      128u);
  error(ServiceKind::GetDirEntries64, {Dir, Base + 256, 279, Base + 2048}, 22);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Dir, 0, 1}), 99u);
  EXPECT_EQ(
      ok(ServiceKind::GetDirEntries64, {Dir, Base + 256, 280, Base + 2048}),
      280u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + 256 + 16, 2)), 280u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + 256 + 18, 2)), 255u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + 256 + 276, 4)), 0u);
}

TEST(DarwinFileOptions,
     DirectorySnapshotAdmissionRejectsIncoherentObservations) {
  DarwinFileOptions Original;
  Original.Files["/data"] = {};
  Original.Directories.insert("/empty");
  Original.DirectoryContents["/"] = darwin_test::directoryContents();
  Original.DirectoryContents["/empty"] = {
      {{".", 42, 4, 11, 0}, {"..", 41, 4, 22, 0}}, 1};
  ASSERT_FALSE(bool(
      validateFileOptions(Original))); // Cookies are local to each directory.
  for (unsigned Case = 0; Case != 17; ++Case) {
    auto O = Original;
    auto &C = O.DirectoryContents["/"];
    switch (Case) {
    case 0:
      C.MinimumBufferSize = 0;
      break;
    case 1:
      C.MinimumBufferSize = 128 * 1024 * 1024 + 1;
      break;
    case 2:
      C.Entries[0].Inode = 0;
      break;
    case 3:
      C.Entries[0].NextOffset = 0;
      break;
    case 4:
      C.Entries[0].NextOffset = uint64_t(INT64_MAX) + 1;
      break;
    case 5:
      C.Entries[0].NextOffset = C.Entries[1].NextOffset;
      break;
    case 6:
      C.Entries[0].Type = 8;
      break;
    case 7:
      C.Entries[2].Type = 10;
      break;
    case 8:
      C.Entries[1].Name = ".";
      break;
    case 9:
      C.Entries[2].Name = "absent";
      break;
    case 10:
      C.Entries[2].Name = "empty/child";
      break;
    case 11:
      C.Entries[2].Name = std::string(256, 'a');
      break;
    case 12:
      C.Entries[2].Name = std::string("bad\0name", 8);
      break;
    case 13:
      C.Entries[0].Inode = 43;
      break; // Root . and .. name the same object.
    case 14:
      O.DirectoryContents["/empty"].Entries[0].Inode = 43;
      break;
    case 15:
      C.Entries.pop_back();
      break;
    case 16:
      C.Entries[0].MinimumBufferSize = UINT32_MAX;
      break;
    }
    SCOPED_TRACE(Case);
    EXPECT_EQ(llvm::toString(validateFileOptions(O)),
              diagnostic::DirectoryContentsOption);
  }
  auto O = Original;
  O.Metadata["/data"] = darwin_test::metadata(0);
  ASSERT_FALSE(bool(validateFileOptions(O)));
  ++O.Metadata["/data"].Inode;
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::DirectoryContentsOption);
  O = Original;
  O.DirectoryContents["/"].Entries[2].Type = 0;
  O.DirectoryContents["/"].Entries[2].SeekOffset = UINT64_MAX;
  ASSERT_FALSE(bool(validateFileOptions(O)));
  for (auto Bad : {"/data", "/absent", "relative"}) {
    O.DirectoryContents[Bad] = {};
    EXPECT_EQ(llvm::toString(validateFileOptions(O)),
              diagnostic::DirectoryContentsOption);
    O.DirectoryContents.erase(Bad);
  }
}

TEST(DarwinFileOptions, DirectorySnapshotBoundsDeduplicateMetadataPaths) {
  DarwinFileOptions O;
  for (unsigned I = 0; I != 254; ++I)
    O.Directories.insert("/dir" + std::to_string(I));
  O.Files["/tree/data"] = {};
  auto M = darwin_test::metadata(128);
  M.Mode = 0040755;
  M.Inode = 41;
  O.Metadata["/tree"] = M;
  auto &C = O.DirectoryContents["/tree"];
  C = {{{".", 41, 4, 1, 0}, {"..", 40, 4, 2, 0}, {"data", 42, 8, 3, 0}}, 1};
  ASSERT_FALSE(
      bool(validateFileOptions(O))); // 255 nodes + shared implicit /tree = 256.
  O.Directories.insert("/extra");
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::FileOptionsLimit);
  O = {};
  O.Directories.insert("/empty");
  O.Files["/data"] = std::vector<uint8_t>(darwin_file_limits::Bytes - 143);
  O.DirectoryContents["/"] = darwin_test::directoryContents();
  // /data NUL=6, /empty NUL=7, implicit root key=2, four records=128.
  ASSERT_FALSE(bool(validateFileOptions(O)));
  O.Files["/data"].push_back(0);
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::FileOptionsLimit);
  O = {};
  O.DirectoryContents["/"] = {};
  O.DirectoryContents["/"].MinimumBufferSize = 1;
  O.DirectoryContents["/"].Entries.resize(darwin_file_limits::DirectoryEntries +
                                          1);
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::FileOptionsLimit);
}

INSTANTIATE_TEST_SUITE_P(OSPages, DarwinFileTest, testing::Values(4096, 16384));
} // namespace
} // namespace neverd::emulation::darwin_model
