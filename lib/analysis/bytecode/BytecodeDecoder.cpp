//===- BytecodeDecoder.cpp - Externally described bytecode ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/analysis/BytecodeDecoder.h"

#include "BytecodeInternal.h"

#include "llvm/ADT/StringExtras.h"

#include <deque>
#include <map>
#include <utility>

using namespace neverd;
using namespace neverd::analysis;

namespace {

llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "bytecode: " + Message);
}

bool scalarSize(unsigned Size) {
  return Size == 1 || Size == 2 || Size == 4 || Size == 8;
}

uint64_t mask(unsigned Bits) {
  return Bits == 64 ? UINT64_MAX : (UINT64_C(1) << Bits) - 1;
}

llvm::Error validateOperand(const BytecodeOperand &Operand,
                            const BytecodeEncoding &Encoding, bool Output) {
  if (Output && Operand.Size == 0)
    return llvm::Error::success();
  if (!scalarSize(Operand.Size) ||
      (Operand.Space != VnodeSpace::REG && Operand.Space != VnodeSpace::TEMP &&
       (Output || Operand.Space != VnodeSpace::CONST)))
    return invalid("unsupported operand space or width");
  const auto &Value = Operand.Value;
  if ((Value.Bytes && !scalarSize(Value.Bytes)) ||
      Value.Offset > Encoding.Size ||
      Value.Bytes > Encoding.Size - Value.Offset || Value.Shift >= 64 ||
      (Value.Bytes && Value.Shift >= Value.Bytes * 8) || Value.ValueBits == 0 ||
      Value.ValueBits > 64 || Value.Lookup.size() > 65536)
    return invalid("invalid operand field");
  if (Value.PCRelative && Operand.Space != VnodeSpace::CONST)
    return invalid("a register location cannot be PC relative");
  return llvm::Error::success();
}

llvm::Expected<NdVar> operand(const BytecodeOperand &Operand,
                              llvm::ArrayRef<uint8_t> Bytes, va_t PC,
                              const BytecodeProfile &Profile) {
  if (!Operand.Size)
    return NdVar{};
  const auto &Field = Operand.Value;
  uint64_t Value = 0;
  for (unsigned I = 0; I != Field.Bytes; ++I) {
    unsigned Lane =
        Profile.ByteOrder == llvm::endianness::little ? I : Field.Bytes - 1 - I;
    Value |= uint64_t(Bytes[Field.Offset + I]) << (Lane * 8);
  }
  Value = (Value >> Field.Shift) & Field.Mask;
  if (!Field.Lookup.empty()) {
    if (Value >= Field.Lookup.size())
      return invalid("operand lookup index out of range");
    Value = Field.Lookup[Value];
  }
  // Integer immediates deliberately use unsigned modular arithmetic. Storage
  // offsets must not wrap into an apparently valid register or temporary.
  if (Operand.Space != VnodeSpace::CONST &&
      ((Field.Scale && Value > UINT64_MAX / Field.Scale) ||
       Value * Field.Scale > UINT64_MAX - Field.Addend))
    return invalid("storage offset overflow");
  Value = Value * Field.Scale + Field.Addend;
  if (Field.PCRelative)
    Value += PC;
  if (Operand.Space != VnodeSpace::CONST && (Value & ~mask(Field.ValueBits)))
    return invalid("storage offset truncation");
  Value &= mask(Field.ValueBits);
  if (Operand.Space == VnodeSpace::CONST)
    return NdVar::scalar(Value & mask(Operand.Size * 8), Operand.Size);
  uint64_t Limit = Operand.Space == VnodeSpace::REG ? Profile.RegisterBytes
                                                    : Profile.TemporaryBytes;
  if (Value > Limit || Operand.Size > Limit - Value)
    return invalid("storage operand outside its declared bank");
  return NdVar{Operand.Space, Value, Operand.Size};
}

bool terminator(NdOp Op) {
  return Op == NdOp::BRANCH || Op == NdOp::COND_BR || Op == NdOp::INDIR_BR ||
         Op == NdOp::RETURN;
}

