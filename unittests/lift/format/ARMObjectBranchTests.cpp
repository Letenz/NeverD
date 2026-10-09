//===- ARMObjectBranchTests.cpp - COFF Thumb branch relocations ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include "neverd/decode/Decoder.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Error.h"

#include <cstring>
#include <iterator>

using namespace neverd;

namespace {

class ARMObjectBranch : public NeverDLiftTest {
protected:
  fs::path compile(const std::string &Name, const std::string &Source) {
    const auto Assembly = tmpFile(Name + ".s");
    const auto Object = tmpFile(Name + ".obj");
    std::ofstream(Assembly) << Source;
    const auto Result =
        exec(NEVERD_TEST_CLANG, {"-target", "thumbv7-windows-msvc", "-c",
                                 Assembly.string(), "-o", Object.string()});
    EXPECT_TRUE(Result.ok()) << Result.err;
    return Object;
  }
};

constexpr auto Branches = R"(
.syntax unified
.text
.thumb
.globl call_external
.def call_external; .scl 2; .type 32; .endef
.thumb_func
call_external:
  push {r4,lr}
.globl call_site
call_site:
  bl external_call
  pop {r4,pc}
.globl jump_site
.def jump_site; .scl 2; .type 32; .endef
.thumb_func
jump_site:
  b.w external_call
.globl cond_site
.def cond_site; .scl 2; .type 32; .endef
.thumb_func
cond_site:
  beq.w external_call
  bx lr
.section .text$last,"xr"
.globl back_site
.def back_site; .scl 2; .type 32; .endef
.thumb_func
back_site:
  bl call_external
  bx lr
)";

TEST_F(ARMObjectBranch, CallsJumpsAndConditionalBranchesReachTheirSymbols) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires cross-target Clang";
  auto Object = compile("branches", Branches);
  auto Image = loadBinary(Object);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const Symbol *External = Image->findSymbol("external_call");
  const Symbol *Internal = Image->findSymbol("call_external");
  ASSERT_NE(External, nullptr);
  ASSERT_NE(Internal, nullptr);
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Arch::ARM, InstructionMode::Thumb));
  for (const char *Name :
       {"call_site", "jump_site", "cond_site", "back_site"}) {
    SCOPED_TRACE(Name);
    const Symbol *Site = Image->findSymbol(Name);
    ASSERT_NE(Site, nullptr);
    const auto *Bytes = Image->readVA(Site->Addr, 4);
    ASSERT_NE(Bytes, nullptr);
    DecodedInsn Instruction{};
    ASSERT_EQ(Decode.decodeOne(Bytes, 4, Site->Addr, Instruction), 4);
    ASSERT_NE(Instruction.Raw, nullptr);
    const auto &ARM = Instruction.Raw->detail->arm;
    ASSERT_EQ(ARM.op_count, 1u);
    ASSERT_EQ(ARM.operands[0].type, ARM_OP_IMM);
    EXPECT_EQ(static_cast<va_t>(ARM.operands[0].imm),
              std::string_view(Name) == "back_site" ? Internal->Addr
                                                    : External->Addr);
  }
  // The all-stage route must see an actual external call rather than a
  // recursive call into the current function's next instruction.
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  Options.OnlyFunctionEntries = {Internal->Addr};
  const auto Result = Pipeline().run(*Image, Context, Options);
  EXPECT_TRUE(Result.Success) << Result.Error;
}

