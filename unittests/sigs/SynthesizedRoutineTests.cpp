//===- SynthesizedRoutineTests.cpp - Routines a compiler synthesized ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sigs/PatternGenerator.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace neverd::sigs;
using namespace llvm;

namespace {

TEST(SynthesizedRoutine, NamesTheMachineOutlinersFragments) {
  // ELF, Mach-O (one reserved `_` in front) and a rerun's `<round>_<n>`.
  EXPECT_TRUE(isSynthesizedRoutineName("OUTLINED_FUNCTION_0"));
  EXPECT_TRUE(isSynthesizedRoutineName("_OUTLINED_FUNCTION_217"));
  EXPECT_TRUE(isSynthesizedRoutineName("OUTLINED_FUNCTION_2_15"));
  // A routine of the source, whatever its name holds.
  EXPECT_FALSE(isSynthesizedRoutineName("OUTLINED_FUNCTION_"));
  EXPECT_FALSE(isSynthesizedRoutineName("OUTLINED_FUNCTION_x"));
  EXPECT_FALSE(isSynthesizedRoutineName("OUTLINED_FUNCTION_1_"));
  EXPECT_FALSE(isSynthesizedRoutineName("__OUTLINED_FUNCTION_1"));
  EXPECT_FALSE(isSynthesizedRoutineName("my_OUTLINED_FUNCTION_1"));
  EXPECT_FALSE(isSynthesizedRoutineName("_ZN17OUTLINED_FUNCTION_1E"));
  EXPECT_FALSE(isSynthesizedRoutineName("compile_regex"));
  EXPECT_FALSE(isSynthesizedRoutineName("jit_compile.cold.23"));
}

/// An x64 COFF object with one `.text` section.
class COFFObject {
public:
  explicit COFFObject(std::vector<uint8_t> Code) : Code(std::move(Code)) {}

  /// A function symbol at \p Offset of `.text`.
  void addFunction(StringRef Name, uint32_t Offset, bool External) {
    Symbols.push_back({Name.str(), Offset, 1,
                       uint8_t(External ? COFF::IMAGE_SYM_CLASS_EXTERNAL
                                        : COFF::IMAGE_SYM_CLASS_STATIC)});
  }
  /// A function the object calls and does not define.
  void addUndefined(StringRef Name) {
    Symbols.push_back({Name.str(), 0, COFF::IMAGE_SYM_UNDEFINED,
                       uint8_t(COFF::IMAGE_SYM_CLASS_EXTERNAL)});
  }
  /// A REL32 relocation at \p Offset naming symbol \p Symbol.
  void addRel32(uint32_t Offset, uint32_t Symbol) {
    Relocations.push_back({Offset, Symbol});
  }