llvm::Error validateOperation(const LowOp &Op) {
  auto Fail = [&]() {
    return invalid(llvm::Twine("invalid ") + ndOpName(Op.Opcode));
  };
  auto Arity = [&](unsigned Count, bool Output) {
    return Op.NumInputs == Count && (Op.Output.Size != 0) == Output;
  };
  auto Same = [&](unsigned Index) {
    return Op.Inputs[Index].Size == Op.Output.Size;
  };
  auto Binary = [&]() { return Arity(2, true) && Same(0) && Same(1); };
  auto Compare = [&]() {
    return Arity(2, true) && Op.Output.Size == 1 &&
           Op.Inputs[0].Size == Op.Inputs[1].Size;
  };
  bool OK = false;
  switch (Op.Opcode) {
  case NdOp::COPY:
  case NdOp::INT_NEGATE:
  case NdOp::INT_NOT:
  case NdOp::INT_NEG2:
    OK = Arity(1, true) && Same(0);
    break;
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_AND:
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
  case NdOp::INT_MULT:
  case NdOp::INT_DIV:
  case NdOp::INT_SDIV:
  case NdOp::INT_REM:
  case NdOp::INT_SREM:
    OK = Binary();
    break;
  case NdOp::INT_LEFT:
  case NdOp::INT_RIGHT:
  case NdOp::INT_ASHR:
    OK = Arity(2, true) && Same(0);
    break;
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_SLESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESSEQUAL:
  case NdOp::INT_CARRY:
  case NdOp::INT_SOVF:
  case NdOp::INT_SBOR:
    OK = Compare();
    break;
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    OK = Arity(1, true) && Op.Output.Size > Op.Inputs[0].Size;
    break;
  case NdOp::SUBBYTES:
    OK = Arity(2, true) && Op.Inputs[1].isConst() &&
         Op.Inputs[1].Offset <= Op.Inputs[0].Size &&
         Op.Output.Size <= Op.Inputs[0].Size - Op.Inputs[1].Offset;
    break;
  case NdOp::CONCAT:
    OK = Arity(2, true) &&
         Op.Output.Size == Op.Inputs[0].Size + Op.Inputs[1].Size;
    break;
  case NdOp::BOOL_AND:
  case NdOp::BOOL_OR:
  case NdOp::BOOL_XOR:
    OK = Binary() && Op.Output.Size == 1;
    break;
  case NdOp::BOOL_NOT:
    OK = Arity(1, true) && Same(0) && Op.Output.Size == 1;
    break;
  case NdOp::SELECT:
    OK = Arity(3, true) && Op.Inputs[0].Size == 1 && Same(1) && Same(2);
    break;
  case NdOp::LOAD:
    OK = Arity(1, true) && Op.Inputs[0].Size == 8;
    break;
  case NdOp::STORE:
    OK = Arity(2, false) && Op.Inputs[0].Size == 8;
    break;
  case NdOp::FLOAT_ADD:
  case NdOp::FLOAT_SUB:
  case NdOp::FLOAT_MULT:
  case NdOp::FLOAT_DIV:
  case NdOp::FLOAT_MIN:
  case NdOp::FLOAT_MAX:
  case NdOp::FLOAT_MINNUM:
  case NdOp::FLOAT_MAXNUM:
    OK = Binary() && Op.Output.Size >= 4;
    break;
  case NdOp::FLOAT_EQUAL:
  case NdOp::FLOAT_NOTEQUAL:
  case NdOp::FLOAT_LESS:
  case NdOp::FLOAT_LESSEQUAL:
    OK = Compare() && Op.Inputs[0].Size >= 4;
    break;
  case NdOp::FLOAT_ISNAN:
    OK = Arity(1, true) && Op.Output.Size == 1 && Op.Inputs[0].Size >= 4;
    break;
  case NdOp::FLOAT_NEG:
  case NdOp::FLOAT_ABS:
  case NdOp::FLOAT_SQRT:
    OK = Arity(1, true) && Same(0) && Op.Output.Size >= 4;
    break;
  case NdOp::FLOAT_INT2FLOAT:
  case NdOp::FLOAT_UINT2FLOAT:
    OK = Arity(1, true) && Op.Output.Size >= 4;
    break;
  case NdOp::FLOAT_FLOAT2INT:
  case NdOp::FLOAT_FLOAT2UINT:
    OK = Arity(1, true) && Op.Inputs[0].Size >= 4;
    break;
  case NdOp::FLOAT_FLOAT2FLOAT:
    OK = Arity(1, true) && Op.Inputs[0].Size >= 4 && Op.Output.Size >= 4;
    break;
  case NdOp::BRANCH:
    OK = Arity(1, false) && Op.Inputs[0].isConst() && Op.Inputs[0].Size == 8;
    break;
  case NdOp::COND_BR:
    OK = Arity(2, false) && Op.Inputs[0].isConst() && Op.Inputs[0].Size == 8 &&
         Op.Inputs[1].Size == 1;
    break;
  case NdOp::INDIR_BR:
    OK = Arity(1, false) && Op.Inputs[0].Size == 8;
    break;
  case NdOp::CALL:
  case NdOp::INDIR_CALL:
    OK = Arity(1, false) && Op.Inputs[0].Size == 8 &&
         (Op.Opcode != NdOp::CALL || Op.Inputs[0].isConst());
    break;
  case NdOp::RETURN:
  case NdOp::NOP:
    OK = Arity(0, false);
    break;
  default:
    return invalid(llvm::Twine("unsupported LowIR operation ") +
                   ndOpName(Op.Opcode));
  }
  return OK ? llvm::Error::success() : Fail();
}

} // namespace

