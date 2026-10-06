//===- DarwinVectorTests.cpp - Ordered Darwin vector I/O contracts -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinFileTestData.h"
#include "gtest/gtest.h"
#include "os/darwin/kernel/DarwinFiles.h"

#include "neverd/emulation/AddressSpace.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

namespace neverd::emulation::darwin_model {
namespace {
class VectorMemory : public GuestMemory {
  GuestMemory &Memory;

public:
  std::optional<uint64_t> FailAccess, FailRead, FailWrite;
  mutable std::vector<uint64_t> Queries;
  explicit VectorMemory(GuestMemory &Memory) : Memory(Memory) {}
  llvm::Error map(uint64_t A, uint64_t S, unsigned P) override {
    return Memory.map(A, S, P);
  }
  llvm::Error protect(uint64_t A, uint64_t S, unsigned P) override {
    return Memory.protect(A, S, P);
  }
  llvm::Error read(uint64_t A, llvm::MutableArrayRef<uint8_t> B) override {
    if (FailRead == A) {
      B.front() = 'x'; // Scratch writes must not become file/capture effects.
      return failure("vector read transport failed");
    }
    return Memory.read(A, B);
  }
  llvm::Error write(uint64_t A, llvm::ArrayRef<uint8_t> B) override {
    if (FailWrite == A)
      return failure("vector write transport failed");
    return Memory.write(A, B);
  }
  llvm::Expected<bool> canAccess(uint64_t A, uint64_t S,
                                 unsigned P) const override {
    Queries.push_back(A);
    if (FailAccess == A)
      return failure("vector preflight failed");
    return Memory.canAccess(A, S, P);
  }
};
class DarwinVectorTest : public testing::TestWithParam<uint64_t> {
protected:
  static constexpr uint64_t Base = 0x100000, Input = Base + 128,
                            Output = Base + 256, Status = Base + 512;
  std::shared_ptr<AddressSpace> Space;
  std::unique_ptr<VectorMemory> Memory;
  std::optional<DarwinFileOptions> Options;
  std::unique_ptr<DarwinFiles> Files;
  ProcessResult Result{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn, "memory-only test"};
  uint64_t Page, Header;
  using Span = std::pair<uint64_t, uint64_t>;
  void SetUp() override {
    Page = GetParam();
    Header = Base + Page + 3; // LP64 iovec need not be aligned.
    auto Physical = PhysicalMemory::create(Page * 8);
    ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
    auto Created = AddressSpace::create(*Physical, Page * 8);
    ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
    Space = std::move(*Created);
    ASSERT_FALSE(
        bool(Space->map(Base, Page * 6, Read | Write | UserAccessible)));
    Memory = std::make_unique<VectorMemory>(*Space);
    Options.emplace();
    Options->Files["/data"] = {'0', '1', '2', '3', '4',
                               '5', '6', '7', '8', '9'};
    Options->StandardInput = {'i', 0, 0xff, 'n'};
    Options->WritableFiles.insert("/data");
    Options->Metadata["/data"] = darwin_test::mutationMetadata(10);
    Options->MutationPolicies["/data"] = {4096, {-7, 123456789}};
    reset();
    put(Base, llvm::StringRef("/data\0", 6));
    put(Input, "ABCDE");
    put(Output, std::string(64, '\xa5'));
  }
  void reset(uint64_t Limit = process_defaults::Output) {
    Files = std::make_unique<DarwinFiles>(*Memory, Options, Limit);
  }
  void put(uint64_t A, llvm::StringRef Bytes) {
    ASSERT_FALSE(bool(Space->write(A, llvm::arrayRefFromStringRef(Bytes))));
  }
  std::string bytes(uint64_t A, size_t N) {
    std::vector<uint8_t> B(N);
    llvm::cantFail(Space->read(A, B));
    return std::string(B.begin(), B.end());
  }
  uint64_t vectors(llvm::ArrayRef<Span> Spans, uint64_t Address = 0) {
    if (!Address)
      Address = Header;
    std::vector<uint8_t> B(Spans.size() * 16);
    for (size_t I = 0; I < Spans.size(); ++I) {
      llvm::support::endian::write64le(B.data() + I * 16, Spans[I].first);
      llvm::support::endian::write64le(B.data() + I * 16 + 8, Spans[I].second);
    }
    llvm::cantFail(Space->write(Address, B));
    return Address;
  }
  std::optional<ServiceResult> invoke(ServiceKind Kind,
                                      std::array<uint64_t, 6> A) {
    auto R = Files->handle(Kind, {0, 0, A, std::nullopt}, Result);
    EXPECT_TRUE(bool(R)) << (R ? "" : llvm::toString(R.takeError()));
    return R ? *R : std::nullopt;
  }
  uint64_t ok(ServiceKind Kind, std::array<uint64_t, 6> A) {
    auto R = invoke(Kind, A);
    EXPECT_TRUE(R.has_value()) << Result.Diagnostic;
    if (!R)
      return UINT64_MAX;
    EXPECT_FALSE(R->Error) << R->Value;
    return R->Value;
  }
  void error(ServiceKind Kind, std::array<uint64_t, 6> A, uint64_t Code) {
    auto R = invoke(Kind, A);
    ASSERT_TRUE(R.has_value()) << Result.Diagnostic;
    EXPECT_TRUE(R->Error);
    EXPECT_EQ(R->Value, Code);
  }
  void unsupported(ServiceKind Kind, std::array<uint64_t, 6> A,
                   const char *Reason) {
    EXPECT_FALSE(invoke(Kind, A));
    EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(Result.Diagnostic, Reason);
  }
  uint64_t open(uint32_t Flags = 2) {
    return ok(ServiceKind::Open, {Base, Flags});
  }
  uint64_t cursor(uint64_t FD) { return ok(ServiceKind::Lseek, {FD, 0, 1}); }
  uint64_t flags(uint64_t FD) { return ok(ServiceKind::Fcntl, {FD, 3}); }
  std::string status(uint64_t FD) {
    EXPECT_EQ(ok(ServiceKind::Fstat64, {FD, Status}), 0u);
    return bytes(Status, 144);
  }
  void contents(uint64_t FD, llvm::StringRef Expected) {
    EXPECT_EQ(ok(ServiceKind::Pread, {FD, Output, 64}), Expected.size());
    EXPECT_EQ(bytes(Output, Expected.size()), Expected);
  }
};

TEST_P(DarwinVectorTest, CountAndCompleteHeaderPrecedeDescriptorLookup) {
  const auto FD = open();
  for (auto K : {ServiceKind::Readv, ServiceKind::Writev, ServiceKind::Preadv,
                 ServiceKind::Pwritev}) {
    for (uint64_t Count :
         {uint64_t(0), uint64_t(1025), uint64_t(0xffffffff), UINT64_MAX})
      error(K, {999, 1, Count}, 22);
    error(K, {999, 1, 1}, 14);
    vectors({{1, UINT64_MAX}});
    error(K, {999, Header, 1}, 9);
    std::vector<Span> Zeros(1024, {UINT64_MAX, 0});
    vectors(Zeros);
    EXPECT_EQ(ok(K, {FD, Header, 1024}), 0u);
    EXPECT_EQ(ok(K, {FD, Header, 0xdeadbeef00000002ULL}), 0u);
  }
  for (uint64_t Negative : {UINT64_MAX, UINT64_MAX - 1, uint64_t(1) << 63}) {
    error(ServiceKind::Pwritev, {999, 1, 2, Negative}, 22);
    error(ServiceKind::Preadv, {999, 1, 2, Negative}, 14);
  }
  EXPECT_EQ(cursor(FD), 0u);
  EXPECT_EQ(flags(FD), 2u);
}

TEST_P(DarwinVectorTest, HeaderPermissionsAndPartialMemoryStopBeforeEffects) {
  const auto FD = open();
  const uint64_t Tail = Base + Page * 6 - 8;
  unsupported(ServiceKind::Writev, {999, Tail, 1},
              diagnostic::FilePartialVectors);
  error(ServiceKind::Readv, {999, 0x800000000000ULL, 1}, 14);
  vectors({{Input, 2}, {Input + 2, 1}});
  ASSERT_FALSE(bool(Space->protect(Base + Page, Page, Read | UserAccessible)));
  EXPECT_EQ(ok(ServiceKind::Writev, {FD, Header, 2}), 3u);
  EXPECT_EQ(cursor(FD), 3u);
  ASSERT_FALSE(bool(Space->protect(Base + Page, Page, Write | UserAccessible)));
  error(ServiceKind::Writev, {FD, Header, 2}, 14);
  EXPECT_EQ(cursor(FD), 3u);
  contents(FD, "ABC3456789");
}

TEST_P(DarwinVectorTest, AccessAndStreamKindPrecedeAggregateLengthValidation) {
  const auto RO = open(0), WO = open(1), RW = open();
  for (auto Spans : {std::vector<Span>{{1, UINT64_MAX}},
                     std::vector<Span>{{1, INT64_MAX}, {1, 1}},
                     std::vector<Span>{{1, 0x80000000ULL}}}) {
    vectors(Spans);
    const auto N = Spans.size();
    for (auto K : {ServiceKind::Readv, ServiceKind::Preadv}) {
      error(K, {WO, Header, N}, 9);
      error(K, {RW, Header, N}, 22);
      error(K, {1, Header, N}, 9);
    }
    for (auto K : {ServiceKind::Writev, ServiceKind::Pwritev}) {
      error(K, {RO, Header, N}, 9);
      error(K, {RW, Header, N}, 22);
      error(K, {0, Header, N}, 9);
    }
    error(ServiceKind::Preadv, {0, Header, N}, 29);
    error(ServiceKind::Pwritev, {1, Header, N}, 29);
  }
  vectors({{Output, 0x80000000ULL}});
  EXPECT_EQ(ok(ServiceKind::Readv, {0, Header, 1}), 4u);
  EXPECT_EQ(bytes(Output, 4), std::string("i\0\xffn", 4));
  EXPECT_FALSE(invoke(ServiceKind::Writev, {1, Header, 1}));
  EXPECT_EQ(Result.Stop, ProcessStopReason::OutputLimit);
  EXPECT_TRUE(Result.StandardOutput.empty());
  vectors({{1, INT64_MAX}, {1, 1}});
  error(ServiceKind::Writev, {1, Header, 2}, 22);
  error(ServiceKind::Readv, {0, Header, 2}, 22);
}

TEST_P(DarwinVectorTest, ZeroSpansRetainOffsetTypeAndMissingInputChecks) {
  const auto FD = open();
  vectors({{UINT64_MAX, 0}, {0x800000000000ULL, 0}});
  for (auto K : {ServiceKind::Readv, ServiceKind::Writev})
    EXPECT_EQ(ok(K, {FD, Header, 2}), 0u);
  EXPECT_EQ(ok(ServiceKind::Preadv, {FD, Header, 2, INT64_MAX}), 0u);
  error(ServiceKind::Pwritev, {FD, Header, 2, INT64_MAX}, 27);
  error(ServiceKind::Preadv, {FD, Header, 2, UINT64_MAX - 1}, 22);
  put(Base + 32, llvm::StringRef("/\0", 2));
  const auto Dir = ok(ServiceKind::Open, {Base + 32});
  error(ServiceKind::Readv, {Dir, Header, 2}, 21);
  error(ServiceKind::Writev, {Dir, Header, 2}, 9);
  Options->StandardInput.reset();
  reset(0);
  EXPECT_EQ(ok(ServiceKind::Readv, {0, Header, 2}), 0u);
  EXPECT_EQ(ok(ServiceKind::Writev, {1, Header, 2}), 0u);
  vectors({{1, 1}});
  unsupported(ServiceKind::Readv, {0, Header, 1}, diagnostic::FileInput);
}

TEST_P(DarwinVectorTest, ReadOrderOverlapEOFAndDuplicatedCursors) {
  const auto FD = open(), Dup = ok(ServiceKind::Dup, {FD});
  vectors({{UINT64_MAX, 0}, {Output + 1, 3}, {Output + 1, 3}});
  EXPECT_EQ(ok(ServiceKind::Readv, {Dup, Header, 3}), 6u);
  EXPECT_EQ(bytes(Output, 5), std::string("\xa5"
                                          "345\xa5",
                                          5));
  EXPECT_EQ(cursor(FD), 6u);
  vectors({{Output, 12}, {1, 3}});
  EXPECT_EQ(ok(ServiceKind::Readv, {FD, Header, 2}), 4u);
  EXPECT_EQ(bytes(Output, 4), "6789");
  EXPECT_EQ(cursor(Dup), 10u);
  vectors({{1, 3}});
  EXPECT_EQ(ok(ServiceKind::Readv, {FD, Header, 1}), 0u);
  vectors({{Output, 2}, {Output + 2, 3}});
  EXPECT_EQ(ok(ServiceKind::Preadv, {FD, Header, 2, 1}), 5u);
  EXPECT_EQ(bytes(Output, 5), "12345");
  EXPECT_EQ(cursor(FD), 10u);
}

TEST_P(DarwinVectorTest, ReadSnapshotsMetadataBeforeOverlappingOutputs) {
  const auto FD = open();
  vectors({{Header + 16, 8}, {Output + 1, 2}});
  EXPECT_EQ(ok(ServiceKind::Readv, {FD, Header, 2}), 10u);
  EXPECT_EQ(bytes(Header + 16, 8), "01234567");
  EXPECT_EQ(bytes(Output, 4), std::string("\xa5"
                                          "89\xa5",
                                          4));
  EXPECT_EQ(cursor(FD), 10u);
}

TEST_P(DarwinVectorTest, ReadFaultAndTransportKeepCompletedEarlierSpans) {
  const auto FD = open();
  const uint64_t Partial = Base + Page * 6 - 2;
  for (uint64_t Bad : {uint64_t(1), Partial}) {
    EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 2, 0}), 2u);
    put(Partial, "zz");
    vectors({{Output, 3}, {Bad, 3}});
    if (Bad == 1)
      error(ServiceKind::Readv, {FD, Header, 2}, 14);
    else
      unsupported(ServiceKind::Readv, {FD, Header, 2},
                  diagnostic::FilePartialRead);
    EXPECT_EQ(bytes(Output, 3), "234");
    EXPECT_EQ(bytes(Partial, 2), "zz");
    EXPECT_EQ(cursor(FD), 5u);
  }
  vectors({{Output, 1}, {Output + 8, 2}});
  Memory->FailWrite = Output + 8;
  auto Failed = Files->handle(ServiceKind::Readv,
                              {0, 0, {FD, Header, 2}, std::nullopt}, Result);
  ASSERT_FALSE(bool(Failed));
  EXPECT_EQ(llvm::toString(Failed.takeError()),
            "vector write transport failed");
  EXPECT_EQ(bytes(Output, 1), "5");
  EXPECT_EQ(cursor(FD), 6u);
  vectors({{Output, 2}, {1, 2}});
  error(ServiceKind::Preadv, {FD, Header, 2, 1}, 14);
  EXPECT_EQ(bytes(Output, 2), "12");
  EXPECT_EQ(cursor(FD), 6u);
}

