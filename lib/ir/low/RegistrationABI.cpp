//===- RegistrationABI.cpp - Checked PE32 registration call ABI ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/RegistrationABI.h"

#include "RegistrationFrame.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowNoReturn.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

#include <deque>
#include <set>

namespace neverd {
namespace {
struct PrivateFrameState {
  registration_state::FrameState Frame;
  registration_state::FrameState Borrow;
  registration_state::FrameState BorrowSpills;
  std::set<int32_t> Initialized;

  bool merge(const PrivateFrameState &Other) {
    bool Changed = Frame.merge(Other.Frame);
    Changed |= Borrow.merge(Other.Borrow);
    Changed |= BorrowSpills.merge(Other.BorrowSpills);
    for (auto It = Initialized.begin(); It != Initialized.end();)
      if (!Other.Initialized.count(*It)) {
        It = Initialized.erase(It);
        Changed = true;
      } else
        ++It;
    return Changed;
  }
};
struct ImageFrameEffects {
  std::set<std::pair<va_t, va_t>> Reads;
  std::set<std::pair<va_t, va_t>> Writes;
  std::set<std::pair<va_t, va_t>> CallerPCWrites;
  std::set<std::pair<int32_t, int32_t>> ECXReads;
  std::set<std::pair<int32_t, int32_t>> ECXWrites;
};

template <typename Set, typename Vector>
void copyExtents(const Set &From, Vector &To) {
  for (const auto &[Begin, End] : From) {
    if (!To.empty() && Begin <= To.back().End)
      To.back().End = std::max(To.back().End, End);
    else
      To.push_back({Begin, End});
  }
}

bool callerPCIsNotReadBack(const ImageFrameEffects &Effects) {
  auto Read = Effects.Reads.begin();
  for (const auto &[Begin, End] : Effects.CallerPCWrites) {
    while (Read != Effects.Reads.end() && Read->second <= Begin)
      ++Read;
    if (Read != Effects.Reads.end() && Read->first < End)
      return false;
  }
  return true;
}

bool chargeCalleeWork(size_t &Work, size_t Amount) {
  if (Work > limits::kMaxRegistrationEHStateWork ||
      Amount > limits::kMaxRegistrationEHStateWork - Work) {
    Work = limits::kMaxRegistrationEHStateWork;
    return false;
  }
  Work += Amount;
  return true;
}

bool chargeDecodedCallee(size_t &Work, const LowFunc &Function) {
  for (const auto &Block : Function.Blocks)
    if (!chargeCalleeWork(Work, Block.Ops.size() +
                                    Block.InstructionBoundaries.size() + 1))
      return false;
  return true;
}

bool hasPrivateCallerFrame(const LowFunc &Function, const BinaryImage &Image,
                           size_t &Work, ImageFrameEffects &Effects,
                           bool BorrowECX = false,
                           RegistrationThrowCalleeABI *ThrowProof = nullptr) {
  using namespace registration_state;
  auto Charge = [&](size_t Amount) {
    if (Amount > limits::kMaxRegistrationEHStateWork - Work)
      return false;
    Work += Amount;
    return true;
  };
  std::map<int, const LowBlock *> Blocks;
  PrivateFrameState Initial;
  for (unsigned Index = 0; Index != Initial.Frame.Registers.size(); ++Index)
    Initial.Frame.Registers[Index] = {{},   {},    false,
                                      true, false, uint8_t(Index + 1)};
  Initial.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
      FrameValue::frame(4);
  FrameValue CallerPC;
  CallerPC.MayBeFrame = CallerPC.ReturnPC = true;
  Initial.Frame.Cells[4] = CallerPC;
  if (BorrowECX) {
    Initial.Frame.Registers[x86reg::RCX / x86reg::GeneralRegStride] = {};
    Initial.Borrow.Registers[x86reg::RCX / x86reg::GeneralRegStride] =
        FrameValue::frame(0);
  }
  for (const auto &Block : Function.Blocks) {
    Blocks.emplace(Block.Id, &Block);
    for (const auto &Op : Block.Ops)
      for (unsigned Index = 0; Index != Op.NumInputs; ++Index) {
        const auto &Input = Op.Inputs[Index];
        if (!Charge(1 + Input.Size))
          return false;
        if (Input.isReg() && Input.Offset >= 8 * x86reg::GeneralRegStride)
          for (uint64_t Byte = 0; Byte != Input.Size; ++Byte)
            Initial.Frame.OtherRegisterBytes[Input.Offset + Byte] = {
                {}, {}, false, true};
      }
  }
  const auto Entry = llvm::find_if(Function.Blocks, [&](const LowBlock &Block) {
    return Block.StartAddr == Function.Entry;
  });
  if (Entry == Function.Blocks.end())
    return false;
  std::map<int, PrivateFrameState> Incoming;
  Incoming.emplace(Entry->Id, Initial);
  std::deque<int> Pending{Entry->Id};
  while (!Pending.empty()) {
    const int Id = Pending.front();
    Pending.pop_front();
    if (!Blocks.count(Id) ||
        !Charge(1 + Incoming.at(Id).Frame.Cells.size() +
                Incoming.at(Id).Frame.OtherRegisterBytes.size() +
                Incoming.at(Id).Borrow.Cells.size() +
                Incoming.at(Id).BorrowSpills.Cells.size() +
                Incoming.at(Id).Borrow.OtherRegisterBytes.size() +
                Incoming.at(Id).Initialized.size()))
      return false;
    PrivateFrameState State = Incoming.at(Id);
    FrameTransfer Transfer(State.Frame, 0);
    FrameTransfer BorrowTransfer(State.Borrow, 0);
    bool Threw = false;
    for (const auto &Op : Blocks.at(Id)->Ops) {
      size_t RegisterBytes = Op.Output.Size;
      for (unsigned Index = 0; Index != Op.NumInputs; ++Index)
        RegisterBytes += Op.Inputs[Index].Size;
      if (!Charge(1 + RegisterBytes) || Op.Opcode == NdOp::INTRINSIC ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return false;
      Transfer.beginInstruction(Op.Addr);
      BorrowTransfer.beginInstruction(Op.Addr);
      auto BorrowValue = BorrowTransfer.evaluate(Op, false);
      const auto Memory = lowMemoryOperands(Op);
      if (Memory.Address) {
        if (!Memory.Complete ||
            !Charge(State.Frame.Cells.size() + State.Borrow.Cells.size() +
                    State.BorrowSpills.Cells.size() + Memory.AccessSize))
          return false;
        const auto Address = Transfer.read(*Memory.Address);
        const auto Object = BorrowTransfer.read(*Memory.Address);
        if (Object.MayBeFrame && !Object.Offset)
          return false;
        if (BorrowECX && Object.Offset) {
          const int64_t End = int64_t(*Object.Offset) + Memory.AccessSize;
          if (Address.Offset || Address.Constant || Address.MayBeFrame ||
              *Object.Offset < 0 || End > INT32_MAX ||
              End > limits::kMaxRegistrationEHStateWork)
            return false;
          if (Op.Opcode != NdOp::STORE)
            Effects.ECXReads.emplace(*Object.Offset, int32_t(End));
          if (Memory.StoredValue) {
            const auto Stored = Transfer.read(*Memory.StoredValue);
            const auto Borrowed = BorrowTransfer.read(*Memory.StoredValue);
            if (Op.Opcode != NdOp::STORE || Stored.MayBeFrame ||
                Borrowed.MayBeFrame)
              return false;
            Effects.ECXWrites.emplace(*Object.Offset, int32_t(End));
            State.Borrow.store(*Object.Offset, Memory.AccessSize, Borrowed);
          }
        } else {
          // An unknown address can point into the caller frame. Stack arguments
          // also require a separate extent/initialization proof, not a guessed
          // parameter list from an arbitrary positive EBP displacement.
          if ((!Address.Offset && !Address.Constant) ||
              (Address.MayBeFrame && !Address.Offset) ||
              (Address.Offset &&
               int64_t(*Address.Offset) + Memory.AccessSize > 8))
            return false;
          if (Address.Constant && *Address.Constant != 0) {
            const auto *Owner = Image.getSegmentFor(*Address.Constant);
            if (!Owner || *Address.Constant < Owner->VA ||
                uint64_t(*Address.Constant) - Owner->VA > Owner->Size ||
                Memory.AccessSize >
                    Owner->Size - (uint64_t(*Address.Constant) - Owner->VA))
              return false;
          }
          if (Address.Constant && Op.Opcode != NdOp::STORE)
            Effects.Reads.emplace(*Address.Constant,
                                  va_t(*Address.Constant) + Memory.AccessSize);
          if (Address.Constant && Memory.StoredValue)
            Effects.Writes.emplace(*Address.Constant,
                                   va_t(*Address.Constant) + Memory.AccessSize);
          if (Address.Offset) {
            const int64_t End = int64_t(*Address.Offset) + Memory.AccessSize;
            if (End > INT32_MAX)
              return false;
            const bool Reads = Op.Opcode != NdOp::STORE;
            // The real caller PC intentionally identifies the regenerated call
            // site. Other bytes need a store by this invocation on every path.
            const bool CallerPC =
                *Address.Offset == 4 && Memory.AccessSize == 4;
            const auto SP =
                State.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride]
                    .Offset;
            if (!CallerPC && (!SP || *Address.Offset < *SP))
              return false;
            if (Reads && !CallerPC)
              for (int64_t Byte = *Address.Offset; Byte < End; ++Byte)
                if (!State.Initialized.count(int32_t(Byte)))
                  return false;
            if (Memory.StoredValue) {
              if (End > 4 || Op.Opcode != NdOp::STORE)
                return false;
              const auto Value = Transfer.read(*Memory.StoredValue);
              State.Frame.store(*Address.Offset, Memory.AccessSize, Value);
              if (BorrowECX)
                State.BorrowSpills.store(
                    *Address.Offset, Memory.AccessSize,
                    BorrowTransfer.read(*Memory.StoredValue));
              for (int64_t Byte = *Address.Offset; Byte < End; ++Byte)
                State.Initialized.insert(int32_t(Byte));
            }
            if (Op.Opcode == NdOp::LOAD)
              BorrowValue =
                  State.BorrowSpills.load(Address.Offset, Memory.AccessSize);
          } else if (Memory.StoredValue &&
                     Transfer.read(*Memory.StoredValue).MayBeFrame) {
            if (!Transfer.read(*Memory.StoredValue).ReturnPC)
              return false;
            Effects.CallerPCWrites.emplace(
                *Address.Constant, va_t(*Address.Constant) + Memory.AccessSize);
          }
          if (Memory.StoredValue && !Address.Offset &&
              BorrowTransfer.read(*Memory.StoredValue).MayBeFrame)
            return false;
        }
      }
      std::optional<int32_t> AfterCallSP;
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        if (BorrowECX)
          return false;
        if (ThrowProof) {
          const auto SP =
              State.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride]
                  .Offset;
          if (!SP || *SP > INT32_MAX - 8 || Op.Opcode != NdOp::CALL ||
              Op.NumInputs != 1 || !Op.Inputs[0].isConst() ||
              Op.Inputs[0].Size != 4 ||
              Op.Inputs[0].Offset != ThrowProof->ImportVA ||
              Op.Addr != ThrowProof->ThrowCallVA ||
              Op.Seq != ThrowProof->ThrowOpSeq)
            return false;
          const auto Object = State.Frame.load(*SP, 4);
          const auto Table = State.Frame.load(*SP + 4, 4);
          if (!Object.Offset || Object.ReturnPC || Object.FrameOnlyFromCall ||
              !Table.Constant || Table.MayBeFrame)
            return false;
          auto Info = coff_loader::getCheckedX86SimpleCxxThrowInfo(
              Image, *Table.Constant);
          if (!Info ||
              !Charge(4096 + Info->ObjectSize + State.Frame.Cells.size()))
            return false;
          const int64_t End = int64_t(*Object.Offset) + Info->ObjectSize;
          if (*Object.Offset < *SP + 8 || End > 4)
            return false;
          for (int64_t Byte = *Object.Offset; Byte != End; ++Byte)
            if (!State.Initialized.count(int32_t(Byte)))
              return false;
          for (const auto &[Cell, Value] : State.Frame.Cells)
            if (Value.MayBeFrame && int64_t(Cell) < End &&
                int64_t(*Object.Offset) < int64_t(Cell) + 4)
              return false;
          for (const auto &Range : Info->ReadOnlyRanges)
            Effects.Reads.emplace(Range.Begin, Range.End);
          Effects.Reads.emplace(Info->TypeDescriptorRange.Begin,
                                Info->TypeDescriptorRange.End);
          ThrowProof->ObjectOffset = *Object.Offset;
          ThrowProof->ThrowInfo = std::move(*Info);
          Threw = true;
          break;
        }
        if (!Charge(State.Frame.Cells.size() + State.Initialized.size()))
          return false;
        for (unsigned Register = 0; Register != State.Frame.Registers.size();
             ++Register)
          if (Register != x86reg::RBP / x86reg::GeneralRegStride &&
              Register != x86reg::RSP / x86reg::GeneralRegStride &&
              State.Frame.Registers[Register].MayBeFrame &&
              !State.Frame.Registers[Register].ReturnPC &&
              State.Frame.Registers[Register].SavedRegister != Register + 1)
            return false;
        for (const auto &[Offset, Value] : State.Frame.Cells)
          if (Offset < 0 && Value.MayBeFrame)
            return false;
        const auto SP =
            State.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride]
                .Offset;
        if (!SP)
          return false;
        unsigned Pop = 0;
        if (Op.NumInputs == 1 && Op.Inputs[0].isConst())
          if (const auto *Import = Image.findImportAt(Op.Inputs[0].Offset))
            if (Import->Name == "RaiseException")
              Pop = 16;
        if (int64_t(*SP) + Pop > INT32_MAX)
          return false;
        AfterCallSP = int32_t(int64_t(*SP) + Pop);
        // A callee owns everything below call SP and may overwrite its
        // outgoing argument area. A later read needs a fresh local store.
        for (auto It = State.Initialized.begin();
             It != State.Initialized.end();)
          if (*It < *AfterCallSP)
            It = State.Initialized.erase(It);
          else
            ++It;
        for (auto It = State.Frame.Cells.begin();
             It != State.Frame.Cells.end();)
          if (It->first < *AfterCallSP)
            It = State.Frame.Cells.erase(It);
          else
            ++It;
        for (auto It = State.BorrowSpills.Cells.begin();
             It != State.BorrowSpills.Cells.end();)
          if (It->first < *AfterCallSP)
            It = State.BorrowSpills.Cells.erase(It);
          else
            ++It;
      }
      if (Op.Opcode == NdOp::COND_BR || Op.Opcode == NdOp::INDIR_BR ||
          Op.Opcode == NdOp::RETURN || Op.Opcode == NdOp::INTRINSIC)
        for (unsigned Index = 0; Index != Op.NumInputs; ++Index) {
          const auto Value = Transfer.read(Op.Inputs[Index]);
          if (BorrowTransfer.read(Op.Inputs[Index]).MayBeFrame)
            return false;
          if (Value.MayBeFrame &&
              !(Op.Opcode == NdOp::RETURN &&
                (Value.FrameOnlyFromCall || Value.ReturnPC ||
                 Value.SavedRegister ==
                     x86reg::RAX / x86reg::GeneralRegStride + 1)))
            return false;
        }
      if (Op.Opcode == NdOp::RETURN) {
        if (State.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride]
                .Offset != 4)
          return false;
        for (uint64_t Register :
             {x86reg::RBX, x86reg::RBP, x86reg::RSI, x86reg::RDI}) {
          const auto Index = Register / x86reg::GeneralRegStride;
          if (State.Frame.Registers[Index] != Initial.Frame.Registers[Index])
            return false;
          if (State.Borrow.Registers[Index] != Initial.Borrow.Registers[Index])
            return false;
        }
      }
      Transfer.write(Op, Transfer.evaluate(Op, false));
      if (BorrowECX)
        BorrowTransfer.write(Op, BorrowValue);
      if (AfterCallSP)
        State.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
            FrameValue::frame(*AfterCallSP);
    }
    if (Threw)
      continue;
    for (int Successor : Blocks.at(Id)->Succs) {
      const auto Existing = Incoming.find(Successor);
      const auto StateSize = [](const PrivateFrameState &S) {
        return S.Frame.Cells.size() + S.Frame.OtherRegisterBytes.size() +
               S.Borrow.Cells.size() + S.BorrowSpills.Cells.size() +
               S.Borrow.OtherRegisterBytes.size() + S.Initialized.size();
      };
      if (!Charge(
              1 + StateSize(State) +
              (Existing == Incoming.end() ? 0 : StateSize(Existing->second))))
        return false;
      auto [It, New] = Incoming.emplace(Successor, State);
      if (New || It->second.merge(State))
        Pending.push_back(Successor);
    }
  }
  return true;
}
} // namespace

