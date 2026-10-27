//===- SourcePatchCallbackTests.cpp - Original entry CPU equivalence -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare relocated entry instructions with execution at their original PC.
/// CPU equivalence does not establish native loader or unwind registration.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/codegen/SourcePatchCallback.h"
#include "neverd/loader/DWARF/EHFrame.h"
#include "neverd/support/TargetCodegenInfo.h"

#include "llvm/ADT/ScopeExit.h"

#include <array>
#include <map>
#include <string>
#include <unicorn/unicorn.h>

namespace {
using namespace neverd;
struct Entry {
  const char *Name;
  Arch Architecture;
  InstructionMode Mode;
  std::vector<uint8_t> Bytes;
  uint64_t Original = 0x1000;
  uint64_t Callback = 0x2000;
};

class SourceCallbackCPU : public testing::TestWithParam<Entry> {};

TEST_P(SourceCallbackCPU, PreservesRegistersFlagsStackAndPCRelativeStorage) {
  const auto &E = GetParam();
  auto Prefix = relocateSourceCallbackPrefix(E.Architecture, E.Mode, E.Bytes,
                                             E.Original, E.Callback);
  ASSERT_TRUE(bool(Prefix)) << llvm::toString(Prefix.takeError());
  struct State {
    std::map<int, uint64_t> Registers;
    std::array<uint8_t, 4096> Stack{};
  } States[2];
  for (unsigned Copy = 0; Copy < 2; ++Copy) {
    const uc_arch Architecture = E.Architecture == Arch::ARM ? UC_ARCH_ARM
                                 : E.Architecture == Arch::AArch64
                                     ? UC_ARCH_ARM64
                                     : UC_ARCH_X86;
    const uc_mode Mode = E.Architecture == Arch::X64        ? UC_MODE_64
                         : E.Architecture == Arch::X86      ? UC_MODE_32
                         : E.Mode == InstructionMode::Thumb ? UC_MODE_THUMB
                                                            : UC_MODE_ARM;
    uc_engine *CPU = nullptr;
    ASSERT_EQ(uc_open(Architecture, Mode, &CPU), UC_ERR_OK);
    auto Close = llvm::scope_exit([&] { uc_close(CPU); });
    ASSERT_EQ(uc_mem_map(CPU, 0x1000, 0x8000, UC_PROT_ALL), UC_ERR_OK);
    std::array<uint8_t, 0x8000> Initial;
    for (size_t I = 0; I < Initial.size(); ++I)
      Initial[I] = uint8_t((I * 13 + 7) % 251);
    ASSERT_EQ(uc_mem_write(CPU, 0x1000, Initial.data(), Initial.size()),
              UC_ERR_OK);
    // Both executions see the same original and callback byte images.
    ASSERT_EQ(uc_mem_write(CPU, E.Original, E.Bytes.data(), E.Bytes.size()),
              UC_ERR_OK);
    ASSERT_EQ(uc_mem_write(CPU, E.Callback, Prefix->Bytes.data(),
                           Prefix->Bytes.size()),
              UC_ERR_OK);
    std::vector<uint8_t> Continuation;
    ASSERT_TRUE(
        getTargetCodegenInfo(E.Architecture, E.Mode).appendNop(Continuation));
    ASSERT_EQ(uc_mem_write(CPU, E.Original + Prefix->CopiedSize,
                           Continuation.data(), Continuation.size()),
              UC_ERR_OK);
    auto &R = States[Copy].Registers;
    if (E.Architecture == Arch::X64) {
      for (int Reg :
           {UC_X86_REG_RAX, UC_X86_REG_RBX, UC_X86_REG_RCX, UC_X86_REG_RDX,
            UC_X86_REG_RSI, UC_X86_REG_RDI, UC_X86_REG_RBP, UC_X86_REG_R8,
            UC_X86_REG_R9, UC_X86_REG_R10, UC_X86_REG_R11, UC_X86_REG_R12,
            UC_X86_REG_R13, UC_X86_REG_R14, UC_X86_REG_R15})
        R[Reg] = 0x5100 + Reg;
      R[UC_X86_REG_RSP] = 0x7f00;
      R[UC_X86_REG_EFLAGS] = 0x202;
    } else if (E.Architecture == Arch::X86) {
      for (int Reg :
           {UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX,
            UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP})
        R[Reg] = 0x5100 + Reg;
      R[UC_X86_REG_ESP] = 0x7f00;
      R[UC_X86_REG_EFLAGS] = 0x202;
    } else if (E.Architecture == Arch::ARM) {
      for (int Reg = UC_ARM_REG_R0; Reg <= UC_ARM_REG_R12; ++Reg)
        R[Reg] = 0x5100 + Reg;
      R[UC_ARM_REG_SP] = 0x7f00;
      R[UC_ARM_REG_LR] = 0x8200;
      R[UC_ARM_REG_CPSR] =
          E.Mode == InstructionMode::Thumb ? 0x60000030 : 0x60000010;
    } else {
      for (int Reg = UC_ARM64_REG_X0; Reg <= UC_ARM64_REG_X28; ++Reg)
        R[Reg] = 0x5100 + Reg;
      R[UC_ARM64_REG_FP] = 0x7900;
      R[UC_ARM64_REG_LR] = 0x8200;
      R[UC_ARM64_REG_SP] = 0x7f00;
      R[UC_ARM64_REG_NZCV] = 0x60000000;
    }
    for (const auto &[Reg, Value] : R)
      ASSERT_EQ(uc_reg_write(CPU, Reg, &Value), UC_ERR_OK);
    const auto Start = (Copy ? E.Callback : E.Original) |
                       (E.Mode == InstructionMode::Thumb ? 1 : 0);
    // Unicorn's end-address helper calls arm_set_pc with an even address and
    // clears CPSR.T on a taken branch. Stop at an executed code hook instead:
    // this observes the architectural mode before any synthetic exit rewrite.
    struct StopAt {
      uint64_t Address;
      bool Reached = false;
    } Stop{E.Original + Prefix->CopiedSize};
    uc_hook Hook;
    const auto OnCode =
        +[](uc_engine *Engine, uint64_t Address, uint32_t, void *User) {
          auto &Stop = *static_cast<StopAt *>(User);
          if (Address == Stop.Address) {
            Stop.Reached = true;
            uc_emu_stop(Engine);
          }
        };
    ASSERT_EQ(uc_hook_add(CPU, &Hook, UC_HOOK_CODE,
                          reinterpret_cast<void *>(OnCode), &Stop, 1, 0),
              UC_ERR_OK);
    ASSERT_EQ(uc_emu_start(CPU, Start, 0, 1000000, 20), UC_ERR_OK);
    ASSERT_TRUE(Stop.Reached);
    const int PCReg = E.Architecture == Arch::X64   ? int(UC_X86_REG_RIP)
                      : E.Architecture == Arch::X86 ? int(UC_X86_REG_EIP)
                      : E.Architecture == Arch::ARM ? int(UC_ARM_REG_PC)
                                                    : int(UC_ARM64_REG_PC);
    uint64_t PC = 0;
    ASSERT_EQ(uc_reg_read(CPU, PCReg, &PC), UC_ERR_OK);
    EXPECT_EQ(PC, E.Original + Prefix->CopiedSize);
    for (auto &[Reg, Value] : R) {
      Value = 0;
      ASSERT_EQ(uc_reg_read(CPU, Reg, &Value), UC_ERR_OK);
    }
    ASSERT_EQ(uc_mem_read(CPU, 0x7000, States[Copy].Stack.data(), 4096),
              UC_ERR_OK);
  }
  EXPECT_EQ(States[0].Registers, States[1].Registers);
  EXPECT_EQ(States[0].Stack, States[1].Stack);
}

INSTANTIATE_TEST_SUITE_P(
    NativeInstructionSets, SourceCallbackCPU,
    testing::Values(Entry{"X86Stack",
                          Arch::X86,
                          InstructionMode::Default,
                          {0x55, 0x89, 0xe5, 0x83, 0xec, 0x20}},
                    Entry{"X64Stack",
                          Arch::X64,
                          InstructionMode::Default,
                          {0x55, 0x48, 0x89, 0xe5, 0x48, 0x83, 0xec, 0x20}},
                    Entry{"X64Address",
                          Arch::X64,
                          InstructionMode::Default,
                          {0x48, 0x8d, 0x05, 0x00, 0x07, 0x00, 0x00}},
                    Entry{"X64Load",
                          Arch::X64,
                          InstructionMode::Default,
                          {0x48, 0x8b, 0x05, 0xf9, 0x3f, 0x00, 0x00}},
                    Entry{"ARMStack",
                          Arch::ARM,
                          InstructionMode::ARM,
                          {0x10, 0x40, 0x2d, 0xe9}},
                    Entry{"ThumbStack",
                          Arch::ARM,
                          InstructionMode::Thumb,
                          {0x10, 0xb5, 0x84, 0xb0}},
                    Entry{"ARMLiteral",
                          Arch::ARM,
                          InstructionMode::ARM,
                          {0x00, 0x04, 0x9f, 0xe5}},
                    Entry{"ThumbLiteral",
                          Arch::ARM,
                          InstructionMode::Thumb,
                          {0x04, 0x48, 0x00, 0xbf},
                          0x1200,
                          0x1100},
                    Entry{"Thumb2Literal",
                          Arch::ARM,
                          InstructionMode::Thumb,
                          {0xdf, 0xf8, 0x00, 0x04}},
                    Entry{"ARM64Stack",
                          Arch::AArch64,
                          InstructionMode::Default,
                          {0xfd, 0x7b, 0xbf, 0xa9}},
                    Entry{"ARM64Address",
                          Arch::AArch64,
                          InstructionMode::Default,
                          {0x00, 0x40, 0x00, 0x10}},
                    Entry{"ARM64PageAddress",
                          Arch::AArch64,
                          InstructionMode::Default,
                          {0x20, 0x00, 0x00, 0x90}},
                    Entry{"ARM64Literal",
                          Arch::AArch64,
                          InstructionMode::Default,
                          {0x00, 0x10, 0x00, 0x58}}),
    [](const testing::TestParamInfo<Entry> &Info) { return Info.param.Name; });

TEST(SourceCallbackAdmission,
     RejectsControlTransfersAndUnmodeledPCObservations) {
  for (const auto &E :
       {Entry{"Call", Arch::X64, InstructionMode::Default, {0xe8, 0, 0, 0, 0}},
        Entry{"Loop",
              Arch::X86,
              InstructionMode::Default,
              {0xeb, 0xfe, 0x90, 0x90, 0x90}},
        Entry{"Return",
              Arch::X64,
              InstructionMode::Default,
              {0xc3, 0x90, 0x90, 0x90, 0x90}},
        Entry{"Fstenv",
              Arch::X86,
              InstructionMode::Default,
              {0xd9, 0x34, 0x24, 0x90, 0x90}},
        Entry{
            "ARMPC", Arch::ARM, InstructionMode::ARM, {0x0f, 0x00, 0xa0, 0xe1}},
        Entry{"ThumbIT",
              Arch::ARM,
              InstructionMode::Thumb,
              {0x08, 0xbf, 0x01, 0x20}},
        Entry{"ARM64Call",
              Arch::AArch64,
              InstructionMode::Default,
              {0, 0, 0, 0x94}}}) {
    auto P = relocateSourceCallbackPrefix(E.Architecture, E.Mode, E.Bytes,
                                          E.Original, E.Callback);
    EXPECT_FALSE(bool(P)) << E.Name;
    llvm::consumeError(P.takeError());
  }
}

TEST(SourceCallbackAdmission,
     RejectsTruncatedInstructionsAndOutOfReachTargets) {
  for (const auto &E : {Entry{"Truncated",
                              Arch::X64,
                              InstructionMode::Default,
                              {0x48, 0xb8, 1, 2, 3}},
                        Entry{"FarRIP",
                              Arch::X64,
                              InstructionMode::Default,
                              {0x48, 0x8b, 0x05, 0xf9, 0x3f, 0, 0},
                              0x1000,
                              0x900000000},
                        Entry{"FarARM",
                              Arch::ARM,
                              InstructionMode::ARM,
                              {0x00, 0x04, 0x9f, 0xe5},
                              0x1000,
                              0x100000},
                        Entry{"FarLiteral",
                              Arch::AArch64,
                              InstructionMode::Default,
                              {0, 0x10, 0, 0x58},
                              0x1000,
                              0x500000}}) {
    auto P = relocateSourceCallbackPrefix(E.Architecture, E.Mode, E.Bytes,
                                          E.Original, E.Callback);
    EXPECT_FALSE(bool(P)) << E.Name;
    llvm::consumeError(P.takeError());
  }
}

TEST(SourceCallbackUnwind,
     ReencodesOnlyRulesForCopiedInstructionsAndContinuation) {
  const std::vector<uint8_t> Prologue{0x55, 0x48, 0x89, 0xe5,
                                      0x48, 0x83, 0xec, 0x20};
  auto Prefix = relocateSourceCallbackPrefix(
      Arch::X64, InstructionMode::Default, Prologue, 0x1000, 0x2000);
  ASSERT_TRUE(bool(Prefix)) << llvm::toString(Prefix.takeError());
  DwarfCIE CIE;
  CIE.CodeAlignmentFactor = 1;
  CIE.DataAlignmentFactor = -8;
  CIE.ReturnAddressRegister = 16;
  CIE.InitialInstructions = {{CFIOpKind::DefCFA, 0, 7, 0, 8},
                             {CFIOpKind::Offset, 0, 16, 0, -8}};
  DwarfFDE FDE;
  FDE.InitialLocation = 0x1000;
  FDE.AddressRange = 0x40;
  FDE.Instructions = {{CFIOpKind::DefCFAOffset, 1, 0, 0, 16},
                      {CFIOpKind::Offset, 1, 6, 0, -16},
                      {CFIOpKind::DefCFARegister, 4, 6},
                      {CFIOpKind::DefCFA, 0x3f, 7, 0, 8}};
  auto Frame = encodeSourceCallbackDwarfFrame(*Prefix, CIE, FDE, 0x3000);
  ASSERT_TRUE(bool(Frame)) << llvm::toString(Frame.takeError());
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  const auto Decoded =
      dwarf_eh::parseEHFrame(Image, {Frame->data(), Frame->size(), 0x3000}, {});
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_EQ(Decoded.CIEs.size(), 1u);
  ASSERT_EQ(Decoded.FDEs.size(), 1u);
  EXPECT_EQ(Decoded.CIEs[0].DataAlignmentFactor, -8);
  EXPECT_EQ(Decoded.FDEs[0].InitialLocation, 0x2000u);
  EXPECT_EQ(Decoded.FDEs[0].AddressRange, Prefix->Bytes.size());
  unsigned Rules = 0;
  for (const auto &I : Decoded.FDEs[0].Instructions) {
    EXPECT_LE(I.CodeOffset, Prefix->CopiedSize);
    if (I.Kind == CFIOpKind::DefCFARegister) {
      EXPECT_EQ(I.Register, 6u);
      EXPECT_EQ(I.CodeOffset, 4u);
      ++Rules;
    }
    if (I.Kind == CFIOpKind::Offset) {
      EXPECT_EQ(I.Register, 6u);
      EXPECT_EQ(I.Offset, -16);
      EXPECT_EQ(I.CodeOffset, 1u);
      ++Rules;
    }
  }
  EXPECT_EQ(Rules, 2u);
  // A frame rule in the middle of an instruction is not silently rounded.
  FDE.Instructions[2].CodeOffset = 3;
  auto Bad = encodeSourceCallbackDwarfFrame(*Prefix, CIE, FDE, 0x3000);
  EXPECT_FALSE(bool(Bad));
  llvm::consumeError(Bad.takeError());
  FDE.Instructions[2].CodeOffset = 4;
  FDE.LSDAVA = 0x4000;
  auto Language = encodeSourceCallbackDwarfFrame(*Prefix, CIE, FDE, 0x3000);
  EXPECT_FALSE(bool(Language));
  llvm::consumeError(Language.takeError());
}
} // namespace
