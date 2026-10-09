#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/ir/low/InternalNoReturn.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/ProloguePatterns.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string_view>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t Base = 0x1000;

/// x64 code at Base, one function every 0x10 bytes.
BinaryImage imageWith(std::vector<uint8_t> Code,
                      std::vector<std::pair<va_t, const char *>> Functions) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Image.Base = Base;
  Segment Text;
  Text.VA = Base;
  Text.Size = Code.size();
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = std::move(Code);
  Image.Segments.push_back(std::move(Text));
  for (const auto &[Address, Name] : Functions) {
    auto Function = Symbol::makeFunc(Address);
    Function.Name = Name;
    Image.Symbols.push_back(Function);
  }
  return Image;
}

/// `call Target` at \p From, as e8 rel32.
void putCall(std::vector<uint8_t> &Code, va_t From, va_t Target) {
  const auto Offset = static_cast<size_t>(From - Base);
  const auto Rel = static_cast<int32_t>(Target - (From + 5));
  Code[Offset] = 0xe8;
  for (unsigned I = 0; I < 4; ++I)
    Code[Offset + 1 + I] = static_cast<uint8_t>(Rel >> (8 * I));
}

/// Whether the call that starts the function at Base ends its block, as
/// built with an InternalNoReturnIndex over \p Image.
bool callAtBaseEndsItsBlock(const BinaryImage &Image) {
  Decoder Dec;
  EXPECT_TRUE(Dec.init(Image.Arch));
  std::set<va_t> Entries;
  for (const auto &Sym : Image.Symbols)
    Entries.insert(Sym.Addr);
  const libc::NoReturnTargetIndex Names(Image);
  const InternalNoReturnIndex Callees(Image, &Entries, &Names, nullptr,
                                      nullptr);
  CFGBuilder Builder;
  Builder.setKnownFuncEntries(&Entries);
  Builder.setNoReturnTargetIndex(&Names);
  Builder.setNoReturnCalleeProver(&Callees);
  const LowFunc Caller = Builder.build(Image, Dec, Base, "caller");
  for (const LowBlock &Block : Caller.Blocks)
    for (const LowInstructionBoundary &Insn : Block.InstructionBoundaries)
      if (Insn.Address == Base)
        return hasLowInstructionControlFlag(
            Insn.ControlFlags, LowInstructionControlFlag::NoReturn);
  ADD_FAILURE() << "no instruction at the caller's entry";
  return false;
}

/// caller: call callee; int3; ret.  The callee at Base + 0x10 is \p Callee.
std::vector<uint8_t> callerThen(std::vector<uint8_t> Callee) {
  std::vector<uint8_t> Code(0x10, 0xcc);
  putCall(Code, Base, Base + 0x10);
  Code[6] = 0xc3;
  Code.insert(Code.end(), Callee.begin(), Callee.end());
  Code.resize(0x40, 0xcc);
  return Code;
}

TEST(InternalNoReturn, CallThroughALoaderBoundNoReturnSlotEndsItsBlock) {
  // `_start: call *__libc_start_main@GOT(%rip); hlt`. The import the loader
  // binds to the slot is the callee; nothing after the call runs.
  for (const char *Bound : {"__libc_start_main", "puts"}) {
    SCOPED_TRACE(Bound);
    std::vector<uint8_t> Code(0x40, 0xcc);
    // call qword ptr [rip + 0x1ffa] (the slot at 0x3000); hlt
    const uint8_t Call[] = {0xff, 0x15, 0xfa, 0x1f, 0x00, 0x00, 0xf4};
    std::copy(std::begin(Call), std::end(Call), Code.begin());
    auto Image = imageWith(Code, {{Base, "_start"}});
    Segment Got;
    Got.VA = 0x3000;
    Got.Size = Got.FileSz = 8;
    Got.Data.resize(8);
    Got.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Image.Segments.push_back(std::move(Got));
    ASSERT_TRUE(Image.recordImportStorageSlot(
        0x3000, Bound, 0, ImportStorageEvidence::LoaderBind));
    EXPECT_EQ(callAtBaseEndsItsBlock(Image),
              std::string_view(Bound) == "__libc_start_main");
  }
}

