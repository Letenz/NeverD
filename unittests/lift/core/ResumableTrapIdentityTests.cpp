//===- ResumableTrapIdentityTests.cpp - Debug exception identity ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <vector>

using namespace neverd;

namespace {

std::string emitTraps(const std::vector<uint8_t> &Traps,
                      const std::string &Name, Arch Architecture,
                      BinaryFormat Format, bool LLVMRoute, bool NoOpt,
                      InstructionMode Mode = InstructionMode::Default) {
  constexpr va_t Entry = 0x1000;
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Bits = Architecture == Arch::X64 ? Bitness::Bits64 : Bitness::Bits32;
  Image.Format = Format;
  Image.Mode = Mode;
  Image.Base = Image.Entry = Entry;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  // Define the return value before the breaks, which must preserve it.
  if (Architecture == Arch::ARM)
    Text.Data = Mode == InstructionMode::Thumb
                    ? std::vector<uint8_t>{7, 0x20}
                    : std::vector<uint8_t>{7, 0, 0xa0, 0xe3};
  else
    Text.Data = {0xb8, 7, 0, 0, 0};
  Text.Data.insert(Text.Data.end(), Traps.begin(), Traps.end());
  if (Architecture == Arch::ARM) {
    const auto Return = Mode == InstructionMode::Thumb
                            ? std::vector<uint8_t>{0x70, 0x47}
                            : std::vector<uint8_t>{0x1e, 0xff, 0x2f, 0xe1};
    Text.Data.insert(Text.Data.end(), Return.begin(), Return.end());
  } else
    Text.Data.push_back(0xc3);
  Text.Size = Text.Data.size();
  auto Symbol = Symbol::makeFunc(Entry, Text.Size);
  Symbol.Name = Name;
  Image.Symbols.push_back(std::move(Symbol));
  Image.KnownCodeRanges.emplace_back(Entry, Entry + Text.Size);
  Image.Segments.push_back(std::move(Text));
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  Options.OnlyFunctionEntries.insert(Entry);
  Options.LiftMode = LLVMRoute;
  Options.SourceProjection = LLVMRoute;
  Options.NoOpt = NoOpt;
  auto Result = Pipeline().run(Image, Context, Options);
  if (!Result.Success) {
    ADD_FAILURE() << Result.Error;
    return {};
  }
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions COptions;
  COptions.TheArch = Architecture;
  COptions.Format = Format;
  COptions.Image = &Image;
  if (LLVMRoute) {
    if (!Result.LlvmModule) {
      ADD_FAILURE() << "missing LLVM module";
      return {};
    }
    EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
    EXPECT_TRUE(
        LLVMCEmitter().emit(*Result.LlvmModule, OS, COptions, nullptr, &Image));
  } else {
    EXPECT_FALSE(Result.HighFuncs.empty());
    EXPECT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, COptions));
  }
  return Source;
}

size_t occurrences(llvm::StringRef Text, llvm::StringRef Needle) {
  size_t Count = 0;
  for (size_t Position = 0;
       (Position = Text.find(Needle, Position)) != llvm::StringRef::npos;
       Position += Needle.size())
    ++Count;
  return Count;
}

TEST(ResumableTrapIdentity, BothSourceRoutesKeepInt1DistinctFromInt3) {
  for (Arch Architecture : {Arch::X86, Arch::X64})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
      for (bool LLVMRoute : {false, true})
        for (bool NoOpt : {false, true}) {
          SCOPED_TRACE(::testing::Message()
                       << int(Architecture) << "/" << int(Format) << "/"
                       << LLVMRoute << "/" << NoOpt);
          const auto Source = emitTraps({0xf1, 0xcc, 0xf1}, "trap_identity",
                                        Architecture, Format, LLVMRoute, NoOpt);
          const char *Int1 = !LLVMRoute && Format == BinaryFormat::COFF
                                 ? "_emit 0xf1"
                                 : ".byte 0xf1";
          const char *Int3 = Format == BinaryFormat::COFF
                                 ? "__debugbreak()"
                                 : "__builtin_debugtrap()";
          EXPECT_EQ(occurrences(Source, Int1), 2u) << Source;
          EXPECT_EQ(occurrences(Source, Int3), 1u) << Source;
          EXPECT_NE(Source.find("return 7;"), std::string::npos) << Source;
        }
}

