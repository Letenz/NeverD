//===- MachOARMHalfRelocationTests.cpp - ARM paired differences -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/Object/MachO.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

#include <cstring>
#include <iterator>

using namespace neverd;

namespace {

class MachOARMHalfRelocation : public NeverDLiftTest {
protected:
  fs::path compile(bool Thumb) {
    const auto Assembly = tmpFile(Thumb ? "half-thumb.s" : "half-arm.s");
    const auto Object = tmpFile(Thumb ? "half-thumb.o" : "half-arm.o");
    std::ofstream OS(Assembly);
    OS << ".syntax unified\n.text\n"
       << (Thumb ? ".thumb\n" : ".arm\n") << ".globl _get_data\n.p2align 2\n";
    if (Thumb)
      OS << ".thumb_func _get_data\n";
    OS << "_get_data:\n"
       << "  movw r0, :lower16:(Ldata-(Lpc+" << (Thumb ? 4 : 8) << "))\n"
       << "  movt r0, :upper16:(Ldata-(Lpc+" << (Thumb ? 4 : 8) << "))\n"
       << "Lpc:\n  add r0,pc\n  ldr r0,[r0]\n  bx lr\n"
       << ".data\n.p2align 2\nLdata:\n.long 37\n";
    OS.close();
    const auto Result =
        exec(NEVERD_TEST_CLANG, {"-target", "armv7-apple-darwin", "-c",
                                 Assembly.string(), "-o", Object.string()});
    EXPECT_TRUE(Result.ok()) << Result.err;
    return Object;
  }
};

TEST_F(MachOARMHalfRelocation, BothInstructionModesKeepTheDifferenceValue) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires cross-target Clang";
  for (bool Thumb : {false, true}) {
    SCOPED_TRACE(Thumb);
    const auto Object = compile(Thumb);
    auto Buffer = llvm::MemoryBuffer::getFile(Object.string());
    ASSERT_TRUE(bool(Buffer));
    auto Parsed = llvm::object::MachOObjectFile::create(
        (*Buffer)->getMemBufferRef(), true, false);
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
    unsigned Differences = 0;
    for (const auto &Sec : (*Parsed)->sections())
      for (const auto &Reloc : Sec.relocations())
        Differences += Reloc.getType() == llvm::MachO::ARM_RELOC_HALF_SECTDIFF;
    ASSERT_EQ(Differences, 2u);
    auto Image = loadBinary(Object);
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    const auto *Function = Image->findSymbol("_get_data");
    ASSERT_NE(Function, nullptr);
    const auto *Bytes = Image->readVA(Function->Addr, 8);
    ASSERT_NE(Bytes, nullptr);
    auto Half = [Thumb](const uint8_t *At) {
      const uint32_t Word = readLE<uint32_t>(At);
      if (!Thumb)
        return ((Word >> 4) & 0xf000u) | (Word & 0x0fffu);
      const uint16_t Hi = Word, Lo = Word >> 16;
      return ((Hi & 15u) << 12) | ((Hi & 0x400u) << 1) | ((Lo & 0x7000u) >> 4) |
             (Lo & 255u);
    };
    const uint32_t Delta = Half(Bytes) | (Half(Bytes + 4) << 16);
    const va_t Target = Function->Addr + 8 + (Thumb ? 4 : 8) + Delta;
    const auto *Data = Image->readVA(Target, 4);
    ASSERT_NE(Data, nullptr);
    EXPECT_EQ(readLE<uint32_t>(Data), 37u);
    EXPECT_EQ(Image->InstructionAddressMaterializations.count(Function->Addr),
              0u);
    EXPECT_EQ(
        Image->InstructionAddressMaterializations.count(Function->Addr + 4),
        0u);
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {Function->Addr};
    const auto Result = Pipeline().run(*Image, Context, Options);
    EXPECT_TRUE(Result.Success) << Result.Error;
    EXPECT_EQ(Result.HighFuncs.size(), 1u);
  }
}

TEST_F(MachOARMHalfRelocation, RejectsMissingOrIncompatiblePair) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires cross-target Clang";
  for (bool WrongLength : {false, true}) {
    SCOPED_TRACE(WrongLength);
    const auto Object = compile(true);
    std::ifstream Input(Object, std::ios::binary);
    std::vector<uint8_t> Bytes(std::istreambuf_iterator<char>(Input), {});
    Input.close();
    auto Buffer = llvm::MemoryBuffer::getMemBufferCopy(llvm::StringRef(
        reinterpret_cast<const char *>(Bytes.data()), Bytes.size()));
    auto Parsed = llvm::object::MachOObjectFile::create(
        Buffer->getMemBufferRef(), true, false);
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
    bool Mutated = false;
    for (const auto &Sec : (*Parsed)->sections()) {
      const auto Header = (*Parsed)->getSection(Sec.getRawDataRefImpl());
      if (Header.nreloc < 2)
        continue;
      const size_t PairAt =
          Header.reloff + sizeof(llvm::MachO::relocation_info);
      uint32_t Pair = readLE<uint32_t>(Bytes.data() + PairAt);
      if (WrongLength)
        Pair ^= 1u << 28;
      else
        Pair = (Pair & ~(15u << 24)) | (llvm::MachO::ARM_RELOC_VANILLA << 24);
      writeLE<uint32_t>(Bytes.data() + PairAt, Pair);
      Mutated = true;
      break;
    }
    ASSERT_TRUE(Mutated);
    std::ofstream Output(Object, std::ios::binary | std::ios::trunc);
    Output.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    Output.close();
    auto Image = loadBinary(Object);
    ASSERT_FALSE(bool(Image));
    EXPECT_NE(llvm::toString(Image.takeError()).find("malformed ARM HALF pair"),
              std::string::npos);
  }
}

} // namespace