TEST(InternalNoReturn, CallToACalleeEndingInFastFailEndsItsBlock) {
  // callee: int 0x29.
  const auto Image =
      imageWith(callerThen({0xcd, 0x29}), {{Base, "caller"}, {0x1010, "b"}});
  EXPECT_TRUE(callAtBaseEndsItsBlock(Image));
}

TEST(InternalNoReturn, CalleeThatCanReturnKeepsTheFallThrough) {
  // callee: test ecx, ecx; jz +2; int 0x29; ret -- one path returns.
  const auto Image =
      imageWith(callerThen({0x85, 0xc9, 0x74, 0x02, 0xcd, 0x29, 0xc3}),
                {{Base, "caller"}, {0x1010, "b"}});
  EXPECT_FALSE(callAtBaseEndsItsBlock(Image));
}

TEST(InternalNoReturn, CalleeEndingInAKnownNoReturnCallEndsItsBlock) {
  // callee: call abort; int3.  abort is in the no-return name list.
  std::vector<uint8_t> Callee(0x10, 0xcc);
  auto Code = callerThen(Callee);
  putCall(Code, Base + 0x10, Base + 0x30);
  Code[0x30] = 0xc3;
  const auto Image =
      imageWith(Code, {{Base, "caller"}, {0x1010, "b"}, {0x1030, "abort"}});
  EXPECT_TRUE(callAtBaseEndsItsBlock(Image));
}

TEST(InternalNoReturn, MutualRecursionWithoutATrapProvesNothing) {
  // callee: call caller; int3 -- a cycle with no terminating fact.
  std::vector<uint8_t> Callee(0x10, 0xcc);
  auto Code = callerThen(Callee);
  putCall(Code, Base + 0x10, Base);
  const auto Image = imageWith(Code, {{Base, "caller"}, {0x1010, "b"}});
  EXPECT_FALSE(callAtBaseEndsItsBlock(Image));
}

TEST(InternalNoReturn, ProofsStopAtTheDepthLimit) {
  // A chain of callees, each `call next; int3`, ending in `int 0x29`: one
  // within the depth limit is proved, one past it is not.
  for (unsigned Length :
       {limits::kMaxNoReturnProofDepth, limits::kMaxNoReturnProofDepth + 1}) {
    SCOPED_TRACE(Length);
    std::vector<uint8_t> Code(0x10 * (Length + 2), 0xcc);
    std::vector<std::pair<va_t, const char *>> Functions{{Base, "caller"}};
    static const char *Names[] = {"b1", "b2", "b3", "b4", "b5", "b6"};
    putCall(Code, Base, Base + 0x10);
    for (unsigned I = 1; I <= Length; ++I) {
      const va_t At = Base + 0x10 * I;
      Functions.push_back({At, Names[I - 1]});
      if (I == Length) {
        Code[At - Base] = 0xcd;
        Code[At - Base + 1] = 0x29;
      } else {
        putCall(Code, At, At + 0x10);
      }
    }
    const auto Image = imageWith(Code, Functions);
    EXPECT_EQ(callAtBaseEndsItsBlock(Image),
              Length <= limits::kMaxNoReturnProofDepth);
  }
}

TEST(InternalNoReturn, CallFollowedByCodeIsNotProved) {
  // caller: call callee; ret -- no padding follows and no metadata gives the
  // callee a code range to check, so the callee that never returns is not
  // lifted and the fall-through stays.
  std::vector<uint8_t> Code(0x10, 0xcc);
  putCall(Code, Base, Base + 0x10);
  Code[5] = 0xc3;
  Code.insert(Code.end(), {0xcd, 0x29});
  Code.resize(0x20, 0xcc);
  const auto Image = imageWith(Code, {{Base, "caller"}, {0x1010, "b"}});
  EXPECT_FALSE(callAtBaseEndsItsBlock(Image));
}

