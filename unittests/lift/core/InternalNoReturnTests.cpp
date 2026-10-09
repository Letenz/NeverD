#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/ir/low/InternalNoReturn.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"

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

TEST(InternalNoReturn, NoReturnInstructionStillFollowsExceptionalPaths) {
  for (const auto Kind :
       {ExceptionalEdgeKind::CxxCatch, ExceptionalEdgeKind::SEHHandler,
        ExceptionalEdgeKind::ItaniumCatchPad, ExceptionalEdgeKind::GoRecover}) {
    SCOPED_TRACE(static_cast<int>(Kind));
    for (unsigned Destination = 0; Destination != 4; ++Destination) {
      SCOPED_TRACE(Destination);
      auto NoReturnBlock = [](int Id) {
        LowBlock Block;
        Block.Id = Id;
        Block.StartAddr = Base + 0x10 * Id;
        Block.EndAddr = Block.StartAddr + 5;
        LowInstructionBoundary Insn;
        Insn.Address = Block.StartAddr;
        Insn.Size = 5;
        Insn.ControlFlags = LowInstructionControlFlag::NoReturn;
        Block.InstructionBoundaries.push_back(Insn);
        return Block;
      };
      auto Entry = NoReturnBlock(0);
      Entry.Succs.push_back(2);
      Entry.ExceptionalSuccs.push_back({Destination == 2   ? -1
                                        : Destination == 3 ? 99
                                                           : 1,
                                        Base + 0x10, Kind});
      auto Handler = NoReturnBlock(1);
      auto Returning = NoReturnBlock(2);
      LowOp Return;
      Return.Opcode = NdOp::RETURN;
      Returning.InstructionBoundaries.clear();
      Returning.Ops.push_back(Return);
      if (Destination != 1) {
        Handler.InstructionBoundaries.clear();
        Handler.Ops.push_back(Return);
      }
      LowFunc Function;
      Function.Entry = Base;
      Function.Blocks = {Entry, Handler, Returning};
      EXPECT_EQ(lowFunctionNeverReturns(Function, Arch::X86), Destination == 1);
    }
  }
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
  // caller: call callee; ret -- no padding follows, so the callee that
  // never returns is not lifted and the fall-through stays.
  std::vector<uint8_t> Code(0x10, 0xcc);
  putCall(Code, Base, Base + 0x10);
  Code[5] = 0xc3;
  Code.insert(Code.end(), {0xcd, 0x29});
  Code.resize(0x20, 0xcc);
  const auto Image = imageWith(Code, {{Base, "caller"}, {0x1010, "b"}});
  EXPECT_FALSE(callAtBaseEndsItsBlock(Image));
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