TEST_P(DarwinVectorTest, WholeWriteFaultKeepsPrefixAndInvalidatesMetadata) {
  const auto FD = open(), Dup = ok(ServiceKind::Dup, {FD});
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 2, 0}), 2u);
  const uint64_t Partial = Base + Page * 6 - 2;
  vectors({{Input, 3}, {1, 3}, {Partial, 3}});
  Memory->Queries.clear();
  error(ServiceKind::Writev, {FD, Header, 3}, 14);
  EXPECT_FALSE(llvm::is_contained(Memory->Queries, Partial));
  EXPECT_EQ(cursor(Dup), 5u);
  EXPECT_EQ(flags(Dup), 0x10002u);
  contents(FD, "01ABC56789");
  unsupported(ServiceKind::Fstat64, {FD, Status},
              diagnostic::FileMutatedMetadata);
  vectors({{Input, 1}});
  EXPECT_EQ(ok(ServiceKind::Pwritev, {FD, Header, 1, 9}), 1u);
  EXPECT_EQ(cursor(FD), 5u);
  unsupported(ServiceKind::Fstat64, {FD, Status},
              diagnostic::FileMutatedMetadata);
  contents(FD, "01ABC5678A");
}

TEST_P(DarwinVectorTest, PositionedFaultGrowthChargesOnlyTheCompletedPrefix) {
  Options->Files["/fill"] = {};
  Options->WritableFiles.insert("/fill");
  reset();
  const auto FD = open(), Dup = ok(ServiceKind::Dup, {FD});
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 2, 0}), 2u);
  vectors({{Input, 3}, {1, 17}});
  error(ServiceKind::Pwritev, {FD, Header, 2, 8192}, 14);
  EXPECT_EQ(cursor(FD), 2u);
  EXPECT_EQ(cursor(Dup), 2u);
  EXPECT_EQ(flags(Dup), 0x10002u);
  EXPECT_EQ(ok(ServiceKind::Pread, {FD, Output, 32, 8190}), 5u);
  EXPECT_EQ(bytes(Output, 5), std::string("\0\0ABC", 5));
  const auto Independent = open(0);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Independent, 0, 2}), 8195u);
  EXPECT_EQ(cursor(FD), 2u);
  unsupported(ServiceKind::Fstat64, {FD, Status},
              diagnostic::FileMutatedMetadata);
  put(Base + 32, llvm::StringRef("/fill\0", 6));
  const auto Fill = ok(ServiceKind::Open, {Base + 32, 2});
  // Five six-byte path references (two files, two grants, one policy), four
  // stdin bytes, and only the completed sparse file extent consume storage.
  const uint64_t Remaining = darwin_file_limits::Bytes - 5 * 6 - 4 - 8195;
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Fill, Remaining}), 0u);
  unsupported(ServiceKind::Ftruncate, {Fill, Remaining + 1},
              diagnostic::FileMutationLimit);
  EXPECT_EQ(cursor(Dup), 2u);
  unsupported(ServiceKind::Fstat64, {FD, Status},
              diagnostic::FileMutatedMetadata);
  EXPECT_EQ(Options->Files.at("/data").size(), 10u);
}