TEST(InternalNoReturn, RecognizesX86AlignmentNoOps) {
  struct Case {
    std::vector<uint8_t> Bytes;
    bool Is64Bit;
    size_t Length;
  };
  const Case Cases[] = {
      {{0x90}, true, 1},
      {{0x66, 0x90}, true, 2},
      {{0x0f, 0x1f, 0x00}, true, 3},
      {{0x0f, 0x1f, 0x40, 0x00}, true, 4},
      {{0x0f, 0x1f, 0x44, 0x00, 0x00}, true, 5},
      {{0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00}, true, 6},
      {{0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00}, true, 7},
      {{0x66, 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
       true,
       11},
      // The 32-bit fills that copy a register onto itself.
      {{0x89, 0xf6}, false, 2},
      {{0x8b, 0xff}, false, 2},
      {{0x8d, 0x76, 0x00}, false, 3},
      {{0x8d, 0x74, 0x26, 0x00}, false, 4},
      {{0x8d, 0xb4, 0x26, 0x00, 0x00, 0x00, 0x00}, false, 7},
      // In 64-bit code they zero the upper half.
      {{0x8d, 0x76, 0x00}, true, 0},
      // Not no-ops: pause, `nop` with a nonzero reg field, an adding lea, a
      // copy between two registers, and a truncated `nop r/m`.
      {{0xf3, 0x90}, true, 0},
      {{0x0f, 0x1f, 0x48, 0x00}, true, 0},
      {{0x8d, 0x76, 0x01}, false, 0},
      {{0x89, 0xf7}, false, 0},
      {{0x0f, 0x1f, 0x84, 0x00}, true, 0},
  };
  for (const Case &C : Cases)
    EXPECT_EQ(x86AlignmentNopLength(C.Bytes.data(), C.Bytes.size(), C.Is64Bit),
              C.Length);
}

TEST(InternalNoReturn, CallFollowedByAlignmentNoOpsEndsItsBlock) {
  // caller: call b, then GCC's no-ops up to b's entry; b: int 0x29.
  std::vector<uint8_t> Code(0x10, 0xcc);
  putCall(Code, Base, Base + 0x10);
  const uint8_t NoOps[] = {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00,
                           0x0f, 0x1f, 0x44, 0x00, 0x00};
  std::copy(std::begin(NoOps), std::end(NoOps), Code.begin() + 5);
  Code.insert(Code.end(), {0xcd, 0x29});
  Code.resize(0x20, 0xcc);
  const auto Image = imageWith(Code, {{Base, "caller"}, {0x1010, "b"}});
  EXPECT_TRUE(callAtBaseEndsItsBlock(Image));
}

TEST(InternalNoReturn, AlignmentNoOpsBeforeTheCallersOwnCodeProveNothing) {
  // caller: call b; nop; ret at an aligned label of its own -- falling
  // through the no-op reaches the label, so it is no padding.  b at 0x1020.
  std::vector<uint8_t> Code(0x20, 0xcc);
  putCall(Code, Base, Base + 0x20);
  Code[5] = 0x0f;
  Code[6] = 0x1f;
  Code[7] = 0x00;
  Code[8] = 0xc3;
  Code.insert(Code.end(), {0xcd, 0x29});
  Code.resize(0x30, 0xcc);
  const auto Image = imageWith(Code, {{Base, "caller"}, {0x1020, "b"}});
  EXPECT_FALSE(callAtBaseEndsItsBlock(Image));
}

TEST(InternalNoReturn, CallEndingItsFunctionsCodeRangeEndsItsBlock) {
  // caller: call b; ret -- the caller's sized symbol ends with the call, so
  // the `ret` is not its code and the call does not return where it is made,
  // though b can (error(3) with a nonzero status is such a callee).  Without
  // the size, the fall-through stays.
  for (bool Sized : {true, false}) {
    SCOPED_TRACE(Sized);
    std::vector<uint8_t> Code(0x10, 0xcc);
    putCall(Code, Base, Base + 0x10);
    Code[5] = 0xc3;
    Code.insert(Code.end(), {0xc3});
    Code.resize(0x20, 0xcc);
    auto Image = imageWith(Code, {{Base, "caller"}, {0x1010, "b"}});
    if (Sized)
      Image.Symbols.front().Size = 5;
    EXPECT_EQ(callAtBaseEndsItsBlock(Image), Sized);
  }
}

TEST(InternalNoReturn, CallerWithAnUnresolvedBranchStillListsItsCallees) {
  // caller: call b; int3.  A switch the caller cannot resolve leaves its own
  // effect unknown, but b's summary does not depend on it: the caller still
  // lists b, so b is lifted and the call passes b's parameters.
  const auto Image =
      imageWith(callerThen({0xc3}), {{Base, "caller"}, {0x1010, "b"}});
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Image.Arch));
  std::set<va_t> Entries{Base, 0x1010};
  CFGBuilder Builder;
  Builder.setKnownFuncEntries(&Entries);
  LowFunc Caller = Builder.build(Image, Dec, Base, "caller");
  Caller.UnsafeIndirectBranchAddresses.insert(Base);
  const LocalRegisterEffect Effect =
      localRegisterEffect(Image, Caller, nullptr);
  EXPECT_TRUE(Effect.Incomplete);
  EXPECT_TRUE(Effect.Unknown);
  EXPECT_EQ(Effect.Callees, std::set<va_t>{0x1010});
}

