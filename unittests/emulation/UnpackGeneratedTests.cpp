//===- UnpackGeneratedTests.cpp - Observation on every instruction set ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The rules that establish an entry belong to no protector, container
/// layout or instruction set. These cases check them against a program that
/// is packed here, by the test, from a file whose every byte is known: the
/// program as it was linked.
///
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "UnpackTestSupport.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"
#include "neverd/unpack/Unpack.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"

namespace neverd::unpack {
namespace {
using namespace emulation;
using test::differingBytes;
using test::Image;
namespace generated {
#define NEVERD_GENERATED_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_GENERATED_MODE(Name, Value)                                     \
  constexpr uint32_t Name##Mode = Value;
#define NEVERD_GENERATED_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/UnpackGeneratedCases.def"
#undef NEVERD_GENERATED_TEXT
#undef NEVERD_GENERATED_MODE
#undef NEVERD_GENERATED_VALUE
} // namespace generated
using namespace generated;

struct Profile {
  const char *Name;
  ExecutionBackendKind Backend;
  GuestArchitecture ISA;
  ExecutionContract Contract;
  const char *Directory;
};
constexpr Profile Profiles[] = {
#define NEVERD_USER_PROFILE(Name, Backend, ISA, Contract)                      \
  {#Name, ExecutionBackendKind::Backend, GuestArchitecture::ISA,               \
   ExecutionContract::Contract, ISA##Dir},
#include "UserExecutionCases.def"
#undef NEVERD_USER_PROFILE
};
void PrintTo(const Profile &P, std::ostream *OS) { *OS << P.Name; }

class UnpackGenerated : public testing::TestWithParam<Profile> {
protected:
  void SetUp() override {
#ifndef NEVERD_UNPACK_GENERATED_FIXTURE_DIR
    if (requireHvf(GetParam().Backend, GetParam().ISA))
      FAIL() << MissingTools;
    GTEST_SKIP() << MissingTools;
#else
    const auto &P = GetParam();
    ExecutionConfiguration Config;
    Config.Backend = P.Backend;
    Config.Architecture = P.ISA;
    Config.Contract = P.Contract;
    auto Probe = probeExecutionBackend(Config);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available) {
      if (requireHvf(P.Backend, P.ISA))
        FAIL() << Probe->Reason;
      GTEST_SKIP() << Probe->Reason;
    }
    Original = test::readImage(
        std::filesystem::path(NEVERD_UNPACK_GENERATED_FIXTURE_DIR) /
        P.Directory / ProgramFile);
    ASSERT_FALSE(HasFailure());
    llvm::SmallString<128> Created;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(ScratchPrefix, Created));
    Scratch = Created.str().str();
    Options.Process.Backend = P.Backend;
    Options.Process.Limits.Instructions = InstructionLimit;
    Options.Process.Limits.TimeoutMicroseconds = TimeoutMicroseconds;
#endif
  }
  void TearDown() override {
    if (!Scratch.empty())
      std::filesystem::remove_all(Scratch);
  }