TEST_P(DarwinVectorTest, AppendClipsWholeRequestBeforeChoosingEOF) {
  const auto FD = open(10), Dup = ok(ServiceKind::Dup, {FD});
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, uint64_t(INT64_MAX) - 2, 0}),
            uint64_t(INT64_MAX) - 2);
  vectors({{Input, 1}, {Input + 1, 2}, {1, 0}});
  EXPECT_EQ(ok(ServiceKind::Writev, {Dup, Header, 3}), 2u);
  EXPECT_EQ(cursor(FD), 12u);
  contents(FD, "0123456789AB");
  EXPECT_EQ(ok(ServiceKind::Pwritev, {FD, Header, 3, 1}), 3u);
  EXPECT_EQ(cursor(Dup), 12u);
  contents(FD, "0ABC456789AB");
  vectors({{Input, 3}, {1, 2}});
  error(ServiceKind::Writev, {FD, Header, 2}, 14);
  EXPECT_EQ(cursor(Dup), 15u);
  contents(FD, "0ABC456789ABABC");
}

TEST_P(DarwinVectorTest, FirstAppendFaultChoosesEOFWithoutMarkingWritten) {
  const auto FD = open(10);
  vectors({{1, 3}});
  error(ServiceKind::Writev, {FD, Header, 1}, 14);
  EXPECT_EQ(cursor(FD), 10u);
  EXPECT_EQ(flags(FD), 10u);
  contents(FD, "0123456789");
  unsupported(ServiceKind::Fstat64, {FD, Status},
              diagnostic::FileMutatedMetadata);
}

