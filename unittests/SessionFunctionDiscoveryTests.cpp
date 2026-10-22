//===- SessionFunctionDiscoveryTests.cpp - Recovered function extents
//------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SessionImpl.h"
#include "SessionTestAccess.h"
#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/Object/ELFTypes.h"
#include "llvm/Support/FileSystem.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace {
std::string takeString(const char *Text) {
  const std::string Result = Text ? Text : "";
  neverd_free_string(Text);
  return Result;
}

std::string makeNativeELF(bool AArch64, uint64_t Base = 0x400000,
                          std::string_view TrailingCode = {},
                          std::string_view Body = {}) {
  using ELF = llvm::object::ELF64LE;
  using namespace llvm::ELF;
  std::string Code = !Body.empty() ? std::string(Body)
                     : AArch64
                         ? std::string("\xe0\x00\x80\x52\xc0\x03\x5f\xd6", 8)
                         : std::string("\xb8\x07\x00\x00\x00\xc3", 6);
  Code += TrailingCode;
  const size_t CodeOffset = sizeof(ELF::Ehdr) + sizeof(ELF::Phdr);
  std::string Bytes(CodeOffset + Code.size(), '\0');
  ELF::Ehdr Header{};
  std::memcpy(Header.e_ident, ElfMagic, sizeof(ElfMagic) - 1);
  Header.e_ident[EI_CLASS] = ELFCLASS64;
  Header.e_ident[EI_DATA] = ELFDATA2LSB;
  Header.e_ident[EI_VERSION] = EV_CURRENT;
  Header.e_type = ET_EXEC;
  Header.e_machine = AArch64 ? EM_AARCH64 : EM_X86_64;
  Header.e_version = EV_CURRENT;
  Header.e_entry = Base + CodeOffset;
  Header.e_phoff = sizeof(ELF::Ehdr);
  Header.e_ehsize = sizeof(ELF::Ehdr);
  Header.e_phentsize = sizeof(ELF::Phdr);
  Header.e_phnum = 1;
  Header.e_shentsize = sizeof(ELF::Shdr);
  ELF::Phdr Segment{};
  Segment.p_type = PT_LOAD;
  Segment.p_flags = PF_R | PF_X;
  Segment.p_vaddr = Base;
  Segment.p_paddr = Base;
  Segment.p_filesz = Bytes.size();
  Segment.p_memsz = Bytes.size();
  Segment.p_align = 0x1000;
  std::memcpy(Bytes.data(), &Header, sizeof(Header));
  std::memcpy(Bytes.data() + sizeof(Header), &Segment, sizeof(Segment));
  Bytes.replace(CodeOffset, Code.size(), Code);
  return Bytes;
}

class SessionFunctionDiscovery : public ::testing::Test {
protected:
  neverd_session_t Session = nullptr;
  std::filesystem::path Directory;
  void SetUp() override {
    llvm::SmallString<128> Path;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-discovery", Path));
    Directory = Path.c_str();
    Session = neverd_session_create();
    ASSERT_NE(Session, nullptr);
  }
  void TearDown() override {
    neverd_session_destroy(Session);
    std::error_code Error;
    std::filesystem::remove_all(Directory, Error);
  }
  std::string write(std::string_view Name, std::string_view Bytes) {
    const auto File = Directory / Name;
    std::ofstream Stream(File, std::ios::binary);
    Stream.write(Bytes.data(), static_cast<std::streamsize>(Bytes.size()));
    return File.string();
  }
};

TEST_F(SessionFunctionDiscovery,
       NativeDiscoveryBeforeAnalysisPublishesRecoveredExtents) {
  for (bool AArch64 : {false, true}) {
    for (bool Lazy : {false, true}) {
      for (neverd_va_t Base : {0x400000ULL, 0xffff800000400000ULL}) {
        SCOPED_TRACE(AArch64);
        SCOPED_TRACE(Lazy);
        SCOPED_TRACE(Base);
        const std::string Path =
            write("discovered.elf", makeNativeELF(AArch64, Base));
        ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
        const auto Entry = neverd_session_entry_addr(Session);
        ASSERT_EQ(neverd_session_discover_functions(Session), 1);
        ASSERT_EQ(neverd_func_size(Session, 0), 0);
        const auto Name = takeString(neverd_func_name(Session, 0));
        if (Lazy)
          ASSERT_FALSE(takeString(neverd_ir_low(Session, Entry)).empty());
        else
          ASSERT_EQ(neverd_session_analyze(Session), 1);
        EXPECT_EQ(neverd_func_entry(Session, 0), Entry);
        EXPECT_EQ(neverd_func_size(Session, 0), AArch64 ? 8 : 6);
        EXPECT_EQ(takeString(neverd_func_name(Session, 0)), Name);
        ASSERT_FALSE(takeString(neverd_ir_med(Session, Entry)).empty());
        ASSERT_EQ(neverd_session_analyze(Session), 1);
        EXPECT_EQ(neverd_func_size(Session, 0), AArch64 ? 8 : 6);
      }
    }
  }
}