std::optional<RegistrationLeafCalleeABI>
getCheckedX86RegistrationLeafCalleeABI(const BinaryImage &Image, va_t Target,
                                       size_t *CumulativeWork) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || Target > UINT32_MAX ||
      !Image.isCodeAddress(Target) ||
      Image.ExceptionMetadata.findFunction(Target))
    return std::nullopt;
  size_t LocalWork = 0;
  size_t &Work = CumulativeWork ? *CumulativeWork : LocalWork;
  if (!chargeCalleeWork(Work, 1))
    return std::nullopt;
  Decoder Decoder;
  if (!Decoder.init(Image))
    return std::nullopt;
  CFGBuilder Builder;
  const LowFunc Callee = Builder.build(Image, Decoder, Target, "abi-leaf");
  if (!chargeDecodedCallee(Work, Callee) || Callee.CalleePopBytes ||
      !Callee.hasCompleteLiftCoverage() || Callee.Blocks.empty() ||
      Callee.ExceptionMetadata)
    return std::nullopt;
  for (const auto &Block : Callee.Blocks) {
    if (Block.Succs.empty() &&
        (Block.Ops.empty() || Block.Ops.back().Opcode != NdOp::RETURN))
      return std::nullopt;
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
        return std::nullopt;
  }
  ImageFrameEffects Effects;
  if (!hasPrivateCallerFrame(Callee, Image, Work, Effects, true))
    return std::nullopt;
  if (!callerPCIsNotReadBack(Effects))
    return std::nullopt;
  RegistrationLeafCalleeABI Result;
  Result.Target = Target;
  copyExtents(Effects.ECXReads, Result.ECXReads);
  copyExtents(Effects.ECXWrites, Result.ECXWrites);
  copyExtents(Effects.Reads, Result.ImageReads);
  copyExtents(Effects.Writes, Result.ImageWrites);
  copyExtents(Effects.CallerPCWrites, Result.CallerPCWrites);
  return Result;
}