  /// Replace each import call in the program with the six-byte tail call the
  /// rebuilder recognizes. The linked file stays the oracle; only this copy
  /// carries the protector's shape.
  bool mutateImportCalls(std::vector<uint8_t> &Bytes) {
    using namespace llvm::support::endian;
    if (GetParam().ISA != GuestArchitecture::X64) {
      ADD_FAILURE() << X64CallShape;
      return false;
    }
    const auto *Program = Original.section(ProgramSection);
    if (!Program || Program->VirtualSize > Program->FileSize ||
        Program->FileOffset + Program->VirtualSize > Bytes.size()) {
      ADD_FAILURE() << MissingRecord;
      return false;
    }
    const char *Names[] = {"ExitProcess", "GetStdHandle", "WriteFile"};
    bool Seen[] = {false, false, false};
    for (uint32_t At = 0; At + 6 <= Program->VirtualSize; ++At) {
      uint8_t *Site = Bytes.data() + Program->FileOffset + At;
      if (Site[0] != 0xff || Site[1] != 0x15)
        continue;
      const int32_t Displacement = read32le(Site + 2);
      const uint64_t Next = Original.Base + Program->RVA + At + 6;
      const uint64_t Slot = uint64_t(int64_t(Next) + Displacement);
      const Image::Import *Match = nullptr;
      for (const auto &Import : Original.Imports)
        if (Original.Base + Import.Slot == Slot)
          Match = &Import;
      if (!Match)
        continue;
      const auto Tail = Original.Exports.find("tail_" + Match->Name);
      if (Tail == Original.Exports.end()) {
        ADD_FAILURE() << Match->Name << MissingTail;
        return false;
      }
      const int64_t Relative =
          int64_t(Original.Base + Tail->second) - int64_t(Next);
      if (Relative != int32_t(Relative)) {
        ADD_FAILURE() << TailOutOfRange;
        return false;
      }
      Site[0] = 0x51;
      Site[1] = 0xe8;
      write32le(Site + 2, uint32_t(Relative));
      for (size_t I = 0; I < sizeof Names / sizeof Names[0]; ++I)
        if (Match->Name == Names[I])
          Seen[I] = true;
      At += 5;
    }
    for (bool Found : Seen)
      if (!Found) {
        ADD_FAILURE() << NoImportCall;
        return false;
      }
    return true;
  }

  /// Do to the linked program what a packer does: keep its code only in a
  /// transformed copy and start at the loader that restores it.
  std::filesystem::path pack(uint32_t Mode) {
    using namespace llvm::support::endian;
    const auto *Pay = Original.section(PackSection);
    const auto Loader = Original.Exports.find(LoaderExport);
    if (!Pay || Loader == Original.Exports.end() ||
        Pay->FileSize < RelayOffset + Capacity) {
      ADD_FAILURE() << MissingRecord;
      return {};
    }
    auto Bytes = Original.File;
    uint8_t *Record = Bytes.data() + Pay->FileOffset;
    write32le(Record + ModeOffset, Mode);
    write32le(Record + KeyOffset, uint32_t(Key));
    auto Move = [&](const char *Name, uint64_t SizeAt, uint64_t DataAt) {
      const auto *S = Original.section(Name);
      if (!S || S->VirtualSize > Capacity || S->VirtualSize > S->FileSize) {
        ADD_FAILURE() << Name << RecordOverflow;
        return;
      }
      for (uint32_t I = 0; I < S->VirtualSize; ++I)
        Record[DataAt + I] = uint8_t(Bytes[S->FileOffset + I] ^ Key);
      std::fill_n(Bytes.begin() + S->FileOffset, S->FileSize, 0);
      write32le(Record + SizeAt, S->VirtualSize);
    };
    if (Mode == MutatedMode && !mutateImportCalls(Bytes))
      return {};
    Move(ProgramSection, ProgramBytesOffset, ProgramOffset);
    if (Mode == StagedMode)
      Move(RelaySection, RelayBytesOffset, RelayOffset);
    write32le(Bytes.data() + Original.EntryOffset, Loader->second);
    const auto Path = Scratch / PackedFile;
    test::writeFile(Path, Bytes);
    return Path;
  }

  UnpackResult unpack(uint32_t Mode) {
    const auto Packed = pack(Mode);
    if (HasFailure())
      return {};
    auto Result = unpackFile(Packed, Options);
    if (!Result) {
      ADD_FAILURE() << llvm::toString(Result.takeError());
      return {};
    }
    return std::move(*Result);
  }

  ProcessResult run(const std::filesystem::path &Path) {
    auto Result =
        emulateProcess(Path, ProcessProfile::WindowsPE64, Options.Process);
    if (!Result) {
      ADD_FAILURE() << llvm::toString(Result.takeError());
      return {};
    }
    return std::move(*Result);
  }
  ProcessResult runOriginal() {
    const auto Path = Scratch / ProgramFile;
    test::writeFile(Path, Original.File);
    return run(Path);
  }

