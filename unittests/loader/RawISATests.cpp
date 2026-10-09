//===- RawISATests.cpp - The instruction set a binary file holds ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/LoadCandidate.h"
#include "neverd/loader/Raw/ISAIdentify.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

#include <fstream>
#include <initializer_list>
#include <random>
#include <string>
#include <vector>

namespace neverd {
namespace {

/// The processor this test's own code was compiled for, as RawLoader.def
/// names it.
llvm::StringRef hostProcessor() {
#if defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  return "aarch64";
#elif defined(__i386__) || defined(_M_IX86)
  return "x86";
#else
  return "";
#endif
}

void anchor() {}

/// This test's own executable.
std::vector<uint8_t> ownBytes() {
  const std::string Self = llvm::sys::fs::getMainExecutable(
      nullptr, reinterpret_cast<void *>(&anchor));
  auto Buffer = llvm::MemoryBuffer::getFile(Self, /*IsText=*/false,
                                            /*RequiresNullTerminator=*/false);
  if (!Buffer)
    return {};
  const llvm::StringRef Bytes = (*Buffer)->getBuffer();
  return std::vector<uint8_t>(Bytes.begin(), Bytes.end());
}

/// The words, repeated to a window, in the byte order given.
std::vector<uint8_t> words(std::initializer_list<uint32_t> Words, bool Big) {
  std::vector<uint8_t> Bytes;
  while (Bytes.size() < 4096)
    for (const uint32_t Word : Words) {
      uint8_t Out[4];
      if (Big)
        llvm::support::endian::write32be(Out, Word);
      else
        llvm::support::endian::write32le(Out, Word);
      Bytes.insert(Bytes.end(), Out, Out + 4);
    }
  return Bytes;
}

/// The bytes, repeated to a window.
std::vector<uint8_t> repeated(std::initializer_list<uint8_t> Bytes) {
  std::vector<uint8_t> Out;
  while (Out.size() < 4096)
    Out.insert(Out.end(), Bytes);
  return Out;
}

TEST(RawISA, DataNamesNoProcessor) {
  // Padding, random bytes and text: no window looks like code.
  const std::vector<uint8_t> Zeros(1 << 16, 0);
  std::vector<uint8_t> Random(1 << 16);
  std::mt19937 Generator(3389);
  for (uint8_t &Byte : Random)
    Byte = static_cast<uint8_t>(Generator());
  std::string Text;
  while (Text.size() < (1 << 16))
    Text += "A binary file names no processor; its bytes name one. ";
  for (const llvm::ArrayRef<uint8_t> Bytes :
       {llvm::ArrayRef<uint8_t>(Zeros), llvm::ArrayRef<uint8_t>(Random),
        llvm::arrayRefFromStringRef(Text)}) {
    const ISAIdentification Found = identifyISA(Bytes);
    EXPECT_TRUE(Found.Guesses.empty());
    EXPECT_EQ(Found.CodeShare, 0);
    EXPECT_TRUE(Found.detectedProcessor().empty());
  }
}

TEST(RawISA, ThisProgramsCodeNamesItsProcessor) {
  // This test's own executable, headers, code and data alike, read as a
  // binary file: the bytes name the processor it was compiled for.
  if (hostProcessor().empty())
    GTEST_SKIP() << "no model processor for this host";
  const std::vector<uint8_t> Bytes = ownBytes();
  ASSERT_FALSE(Bytes.empty());
  const ISAIdentification Found = identifyISA(Bytes);
  ASSERT_FALSE(Found.Guesses.empty());
  EXPECT_EQ(Found.Guesses.front().Processor, hostProcessor());
  EXPECT_GE(Found.Guesses.front().Share, 0.9) << Found.describe();
  EXPECT_EQ(Found.detectedProcessor(), hostProcessor());
}

TEST(RawISA, CodeTwoBytesInSaysWhereItsInstructionsStart) {
  if (hostProcessor().empty())
    GTEST_SKIP() << "no model processor for this host";
  std::vector<uint8_t> Bytes = ownBytes();
  ASSERT_FALSE(Bytes.empty());
  Bytes.insert(Bytes.begin(), {0, 0});
  const ISAIdentification Found = identifyISA(Bytes);
  EXPECT_EQ(Found.Outcome, ISAIdentification::Verdict::Settled);
  EXPECT_EQ(Found.detectedProcessor(), hostProcessor());
  // x86 code aligns to bytes; fixed-size words align two bytes in.
  EXPECT_EQ(Found.CodeOffset, 2 % Found.CodeUnit);
}

TEST(RawISA, SixtyFourBitOnlyInstructionsTellTheWidth) {
  // Each family's 32-bit code holds no instruction only its 64-bit set has;
  // the 64-bit set's code is full of them.  Encodings from llvm-mc.
  struct Case {
    const char *Family;
    std::vector<uint8_t> Narrow, Wide;
  };
  // lw, sw, addiu, jal, nop, jr ra, nop, lui, addu; ld, sd, daddiu, daddu,
  // dsll.
  const std::initializer_list<uint32_t> Mips32 = {
      0x8fbf0010, 0xafbf0010, 0x27bdffe0, 0x0c100000, 0,
      0x03e00008, 0,          0x3c1c0042, 0x00851021};
  const std::initializer_list<uint32_t> Mips64 = {
      0xdfbf0010, 0xffbf0010, 0x67bdffe0, 0x0085102d, 0x000210b8};
  const Case Cases[] = {
      {"mips-be", words(Mips32, true), words(Mips64, true)},
      {"mips-le", words(Mips32, false), words(Mips64, false)},
      // stwu, mflr, stw, lwz, blr; std, ld, stdu, rldicl.
      {"ppc-be",
       words({0x9421fff0, 0x7c0802a6, 0x90010014, 0x80010014, 0x4e800020},
             true),
       words({0xf8010010, 0xe8010010, 0xf821ff91, 0x78630020}, true)},
      // save, ld, ret, restore; ldx, stx, sllx.
      {"sparc", words({0x9de3bfa0, 0xd007bffc, 0x81c7e008, 0x81e80000}, true),
       words({0xd05fa7ff, 0xd077a7f7, 0x912a3020}, true)},
      // addi, sw, lw, ret; sd, ld, addiw, subw.
      {"riscv", words({0xff010113, 0x00112623, 0x00c12083, 0x00008067}, false),
       words({0x00113423, 0x00813083, 0x0015051b, 0x40b5053b}, false)},
      // push ebp; mov ebp, esp; mov eax, [ebp+8]; add eax, ecx; pop; ret,
      // and the same in 64-bit registers.
      {"x86",
       repeated({0x55, 0x89, 0xe5, 0x8b, 0x45, 0x08, 0x01, 0xc8, 0x5d, 0xc3}),
       repeated({0x55, 0x48, 0x89, 0xe5, 0x48, 0x8b, 0x45, 0xf8, 0x48, 0x01,
                 0xc8, 0x48, 0x8d, 0x78, 0x08, 0xc3})},
  };
  for (const Case &C : Cases) {
    SCOPED_TRACE(C.Family);
    EXPECT_EQ(readWideShare(C.Family, C.Narrow), 0.0);
    EXPECT_GT(readWideShare(C.Family, C.Wide).value_or(0), 0.2);
  }
  // Read off their alignment, 32-bit MIPS words look 64-bit; read where
  // they start, they do not.
  std::vector<uint8_t> Shifted = words(Mips32, true);
  Shifted.insert(Shifted.begin(), {0, 0});
  EXPECT_GT(readWideShare("mips-be", Shifted, 0).value_or(0), 0.0);
  EXPECT_EQ(readWideShare("mips-be", Shifted, 2), 0.0);
  // An offset past a word reads from where it falls within one.
  EXPECT_EQ(readWideShare("mips-be", Shifted, 6), 0.0);
  // A family of one width has no such share.
  EXPECT_FALSE(readWideShare("aarch64", words(Mips32, false)));
}

TEST(RawISA, AFileStartingWithZerosIsABinaryFileItsBytesName) {
  // Two zero bytes open a COFF object for no machine; with no sections it
  // is no object, and only the bytes name the processor.
  if (hostProcessor().empty())
    GTEST_SKIP() << "no model processor for this host";
  std::vector<uint8_t> Bytes = ownBytes();
  ASSERT_GT(Bytes.size(), 4u);
  std::fill(Bytes.begin(), Bytes.begin() + 4, 0);
  llvm::SmallString<128> Path;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("raw-isa", "bin", Path));
  {
    std::ofstream Out(Path.str().str(), std::ios::binary);
    Out.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
  }
  const std::vector<LoadCandidate> Rows = identifyFile(Path.str().str());
  llvm::sys::fs::remove(Path);
  ASSERT_EQ(Rows.size(), 1u);
  ASSERT_TRUE(Rows.front().ISA);
  EXPECT_EQ(Rows.front().ISA->detectedProcessor(), hostProcessor());
}

TEST(RawISA, ACortexMVectorTableNamesThumbAndWhereTheCodeRuns) {
  // The stack in SRAM, Reset, NMI and HardFault handlers with the Thumb bit,
  // MemManage..UsageFault unused, four reserved zero words, SVCall; then
  // the handlers: bx lr.
  const std::vector<uint32_t> Table = {
      0x20005000, 0x08000041, 0x08000045, 0x08000045, 0, 0,
      0,          0,          0,          0,          0, 0x08000045};
  std::vector<uint8_t> Image(16 * 1024, 0xff);
  for (size_t I = 0; I < Table.size(); ++I)
    for (unsigned Byte = 0; Byte < 4; ++Byte)
      Image[I * 4 + Byte] = static_cast<uint8_t>(Table[I] >> (8 * Byte));
  Image[0x40] = 0x70; // bx lr
  Image[0x41] = 0x47;
  Image[0x44] = 0x70;
  Image[0x45] = 0x47;
  const ISAIdentification Found = identifyISA(Image);
  ASSERT_TRUE(Found.Fingerprint);
  EXPECT_EQ(Found.Fingerprint->Processor, "thumb");
  EXPECT_EQ(Found.Fingerprint->Entry, 0x08000040u);
  EXPECT_EQ(Found.Fingerprint->Base, 0x08000000u);
  // The structure outranks byte statistics, which so little code lacks.
  EXPECT_EQ(Found.detectedProcessor(), "thumb");
  EXPECT_NE(Found.describe().find("Cortex-M"), std::string::npos);

  // A word in SRAM at the start of other data is no vector table.
  std::vector<uint8_t> Data = Image;
  Data[4] = 0x40; // Reset without the Thumb bit
  EXPECT_FALSE(readRawFingerprint(Data));
  Data = Image;
  Data[7 * 4] = 1; // a reserved word set
  EXPECT_FALSE(readRawFingerprint(Data));
}

} // namespace
} // namespace neverd