std::optional<RegistrationThrowCalleeABI>
getCheckedX86RegistrationThrowCalleeABI(const BinaryImage &Image, va_t Target,
                                        size_t *CumulativeWork) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || Target > UINT32_MAX ||
      !Image.isCodeAddress(Target) ||
      Image.ExceptionMetadata.findFunction(Target))
    return std::nullopt;
  size_t LocalWork = 0;
  size_t &Work = CumulativeWork ? *CumulativeWork : LocalWork;
  if (!chargeCalleeWork(Work, 1))
    return std::nullopt;
  Decoder Decoder;
  if (!Decoder.init(Image))
    return std::nullopt;
  CFGBuilder Builder;
  const LowFunc Callee = Builder.build(Image, Decoder, Target, "abi-throw");
  if (!chargeDecodedCallee(Work, Callee) || Callee.CalleePopBytes ||
      !Callee.hasCompleteLiftCoverage() || Callee.Blocks.empty() ||
      Callee.ExceptionMetadata || !lowFunctionNeverReturns(Callee, Arch::X86))
    return std::nullopt;
  RegistrationThrowCalleeABI Result;
  Result.Target = Target;
  unsigned Calls = 0;
  for (const auto &Block : Callee.Blocks) {
    bool ThrowAtExit = false;
    for (const auto &Op : Block.Ops) {
      if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
        continue;
      if (++Calls != 1 || Op.Opcode != NdOp::CALL || Op.NumInputs != 1 ||
          !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4 || Op.Seq < 0)
        return std::nullopt;
      const auto *Import = Image.findImportStubAt(Op.Inputs[0].Offset);
      if (!Import || Import->Name != "_CxxThrowException" ||
          (!llvm::StringRef(Import->Module)
                .equals_insensitive("vcruntime140.dll") &&
           !llvm::StringRef(Import->Module)
                .equals_insensitive("vcruntime140d.dll")) ||
          !Import->IATAddr || !Image.readVA(Import->IATAddr, 4))
        return std::nullopt;
      const auto Boundary =
          llvm::find_if(Block.InstructionBoundaries,
                        [&](const auto &B) { return B.Address == Op.Addr; });
      if (Boundary == Block.InstructionBoundaries.end() ||
          Boundary->Control != LowInstructionControl::Call ||
          !hasLowInstructionControlFlag(Boundary->ControlFlags,
                                        LowInstructionControlFlag::NoReturn) ||
          hasLowInstructionControlFlag(
              Boundary->ControlFlags, LowInstructionControlFlag::Conditional) ||
          Op.Addr + Boundary->Size != Block.EndAddr)
        return std::nullopt;
      Result.ImportVA = Op.Inputs[0].Offset;
      Result.ImportIATVA = Import->IATAddr;
      Result.ThrowCallVA = Op.Addr;
      Result.ThrowCallEndVA = Op.Addr + Boundary->Size;
      Result.ThrowOpSeq = Op.Seq;
      ThrowAtExit = true;
    }
    if (Block.Succs.empty() && !ThrowAtExit)
      return std::nullopt;
  }
  if (Calls != 1)
    return std::nullopt;
  ImageFrameEffects Effects;
  if (!hasPrivateCallerFrame(Callee, Image, Work, Effects, false, &Result) ||
      Result.ThrowInfo.Address == InvalidVA || !callerPCIsNotReadBack(Effects))
    return std::nullopt;
  const auto &Info = Result.ThrowInfo;
  if (Effects.Writes.size() > (limits::kMaxRegistrationEHStateWork - Work) /
                                  (Info.ReadOnlyRanges.size() + 1))
    return std::nullopt;
  Work += Effects.Writes.size() * (Info.ReadOnlyRanges.size() + 1);
  for (const auto &[Begin, End] : Effects.Writes) {
    const ExceptionAddressRange Write{Begin, End};
    if (Write.overlaps(Info.TypeDescriptorRange))
      return std::nullopt;
    for (const auto &Range : Info.ReadOnlyRanges)
      if (Write.overlaps(Range))
        return std::nullopt;
  }
  copyExtents(Effects.Reads, Result.ImageReads);
  copyExtents(Effects.Writes, Result.ImageWrites);
  copyExtents(Effects.CallerPCWrites, Result.CallerPCWrites);
  return Result;
}