TEST_P(DarwinVectorTest, PartialSourcesAndLateTransportFailuresAreAtomic) {
  const auto FD = open();
  const auto Before = status(FD);
  vectors({{Input, 3}, {Base + Page * 6 - 2, 3}});
  unsupported(ServiceKind::Writev, {FD, Header, 2},
              diagnostic::FilePartialWrite);
  EXPECT_EQ(status(FD), Before);
  EXPECT_EQ(cursor(FD), 0u);
  for (bool Access : {true, false}) {
    vectors({{Input, 2}, {Input + 2, 3}});
    (Access ? Memory->FailAccess : Memory->FailRead) = Input + 2;
    auto Failed = Files->handle(ServiceKind::Writev,
                                {0, 0, {FD, Header, 2}, std::nullopt}, Result);
    ASSERT_FALSE(bool(Failed));
    EXPECT_EQ(llvm::toString(Failed.takeError()),
              Access ? "vector preflight failed"
                     : "vector read transport failed");
    Memory->FailAccess.reset();
    Memory->FailRead.reset();
    EXPECT_EQ(status(FD), Before);
    EXPECT_EQ(cursor(FD), 0u);
    EXPECT_EQ(flags(FD), 2u);
    contents(FD, "0123456789");
  }
}

TEST_P(DarwinVectorTest, StorageAndMappingAdmissionPrecedeAnyVectorEffects) {
  const auto FD = open();
  const auto Before = status(FD);
  vectors({{Input, 2}, {1, 1}});
  {
    auto Lease = Files->mappingSource(FD);
    ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Lease));
    unsupported(ServiceKind::Writev, {FD, Header, 2},
                diagnostic::FileMutationMapping);
  }
  unsupported(ServiceKind::Pwritev,
              {FD, Header, 2, darwin_file_limits::Bytes - 2},
              diagnostic::FileMutationLimit);
  EXPECT_EQ(cursor(FD), 0u);
  EXPECT_EQ(flags(FD), 2u);
  EXPECT_EQ(status(FD), Before);
  contents(FD, "0123456789");
}