  std::vector<uint8_t> build() const {
    std::vector<uint8_t> Out;
    auto put = [&](uint64_t Value, unsigned Bytes) {
      for (unsigned I = 0; I < Bytes; ++I)
        Out.push_back(static_cast<uint8_t>(Value >> (8 * I)));
    };
    const uint32_t CodeOffset = COFF::Header16Size + COFF::SectionSize;
    const uint32_t RelocationOffset = CodeOffset + Code.size();
    const uint32_t SymbolOffset =
        RelocationOffset + Relocations.size() * COFF::RelocationSize;

    put(COFF::IMAGE_FILE_MACHINE_AMD64, 2);
    put(1, 2);
    put(0, 4);
    put(SymbolOffset, 4);
    put(Symbols.size(), 4);
    put(0, 2);
    put(0, 2);

    const std::string SectionName(".text\0\0\0", COFF::NameSize);
    Out.insert(Out.end(), SectionName.begin(), SectionName.end());
    put(0, 4);
    put(0, 4);
    put(Code.size(), 4);
    put(CodeOffset, 4);
    put(RelocationOffset, 4);
    put(0, 4);
    put(Relocations.size(), 2);
    put(0, 2);
    put(COFF::IMAGE_SCN_CNT_CODE | COFF::IMAGE_SCN_MEM_EXECUTE |
            COFF::IMAGE_SCN_MEM_READ | COFF::IMAGE_SCN_ALIGN_16BYTES,
        4);

    Out.insert(Out.end(), Code.begin(), Code.end());
    for (const Relocation &Rel : Relocations) {
      put(Rel.Offset, 4);
      put(Rel.Symbol, 4);
      put(COFF::IMAGE_REL_AMD64_REL32, 2);
    }
    // A name longer than a symbol's eight bytes lives in the string table,
    // which starts with its own size.
    std::string Strings(4, '\0');
    for (const Symbol &Sym : Symbols) {
      if (Sym.Name.size() <= COFF::NameSize) {
        const std::string Short = Sym.Name + std::string(8, '\0');
        Out.insert(Out.end(), Short.begin(), Short.begin() + COFF::NameSize);
      } else {
        put(0, 4);
        put(Strings.size(), 4);
        Strings += Sym.Name;
        Strings.push_back('\0');
      }
      put(Sym.Value, 4);
      put(static_cast<uint16_t>(Sym.Section), 2);
      put(COFF::IMAGE_SYM_DTYPE_FUNCTION << COFF::SCT_COMPLEX_TYPE_SHIFT, 2);
      Out.push_back(Sym.StorageClass);
      Out.push_back(0);
    }
    const uint32_t StringsSize = Strings.size();
    for (unsigned I = 0; I < 4; ++I)
      Strings[I] = static_cast<char>(StringsSize >> (8 * I));
    Out.insert(Out.end(), Strings.begin(), Strings.end());
    return Out;
  }

private:
  struct Symbol {
    std::string Name;
    uint32_t Value;
    int16_t Section;
    uint8_t StorageClass;
  };
  struct Relocation {
    uint32_t Offset;
    uint32_t Symbol;
  };
  std::vector<uint8_t> Code;
  std::vector<Symbol> Symbols;
  std::vector<Relocation> Relocations;
};

TEST(SynthesizedRoutine, COFFFragmentEndsItsNeighbourAndNamesNothing) {
  std::vector<uint8_t> Code(64);
  for (size_t I = 0; I < Code.size(); ++I)
    Code[I] = static_cast<uint8_t>(0x40 + I);
  // call OUTLINED_FUNCTION_0; call helper
  Code[4] = 0xE8;
  Code[12] = 0xE8;
  COFFObject Obj(Code);
  Obj.addFunction("caller", 0, /*External=*/true);
  Obj.addFunction("OUTLINED_FUNCTION_0", 32, /*External=*/false);
  Obj.addUndefined("helper");
  Obj.addRel32(5, 1);
  Obj.addRel32(13, 2);
  const std::vector<uint8_t> Bytes = Obj.build();
  Expected<std::unique_ptr<object::ObjectFile>> File =
      object::ObjectFile::createObjectFile(MemoryBufferRef(
          StringRef(reinterpret_cast<const char *>(Bytes.data()), Bytes.size()),
          "test.obj"));
  ASSERT_TRUE(static_cast<bool>(File)) << toString(File.takeError());

  PatternGeneratorOptions Opts;
  Opts.TailLen = 0xFFFF;
  Opts.EmitReferences = true;
  std::string Text;
  raw_string_ostream OS(Text);
  const PatternGeneratorStats Stats = generatePatterns(**File, Opts, OS);
  OS.flush();
  // One line: `caller`, 32 bytes long, whose only reference is `helper`.
  EXPECT_EQ(StringRef(Text).count('\n'), 1u) << Text;
  EXPECT_NE(Text.find(" 0020 :0000 caller ^000D helper"), std::string::npos)
      << Text;
  EXPECT_EQ(Text.find("OUTLINED"), std::string::npos) << Text;
  EXPECT_EQ(Stats.Synthesized, 1u);
}

} // namespace