std::optional<RegistrationCleanupRelayABI>
getCheckedX86RegistrationCleanupRelayABI(const BinaryImage &Image, va_t Target,
                                         size_t *CumulativeWork) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || Target > UINT32_MAX ||
      !Image.isCodeAddress(Target) ||
      Image.ExceptionMetadata.findFunction(Target))
    return std::nullopt;
  size_t LocalWork = 0;
  size_t &Work = CumulativeWork ? *CumulativeWork : LocalWork;
  if (!chargeCalleeWork(Work, 1))
    return std::nullopt;
  auto Header = readImmutableCodeBytes(Image, Target, 3);
  if (!Header || (*Header)[0] != 0x8d ||
      ((*Header)[1] != 0x4d && (*Header)[1] != 0x8d))
    return std::nullopt;
  const unsigned JumpOffset = (*Header)[1] == 0x4d ? 3 : 6;
  const unsigned Size = JumpOffset + 5;
  const va_t End = Target + Size;
  if (End > uint64_t(UINT32_MAX) + 1 || !chargeCalleeWork(Work, Size))
    return std::nullopt;
  auto Bytes = readImmutableCodeBytes(Image, Target, Size);
  if (!Bytes || (*Bytes)[JumpOffset] != 0xe9)
    return std::nullopt;
  const int32_t Offset = JumpOffset == 3 ? int8_t((*Bytes)[2])
                                         : readLE<int32_t>(Bytes->data() + 2);
  const uint32_t LeafTarget =
      uint32_t(End) + readLE<uint32_t>(Bytes->data() + JumpOffset + 1);
  auto Leaf = getCheckedX86RegistrationLeafCalleeABI(Image, LeafTarget, &Work);
  if (!Leaf)
    return std::nullopt;
  return RegistrationCleanupRelayABI{Target, End, Offset, std::move(*Leaf)};
}