TEST_P(DarwinVectorTest,
       SuccessfulSparseWriteSharesAllocationAndMetadataOwner) {
  const auto FD = open();
  vectors({{Input, 1}, {Input + 1, 2}});
  EXPECT_EQ(ok(ServiceKind::Pwritev, {FD, Header, 2, 8191}), 3u);
  const auto M = status(FD);
  EXPECT_EQ(llvm::support::endian::read64le(M.data() + 96), 8194u);
  EXPECT_EQ(llvm::support::endian::read64le(M.data() + 104), 24u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 3}), 8194u);
  EXPECT_EQ(cursor(FD), 8194u);
  EXPECT_EQ(ok(ServiceKind::Pread, {FD, Output, 5, 8189}), 5u);
  EXPECT_EQ(bytes(Output, 5), std::string("\0\0ABC", 5));
  EXPECT_EQ(flags(FD), 0x10002u);
  EXPECT_EQ(Options->Files.at("/data").size(), 10u);
}

TEST_P(DarwinVectorTest,
       CaptureRetainsFaultPrefixesAcrossRedirectedDescriptions) {
  const auto Saved = ok(ServiceKind::Dup, {1});
  EXPECT_EQ(ok(ServiceKind::Dup2, {2, 1}), 1u);
  put(Input, llvm::StringRef("A\0\xff"
                             "B",
                             4));
  vectors({{Input, 2}, {Input + 2, 2}, {1, 1}});
  error(ServiceKind::Writev, {Saved, Header, 3}, 14);
  error(ServiceKind::Writev, {1, Header, 3}, 14);
  EXPECT_EQ(Result.StandardOutput, std::string("A\0\xff"
                                               "B",
                                               4));
  EXPECT_EQ(Result.StandardError, Result.StandardOutput);
  EXPECT_EQ(flags(Saved), 0x10001u);
  EXPECT_EQ(flags(2), 0x10001u);
  EXPECT_EQ(ok(ServiceKind::Close, {Saved}), 0u);
  const auto FD = open();
  EXPECT_EQ(FD, Saved);
  EXPECT_EQ(ok(ServiceKind::Writev, {FD, Header, 2}), 4u);
  EXPECT_EQ(Result.StandardOutput.size(), 4u);
}

