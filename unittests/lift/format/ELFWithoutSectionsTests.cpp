//===- ELFWithoutSectionsTests.cpp - ELF images without section headers ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A linked ELF image runs without its section header table (`sstrip`,
// `llvm-objcopy --strip-sections`): its dynamic linker and unwinder read
// every table they need through the program headers.  The loader reads the
// same tables, so a copy of a linked fixture without section headers loads
// to the imports, exports, dynamic facts and relocated bytes the fixture
// itself loads to, and publishes no section.
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/support/BinaryLoading.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

std::filesystem::path fixture(llvm::StringRef Name) {
  return std::filesystem::path(TEST_OBJ_DIR) / Name.str();
}

/// File byte ranges a test rewrote, which a stripped copy need not share.
using FileRanges = std::vector<std::pair<uint64_t, uint64_t>>;

/// The bytes of \p Path with the ELF header naming no section header table,
/// then changed by \p Edit.  \p Edited gains the ranges either rewrote.
std::optional<std::string> withoutSectionHeaders(
    const std::filesystem::path &Path, FileRanges &Edited,
    llvm::function_ref<void(std::string &, FileRanges &)> Edit = nullptr) {
  auto Buffer = llvm::MemoryBuffer::getFile(Path.string());
  if (!Buffer)
    return std::nullopt;
  std::string Bytes = (*Buffer)->getBuffer().str();
  if (Bytes.size() < sizeof(llvm::ELF::Elf64_Ehdr))
    return std::nullopt;
  if (Bytes[llvm::ELF::EI_CLASS] == llvm::ELF::ELFCLASS64) {
    llvm::ELF::Elf64_Ehdr Header;
    std::memcpy(&Header, Bytes.data(), sizeof(Header));
    Header.e_shoff = 0;
    Header.e_shnum = 0;
    Header.e_shstrndx = 0;
    std::memcpy(Bytes.data(), &Header, sizeof(Header));
    Edited.push_back({0, sizeof(Header)});
  } else {
    llvm::ELF::Elf32_Ehdr Header;
    std::memcpy(&Header, Bytes.data(), sizeof(Header));
    Header.e_shoff = 0;
    Header.e_shnum = 0;
    Header.e_shstrndx = 0;
    std::memcpy(Bytes.data(), &Header, sizeof(Header));
    Edited.push_back({0, sizeof(Header)});
  }
  if (Edit)
    Edit(Bytes, Edited);
  return Bytes;
}

/// Every segment of \p Stripped holds the bytes of \p Full's, relocated the
/// same way, apart from the file bytes the test rewrote.
void expectSameBytes(const BinaryImage &Full, const BinaryImage &Stripped,
                     const FileRanges &Edited) {
  ASSERT_EQ(Stripped.Segments.size(), Full.Segments.size());
  for (size_t I = 0; I < Full.Segments.size(); ++I) {
    const Segment &F = Full.Segments[I], &S = Stripped.Segments[I];
    EXPECT_EQ(S.VA, F.VA);
    ASSERT_EQ(S.Data.size(), F.Data.size());
    for (size_t B = 0; B < F.Data.size(); ++B) {
      const uint64_t Offset = F.FileOff + B;
      const bool Rewritten =
          B < F.FileSz && llvm::any_of(
                              Edited,
                              [&](const auto &Range) {
                                return Offset >= Range.first &&
                                       Offset < Range.second;
                              });
      if (!Rewritten && S.Data[B] != F.Data[B]) {
        ADD_FAILURE() << "byte at 0x" << std::hex << F.VA + B;
        break;
      }
    }
  }
}

/// \p Bytes loaded from a file of their own.
llvm::Expected<BinaryImage> loadBytes(const std::string &Bytes) {
  llvm::SmallString<128> Path;
  if (std::error_code EC =
          llvm::sys::fs::createTemporaryFile("neverd-no-sections", "elf", Path))
    return llvm::errorCodeToError(EC);
  {
    std::error_code EC;
    llvm::raw_fd_ostream OS(Path, EC);
    if (EC)
      return llvm::errorCodeToError(EC);
    OS << Bytes;
  }
  auto Image = loadBinary(std::string(Path));
  (void)llvm::sys::fs::remove(Path);
  return Image;
}

std::set<std::tuple<std::string, va_t>> imports(const BinaryImage &Img) {
  std::set<std::tuple<std::string, va_t>> Out;
  for (const Import &Imp : Img.Imports)
    if (Imp.IATAddr != 0)
      Out.insert({Imp.Name, Imp.IATAddr});
  return Out;
}

std::set<std::tuple<std::string, va_t>> exports(const BinaryImage &Img) {
  std::set<std::tuple<std::string, va_t>> Out;
  for (const Export &Exp : Img.Exports)
    Out.insert({Exp.Name, Exp.Addr});
  return Out;
}