std::optional<std::vector<RegistrationCalleeFrameContract>>
RegistrationCallCalleeIndex::contracts(const LowFunc &Function) {
  std::set<va_t> Targets;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops) {
      if (!chargeCalleeWork(Work, 1))
        return std::nullopt;
      if (Op.Opcode == NdOp::CALL && Op.NumInputs == 1 &&
          Op.Inputs[0].isConst() && Op.Inputs[0].Size == 4)
        Targets.insert(Op.Inputs[0].Offset);
      if (Targets.size() > 256)
        return std::nullopt;
    }
  std::vector<RegistrationCalleeFrameContract> Result;
  for (va_t Target : Targets) {
    auto Existing = Cache.find(Target);
    if (Existing == Cache.end()) {
      if (Cache.size() == 256)
        return std::nullopt;
      std::optional<RegistrationCalleeFrameContract> Contract;
      if (auto Leaf =
              getCheckedX86RegistrationLeafCalleeABI(Image, Target, &Work)) {
        Contract.emplace();
        Contract->Target = Target;
        Contract->ECXReads = std::move(Leaf->ECXReads);
        Contract->ECXWrites = std::move(Leaf->ECXWrites);
        Contract->ImageReads = std::move(Leaf->ImageReads);
        Contract->ImageWrites = std::move(Leaf->ImageWrites);
        Contract->CallerPCWrites = std::move(Leaf->CallerPCWrites);
      } else if (auto Throw = getCheckedX86RegistrationThrowCalleeABI(
                     Image, Target, &Work)) {
        Contract.emplace();
        Contract->CalleeKind =
            RegistrationCalleeFrameContract::Kind::PrivateThrow;
        Contract->Target = Target;
        Contract->DoesNotReturn = true;
        Contract->ThrownTypeVA = Throw->ThrowInfo.TypeDescriptorVA;
        Contract->ThrownObjectSize = Throw->ThrowInfo.ObjectSize;
        Contract->ImageReads = std::move(Throw->ImageReads);
        Contract->ImageWrites = std::move(Throw->ImageWrites);
        Contract->CallerPCWrites = std::move(Throw->CallerPCWrites);
      }
      if (Work == limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      Existing = Cache.emplace(Target, std::move(Contract)).first;
    }
    if (Existing->second) {
      const auto &Contract = *Existing->second;
      if (!chargeCalleeWork(Work, Contract.ECXReads.size() +
                                      Contract.ECXWrites.size() +
                                      Contract.ImageReads.size() +
                                      Contract.ImageWrites.size() +
                                      Contract.CallerPCWrites.size() + 1))
        return std::nullopt;
      Result.push_back(Contract);
    }
  }
  return Result;
}