llvm::Error
neverd::analysis::detail::validateBytecodeOperation(const LowOp &Op) {
  return validateOperation(Op);
}

BytecodeDecoder::BytecodeDecoder(BytecodeProfile P) : Profile(std::move(P)) {
  for (uint32_t I = 0; I != Profile.Encodings.size(); ++I)
    for (unsigned First = 0; First != 256; ++First) {
      bool Matches = true;
      for (auto M : Profile.Encodings[I].Match)
        if (M.Offset == 0 && (First & M.Mask) != M.Value)
          Matches = false;
      if (Matches)
        FirstByte[First].push_back(I);
    }
}

namespace {
llvm::Error validateLayout(const BytecodeProfile &P) {
  if (!P.RegisterBytes || P.RegisterBytes > 65536 || !P.TemporaryBytes ||
      P.TemporaryBytes > 65536 ||
      (P.ByteOrder != llvm::endianness::little &&
       P.ByteOrder != llvm::endianness::big))
    return invalid("invalid profile dimensions");
  return llvm::Error::success();
}

llvm::Error validateEncoding(const BytecodeEncoding &E, bool Selected) {
  if (!E.Size || E.Size > BytecodeInstructionByteLimit ||
      (Selected ? !E.Match.empty() : E.Match.empty()) ||
      E.Match.size() > E.Size || E.Operations.empty() ||
      E.Operations.size() > 4096)
    return invalid("invalid encoding dimensions");
  std::set<uint16_t> Offsets;
  for (auto M : E.Match)
    if (M.Offset >= E.Size || !M.Mask || (M.Value & ~M.Mask) ||
        !Offsets.insert(M.Offset).second)
      return invalid("invalid encoding match");
  for (size_t I = 0; I != E.Operations.size(); ++I) {
    const auto &Op = E.Operations[I];
    if (Op.Inputs.size() > 6 ||
        static_cast<unsigned>(Op.Opcode) >= static_cast<unsigned>(NdOp::_COUNT))
      return invalid("invalid operation");
    if ((terminator(Op.Opcode) || Op.Opcode == NdOp::CALL ||
         Op.Opcode == NdOp::INDIR_CALL) &&
        I + 1 != E.Operations.size())
      return invalid("control operation must end its instruction");
    if (auto Err = validateOperand(Op.Output, E, true))
      return Err;
    for (const auto &Input : Op.Inputs)
      if (auto Err = validateOperand(Input, E, false))
        return Err;
  }
  return llvm::Error::success();
}
} // namespace

llvm::Expected<std::unique_ptr<BytecodeDecoder>>
BytecodeDecoder::create(BytecodeProfile P) {
  if (auto Error = validateLayout(P))
    return std::move(Error);
  if (P.Encodings.empty() || P.Encodings.size() > 65536)
    return invalid("invalid profile dimensions");
  uint64_t OperationCount = 0;
  for (const auto &E : P.Encodings) {
    OperationCount += E.Operations.size();
    if (OperationCount > 1000000)
      return invalid("invalid encoding dimensions");
    if (auto Error = validateEncoding(E, false))
      return std::move(Error);
  }
  return std::unique_ptr<BytecodeDecoder>(new BytecodeDecoder(std::move(P)));
}

llvm::Expected<std::unique_ptr<BytecodeDecoder>>
BytecodeDecoder::createExternal(BytecodeProfile P,
                                BytecodeDecodeCallback Decode) {
  if (auto Error = validateLayout(P))
    return std::move(Error);
  if (!Decode || !P.Encodings.empty())
    return invalid(
        "external decoding requires a callback and no static encodings");
  auto Result =
      std::unique_ptr<BytecodeDecoder>(new BytecodeDecoder(std::move(P)));
  Result->External = std::move(Decode);
  return Result;
}