  /// The rebuilt file holds the program as it was linked: every section but
  /// the pack record, which the linked file leaves empty.
  void expectOriginalProgram(const UnpackResult &Result) {
    const Image Rebuilt = test::readImage(Result.Image);
    ASSERT_FALSE(HasFailure());
    EXPECT_EQ(Rebuilt.Entry, Original.Entry);
    EXPECT_EQ(Rebuilt.Base, Original.Base);
    for (const auto &S : Original.Sections)
      if (S.Name != PackSection)
        EXPECT_EQ(differingBytes(Original, Rebuilt, S), 0u) << S.Name;
    EXPECT_TRUE(std::includes(Rebuilt.Imports.begin(), Rebuilt.Imports.end(),
                              Original.Imports.begin(),
                              Original.Imports.end()));
    // The loader resolved nothing itself; every cell was bound at load.
    for (const auto &I : Result.Imports)
      EXPECT_EQ(I.Origin, ImportOrigin::Static) << I.Name;
    const auto Path = Scratch / RebuiltFile;
    test::writeFile(Path, Result.Image);
    const auto Expected = runOriginal(), Actual = run(Path);
    ASSERT_FALSE(HasFailure());
    EXPECT_EQ(Expected.Stop, ProcessStopReason::Exited) << Expected.Diagnostic;
    EXPECT_EQ(Expected.ExitStatus, ExitStatus);
    EXPECT_EQ(Expected.StandardOutput, Message);
    EXPECT_EQ(Actual.Stop, Expected.Stop) << Actual.Diagnostic;
    EXPECT_EQ(Actual.ExitStatus, Expected.ExitStatus);
    EXPECT_EQ(Actual.StandardOutput, Expected.StandardOutput);
    // The same program executes the same instructions.
    EXPECT_EQ(Actual.Instructions, Expected.Instructions);
  }