bool hasCallerCleanupRegistrationABI(
    const LowFunc &Function, const BinaryImage &Image,
    std::vector<ExceptionAddressRange> *CallerPCWrites) {
  if (CallerPCWrites)
    CallerPCWrites->clear();
  if (Image.Arch != Arch::X86 || Image.Format != BinaryFormat::COFF ||
      Function.CalleePopBytes || !Function.hasCompleteLiftCoverage())
    return false;
  std::set<va_t> Targets;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops) {
      if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
        continue;
      if (Op.Opcode != NdOp::CALL || Op.NumInputs != 1 ||
          !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4 ||
          !Image.isCodeAddress(Op.Inputs[0].Offset))
        return false;
      if (Function.RegistrationStates &&
          Function.RegistrationStates->SecurityCookiesComplete &&
          Function.RegistrationStates->cookieCheck(Op.Addr, Op.Seq)) {
        const va_t Check = Function.RegistrationStates->CookieCheckVA;
        if (Check != Op.Inputs[0].Offset ||
            !coff_loader::hasCheckedX86CookieCheckSuccessPath(Image, Check))
          return false;
        continue;
      }
      if (Function.ExceptionMetadata &&
          Function.ExceptionMetadata->Registration &&
          Function.RegistrationStates)
        for (const auto &Scope :
             Function.ExceptionMetadata->Registration->Scopes)
          if (Scope.IsFinally && Scope.HandlerVA == Op.Inputs[0].Offset)
            for (const auto &State : Function.RegistrationStates->Blocks)
              if (State.BlockId == Block.Id && State.CallbackOnly)
                return false;
      Targets.insert(Op.Inputs[0].Offset);
      if (Targets.size() > 256)
        return false;
    }
  Decoder Decoder;
  if (!Decoder.init(Image))
    return false;
  CFGBuilder Builder;
  std::deque<va_t> Pending(Targets.begin(), Targets.end());
  std::set<va_t> Examined;
  size_t Work = 0;
  ImageFrameEffects Effects;
  while (!Pending.empty()) {
    const va_t Target = Pending.front();
    Pending.pop_front();
    if (!Examined.insert(Target).second)
      continue;
    if (Examined.size() > 256)
      return false;
    bool OutlinedFinally = false;
    if (Function.ExceptionMetadata && Function.ExceptionMetadata->Registration)
      for (const auto &Scope : Function.ExceptionMetadata->Registration->Scopes)
        if (Target == Scope.FilterVA || Target == Scope.HandlerVA) {
          if (!Scope.IsFinally)
            return false;
          OutlinedFinally = true;
        }
    // Native lowering explicitly replaces ordinary termination calls with
    // its recovered (abnormal, parent-frame) callback ABI.
    if (OutlinedFinally)
      continue;
    if (Target == Function.Entry)
      // A recursive source call can pass values through the synthetic stack.
      // Caller cleanup alone does not project those slots onto the recursive
      // invocation's physical argument area.
      return false;
    const LowFunc Callee = Builder.build(Image, Decoder, Target, "abi-callee");
    if (Callee.CalleePopBytes || !Callee.hasCompleteLiftCoverage() ||
        Callee.Blocks.empty() ||
        !hasPrivateCallerFrame(Callee, Image, Work, Effects))
      return false;
    // A jump to an unexamined tail callee cannot establish its return cleanup.
    for (const auto &Block : Callee.Blocks)
      if (Block.Succs.empty() &&
          (Block.Ops.empty() || Block.Ops.back().Opcode != NdOp::RETURN))
        return false;
    // Check the complete preserved internal-call closure. An opaque helper
    // cannot hide a frame-chain observer one call below the source function.
    for (const auto &Block : Callee.Blocks)
      for (const auto &Op : Block.Ops) {
        if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
          continue;
        if (Op.NumInputs != 1 || !Op.Inputs[0].isConst() ||
            Op.Inputs[0].Size != 4)
          return false;
        const va_t Next = Op.Inputs[0].Offset;
        if (const auto *Import = Image.findImportAt(Next)) {
          // Dispatch intentionally consumes the live registration chain.
          // Other opaque APIs need a separate frame-observation contract.
          if (Import->Name != "RaiseException" ||
              (!llvm::StringRef(Import->Module)
                    .equals_insensitive("kernel32.dll") &&
               !llvm::StringRef(Import->Module)
                    .equals_insensitive("kernelbase.dll")))
            return false;
          continue;
        }
        if (Op.Opcode != NdOp::CALL || !Image.isCodeAddress(Next))
          return false;
        if (Function.ExceptionMetadata &&
            Function.ExceptionMetadata->Registration)
          for (const auto &Scope :
               Function.ExceptionMetadata->Registration->Scopes)
            if (Next == Scope.FilterVA || Next == Scope.HandlerVA)
              return false;
        Pending.push_back(Next);
        if (Pending.size() > 256)
          return false;
      }
  }
  if (Function.ExceptionMetadata &&
      (Function.ExceptionMetadata->Personality ==
           ExceptionPersonality::ExceptHandler3 ||
       Function.ExceptionMetadata->Personality ==
           ExceptionPersonality::ExceptHandler4) &&
      Function.ExceptionMetadata->Registration) {
    const auto Table = coff_loader::getX86RegistrationSEHScopeTableRange(
        *Function.ExceptionMetadata);
    if (!Table)
      return false;
    const bool EH4 = Function.ExceptionMetadata->Personality ==
                     ExceptionPersonality::ExceptHandler4;
    const va_t CookieVA = Image.Base + Image.DynInfo.SecurityCookieRVA;
    if (EH4 && (!Image.DynInfo.SecurityCookieRVA || CookieVA > UINT32_MAX - 3))
      return false;
    for (const auto &[Begin, End] : Effects.Writes)
      if ((EH4 && Begin < CookieVA + 4 && CookieVA < End) ||
          Table->overlaps({Begin, End}))
        return false;
  }
  if (Effects.CallerPCWrites.empty())
    return true;
  // The real return PC can be exposed to an external observer, but a reload
  // inside the rewritten source/callee closure could erase its provenance and
  // use the regenerated address in a branch, arithmetic or dereference.
  if (Function.RegistrationStates) {
    if (!Function.RegistrationStates->ImageReadsComplete)
      return false;
    for (const auto &Read : Function.RegistrationStates->ImageReads)
      Effects.Reads.emplace(Read.Begin, Read.End);
  } else
    for (const auto &Block : Function.Blocks)
      for (const auto &Op : Block.Ops)
        if (Op.Opcode != NdOp::STORE && lowMemoryOperands(Op).Address)
          return false;
  // Sort both sets once, then walk their interval fronts. Avoid multiplying
  // the bounded source/callee footprints by one another.
  if (!callerPCIsNotReadBack(Effects))
    return false;
  if (CallerPCWrites)
    for (const auto &[Begin, End] : Effects.CallerPCWrites) {
      if (!CallerPCWrites->empty() && Begin <= CallerPCWrites->back().End)
        CallerPCWrites->back().End = std::max(CallerPCWrites->back().End, End);
      else
        CallerPCWrites->push_back({Begin, End});
    }
  return true;
}
} // namespace neverd
