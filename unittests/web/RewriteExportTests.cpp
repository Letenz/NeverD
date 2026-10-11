//===- RewriteExportTests.cpp - Local disclosure transaction contracts ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Failed writes clean only owned files through retained directory handles.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "rewrite/Export.h"

#include "neverd/web/Error.h"
#include "neverd/web/SourceRewrite.h"

#include "llvm/Support/FileSystem.h"

#include <filesystem>
#include <fstream>

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
using namespace neverd::web;
using rewrite::Export;
using rewrite::ExportFile;

class WebRewriteExport : public ::testing::Test {
protected:
  std::filesystem::path Root, Output;
  void SetUp() override {
#ifdef _WIN32
    GTEST_SKIP() << "POSIX local export profile required";
#endif
    llvm::SmallString<128> Path;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-rewrite-export", Path));
    Root = Path.str().str();
    Output = Root / "result";
  }
  void TearDown() override {
    if (!Root.empty()) {
      std::error_code EC;
      std::filesystem::remove_all(Root, EC);
    }
  }
  static std::string read(const std::filesystem::path &P) {
    std::ifstream F(P, std::ios::binary);
    return {std::istreambuf_iterator<char>(F), {}};
  }
};

TEST_F(WebRewriteExport, CommittedFilesAreExactAndExistingPathsAreUntouched) {
  {
    Export E(Output.string());
    E.write(ExportFile::Original, "1+2;");
    E.write(ExportFile::Candidate, "(3);");
    E.write(ExportFile::Receipt, "{}");
    E.commit();
  }
  EXPECT_EQ(read(Output / "original.js"), "1+2;");
  EXPECT_EQ(read(Output / "candidate.js"), "(3);");
  EXPECT_EQ(read(Output / "receipt.json"), "{}");
  EXPECT_THROW({ Export Again(Output.string()); }, Error);
  EXPECT_EQ(read(Output / "original.js"), "1+2;");
  std::filesystem::create_directory_symlink(Output, Root / "link");
  EXPECT_THROW({ Export Link((Root / "link").string()); }, Error);
  EXPECT_EQ(read(Output / "candidate.js"), "(3);");
}

TEST_F(WebRewriteExport, CandidateAndReceiptFailureRollBackAllOwnedFiles) {
  for (const auto Failure : {ExportFile::Candidate, ExportFile::Receipt}) {
    EXPECT_THROW(
        {
          Export E(Output.string());
          E.write(ExportFile::Original, "1+2;");
          if (Failure == ExportFile::Receipt)
            E.write(ExportFile::Candidate, "(3);");
          E.write(Failure, std::string(4 * MaxSourceRewriteBytes + 1, 'x'));
        },
        Error);
    EXPECT_FALSE(std::filesystem::exists(Output));
  }
  {
    Export E(Output.string());
    E.write(ExportFile::Original, "1+2;");
    EXPECT_THROW(E.commit(), Error);
  }
  EXPECT_FALSE(std::filesystem::exists(Output));
}

TEST_F(WebRewriteExport,
       PartialDiskWriteFailureRemovesCreatedCandidateOrReceipt) {
#ifdef _WIN32
  GTEST_SKIP() << "POSIX resource-limit injection required";
#else
  for (const auto Failure : {ExportFile::Candidate, ExportFile::Receipt}) {
    const auto Child = fork();
    ASSERT_GE(Child, 0);
    if (Child == 0) {
      // Only this C++ fixture child changes its limits. No input program,
      // shell, script or helper executable is launched.
      signal(SIGXFSZ, SIG_IGN);
      rlimit Limit{};
      if (getrlimit(RLIMIT_FSIZE, &Limit) != 0)
        _exit(10);
      Limit.rlim_cur = 16;
      if (setrlimit(RLIMIT_FSIZE, &Limit) != 0)
        _exit(11);
      try {
        Export E(Output.string());
        E.write(ExportFile::Original, "1+2;");
        if (Failure == ExportFile::Receipt)
          E.write(ExportFile::Candidate, "(3);");
        E.write(Failure, std::string(8192, 'x'));
        _exit(12);
      } catch (const Error &E) {
        if (std::string_view(E.what()) != "export_io_error")
          _exit(13);
      } catch (...) {
        _exit(14);
      }
      _exit(0);
    }
    int Status = 0;
    pid_t Done;
    do {
      Done = waitpid(Child, &Status, 0);
    } while (Done < 0 && errno == EINTR);
    ASSERT_EQ(Done, Child);
    ASSERT_TRUE(WIFEXITED(Status));
    EXPECT_EQ(WEXITSTATUS(Status), 0);
    EXPECT_FALSE(std::filesystem::exists(Output));
  }
#endif
}