llvm::Expected<BytecodeInstruction>
BytecodeDecoder::decode(llvm::ArrayRef<uint8_t> Bytes, va_t Address) const {
  if (Bytes.empty())
    return invalid("empty instruction at 0x" + llvm::utohexstr(Address));
  if (External) {
    // Bound Python's copy cost per callback instead of copying the remaining
    // function at each PC. A valid instruction cannot exceed this window.
    auto Window =
        Bytes.take_front(std::min(Bytes.size(), BytecodeInstructionByteLimit));
    auto Encoding = External(Window, Address);
    if (!Encoding)
      return Encoding.takeError();
    if (auto Error = validateEncoding(*Encoding, true))
      return std::move(Error);
    return materialize(*Encoding, Window, Address, UINT32_MAX);
  }
  std::optional<uint32_t> Match;
  bool Truncated = false;
  for (uint32_t Index : FirstByte[Bytes[0]]) {
    const auto &E = Profile.Encodings[Index];
    bool Matches = true;
    bool Missing = false;
    for (auto M : E.Match) {
      if (M.Offset >= Bytes.size()) {
        Missing = true;
        continue;
      }
      if ((Bytes[M.Offset] & M.Mask) != M.Value)
        Matches = false;
    }
    if (!Matches)
      continue;
    if (Missing || E.Size > Bytes.size()) {
      Truncated = true;
      continue;
    }
    if (Match)
      return invalid("ambiguous instruction at 0x" + llvm::utohexstr(Address));
    Match = Index;
  }
  // A short complete pattern cannot win over a possibly matching longer one.
  if (Truncated || !Match)
    return invalid(llvm::Twine(Truncated ? "truncated" : "unknown") +
                   " instruction at 0x" + llvm::utohexstr(Address));
  return materialize(Profile.Encodings[*Match], Bytes, Address, *Match);
}

llvm::Expected<BytecodeInstruction>
BytecodeDecoder::materialize(const BytecodeEncoding &E,
                             llvm::ArrayRef<uint8_t> Bytes, va_t Address,
                             uint32_t Index) const {
  if (E.Size > Bytes.size())
    return invalid("truncated instruction at 0x" + llvm::utohexstr(Address));
  if (Address > UINT64_MAX - E.Size)
    return invalid("instruction address overflow");
  BytecodeInstruction Result;
  Result.Size = E.Size;
  Result.Encoding = Index;
  // LowIR temporaries are typed values, not an aliasing register bank. A
  // partially overwritten or differently sized view must be reconstructed
  // explicitly with SUBBYTES/CONCAT instead of reading an unrelated SSA id.
  std::map<uint64_t, uint16_t> Defined;
  for (const auto &T : E.Operations) {
    LowOp Op;
    Op.Opcode = T.Opcode;
    Op.Addr = Address;
    Op.Seq = Result.Operations.size();
    auto Output = operand(T.Output, Bytes, Address, Profile);
    if (!Output)
      return Output.takeError();
    Op.Output = *Output;
    for (const auto &Input : T.Inputs) {
      auto V = operand(Input, Bytes, Address, Profile);
      if (!V)
        return V.takeError();
      if (V->isTemp()) {
        auto It = Defined.find(V->Offset);
        if (It == Defined.end() || It->second != V->Size)
          return invalid("temporary read before definition of its exact width");
      }
      Op.addInput(*V);
    }
    if (auto Err = detail::validateBytecodeOperation(Op))
      return std::move(Err);
    if (Op.Output.isTemp()) {
      const auto Begin = Op.Output.Offset;
      const auto End = Begin + Op.Output.Size;
      for (auto It = Defined.begin(); It != Defined.end();) {
        if (It->first < End && Begin < It->first + It->second)
          It = Defined.erase(It);
        else
          ++It;
      }
      Defined.emplace(Begin, Op.Output.Size);
    }
    Result.Operations.push_back(Op);
  }
  return Result;
}