TEST_F(ARMObjectBranch, ConditionalHighBitsAndSignedAddendsDecodeToTheTarget) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires cross-target Clang";
  // Half-megabyte separation distinguishes the uncomplemented J1/J2 fields
  // of conditional branches from the complemented fields of B.W and BL.
  auto Object = compile("distant", R"(
.syntax unified
.thumb
.text
.globl beginning
beginning:
  beq.w far_target
  bl far_target
.section .text$gap,"xr"
.space 0x90000
.section .text$far,"xr"
.p2align 1
.globl far_target
far_target:
  nop
  beq.w beginning
  bx lr
)");
  // The COFF assembler rejects symbolic branch addends. Put +2 into the BL
  // relocation's in-place immediate, as an object producer would.
  std::ifstream Input(Object, std::ios::binary);
  std::vector<uint8_t> Bytes(std::istreambuf_iterator<char>(Input), {});
  Input.close();
  llvm::object::coff_section First;
  ASSERT_GE(Bytes.size(),
            sizeof(llvm::object::coff_file_header) + sizeof(First));
  std::memcpy(&First, Bytes.data() + sizeof(llvm::object::coff_file_header),
              sizeof(First));
  ASSERT_LE(First.PointerToRawData + 8, Bytes.size());
  Bytes[First.PointerToRawData + 6] |= 1;
  std::ofstream Output(Object, std::ios::binary | std::ios::trunc);
  Output.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
  Output.close();
  auto Image = loadBinary(Object);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto *Begin = Image->findSymbol("beginning");
  auto *Far = Image->findSymbol("far_target");
  ASSERT_NE(Begin, nullptr);
  ASSERT_NE(Far, nullptr);
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Arch::ARM, InstructionMode::Thumb));
  for (const auto &[Site, Target] : {std::pair{Begin->Addr, Far->Addr},
                                     std::pair{Begin->Addr + 4, Far->Addr + 2},
                                     std::pair{Far->Addr + 2, Begin->Addr}}) {
    const auto *Bytes = Image->readVA(Site, 4);
    ASSERT_NE(Bytes, nullptr);
    DecodedInsn Instruction{};
    ASSERT_EQ(Decode.decodeOne(Bytes, 4, Site, Instruction), 4);
    ASSERT_EQ(Instruction.Raw->detail->arm.operands[0].type, ARM_OP_IMM);
    EXPECT_EQ(static_cast<va_t>(Instruction.Raw->detail->arm.operands[0].imm),
              Target);
    const auto *Section = Image->getSectionFor(Site);
    ASSERT_NE(Section, nullptr);
    ASSERT_LE(Site - Section->VA + 4, Section->Data.size());
    EXPECT_EQ(std::memcmp(Bytes, Section->Data.data() + Site - Section->VA, 4),
              0);
  }
}

TEST_F(ARMObjectBranch, RejectsMalformedInstructionAndTruncatedField) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires cross-target Clang";
  for (bool Truncated : {false, true}) {
    SCOPED_TRACE(Truncated);
    const auto Object = compile(Truncated ? "truncated" : "opcode", Branches);
    std::ifstream Input(Object, std::ios::binary);
    std::vector<uint8_t> Bytes(std::istreambuf_iterator<char>(Input), {});
    Input.close();
    llvm::object::coff_file_header Header;
    ASSERT_GE(Bytes.size(), sizeof(Header));
    std::memcpy(&Header, Bytes.data(), sizeof(Header));
    llvm::object::coff_section Section;
    std::memcpy(&Section, Bytes.data() + sizeof(Header), sizeof(Section));
    ASSERT_NE(Section.NumberOfRelocations, 0);
    llvm::object::coff_relocation Relocation;
    std::memcpy(&Relocation, Bytes.data() + Section.PointerToRelocations,
                sizeof(Relocation));
    if (Truncated) {
      Relocation.VirtualAddress = Section.SizeOfRawData - 2;
      std::memcpy(Bytes.data() + Section.PointerToRelocations, &Relocation,
                  sizeof(Relocation));
    } else {
      Bytes[Section.PointerToRawData + Relocation.VirtualAddress] = 0;
      Bytes[Section.PointerToRawData + Relocation.VirtualAddress + 1] = 0;
    }
    std::ofstream Output(Object, std::ios::binary | std::ios::trunc);
    Output.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    Output.close();
    auto Image = loadBinary(Object);
    ASSERT_FALSE(bool(Image));
    EXPECT_NE(llvm::toString(Image.takeError()).find("Thumb branch relocation"),
              std::string::npos);
  }
}

TEST_F(ARMObjectBranch, RejectsOutOfRangeConditionAndUnresolvedInterworking) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires cross-target Clang";
  const auto Distant = compile("out-of-range", R"(
.syntax unified
.thumb
.text
  beq.w distant
.section .text$gap,"xr"
.space 0x110000
.section .text$far,"xr"
.globl distant
distant:
  bx lr
)");
  auto Image = loadBinary(Distant);
  ASSERT_FALSE(bool(Image));
  EXPECT_NE(llvm::toString(Image.takeError()).find("exceeds direct range"),
            std::string::npos);
  const auto Interworking = compile("interworking", R"(
.syntax unified
.thumb
.text
  blx external_call
  bx lr
)");
  Image = loadBinary(Interworking);
  ASSERT_FALSE(bool(Image));
  EXPECT_NE(llvm::toString(Image.takeError()).find("interworking veneer"),
            std::string::npos);
}

} // namespace