TEST_F(SessionFunctionDiscovery, CallsAtTheEntryAreSweptInStep) {
  // The image's one PT_LOAD maps its headers too, and a sweep from them
  // decodes `00 e8`, the last header byte with the opcode of the entry's
  // first call, as one instruction.  Swept from the entry, every call is
  // read: main calls a, c and d; a calls b; b calls c.
  constexpr char Code[] = "\xe8\x0b\x00\x00\x00"     // main: call a
                          "\xe8\x12\x00\x00\x00"     // call c
                          "\xe8\x13\x00\x00\x00"     // call d
                          "\xc3"                     // ret
                          "\xe8\x01\x00\x00\x00\xc3" // a: call b; ret
                          "\xe8\x01\x00\x00\x00\xc3" // b: call c; ret
                          "\xb8\x07\x00\x00\x00\xc3" // c: mov eax, 7; ret
                          "\xc3";                    // d: ret
  const std::string Path = write(
      "chain.elf", makeNativeELF(false, 0x400000, {},
                                 std::string_view(Code, sizeof(Code) - 1)));
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
  const auto Entry = neverd_session_entry_addr(Session);
  ASSERT_EQ(neverd_session_discover_functions(Session), 5);
  for (const uint64_t Offset : {0x0, 0x10, 0x16, 0x1c, 0x22})
    EXPECT_GE(neverd_func_find_by_addr(Session, Entry + Offset), 0) << Offset;
}

TEST_F(SessionFunctionDiscovery,
       RecoveredExtentsSurviveSelectingAnotherFunction) {
  // An x86-64 entry that calls the function after it: discovery lists both
  // without extents, and each complete lift gives its function one.
  // Selecting the second keeps the first's, and the list read from the
  // image stays the one a rebuild would make.
  const std::string EntryCode("\xe8\x01\x00\x00\x00\xc3", 6);
  const std::string CalleeCode("\xb8\x07\x00\x00\x00\xc3", 6);
  const std::string Path =
      write("calls.elf", makeNativeELF(false, 0x400000, CalleeCode, EntryCode));
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
  const auto Entry = neverd_session_entry_addr(Session);
  const auto Callee = Entry + EntryCode.size();
  ASSERT_EQ(neverd_session_discover_functions(Session), 2);
  const int EntryIndex = neverd_func_find_by_addr(Session, Entry);
  const int CalleeIndex = neverd_func_find_by_addr(Session, Callee);
  ASSERT_GE(EntryIndex, 0);
  ASSERT_GE(CalleeIndex, 0);
  EXPECT_EQ(neverd_func_size(Session, EntryIndex), 0);
  EXPECT_EQ(neverd_func_size(Session, CalleeIndex), 0);

  // As warming leaves a session: its analysis reads the current signature
  // features, so each view prepared selects its function rather than
  // starting the analysis over.
  auto &State = *reinterpret_cast<neverd::sdk::Session *>(Session);
  State.PipelineFeatureGeneration = State.SigDB.featureGeneration();
  ASSERT_EQ(neverd_prepare_function_view(Session, Entry, "low"), 1);
  EXPECT_EQ(State.OnlyFunctionEntries, std::set<neverd::va_t>{Entry});
  EXPECT_EQ(neverd_func_size(Session, EntryIndex), 6);
  ASSERT_EQ(neverd_prepare_function_view(Session, Callee, "low"), 1);
  EXPECT_EQ(State.OnlyFunctionEntries, std::set<neverd::va_t>{Callee});
  EXPECT_EQ(neverd_func_find_by_addr(Session, Entry), EntryIndex);
  EXPECT_EQ(neverd_func_size(Session, CalleeIndex), 6);
  EXPECT_EQ(neverd_func_size(Session, EntryIndex), 6);
  EXPECT_TRUE(State.functionListCurrent());

  // A rebuild from the image keeps them too.
  neverd::sdk::SessionTestAccess::resetFunctions(State);
  EXPECT_EQ(neverd_func_size(Session, EntryIndex), 6);
  EXPECT_EQ(neverd_func_size(Session, CalleeIndex), 6);
}