TEST_F(WebRewriteExport, ForeignFileOrSymlinkAtLaterSlotSurvivesFailure) {
  const auto Target = Root / "foreign";
  std::ofstream(Target) << "untouched";
  EXPECT_THROW(
      {
        Export E(Output.string());
        E.write(ExportFile::Original, "1+2;");
        std::filesystem::create_symlink(Target, Output / "candidate.js");
        E.write(ExportFile::Candidate, "(3);");
      },
      Error);
  EXPECT_FALSE(std::filesystem::exists(Output / "original.js"));
  EXPECT_TRUE(std::filesystem::is_symlink(Output / "candidate.js"));
  EXPECT_EQ(read(Target), "untouched");
  std::filesystem::remove(Output / "candidate.js");
  std::filesystem::remove(Output);
  EXPECT_THROW(
      {
        Export E(Output.string());
        E.write(ExportFile::Original, "1+2;");
        E.write(ExportFile::Candidate, "(3);");
        std::ofstream(Output / "receipt.json") << "foreign receipt";
        E.write(ExportFile::Receipt, "{}");
      },
      Error);
  EXPECT_FALSE(std::filesystem::exists(Output / "original.js"));
  EXPECT_FALSE(std::filesystem::exists(Output / "candidate.js"));
  EXPECT_EQ(read(Output / "receipt.json"), "foreign receipt");
}

TEST_F(WebRewriteExport, ReplacementDirectoryIsNeverFollowedOrRemoved) {
  const auto Moved = Root / "moved";
  EXPECT_THROW(
      {
        Export E(Output.string());
        E.write(ExportFile::Original, "1+2;");
        std::filesystem::rename(Output, Moved);
        std::filesystem::create_directory(Output);
        std::ofstream(Output / "original.js") << "foreign original";
        E.write(ExportFile::Candidate, "(3);");
      },
      Error);
  EXPECT_EQ(read(Output / "original.js"), "foreign original");
  EXPECT_FALSE(std::filesystem::exists(Moved / "original.js"));
  EXPECT_FALSE(std::filesystem::exists(Output / "candidate.js"));
}

TEST_F(WebRewriteExport, ReplacedOwnedFilenameIsPreservedAndCommitRefuses) {
  EXPECT_THROW(
      {
        Export E(Output.string());
        E.write(ExportFile::Original, "1+2;");
        E.write(ExportFile::Candidate, "(3);");
        E.write(ExportFile::Receipt, "{}");
        std::filesystem::rename(Output / "original.js", Root / "moved.js");
        std::ofstream(Output / "original.js") << "replacement";
        E.commit();
      },
      Error);
  EXPECT_EQ(read(Output / "original.js"), "replacement");
  EXPECT_EQ(read(Root / "moved.js"), "1+2;");
  EXPECT_FALSE(std::filesystem::exists(Output / "candidate.js"));
  EXPECT_FALSE(std::filesystem::exists(Output / "receipt.json"));
}

TEST_F(WebRewriteExport, UnlinkedOwnedFileCannotBecomeAForeignCleanupTarget) {
  EXPECT_THROW(
      {
        Export E(Output.string());
        E.write(ExportFile::Original, "1+2;");
        E.write(ExportFile::Candidate, "(3);");
        E.write(ExportFile::Receipt, "{}");
        std::filesystem::remove(Output / "original.js");
        std::ofstream(Output / "original.js") << "new inode";
        E.commit();
      },
      Error);
  EXPECT_EQ(read(Output / "original.js"), "new inode");
  EXPECT_FALSE(std::filesystem::exists(Output / "candidate.js"));
  EXPECT_FALSE(std::filesystem::exists(Output / "receipt.json"));
}

} // namespace
