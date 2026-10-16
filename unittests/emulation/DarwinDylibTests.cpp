//===- DarwinDylibTests.cpp - Startup graph and TLS admission boundaries
//---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Original-byte mutations and aggregate budgets for explicit dylibs.
#include "DarwinTestImage.h"
#include "gtest/gtest.h"
#include "os/darwin/process/DarwinProcess.h"

#include "neverd/emulation/ProcessReport.h"
#include "neverd/loader/MachO/MachOExecutionImage.h"
#include "neverd/loader/MachO/MachOProgramLinking.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MemoryBuffer.h"
namespace neverd::emulation {
namespace {
using namespace llvm::MachO;
using namespace llvm::support::endian;
using darwin_test::Image;
using darwin_test::TemporaryImage;
class DarwinDylib : public testing::Test {
protected:
  std::filesystem::path Directory;
  ProcessOptions Options;
  void SetUp() override {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
    GTEST_SKIP() << "Darwin compiler fixtures unavailable";
#else
    Directory = std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) /
                "macos-arm64-dylib-classic";
    Options.StackSize = 16384;
    Options.Darwin =
        DarwinProcessOptions{{{"@loader_path/libneverd-base.dylib",
                               Directory / "libneverd-base.dylib"},
                              {"@loader_path/libneverd-leaf.dylib",
                               Directory / "libneverd-leaf.dylib"}}};
#endif
  }
  Image bytes(llvm::StringRef Name) {
    auto Buffer =
        llvm::MemoryBuffer::getFile((Directory / Name.str()).string());
    EXPECT_TRUE(bool(Buffer));
    Image I;
    if (Buffer)
      I.Bytes.assign((*Buffer)->getBufferStart(), (*Buffer)->getBufferEnd());
    return I;
  }
  std::vector<size_t> commands(const Image &I, uint32_t Kind) {
    std::vector<size_t> Out;
    size_t At = 32;
    for (unsigned N = read32le(I.Bytes.data() + 16); N; --N) {
      if (read32le(I.Bytes.data() + At) == Kind)
        Out.push_back(At);
      At += read32le(I.Bytes.data() + At + 4);
    }
    return Out;
  }
  size_t section(const Image &I, uint32_t Kind) {
    for (auto C : commands(I, LC_SEGMENT_64))
      for (unsigned N = read32le(I.Bytes.data() + C + 64), S = 0; S < N; ++S) {
        const auto At = C + 72 + S * 80;
        if ((read32le(I.Bytes.data() + At + 64) & SECTION_TYPE) == Kind)
          return At;
      }
    ADD_FAILURE() << "original fixture lacks requested section";
    return 0;
  }
  auto load() {
    return darwin_model::loadImage(Directory / "main",
                                   darwin_model::macOSProfile(), Options);
  }
  MachOProgramLinking linking(const Image &I) {
    auto R = readMachOProgramLinking(I.Bytes, 16 * 1024 * 1024,
                                     [] { return llvm::Error::success(); });
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return {};
    }
    return std::move(*R);
  }
  Image withAddend(Image I, llvm::StringRef Symbol, int64_t Addend) {
    const auto Facts = linking(I);
    const auto Segments = commands(I, LC_SEGMENT_64);
    llvm::SmallString<512> Stream;
    llvm::raw_svector_ostream Out(Stream);
    unsigned Changed = 0;
    for (const auto &F : Facts.Fixups) {
      if (!F.Bind || F.Origin != MachOProgramFixupOrigin::ClassicBind)
        continue;
      Out << char(BIND_OPCODE_SET_DYLIB_ORDINAL_IMM | F.LibraryOrdinal);
      Out << char(BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM |
                  (F.WeakImport ? BIND_SYMBOL_FLAGS_WEAK_IMPORT : 0));
      Out << F.Symbol << '\0'
          << char(BIND_OPCODE_SET_TYPE_IMM | BIND_TYPE_POINTER)
          << char(BIND_OPCODE_SET_ADDEND_SLEB);
      const bool Selected = F.Symbol == Symbol;
      Changed += Selected;
      llvm::encodeSLEB128(Selected ? Addend : F.Addend, Out);
      unsigned Index = 0;
      for (; Index != Segments.size(); ++Index) {
        const auto C = Segments[Index];
        const auto Begin = read64le(I.Bytes.data() + C + 24);
        const auto Size = read64le(I.Bytes.data() + C + 32);
        if (F.Address >= Begin && F.Address - Begin < Size)
          break;
      }
      if (Index == Segments.size()) {
        ADD_FAILURE() << "original bind destination has no segment";
        return I;
      }
      Out << char(BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | Index);
      llvm::encodeULEB128(
          F.Address - read64le(I.Bytes.data() + Segments[Index] + 24), Out);
      Out << char(BIND_OPCODE_DO_BIND);
    }
    EXPECT_GT(Changed, 0u);
    Out << char(BIND_OPCODE_DONE);
    const auto Info = commands(I, LC_DYLD_INFO_ONLY).front();
    I.u32(Info + 16, uint32_t(I.Bytes.size()));
    I.u32(Info + 20, uint32_t(Stream.size()));
    I.Bytes.insert(I.Bytes.end(), Stream.begin(), Stream.end());
    for (auto C : Segments) {
      if (std::string(reinterpret_cast<const char *>(I.Bytes.data() + C + 8)) !=
          "__LINKEDIT")
        continue;
      const uint64_t Size = I.Bytes.size() - read64le(I.Bytes.data() + C + 40);
      I.u64(C + 48, Size);
      I.u64(C + 32, std::max(Size, read64le(I.Bytes.data() + C + 32)));
    }
    return I;
  }
  auto loadMain(const Image &I) {
    TemporaryImage File(I);
    return darwin_model::loadImage(File.path(), darwin_model::macOSProfile(),
                                   Options);
  }
  void rejectsMain(const Image &I, const char *Contains) {
    auto R = loadMain(I);
    ASSERT_FALSE(bool(R));
    const auto Text = llvm::toString(R.takeError());
    EXPECT_NE(Text.find(Contains), std::string::npos) << Text;
  }
  void rejects(const char *Contains = nullptr) {
    auto R = load();
    ASSERT_FALSE(bool(R));
    const auto Text = llvm::toString(R.takeError());
    if (Contains)
      EXPECT_NE(Text.find(Contains), std::string::npos) << Text;
  }
};
TEST_F(DarwinDylib, SharedDependencyConsumesItsCompleteInputOnlyOnce) {
  auto Base = bytes("libneverd-base.dylib"),
       Leaf = bytes("libneverd-leaf.dylib");
  Base.Bytes.resize(Base.Bytes.size() + 1024 * 1024);
  Leaf.Bytes.resize(Leaf.Bytes.size() + 1024 * 1024);
  TemporaryImage B(Base), L(Leaf);
  Options.Darwin->Modules[0].Path = B.path();
  Options.Darwin->Modules[1].Path = L.path();
  const uint64_t Total =
      Base.Bytes.size() + Leaf.Bytes.size() + bytes("main").Bytes.size();
  Options.MemoryLimit = Total - 1;
  rejects("file byte limit");
  Options.MemoryLimit = Total;
  auto R = load();
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->TLS.size(), 3u);
  EXPECT_EQ(R->TLSDescriptors.size(), 5u);
}
TEST_F(DarwinDylib, ResidentBudgetIncludesAllImagesTLSAndProviderReservations) {
  auto First = load();
  ASSERT_TRUE(bool(First)) << llvm::toString(First.takeError());
  Options.MemoryLimit = First->MetadataBytes + First->Plan.MappedBytes +
                        Options.StackSize + 3 * First->Memory.PageSize;
  auto Exact = load();
  ASSERT_TRUE(bool(Exact)) << llvm::toString(Exact.takeError());
  --Options.MemoryLimit;
  rejects();
}
TEST_F(DarwinDylib, CatalogueRejectsMissingDuplicateReservedAndWrongProfile) {
  Options.Darwin->Modules.pop_back();
  rejects("provider");
  SetUp();
  Options.Darwin->Modules.push_back(Options.Darwin->Modules.front());
  rejects("catalogue");
  SetUp();
  Options.Darwin->Modules.front().Name = "/usr/lib/libSystem.B.dylib";
  rejects("catalogue");
  auto Other =
      emulateProcess(Directory / "main", ProcessProfile::LinuxELF64, Options);
  ASSERT_FALSE(bool(Other));
  EXPECT_NE(llvm::toString(Other.takeError()).find("Darwin profile"),
            std::string::npos);
  for (const char *JSON :
       {R"({"darwin":null})", R"({"darwin":{"unknown":[]}})",
        R"({"darwin":{"modules":{}}})",
        R"({"darwin":{"modules":[{"name":"x","path":""}]}})",
        R"({"darwin":{"modules":[{"name":"x","path":"p","extra":1}]}})"}) {
    auto R = processOptionsFromJSON(JSON);
    EXPECT_FALSE(bool(R)) << JSON;
    llvm::consumeError(R.takeError());
  }
  auto Parsed = processOptionsFromJSON(
      R"({"darwin":{"modules":[{"name":"@loader_path/example","path":"unused"}]}})");
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  ASSERT_TRUE(Parsed->Darwin);
  EXPECT_EQ(Parsed->Darwin->Modules.front().Name, "@loader_path/example");
}
TEST_F(DarwinDylib, EveryReferenceChecksIdentityVersionISAAndPlatform) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    auto I = bytes("libneverd-base.dylib");
    if (Mutation < 2) {
      const auto ID = commands(I, LC_ID_DYLIB).front();
      if (!Mutation)
        I.Bytes[ID + read32le(I.Bytes.data() + ID + 8)] = 'X';
      else
        I.u32(ID + 16, 0);
    } else if (Mutation == 2) {
      I.u32(4, CPU_TYPE_X86_64);
      I.u32(8, CPU_SUBTYPE_X86_64_ALL);
    } else
      I.u32(commands(I, LC_BUILD_VERSION).front() + 8, PLATFORM_IOS);
    TemporaryImage File(I);
    Options.Darwin->Modules[0].Path = File.path();
    rejects();
  }
  // Main already loaded the shared base with compatibility 1.0. The second
  // edge must still enforce the leaf's independent, stronger requirement.
  SetUp();
  auto Leaf = bytes("libneverd-leaf.dylib");
  for (const auto D : commands(Leaf, LC_LOAD_DYLIB)) {
    const auto Offset = D + read32le(Leaf.Bytes.data() + D + 8);
    if (std::string(reinterpret_cast<const char *>(
            Leaf.Bytes.data() + Offset)) == "@loader_path/libneverd-base.dylib")
      Leaf.u32(D + 20, 0x20000);
  }
  TemporaryImage File(Leaf);
  Options.Darwin->Modules[1].Path = File.path();
  rejects("compatibility version");
}
TEST_F(DarwinDylib, SelfDependencyAndExpiredPreparationNeverStartGuestCode) {
  auto I = bytes("libneverd-leaf.dylib");
  for (auto D : commands(I, LC_LOAD_DYLIB)) {
    const auto Offset = D + read32le(I.Bytes.data() + D + 8);
    if (std::string(reinterpret_cast<const char *>(I.Bytes.data() + Offset)) ==
        "@loader_path/libneverd-base.dylib") {
      const char Name[] = "@loader_path/libneverd-leaf.dylib";
      std::copy(std::begin(Name), std::end(Name), I.Bytes.begin() + Offset);
    }
  }
  TemporaryImage File(I);
  Options.Darwin->Modules[1].Path = File.path();
  rejects("cycle");
  SetUp();
  auto Budget = ExecutionBudget::create(
      Options.Limits, ExecutionBudget::Clock::now() - std::chrono::seconds(10));
  ASSERT_TRUE(bool(Budget));
  auto R = darwin_model::loadImage(
      Directory / "main", darwin_model::macOSProfile(), Options, Budget->get());
  ASSERT_FALSE(bool(R));
  bool Timeout = false;
  auto Other = llvm::handleErrors(
      R.takeError(),
      [&](darwin_model::PreparationTimeout &) { Timeout = true; });
  EXPECT_FALSE(bool(Other)) << llvm::toString(std::move(Other));
  EXPECT_TRUE(Timeout);
}
TEST_F(DarwinDylib, InvalidTLSLayoutsFailBeforeAnyExecution) {
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    auto I = bytes("libneverd-leaf.dylib");
    const auto Descriptor = section(I, S_THREAD_LOCAL_VARIABLES);
    const auto Data = section(I, S_THREAD_LOCAL_REGULAR);
    const auto File = read32le(I.Bytes.data() + Descriptor + 48);
    switch (Mutation) {
    case 0:
      I.u64(File + 8, 9);
      break;
    case 1:
      I.u64(File + 16, UINT64_MAX);
      break;
    case 2:
      I.u64(Descriptor + 40, 23);
      break;
    case 3:
      I.u32(Data + 52, 6);
      break;
    case 4:
      I.u32(Data + 64, S_THREAD_LOCAL_INIT_FUNCTION_POINTERS);
      break;
    case 5:
      I.u32(24, read32le(I.Bytes.data() + 24) & ~MH_HAS_TLV_DESCRIPTORS);
      break;
    }
    TemporaryImage FileImage(I);
    Options.Darwin->Modules[1].Path = FileImage.path();
    rejects("TLS");
  }
}
TEST_F(DarwinDylib,
       OrdinaryAddendsKeepProviderChoiceWithRepeatedExportSpellings) {
  const auto Main = bytes("main");
  unsigned Providers = 0;
  for (const char *Name : {"libneverd-base.dylib", "libneverd-leaf.dylib"})
    for (const auto &E : linking(bytes(Name)).Exports)
      Providers += E.Name == "_base_data";
  ASSERT_EQ(Providers, 2u);
  auto Original = load();
  ASSERT_TRUE(bool(Original)) << llvm::toString(Original.takeError());
  auto Offset = loadMain(withAddend(Main, "_base_data", 4));
  ASSERT_TRUE(bool(Offset)) << llvm::toString(Offset.takeError());
  unsigned Compared = 0;
  for (const auto &F : linking(Main).Fixups) {
    if (!F.Bind || F.Symbol != "_base_data")
      continue;
    const auto Before = llvm::find_if(Original->Fixups, [&](const auto &P) {
      return P.Address == F.Address;
    });
    const auto After = llvm::find_if(
        Offset->Fixups, [&](const auto &P) { return P.Address == F.Address; });
    ASSERT_NE(Before, Original->Fixups.end());
    ASSERT_NE(After, Offset->Fixups.end());
    EXPECT_EQ(After->Value, Before->Value + 4);
    ++Compared;
  }
  EXPECT_GT(Compared, 0u);
}
class DarwinDylibAddends : public DarwinDylib {
protected:
  void SetUp() override {
    DarwinDylib::SetUp();
#ifdef NEVERD_DARWIN_FIXTURE_DIR
    Directory = std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) /
                "macos-arm64-dylib-addends-classic";
    Options.Darwin = DarwinProcessOptions{
        { {"@loader_path/libneverd-addends.dylib",
           Directory / "libneverd-addends.dylib"} }};
#endif
  }
  uint64_t providerBase(const Image &Main) {
    auto Prepared = load();
    EXPECT_TRUE(bool(Prepared));
    if (!Prepared) {
      ADD_FAILURE() << llvm::toString(Prepared.takeError());
      return 0;
    }
    for (const auto &F : linking(Main).Fixups)
      if (F.Bind && F.Symbol == "_provider_values" && !F.Addend)
        for (const auto &P : Prepared->Fixups)
          if (P.Address == F.Address)
            return P.Value;
    ADD_FAILURE()
        << "original fixture lacks its zero-addend provider reference";
    return 0;
  }
};
TEST_F(DarwinDylibAddends,
       OriginalDataAndTLSTemplatePointersShareProviderStorage) {
  for (const char *Kind : {"classic", "chained"}) {
    Directory = Directory.parent_path() /
                (std::string("macos-arm64-dylib-addends-") + Kind);
    Options.Darwin->Modules.front().Path =
        Directory / "libneverd-addends.dylib";
    const auto Main = bytes("main");
    const auto Facts = linking(Main);
    const auto TLS = section(Main, S_THREAD_LOCAL_REGULAR);
    const auto TLSBegin = read64le(Main.Bytes.data() + TLS + 32);
    const auto TLSSize = read64le(Main.Bytes.data() + TLS + 40);
    const auto Base = providerBase(Main);
    auto Prepared = load();
    ASSERT_TRUE(bool(Prepared)) << llvm::toString(Prepared.takeError());
    unsigned Positive = 0, Negative = 0, Template = 0;
    for (const auto &F : Facts.Fixups) {
      const bool Plus =
          F.Bind && F.Symbol == "_provider_values" && F.Addend == 8;
      const bool Minus =
          F.Bind && F.Symbol == "_provider_tail" && F.Addend == -4;
      if (!Plus && !Minus)
        continue;
      Positive += Plus;
      Negative += Minus;
      Template += F.Address >= TLSBegin && F.Address - TLSBegin < TLSSize;
      const auto P = llvm::find_if(Prepared->Fixups, [&](const auto &P) {
        return P.Address == F.Address;
      });
      ASSERT_NE(P, Prepared->Fixups.end());
      EXPECT_EQ(P->Value, Base + 8);
    }
    EXPECT_EQ(Positive, 2u);
    EXPECT_EQ(Negative, 1u);
    EXPECT_EQ(Template, 1u);
  }
}
TEST_F(DarwinDylibAddends, ExtremeSignedAddendsFailBeforeExecution) {
  const auto Main = bytes("main");
  ASSERT_NO_FATAL_FAILURE(
      rejectsMain(withAddend(Main, "_provider_values", INT64_MIN),
                  "underflows its provider address"));
  ASSERT_NO_FATAL_FAILURE(rejectsMain(
      withAddend(Main, "_provider_values", INT64_MAX), "outside its provider"));
}
TEST_F(DarwinDylibAddends,
       MainImageRuntimeGatesAndProviderPaddingAreNotProviderTargets) {
  const auto Main = bytes("main");
  auto Library = bytes("libneverd-addends.dylib");
  for (auto C : commands(Library, LC_SEGMENT_64))
    if (std::string(reinterpret_cast<const char *>(Library.Bytes.data() + C +
                                                   8)) == "__LINKEDIT")
      Library.u64(C + 32, read64le(Library.Bytes.data() + C + 48));
  TemporaryImage Provider(Library);
  Options.Darwin->Modules.front().Path = Provider.path();
  const auto Base = providerBase(Main);
  uint64_t Export = 0;
  for (const auto &E : linking(Library).Exports)
    if (E.Name == "_provider_values")
      Export = E.Address;
  ASSERT_NE(Export, 0u);
  const uint64_t Bias = Base - Export;
  uint64_t End = 0;
  for (auto C : commands(Library, LC_SEGMENT_64))
    End = std::max(End, read64le(Library.Bytes.data() + C + 24) +
                            read64le(Library.Bytes.data() + C + 32));
  auto Prepared = load();
  ASSERT_TRUE(bool(Prepared)) << llvm::toString(Prepared.takeError());
  ASSERT_FALSE(Prepared->Imports.empty());
  ASSERT_NE(End % Prepared->Memory.PageSize, 0u);
  uint64_t MainAddress = 0;
  for (auto C : commands(Main, LC_SEGMENT_64))
    if (std::string(reinterpret_cast<const char *>(Main.Bytes.data() + C +
                                                   8)) == "__TEXT")
      MainAddress = read64le(Main.Bytes.data() + C + 24);
  ASSERT_NE(MainAddress, 0u);
  const uint64_t Targets[] = {MainAddress, Prepared->Imports.begin()->first,
                              End + Bias};
  for (const auto Target : Targets) {
    SCOPED_TRACE(Target);
    const int64_t Addend =
        Target >= Base ? int64_t(Target - Base) : -int64_t(Base - Target);
    ASSERT_NO_FATAL_FAILURE(rejectsMain(
        withAddend(Main, "_provider_values", Addend), "outside its provider"));
  }
}
TEST_F(DarwinDylibAddends,
       PointerValuesRequireOneByteMembershipWithoutAnAccessWidth) {
  const auto Main = bytes("main"), Library = bytes("libneverd-addends.dylib");
  const auto Base = providerBase(Main);
  uint64_t Export = 0, Begin = UINT64_MAX, End = 0;
  for (const auto &E : linking(Library).Exports)
    if (E.Name == "_provider_values")
      Export = E.Address;
  ASSERT_NE(Export, 0u);
  const uint64_t Bias = Base - Export;
  for (auto C : commands(Library, LC_SEGMENT_64)) {
    const auto VA = read64le(Library.Bytes.data() + C + 24);
    Begin = std::min(Begin, VA);
    End = std::max(End, VA + read64le(Library.Bytes.data() + C + 32));
  }
  for (const auto Target : {Begin + Bias, End + Bias - 1}) {
    const int64_t Addend =
        Target >= Base ? int64_t(Target - Base) : -int64_t(Base - Target);
    auto R = loadMain(withAddend(Main, "_provider_values", Addend));
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  }
}
TEST_F(DarwinDylib,
       NonzeroTLVDescriptorAndLibSystemAddendsRemainExplicitlyRefused) {
  const auto Main = bytes("main");
  ASSERT_NO_FATAL_FAILURE(rejectsMain(withAddend(Main, "_base_tls", 1),
                                      "TLS import addends are unsupported"));
  auto Library =
      withAddend(bytes("libneverd-base.dylib"), "dyld_stub_binder", 1);
  TemporaryImage Provider(Library);
  Options.Darwin->Modules.front().Path = Provider.path();
  ASSERT_NO_FATAL_FAILURE(rejects("libSystem"));
}
} // namespace
} // namespace neverd::emulation