TEST(ELFWithoutSections, LoadsTheTablesItsRuntimeReads) {
  // i386 REL with a lazy PLT, x86-64 RELA and RELR, AArch64 RELA.
  const char *const Fixtures[] = {
      "test_pic_library_i386.so", "test_elf_pointer_reloc_x64",
      "test_elf_pointer_relr_x64", "test_call_arg_phi_a64_elf.so"};
  unsigned Checked = 0;
  for (const char *Name : Fixtures) {
    SCOPED_TRACE(Name);
    if (!std::filesystem::exists(fixture(Name)))
      continue;
    auto FullOr = loadBinary(fixture(Name));
    ASSERT_TRUE(static_cast<bool>(FullOr))
        << llvm::toString(FullOr.takeError());
    FileRanges Edited;
    const std::optional<std::string> Bytes =
        withoutSectionHeaders(fixture(Name), Edited);
    ASSERT_TRUE(Bytes.has_value());
    auto StrippedOr = loadBytes(*Bytes);
    ASSERT_TRUE(static_cast<bool>(StrippedOr))
        << llvm::toString(StrippedOr.takeError());
    const BinaryImage &Full = *FullOr;
    const BinaryImage &Stripped = *StrippedOr;
    ++Checked;

    EXPECT_FALSE(Full.Sections.empty());
    EXPECT_TRUE(Stripped.Sections.empty());
    for (const LoadDiagnostic &Diagnostic : Stripped.LoadDiagnostics)
      ADD_FAILURE() << Diagnostic.Code << ": " << Diagnostic.Message;
    EXPECT_EQ(imports(Stripped), imports(Full));
    // Only the dynamic symbols survive: a PIE's .symtab is unreachable.
    const auto StrippedExports = exports(Stripped), FullExports = exports(Full);
    EXPECT_TRUE(std::includes(FullExports.begin(), FullExports.end(),
                              StrippedExports.begin(), StrippedExports.end()));
    // A shared library exports through its dynamic symbols alone.
    if (llvm::StringRef(Name).ends_with(".so"))
      EXPECT_EQ(StrippedExports, FullExports);
    EXPECT_EQ(Stripped.DynInfo.NeededLibs, Full.DynInfo.NeededLibs);
    EXPECT_EQ(Stripped.DynInfo.InitArray, Full.DynInfo.InitArray);
    EXPECT_EQ(Stripped.DynInfo.FiniArray, Full.DynInfo.FiniArray);
    EXPECT_EQ(Stripped.DynInfo.PltGotAddr, Full.DynInfo.PltGotAddr);
    // The unwinder finds .eh_frame through PT_GNU_EH_FRAME.
    std::vector<std::pair<va_t, va_t>> FullFrames, StrippedFrames;
    for (const ExceptionFunction &Function : Full.ExceptionMetadata.Functions)
      FullFrames.push_back({Function.CodeRange.Begin, Function.CodeRange.End});
    for (const ExceptionFunction &Function :
         Stripped.ExceptionMetadata.Functions)
      StrippedFrames.push_back(
          {Function.CodeRange.Begin, Function.CodeRange.End});
    EXPECT_EQ(StrippedFrames, FullFrames);
    if (llvm::StringRef(Name) == "test_pic_library_i386.so")
      EXPECT_FALSE(FullFrames.empty());
    expectSameBytes(Full, Stripped, Edited);
    // A segment maps the ELF header, but no object data is there: a small
    // constant is no pointer to it.
    for (const Segment &Seg : Stripped.Segments)
      if (Seg.FileOff == 0 && Seg.FileSz > sizeof(llvm::ELF::Elf32_Ehdr)) {
        EXPECT_FALSE(Stripped.hasObjectDataProvenance(Seg.VA + 0x1C));
        EXPECT_FALSE(Full.hasObjectDataProvenance(Seg.VA + 0x1C));
      }
  }
  if (Checked == 0)
    GTEST_SKIP() << "the fixtures link with ld.lld";
}

// A table the runtime would not find whole is reported and left out; the
// image still loads.
TEST(ELFWithoutSections, ReportsATableOutsideTheFile) {
  const std::filesystem::path Path = fixture("test_pic_library_i386.so");
  if (!std::filesystem::exists(Path))
    GTEST_SKIP() << "the fixture links with ld.lld";
  auto FullOr = loadBinary(Path);
  ASSERT_TRUE(static_cast<bool>(FullOr)) << llvm::toString(FullOr.takeError());
  // Point DT_STRTAB past every loaded segment.
  FileRanges Edited;
  const std::optional<std::string> Bytes = withoutSectionHeaders(
      Path, Edited, [](std::string &Bytes, FileRanges &Edited) {
        llvm::ELF::Elf32_Ehdr Header;
        std::memcpy(&Header, Bytes.data(), sizeof(Header));
        for (unsigned I = 0; I < Header.e_phnum; ++I) {
          llvm::ELF::Elf32_Phdr Program;
          std::memcpy(&Program,
                      Bytes.data() + Header.e_phoff + I * sizeof(Program),
                      sizeof(Program));
          if (Program.p_type != llvm::ELF::PT_DYNAMIC)
            continue;
          for (size_t At = Program.p_offset;
               At + sizeof(llvm::ELF::Elf32_Dyn) <=
               Program.p_offset + Program.p_filesz;
               At += sizeof(llvm::ELF::Elf32_Dyn)) {
            llvm::ELF::Elf32_Dyn Entry;
            std::memcpy(&Entry, Bytes.data() + At, sizeof(Entry));
            if (Entry.d_tag != llvm::ELF::DT_STRTAB)
              continue;
            Entry.d_un.d_ptr = 0x7FFF0000;
            std::memcpy(Bytes.data() + At, &Entry, sizeof(Entry));
            Edited.push_back({At, At + sizeof(Entry)});
          }
        }
      });
  ASSERT_TRUE(Bytes.has_value());
  auto StrippedOr = loadBytes(*Bytes);
  ASSERT_TRUE(static_cast<bool>(StrippedOr))
      << llvm::toString(StrippedOr.takeError());
  const BinaryImage &Stripped = *StrippedOr;
  bool Reported = false;
  for (const LoadDiagnostic &Diagnostic : Stripped.LoadDiagnostics)
    Reported |= Diagnostic.Code == "elf.dynamic_table_invalid" &&
                Diagnostic.Message.find(".dynstr") != std::string::npos;
  EXPECT_TRUE(Reported);
  // No name can be read, but the relocated bytes are still the same.
  EXPECT_TRUE(imports(Stripped).empty());
  expectSameBytes(*FullOr, Stripped, Edited);
}

} // namespace