TEST_F(SessionFunctionDiscovery,
       NativeRecoveryKeepsKnownExtentsAndDeletedEntries) {
  for (bool MissingEntry : {false, true}) {
    auto &State = *reinterpret_cast<neverd::sdk::Session *>(Session);
    State.Loaded = true;
    State.Img.Arch = neverd::Arch::X64;
    State.PipeRan = true;
    State.PipeResult.Success = true;
    State.NativeFunctionsSynchronized = false;
    State.Functions = {{0x1000, 0, "discovered"}, {0x2000, 23, "stated"}};
    State.functionListChanged();
    State.FunctionEdits[0x4000] = false;
    State.PipeResult.LowFuncs.clear();
    for (auto Entry : {0x1000, 0x2000, 0x4000}) {
      neverd::LowFunc F;
      F.Entry = Entry;
      F.OriginalSize = 8;
      State.PipeResult.LowFuncs.push_back(std::move(F));
    }
    if (MissingEntry) {
      neverd::LowFunc F;
      F.Entry = 0x3000;
      F.OriginalSize = 12;
      State.PipeResult.LowFuncs.push_back(std::move(F));
    }
    ASSERT_EQ(neverd_func_count(Session), MissingEntry ? 3 : 2);
    EXPECT_EQ(neverd_func_size(Session, 0), 8);
    EXPECT_EQ(neverd_func_size(Session, 1), 23);
    EXPECT_EQ(neverd_func_find_by_addr(Session, 0x4000), -1);
    if (MissingEntry)
      EXPECT_EQ(neverd_func_size(Session, 2), 12);
  }
}

TEST_F(SessionFunctionDiscovery, CooperativeStopKeepsInventoryAndAllowsRetry) {
  for (bool AArch64 : {false, true}) {
    SCOPED_TRACE(AArch64);
    const std::string Padding(1024 * 1024, AArch64 ? '\0' : '\x90');
    const auto Path =
        write("cancel.elf", makeNativeELF(AArch64, 0x400000, Padding));
    ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
    auto &State = *static_cast<neverd::sdk::Session *>(Session);
    const auto Count = neverd_func_count(Session);
    const auto Revision = State.FunctionListRevision;
    const auto Verified = State.Img.VerifiedFunctionEntries;
    std::atomic<unsigned> Probes{0};
    const auto Cancel = [](void *Context) -> int {
      return static_cast<std::atomic<unsigned> *>(Context)->fetch_add(1) >= 64;
    };
    EXPECT_EQ(
        neverd_session_discover_functions_cancellable(Session, Cancel, &Probes),
        -2);
    EXPECT_GT(Probes.load(), 64u);
    EXPECT_FALSE(State.DiscoveredFunctions);
    EXPECT_FALSE(State.FunctionDiscovery);
    EXPECT_EQ(State.FunctionListRevision, Revision);
    EXPECT_EQ(State.Img.VerifiedFunctionEntries, Verified);
    EXPECT_EQ(neverd_func_count(Session), Count);
    EXPECT_NE(takeString(neverd_last_error(Session)).find("cancelled"),
              std::string::npos);
    ASSERT_EQ(neverd_session_discover_functions(Session), Count);
    EXPECT_TRUE(State.DiscoveredFunctions);
    EXPECT_TRUE(takeString(neverd_last_error(Session)).empty());
  }
}

TEST_F(SessionFunctionDiscovery,
       CooperativeStopJoinsPendingSnapshotBeforeRetry) {
  const auto Path =
      write("pending.elf",
            makeNativeELF(false, 0x400000, std::string(1024 * 1024, '\x90')));
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
  const auto Count = neverd_func_count(Session);
  auto &State = *static_cast<neverd::sdk::Session *>(Session);
  ASSERT_EQ(neverd_session_discover_functions_step(Session), 0);
  ASSERT_TRUE(State.FunctionDiscovery);
  const auto Token = State.FunctionDiscovery->Cancel;
  const auto Revision = State.FunctionListRevision;
  EXPECT_EQ(neverd_session_discover_functions_cancellable(
                Session, [](void *) { return 1; }, nullptr),
            -2);
  EXPECT_TRUE(Token->load());
  EXPECT_FALSE(State.FunctionDiscovery);
  EXPECT_FALSE(State.DiscoveredFunctions);
  EXPECT_EQ(State.FunctionListRevision, Revision);
  EXPECT_EQ(neverd_func_count(Session), Count);
  ASSERT_EQ(neverd_session_discover_functions(Session), Count);
  EXPECT_TRUE(State.DiscoveredFunctions);
}

TEST_F(SessionFunctionDiscovery, ClosingSessionStopsItsOwnedDiscovery) {
  const auto Path =
      write("closing.elf",
            makeNativeELF(false, 0x400000, std::string(1024 * 1024, '\x90')));
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
  ASSERT_EQ(neverd_session_discover_functions_step(Session), 0);
  auto &State = *static_cast<neverd::sdk::Session *>(Session);
  ASSERT_TRUE(State.FunctionDiscovery);
  const auto Token = State.FunctionDiscovery->Cancel;
  neverd_session_destroy(Session);
  Session = nullptr;
  EXPECT_TRUE(Token->load());
}

} // namespace