TEST_P(DarwinVectorTest,
       CapturePartialSpanAndFullRangeBoundaryHaveDistinctEffects) {
  const uint64_t Partial = Base + Page * 6 - 2;
  put(Partial, "xy");
  vectors({{Input, 2}, {Partial, 4}, {Input + 2, 1}});
  error(ServiceKind::Writev, {1, Header, 3}, 14);
  EXPECT_EQ(Result.StandardOutput, "ABxy");
  // Map the last CPU page so a guessed prefix would incorrectly be readable.
  ASSERT_FALSE(bool(Space->map(0x800000000000ULL - Page, Page,
                               Read | Write | UserAccessible)));
  vectors({{Input, 3}, {0x800000000000ULL - 2, 4}});
  error(ServiceKind::Writev, {2, Header, 2}, 14);
  EXPECT_EQ(Result.StandardError, "ABC");
  vectors({{UINT64_MAX, 0}, {UINT64_MAX - 1, 4}});
  error(ServiceKind::Writev, {2, Header, 2}, 14);
  EXPECT_EQ(Result.StandardError, "ABC");
}

TEST_P(DarwinVectorTest, CombinedOutputBudgetAndLateReadFailurePublishNothing) {
  reset(6);
  vectors({{Input, 2}, {Input + 2, 2}});
  EXPECT_EQ(ok(ServiceKind::Writev, {1, Header, 2}), 4u);
  EXPECT_FALSE(invoke(ServiceKind::Writev, {2, Header, 2}));
  EXPECT_EQ(Result.Stop, ProcessStopReason::OutputLimit);
  EXPECT_TRUE(Result.StandardError.empty());
  EXPECT_EQ(flags(2), 1u);
  vectors({{1, 4}});
  EXPECT_FALSE(invoke(ServiceKind::Writev, {2, Header, 1}));
  EXPECT_EQ(Result.Stop, ProcessStopReason::OutputLimit);
  error(ServiceKind::Write, {2, UINT64_MAX, 4}, 14);
  Result.StandardOutput.clear();
  vectors({{Input, 2}, {Input + 2, 2}});
  for (bool Access : {true, false}) {
    (Access ? Memory->FailAccess : Memory->FailRead) = Input + 2;
    auto Failed = Files->handle(ServiceKind::Writev,
                                {0, 0, {2, Header, 2}, std::nullopt}, Result);
    ASSERT_FALSE(bool(Failed));
    llvm::consumeError(Failed.takeError());
    Memory->FailAccess.reset();
    Memory->FailRead.reset();
    EXPECT_TRUE(Result.StandardError.empty());
    EXPECT_EQ(flags(2), 1u);
  }
  EXPECT_EQ(ok(ServiceKind::Writev, {2, Header, 2}), 4u);
  EXPECT_EQ(Result.StandardError, "ABCD");
}

TEST_P(DarwinVectorTest, HeaderBackendFailuresNeverReachDataOrDescriptors) {
  const auto FD = open();
  const auto Before = status(FD);
  vectors({{Input, 2}});
  for (bool Access : {true, false}) {
    (Access ? Memory->FailAccess : Memory->FailRead) = Header;
    auto Failed = Files->handle(ServiceKind::Writev,
                                {0, 0, {FD, Header, 1}, std::nullopt}, Result);
    ASSERT_FALSE(bool(Failed));
    llvm::consumeError(Failed.takeError());
    Memory->FailAccess.reset();
    Memory->FailRead.reset();
    EXPECT_EQ(status(FD), Before);
    EXPECT_EQ(flags(FD), 2u);
    EXPECT_EQ(cursor(FD), 0u);
  }
}

INSTANTIATE_TEST_SUITE_P(PageSizes, DarwinVectorTest,
                         testing::Values(4096, 16384));
} // namespace
} // namespace neverd::emulation::darwin_model