TEST(ResumableTrapIdentity, ArmBreakpointKeepsItsImmediate) {
  for (InstructionMode Mode : {InstructionMode::ARM, InstructionMode::Thumb})
    for (uint32_t Code : {0u, 1u, 0xabu, 0xffu, 0x1234u, 0xffffu}) {
      if (Mode == InstructionMode::Thumb && Code > 0xff)
        continue;
      const uint32_t Word =
          Mode == InstructionMode::Thumb
              ? 0xbe00 | Code
              : 0xe1200070 | ((Code & 0xfff0) << 4) | (Code & 0xf);
      std::vector<uint8_t> Bytes;
      for (unsigned Shift = 0;
           Shift < (Mode == InstructionMode::Thumb ? 16u : 32u); Shift += 8)
        Bytes.push_back(Word >> Shift);
      auto Repeated = Bytes;
      Repeated.insert(Repeated.end(), Bytes.begin(), Bytes.end());
      for (BinaryFormat Format :
           {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
        for (bool LLVMRoute : {false, true})
          for (bool NoOpt : {false, true}) {
            SCOPED_TRACE(::testing::Message()
                         << int(Mode) << "/" << Code << "/" << int(Format)
                         << "/" << LLVMRoute << "/" << NoOpt);
            const auto Source = emitTraps(Repeated, "trap_identity", Arch::ARM,
                                          Format, LLVMRoute, NoOpt, Mode);
            EXPECT_EQ(occurrences(Source, "__builtin_debugtrap"), 0u) << Source;
            EXPECT_EQ(occurrences(Source, "__asm__ volatile(\"bkpt #0x" +
                                              llvm::utohexstr(Code) + "\""),
                      2u)
                << Source;
            EXPECT_NE(Source.find("return 7;"), std::string::npos) << Source;
          }
    }
}

TEST(ResumableTrapIdentity, ArmHaltKeepsItsCodeAndResumedReturn) {
  for (InstructionMode Mode : {InstructionMode::ARM, InstructionMode::Thumb})
    for (uint32_t Code : {0u, 1u, 0x3cu, 0x3fu, 0x1234u, 0xffffu}) {
      if (Mode == InstructionMode::Thumb && Code > 0x3f)
        continue;
      const uint32_t Word =
          Mode == InstructionMode::Thumb
              ? 0xba80 | Code
              : 0xe1000070 | ((Code & 0xfff0) << 4) | (Code & 0xf);
      std::vector<uint8_t> Bytes;
      for (unsigned Shift = 0;
           Shift < (Mode == InstructionMode::Thumb ? 16u : 32u); Shift += 8)
        Bytes.push_back(Word >> Shift);
      auto Repeated = Bytes;
      Repeated.insert(Repeated.end(), Bytes.begin(), Bytes.end());
      for (BinaryFormat Format :
           {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
        for (bool LLVMRoute : {false, true})
          for (bool NoOpt : {false, true}) {
            SCOPED_TRACE(::testing::Message()
                         << int(Mode) << "/" << Code << "/" << int(Format)
                         << "/" << LLVMRoute << "/" << NoOpt);
            const auto Source = emitTraps(Repeated, "trap_identity", Arch::ARM,
                                          Format, LLVMRoute, NoOpt, Mode);
            EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos)
                << Source;
            EXPECT_EQ(occurrences(Source, "__asm__ volatile(\"hlt #0x" +
                                              llvm::utohexstr(Code) + "\""),
                      2u)
                << Source;
            EXPECT_NE(Source.find("return 7;"), std::string::npos) << Source;
          }
    }
}

TEST(ResumableTrapIdentity, ArmUndefinedAliasesKeepTheirCodeAndLiveReturn) {
  for (InstructionMode Mode : {InstructionMode::ARM, InstructionMode::Thumb})
    for (uint32_t Code :
         {0u, 1u, 0xf9u, 0xfeu, 0xffu, 0x1234u, 0xdefeu, 0xfdeeu, 0xffffu}) {
      std::vector<uint8_t> Bytes;
      if (Mode == InstructionMode::Thumb && Code <= 0xff)
        Bytes = {static_cast<uint8_t>(Code), 0xde};
      else if (Mode == InstructionMode::Thumb) {
        const uint16_t First = 0xf7f0 | (Code >> 12);
        const uint16_t Second = 0xa000 | (Code & 0xfff);
        Bytes = {static_cast<uint8_t>(First), static_cast<uint8_t>(First >> 8),
                 static_cast<uint8_t>(Second),
                 static_cast<uint8_t>(Second >> 8)};
      } else {
        const uint32_t Word =
            0xe7f000f0 | ((Code & 0xfff0) << 4) | (Code & 0xf);
        for (unsigned Shift = 0; Shift < 32; Shift += 8)
          Bytes.push_back(Word >> Shift);
      }
      auto Repeated = Bytes;
      Repeated.insert(Repeated.end(), Bytes.begin(), Bytes.end());
      for (BinaryFormat Format :
           {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
        for (bool LLVMRoute : {false, true})
          for (bool NoOpt : {false, true}) {
            SCOPED_TRACE(::testing::Message()
                         << int(Mode) << "/" << Code << "/" << int(Format)
                         << "/" << LLVMRoute << "/" << NoOpt);
            const auto Source = emitTraps(Repeated, "trap_identity", Arch::ARM,
                                          Format, LLVMRoute, NoOpt, Mode);
            EXPECT_EQ(occurrences(Source, "bkpt"), 0u) << Source;
            const std::string Mnemonic = Code > 0xff ? "udf.w" : "udf";
            EXPECT_EQ(occurrences(Source, "__asm__ volatile(\"" + Mnemonic +
                                              " #0x" + llvm::utohexstr(Code) +
                                              "\""),
                      2u)
                << Source;
            EXPECT_NE(Source.find("return 7;"), std::string::npos) << Source;
          }
    }
}

TEST(ResumableTrapIdentity, EveryNarrowThumbTrapKeepsItsCodeAndVoidEffect) {
  constexpr va_t Entry = 0x1000;
  for (const uint8_t Opcode : {0xbe, 0xde, 0xba})
    for (unsigned Code = 0; Code < (Opcode == 0xba ? 64u : 256u); ++Code) {
      SCOPED_TRACE(::testing::Message() << unsigned(Opcode) << "/" << Code);
      const uint8_t Bytes[] = {
          static_cast<uint8_t>(Code | (Opcode == 0xba ? 0x80 : 0)), Opcode};
      const Intrinsic Expected = Opcode == 0xbe   ? Intrinsic::ArmBkpt
                                 : Opcode == 0xba ? Intrinsic::ArmHlt
                                                  : Intrinsic::ArmUdf;
      for (bool Detail : {false, true}) {
        Decoder Dec;
        ASSERT_TRUE(Dec.init(Arch::ARM, InstructionMode::Thumb));
        Dec.setDetail(Detail);
        DecodedInsn Insn;
        ASSERT_EQ(Dec.decodeOneLight(Bytes, sizeof(Bytes), Entry, Insn), 2);
        EXPECT_TRUE(Dec.isResumableTrap(Insn));
        // ARM terminator classification also reads predicates; the decoder's
        // detail-free contract exposes only the opcode and encoded size.
        if (Detail)
          EXPECT_TRUE(Dec.isFunctionTerminator(Insn));
      }
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Arch::ARM, InstructionMode::Thumb));
      DecodedInsn Insn;
      ASSERT_EQ(Dec.decodeOne(Bytes, sizeof(Bytes), Entry, Insn), 2);
      std::vector<LowOp> Ops;
      Dec.liftToLow(Insn, Ops);
      unsigned Count = 0;
      for (const auto &Op : Ops)
        if (Op.Opcode == NdOp::INTRINSIC) {
          ++Count;
          EXPECT_EQ(Op.Output.Size, 0u);
          ASSERT_EQ(Op.NumInputs, 2u);
          EXPECT_EQ(Op.Inputs[0].Offset, static_cast<uint64_t>(Expected));
          EXPECT_TRUE(Op.Inputs[1].isConst());
          EXPECT_EQ(Op.Inputs[1].Offset, Code);
        }
      EXPECT_EQ(Count, 1u);
    }
}

TEST(ResumableTrapIdentity, EveryWideUndefinedEncodingKeepsItsCode) {
  for (InstructionMode Mode : {InstructionMode::ARM, InstructionMode::Thumb}) {
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::ARM, Mode));
    for (uint32_t Code = 0; Code <= 0xffff; ++Code) {
      SCOPED_TRACE(::testing::Message() << int(Mode) << "/" << Code);
      uint32_t Word = 0xe7f000f0 | ((Code & 0xfff0) << 4) | (Code & 0xf);
      if (Mode == InstructionMode::Thumb)
        Word = 0xf7f0 | (Code >> 12) | ((0xa000 | (Code & 0xfff)) << 16);
      const uint8_t Bytes[] = {
          static_cast<uint8_t>(Word), static_cast<uint8_t>(Word >> 8),
          static_cast<uint8_t>(Word >> 16), static_cast<uint8_t>(Word >> 24)};
      DecodedInsn Insn;
      ASSERT_EQ(Dec.decodeOne(Bytes, sizeof(Bytes), 0x1000, Insn), 4);
      EXPECT_TRUE(Dec.isResumableTrap(Insn));
      EXPECT_TRUE(Dec.isFunctionTerminator(Insn));
      std::vector<LowOp> Ops;
      Dec.liftToLow(Insn, Ops);
      unsigned Count = 0;
      for (const auto &Op : Ops)
        if (Op.Opcode == NdOp::INTRINSIC) {
          ++Count;
          EXPECT_EQ(Op.Output.Size, 0u);
          ASSERT_EQ(Op.NumInputs, 2u);
          EXPECT_EQ(Op.Inputs[0].Offset,
                    static_cast<uint64_t>(Intrinsic::ArmUdf));
          EXPECT_TRUE(Op.Inputs[1].isConst());
          EXPECT_EQ(Op.Inputs[1].Offset, Code);
        }
      EXPECT_EQ(Count, 1u);
    }
  }
}

TEST(ResumableTrapIdentity, ArmSourcesRecompileWithTheirTrapCodes) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  const auto Program = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Program)) << "clang is required for trap recompilation";
  const std::string Compiler = *Program;