TEST(InternalNoReturn, CallToACalleeWithNothingThatReturnsIsProved) {
  // caller: call b; ret -- code follows the call, but b's sized symbol
  // covers only `int 0x29`, which does not return, so b is proved.  A b that
  // also holds a `ret`, even one no path reaches, is not worth the proof.
  for (bool HoldsReturn : {false, true}) {
    SCOPED_TRACE(HoldsReturn);
    std::vector<uint8_t> Code(0x10, 0xcc);
    putCall(Code, Base, Base + 0x10);
    Code[5] = 0xc3;
    Code.insert(Code.end(), {0xcd, 0x29, 0xc3});
    Code.resize(0x20, 0xcc);
    auto Image = imageWith(Code, {{Base, "caller"}, {0x1010, "b"}});
    Image.Symbols.back().Size = HoldsReturn ? 3 : 2;
    EXPECT_EQ(callAtBaseEndsItsBlock(Image), !HoldsReturn);
  }
}

TEST(InternalNoReturn, CalleeThatLeavesByAJumpIsNotProvedByItsScreen) {
  // caller: call b; ret -- b holds no `ret`, so the screen sends it to the
  // proof, but b leaves by a jump: to c, which returns, or through RAX to
  // code no one knows.  Either may come back, so the call keeps its
  // fall-through.
  const std::vector<std::vector<uint8_t>> Bodies = {
      {0xe9, 0x0b, 0x00, 0x00, 0x00}, // jmp c (b + 0x10)
      {0xff, 0xe0}};                  // jmp rax
  for (const std::vector<uint8_t> &Body : Bodies) {
    SCOPED_TRACE(Body.size());
    std::vector<uint8_t> Code(0x10, 0xcc);
    putCall(Code, Base, Base + 0x10);
    Code[5] = 0xc3;
    Code.insert(Code.end(), Body.begin(), Body.end());
    Code.resize(0x20, 0xcc);
    Code.push_back(0xc3); // c: ret
    Code.resize(0x30, 0xcc);
    auto Image =
        imageWith(Code, {{Base, "caller"}, {0x1010, "b"}, {0x1020, "c"}});
    for (Symbol &Sym : Image.Symbols)
      if (Sym.Name == "b")
        Sym.Size = Body.size();
    EXPECT_FALSE(callAtBaseEndsItsBlock(Image));
  }
}

TEST(InternalNoReturn, SummaryStillLiftsAProvedCalleeForItsParameters) {
  // caller: call b; int3, with b ending in `int 0x29`.  b's writes reach no
  // caller, but its reads give the call its parameters, so the summary lifts
  // it apart from the callees whose writes count.  A callee the name list
  // knows keeps its prototype and is not lifted.
  for (const char *Name : {"b", "abort"}) {
    SCOPED_TRACE(Name);
    const auto Image =
        imageWith(callerThen({0xcd, 0x29}), {{Base, "caller"}, {0x1010, Name}});
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Image.Arch));
    std::set<va_t> Entries{Base, 0x1010};
    const libc::NoReturnTargetIndex Names(Image);
    const InternalNoReturnIndex Callees(Image, &Entries, &Names, nullptr,
                                        nullptr);
    CFGBuilder Builder;
    Builder.setKnownFuncEntries(&Entries);
    Builder.setNoReturnTargetIndex(&Names);
    Builder.setNoReturnCalleeProver(&Callees);
    const LowFunc Caller = Builder.build(Image, Dec, Base, "caller");
    const LocalRegisterEffect Effect =
        localRegisterEffect(Image, Caller, &Names);
    EXPECT_FALSE(Effect.Callees.count(0x1010));
    EXPECT_EQ(Effect.NoReturnCallees.count(0x1010),
              std::string_view(Name) == "b" ? 1u : 0u);
  }
}

} // namespace