  Image Original;
  std::filesystem::path Scratch;
  UnpackOptions Options;
};

TEST_P(UnpackGenerated, PackedProgramRunsLikeTheLinkedOne) {
  // The oracle itself: packing changed where the code comes from, not what
  // the process does.
  const auto Expected = runOriginal();
  ASSERT_FALSE(HasFailure());
  for (const uint32_t Mode : {JumpMode, CallMode, StagedMode}) {
    SCOPED_TRACE(Mode);
    const auto Packed = pack(Mode);
    ASSERT_FALSE(HasFailure());
    const auto Actual = run(Packed);
    ASSERT_FALSE(HasFailure());
    EXPECT_EQ(Actual.Stop, ProcessStopReason::Exited) << Actual.Diagnostic;
    EXPECT_EQ(Actual.ExitStatus, Expected.ExitStatus);
    EXPECT_EQ(Actual.StandardOutput, Expected.StandardOutput);
    EXPECT_GT(Actual.Instructions, Expected.Instructions);
  }
}

TEST_P(UnpackGenerated, LoaderThatLeavesForTheProgramYieldsTheLinkedImage) {
  const auto Result = unpack(JumpMode);
  ASSERT_FALSE(HasFailure());
  ASSERT_EQ(Result.Outcome, UnpackOutcome::Unpacked) << Result.Diagnostic;
  // No protector is involved, so none is named and none declares an entry.
  EXPECT_EQ(Result.Packer.Kind, PackerKind::Unidentified);
  EXPECT_TRUE(Result.Packer.Evidence.empty());
  EXPECT_EQ(Result.Format, FormatKind::PE64);
  EXPECT_EQ(Result.Architecture, guestArchitectureName(GetParam().ISA));
  EXPECT_EQ(Result.Profile, processProfileName(ProcessProfile::WindowsPE64));
  ASSERT_EQ(Result.Transfers.size(), 1u);
  EXPECT_EQ(Result.Transfers[0].RVA, Original.Entry);
  EXPECT_TRUE(Result.Transfers[0].StackBalanced);
  EXPECT_EQ(Result.Transfers[0].Generation, 1u);
  EXPECT_EQ(Result.EntryRVA, Original.Entry);
  EXPECT_EQ(Result.Source, EntrySource::Transfer);
  expectOriginalProgram(Result);
}

TEST_P(UnpackGenerated, LoaderCallIntoTheProgramPrecedesItsEntry) {
  const auto Result = unpack(CallMode);
  ASSERT_FALSE(HasFailure());
  ASSERT_EQ(Result.Outcome, UnpackOutcome::Unpacked) << Result.Diagnostic;
  // The call enters generated code on a deeper stack and returns. The jump
  // that follows enters the same generation on the entry stack.
  ASSERT_EQ(Result.Transfers.size(), 2u);
  EXPECT_FALSE(Result.Transfers[0].StackBalanced);
  EXPECT_NE(Result.Transfers[0].RVA, Original.Entry);
  EXPECT_EQ(Result.Transfers[0].Generation, 1u);
  EXPECT_TRUE(Result.Transfers[1].StackBalanced);
  EXPECT_EQ(Result.Transfers[1].RVA, Original.Entry);
  EXPECT_EQ(Result.Transfers[1].Generation, 1u);
  EXPECT_EQ(Result.EntryRVA, Original.Entry);
  EXPECT_EQ(Result.Source, EntrySource::Transfer);
  expectOriginalProgram(Result);
}

TEST_P(UnpackGenerated, SecondLoaderIsTheEntryUntilALaterTransferIsNamed) {
  const auto *Relay = Original.section(RelaySection);
  ASSERT_NE(Relay, nullptr);
  // The first loader leaves on the entry stack for code it wrote. Nothing in
  // the image says that this code is another loader.
  const auto First = unpack(StagedMode);
  ASSERT_FALSE(HasFailure());
  ASSERT_EQ(First.Outcome, UnpackOutcome::Unpacked) << First.Diagnostic;
  ASSERT_EQ(First.Transfers.size(), 1u);
  EXPECT_EQ(First.EntryRVA, Relay->RVA);
  EXPECT_EQ(First.Transfers[0].Generation, 1u);
  // That image is still a program: its entry writes the rest and leaves.
  const auto Partial = Scratch / RebuiltFile;
  test::writeFile(Partial, First.Image);
  const auto Ran = run(Partial);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(Ran.Stop, ProcessStopReason::Exited) << Ran.Diagnostic;
  EXPECT_EQ(Ran.ExitStatus, ExitStatus);
  EXPECT_EQ(Ran.StandardOutput, Message);

  Options.Transfer = SecondTransfer;
  const auto Second = unpack(StagedMode);
  ASSERT_FALSE(HasFailure());
  ASSERT_EQ(Second.Outcome, UnpackOutcome::Unpacked) << Second.Diagnostic;
  ASSERT_EQ(Second.Transfers.size(), 2u);
  EXPECT_EQ(Second.Transfers[0].RVA, Relay->RVA);
  EXPECT_TRUE(Second.Transfers[0].StackBalanced);
  EXPECT_EQ(Second.Transfers[0].Generation, 1u);
  // Code written by generated code is one generation further.
  EXPECT_EQ(Second.Transfers[1].RVA, Original.Entry);
  EXPECT_TRUE(Second.Transfers[1].StackBalanced);
  EXPECT_EQ(Second.Transfers[1].Generation, 2u);
  EXPECT_EQ(Second.EntryRVA, Original.Entry);
  expectOriginalProgram(Second);
}

// The linked program calls its imports directly. Packing rewrites each of
// those calls into a six-byte tail call, which is the shape a protector leaves
// when it does not virtualize the call. Unpacking must put the ordinary call
// back, so the recovered section matches the linked one and the program runs.
TEST_P(UnpackGenerated, MutatedImportTailCallsAreOrdinaryImportsAgain) {
  if (GetParam().ISA != GuestArchitecture::X64)
    GTEST_SKIP() << X64CallShape;
  const auto Result = unpack(MutatedMode);
  ASSERT_FALSE(HasFailure());
  ASSERT_EQ(Result.Outcome, UnpackOutcome::Unpacked) << Result.Diagnostic;
  EXPECT_EQ(Result.EntryRVA, Original.Entry);
  EXPECT_EQ(Result.Source, EntrySource::Transfer);
  EXPECT_EQ(Result.ProcessStop, test::StopObserver);
  expectOriginalProgram(Result);
}

INSTANTIATE_TEST_SUITE_P(Backends, UnpackGenerated, testing::ValuesIn(Profiles),
                         [](const auto &Info) {
                           return std::string(Info.param.Name);
                         });
} // namespace
} // namespace neverd::unpack