#endif
  llvm::SmallString<128> SourcePath, ObjectPath, ErrorPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-arm-trap", "c", SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-arm-trap", "o", ObjectPath));
  llvm::FileRemover RemoveObject(ObjectPath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-arm-trap", "err", ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  for (InstructionMode Mode : {InstructionMode::ARM, InstructionMode::Thumb}) {
    std::string Source;
    std::vector<std::string> Encodings;
    unsigned Index = 0;
    for (uint32_t Code : {0xf9u, 0xfeu, 0xffu, 0x100u, 0x1234u, 0xffffu}) {
      std::vector<uint8_t> Bytes;
      if (Mode == InstructionMode::Thumb && Code <= 0xff)
        Bytes = {static_cast<uint8_t>(Code), 0xde};
      else {
        const uint32_t Word =
            Mode == InstructionMode::Thumb
                ? 0xf7f0 | (Code >> 12) | ((0xa000 | (Code & 0xfff)) << 16)
                : 0xe7f000f0 | ((Code & 0xfff0) << 4) | (Code & 0xf);
        for (unsigned Shift = 0; Shift < 32; Shift += 8)
          Bytes.push_back(Word >> Shift);
      }
      Encodings.emplace_back(Bytes.begin(), Bytes.end());
      auto Repeated = Bytes;
      Repeated.insert(Repeated.end(), Bytes.begin(), Bytes.end());
      for (bool LLVMRoute : {false, true})
        for (bool NoOpt : {false, true})
          Source +=
              emitTraps(Repeated, "trap_" + std::to_string(Index++), Arch::ARM,
                        BinaryFormat::ELF, LLVMRoute, NoOpt, Mode);
    }
    for (uint32_t Code : {0u, 0x3cu, 0x3fu, 0x1234u, 0xffffu}) {
      if (Mode == InstructionMode::Thumb && Code > 0x3f)
        continue;
      const uint32_t Word =
          Mode == InstructionMode::Thumb
              ? 0xba80 | Code
              : 0xe1000070 | ((Code & 0xfff0) << 4) | (Code & 0xf);
      std::vector<uint8_t> Bytes;
      for (unsigned Shift = 0;
           Shift < (Mode == InstructionMode::Thumb ? 16u : 32u); Shift += 8)
        Bytes.push_back(Word >> Shift);
      Encodings.emplace_back(Bytes.begin(), Bytes.end());
      auto Repeated = Bytes;
      Repeated.insert(Repeated.end(), Bytes.begin(), Bytes.end());
      for (bool LLVMRoute : {false, true})
        for (bool NoOpt : {false, true})
          Source +=
              emitTraps(Repeated, "trap_" + std::to_string(Index++), Arch::ARM,
                        BinaryFormat::ELF, LLVMRoute, NoOpt, Mode);
    }
    std::error_code EC;
    {
      llvm::raw_fd_ostream Out(SourcePath, EC);
      ASSERT_FALSE(EC);
      Out << Source;
    }
    for (const char *Optimization : {"-O0", "-O2"}) {
      SCOPED_TRACE(::testing::Message() << int(Mode) << "/" << Optimization);
      const llvm::StringRef Arguments[] = {
          Compiler,
          "--target=armv8-none-eabi",
          Mode == InstructionMode::Thumb ? "-mthumb" : "-marm",
          "-mfpu=neon",
          "-mfloat-abi=softfp",
          "-ffreestanding",
          Optimization,
          "-c",
          SourcePath,
          "-o",
          ObjectPath};
      std::string Error;
      const int Result = llvm::sys::ExecuteAndWait(
          Compiler, Arguments, std::nullopt, Redirects, 60, 0, &Error);
      const auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
      ASSERT_EQ(Result, 0) << Error
                           << (Errors ? (*Errors)->getBuffer().str() : "");
      auto Object = llvm::object::ObjectFile::createObjectFile(ObjectPath);
      ASSERT_TRUE(bool(Object)) << llvm::toString(Object.takeError());
      std::vector<size_t> Counts(Encodings.size(), 0);
      for (const auto &Section : Object->getBinary()->sections()) {
        if (!Section.isText())
          continue;
        auto Contents = Section.getContents();
        ASSERT_TRUE(bool(Contents)) << llvm::toString(Contents.takeError());
        for (size_t I = 0; I < Encodings.size(); ++I)
          Counts[I] += occurrences(*Contents, Encodings[I]);
      }
      // Two instructions per function, across both routes and pipeline
      // optimization settings. Check object bytes, not a comment in the C.
      for (size_t Count : Counts)
        EXPECT_EQ(Count, 8u);
    }
  }
}