llvm::Expected<BytecodeFunction>
BytecodeDecoder::function(llvm::ArrayRef<uint8_t> Code, va_t Base, va_t Entry,
                          va_t End, llvm::StringRef Name,
                          const BytecodeDecodeLimits &Limits) const {
  if (Code.size() > UINT64_MAX - Base || Entry < Base || End <= Entry ||
      End > Base + Code.size() || !Limits.MaxInstructions ||
      !Limits.MaxOperations)
    return invalid("invalid function range or budget");
  std::map<va_t, BytecodeInstruction> Instructions;
  std::map<va_t, std::vector<va_t>> Successors;
  std::deque<va_t> Pending{Entry};
  BytecodeFunction Result;
  uint64_t Operations = 0;
  while (!Pending.empty()) {
    va_t PC = Pending.front();
    Pending.pop_front();
    if (PC < Entry || PC >= End)
      return invalid("control flow leaves the declared function at 0x" +
                     llvm::utohexstr(PC));
    if (Instructions.count(PC))
      continue;
    if (Instructions.size() >= Limits.MaxInstructions)
      return invalid("instruction budget exhausted");
    auto Inst = decode(Code.slice(PC - Base, End - PC), PC);
    if (!Inst)
      return Inst.takeError();
    auto Next = Instructions.lower_bound(PC);
    if ((Next != Instructions.end() && PC + Inst->Size > Next->first) ||
        (Next != Instructions.begin() &&
         std::prev(Next)->first + std::prev(Next)->second.Size > PC))
      return invalid("overlapping instructions at 0x" + llvm::utohexstr(PC));
    if (Inst->Operations.size() > Limits.MaxOperations - Operations)
      return invalid("operation budget exhausted");
    Operations += Inst->Operations.size();
    Result.DecodedBytes += Inst->Size;
    const auto &Last = Inst->Operations.back();
    auto &Edges = Successors[PC];
    if (Last.Opcode == NdOp::INDIR_BR)
      return invalid("unresolved indirect branch at 0x" + llvm::utohexstr(PC));
    if (Last.Opcode == NdOp::BRANCH || Last.Opcode == NdOp::COND_BR)
      Edges.push_back(Last.Inputs[0].Offset);
    if (Last.Opcode != NdOp::BRANCH && Last.Opcode != NdOp::RETURN &&
        (Edges.empty() || Edges.front() != PC + Inst->Size))
      Edges.push_back(PC + Inst->Size);
    if (Last.Opcode == NdOp::CALL)
      Result.DirectCalls.insert(Last.Inputs[0].Offset);
    Pending.insert(Pending.end(), Edges.begin(), Edges.end());
    Instructions.emplace(PC, std::move(*Inst));
  }
  auto &F = Result.Function;
  F.Entry = Entry;
  F.Name = Name.str();
  F.OriginalSize = End - Entry;
  F.DecodedInstructionCount = Instructions.size();
  F.LiftedInstructionCount = Instructions.size();
  // Instruction discovery is separate from basic-block construction. Keep
  // straight-line instructions together so source lowering can reason across
  // their state accesses; split every control target and call continuation.
  std::set<va_t> Leaders{Entry};
  for (const auto &[PC, Inst] : Instructions) {
    const auto Last = Inst.Operations.back().Opcode;
    if (Last == NdOp::BRANCH || Last == NdOp::COND_BR || Last == NdOp::CALL ||
        Last == NdOp::INDIR_CALL)
      Leaders.insert(Successors.at(PC).begin(), Successors.at(PC).end());
  }
  std::map<va_t, int> IDs;
  for (const auto &[PC, Inst] : Instructions) {
    if (F.Blocks.empty() || Leaders.count(PC) ||
        F.Blocks.back().EndAddr != PC) {
      LowBlock B;
      B.Id = F.Blocks.size();
      B.StartAddr = PC;
      F.Blocks.push_back(std::move(B));
    }
    auto &B = F.Blocks.back();
    IDs[PC] = B.Id;
    B.EndAddr = PC + Inst.Size;
    for (auto Op : Inst.Operations) {
      Op.Seq = B.Ops.size();
      B.Ops.push_back(std::move(Op));
    }
    // Bytecode is a separate source language. Its encodings do not constitute
    // native instruction boundaries or architecture-definedness certificates.
  }
  for (const auto &[PC, Edges] : Successors) {
    if (F.Blocks[IDs.at(PC)].EndAddr != PC + Instructions.at(PC).Size)
      continue;
    for (va_t Target : Edges) {
      F.Blocks[IDs.at(PC)].Succs.push_back(IDs.at(Target));
      F.Blocks[IDs.at(Target)].Preds.push_back(IDs.at(PC));
    }
  }
  return Result;
}