TEST(ResumableTrapIdentity,
     NativeExceptionVectorsMatchAtBothOptimizationLevels) {
#if defined(__linux__) && defined(__x86_64__)
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  const auto Program = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Program)) << "clang is required for trap execution";
  const std::string Compiler = *Program;
#endif
  std::string Source = "#define _GNU_SOURCE 1\n";
  std::string Cases;
  unsigned Index = 0;
  for (const std::vector<uint8_t> &Traps : std::vector<std::vector<uint8_t>>{
           {0xf1}, {0xf1, 0xf1}, {0xcc}, {0xcc, 0xf1, 0xcc, 0xf1}})
    for (bool LLVMRoute : {false, true})
      for (bool NoOpt : {false, true}) {
        const auto Name = "trap_case_" + std::to_string(Index++);
        Source += emitTraps(Traps, Name, Arch::X64, BinaryFormat::ELF,
                            LLVMRoute, NoOpt);
        Cases += "{\"" + Name + "\", {0xb8, 7, 0, 0, 0";
        for (uint8_t Byte : Traps)
          Cases += ", " + std::to_string(Byte);
        Cases += ", 0xc3}, " + std::to_string(Traps.size() + 6) + ", " +
                 "(fn_t)" + Name + "},\n";
      }
  Source += R"(
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
typedef uint32_t (*fn_t)(void);
static volatile sig_atomic_t count, vectors[8], codes[8];
static void caught(int signal_number, siginfo_t *info, void *context) {
  (void)signal_number;
  unsigned i = count++;
  if (i < 8) {
    vectors[i] = ((ucontext_t *)context)->uc_mcontext.gregs[REG_TRAPNO];
    codes[i] = info->si_code;
  }
}
struct Case { const char *name; unsigned char bytes[16]; size_t size; fn_t lifted; };
static const struct Case cases[] = {
)";
  Source += Cases;
  Source += R"(
};
int main(void) {
  struct sigaction action = {0};
  action.sa_sigaction = caught;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGTRAP, &action, 0)) return 1;
  for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
    const struct Case *t = &cases[c];
    void *memory = mmap(0, 4096, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) return 2;
    memcpy(memory, t->bytes, t->size);
    if (mprotect(memory, 4096, PROT_READ | PROT_EXEC)) return 3;
    for (unsigned repeat = 0; repeat < 32; ++repeat) {
      int expected_vectors[8], expected_codes[8];
      count = 0;
      uint32_t expected = ((fn_t)memory)();
      unsigned expected_count = count;
      if (expected != 7 || expected_count != t->size - 6) return 4;
      for (unsigned i = 0; i < expected_count; ++i) {
        expected_vectors[i] = vectors[i];
        expected_codes[i] = codes[i];
        if (vectors[i] != (t->bytes[5 + i] == 0xf1 ? 1 : 3)) return 5;
      }
      count = 0;
      uint32_t actual = t->lifted();
      if (actual != expected || count != expected_count) return 6;
      for (unsigned i = 0; i < expected_count; ++i)
        if (vectors[i] != expected_vectors[i] || codes[i] != expected_codes[i]) {
          fprintf(stderr, "%s: event %u vector/code %d/%d, expected %d/%d\n",
                  t->name, i, vectors[i], codes[i],
                  expected_vectors[i], expected_codes[i]);
          return 7;
        }
    }
    if (munmap(memory, 4096)) return 8;
  }
  return 0;
}
)";
  llvm::SmallString<128> SourcePath, BinaryPath, ErrorPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-trap-identity", "c",
                                                  SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-trap-identity", "exe",
                                                  BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-trap-identity", "err",
                                                  ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code EC;
  {
    llvm::raw_fd_ostream Out(SourcePath, EC);
    ASSERT_FALSE(EC);
    Out << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  for (const char *Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization);
    const llvm::StringRef Arguments[] = {
        Compiler,   "-std=c11", Optimization, "-Werror=return-type",
        SourcePath, "-o",       BinaryPath};
    std::string Error;
    int Result = llvm::sys::ExecuteAndWait(Compiler, Arguments, std::nullopt,
                                           Redirects, 60, 0, &Error);
    auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Result, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "");
    Result = llvm::sys::ExecuteAndWait(BinaryPath, {BinaryPath}, std::nullopt,
                                       Redirects, 10, 0, &Error);
    Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Result, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "");
  }
#else
  GTEST_SKIP() << "native exception-vector comparison requires Linux x64";
#endif
}

} // namespace
