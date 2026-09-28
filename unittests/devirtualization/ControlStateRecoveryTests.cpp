//===- ControlStateRecoveryTests.cpp - Spilled dispatch control -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/InterpreterSpecialization.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Errc.h"

#include <map>
#include <optional>

using namespace neverd;
using namespace neverd::analysis;

namespace {
constexpr va_t Entry = 0x500;
constexpr va_t Route = 0x520;
constexpr va_t Reload = 0x540;
constexpr va_t Dispatch = 0x560;
constexpr va_t Latch = 0x580;
constexpr va_t Exit = 0x5a0;
constexpr va_t HandlerBase = 0x700;
constexpr va_t TableBase = 0x6000;
constexpr uint64_t ResultRegister = 0;
constexpr uint64_t CounterRegister = 24;
constexpr uint64_t FrameRegister = 32;
constexpr uint64_t LimitRegister = 48;
constexpr uint64_t InputRegister = 56;
constexpr uint64_t SelectorAddressRegister = 64;
constexpr uint64_t PayloadAddressRegister = 72;
constexpr uint64_t BankRegister = 80;
constexpr uint64_t SelectorRegister = 88;
constexpr uint64_t AddressRegister = 96;
constexpr uint64_t ValueRegister = 104;
constexpr uint64_t ConditionRegister = 112;
constexpr uint64_t UnknownPointerRegister = 120;
constexpr int64_t SelectorSlot = -72;

NdVar r(uint64_t Offset, uint16_t Bytes = 8) {
  return NdVar::reg(Offset, Bytes);
}
NdVar c(uint64_t Value, uint16_t Bytes = 8) {
  return NdVar::scalar(Value, Bytes);
}
LowOp operation(NdOp Opcode, NdVar Output,
                std::initializer_list<NdVar> Inputs) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  for (NdVar Input : Inputs)
    Op.addInput(Input);
  return Op;
}
LowOp jump(va_t Address) {
  return operation(NdOp::BRANCH, {}, {NdVar::cst(Address, 8)});
}
LowOp ret() { return operation(NdOp::RETURN, {}, {r(ResultRegister)}); }

class BankProvider : public SpecializationProvider {
public:
  std::map<va_t, SpecializationInstruction> Code;
  std::map<va_t, uint8_t> Image;

  void add(va_t Address, llvm::ArrayRef<LowOp> Ops,
           va_t Fallthrough = InvalidVA) {
    SpecializationInstruction Instruction;
    Instruction.Ops.assign(Ops.begin(), Ops.end());
    Instruction.Origin.Address = Address;
    Instruction.Origin.Size = 1;
    Instruction.Origin.OpCount = Ops.size();
    Instruction.Fallthrough = {Fallthrough == InvalidVA ? Address + 1
                                                        : Fallthrough};
    for (size_t I = 0; I < Instruction.Ops.size(); ++I) {
      LowOp &Op = Instruction.Ops[I];
      Op.Addr = Address;
      Op.Seq = static_cast<int>(I);
      if (Op.Opcode == NdOp::RETURN) {
        Instruction.Origin.Control = LowInstructionControl::Return;
        Instruction.Origin.ControlFlags = LowInstructionControlFlag::Return;
      } else if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR ||
                 Op.Opcode == NdOp::INDIR_BR) {
        Instruction.Origin.Control = LowInstructionControl::Branch;
        Instruction.Origin.ControlFlags = LowInstructionControlFlag::Branch;
        if (Op.Opcode == NdOp::COND_BR)
          Instruction.Origin.ControlFlags |=
              LowInstructionControlFlag::Conditional;
        if (Op.Opcode == NdOp::INDIR_BR)
          Instruction.Origin.ControlFlags |=
              LowInstructionControlFlag::Indirect;
        else
          Instruction.Origin.Immediate = Op.Inputs[0].Offset;
      }
    }
    Code.emplace(Address, std::move(Instruction));
  }

  llvm::Expected<SpecializationInstruction>
  instruction(SpecializationCursor Cursor) override {
    const auto At = Code.find(Cursor.Address);
    if (At == Code.end())
      return llvm::createStringError(llvm::errc::invalid_argument,
                                     "unknown synthetic handler");
    return At->second;
  }

  std::optional<SpecializationImmutableRead>
  immutableRead(va_t Address, uint16_t Bytes) override {
    SpecializationImmutableRead Read;
    Read.Evidence = "independently authored immutable table";
    for (unsigned I = 0; I < Bytes; ++I) {
      const auto At = Image.find(Address + I);
      if (At == Image.end())
        return std::nullopt;
      Read.Bytes.push_back(At->second);
    }
    return Read;
  }
};

enum class SelectorInput { MaskedScalar, UnboundedScalar, UnknownMemory };

// This toy register bank contains one dispatch selector and one unconstrained
// payload. Neither an image-specific layout nor an entry value is assumed.
BankProvider
makeBankProgram(bool Loop, SelectorInput Input = SelectorInput::MaskedScalar) {
  BankProvider P;
  P.add(Entry,
        {operation(NdOp::INT_SUB, r(BankRegister), {r(FrameRegister), c(96)}),
         operation(NdOp::INT_ADD, r(SelectorAddressRegister),
                   {r(BankRegister), c(24)}),
         operation(NdOp::INT_ADD, r(PayloadAddressRegister),
                   {r(BankRegister), c(32)}),
         operation(NdOp::COPY, r(ResultRegister), {c(0)}),
         operation(NdOp::COPY, r(CounterRegister), {c(0)}),
         operation(NdOp::INT_XOR, r(ValueRegister), {r(InputRegister), c(45)}),
         operation(NdOp::STORE, {},
                   {r(PayloadAddressRegister), r(ValueRegister)}),
         Input == SelectorInput::MaskedScalar
             ? operation(NdOp::INT_AND, r(SelectorRegister),
                         {r(InputRegister), c(3)})
         : Input == SelectorInput::UnboundedScalar
             ? operation(NdOp::COPY, r(SelectorRegister), {r(InputRegister)})
             : operation(NdOp::LOAD, r(SelectorRegister),
                         {r(UnknownPointerRegister)}),
         operation(NdOp::STORE, {},
                   {r(SelectorAddressRegister), r(SelectorRegister)}),
         jump(Route)});
  P.add(Route,
        {operation(NdOp::INT_XOR, r(ValueRegister), {r(InputRegister), c(91)}),
         jump(Reload)});
  P.add(Reload, {operation(NdOp::LOAD, r(SelectorRegister),
                           {r(SelectorAddressRegister)}),
                 jump(Dispatch)});
  P.add(Dispatch,
        {operation(NdOp::INT_MULT, r(AddressRegister),
                   {r(SelectorRegister), c(8)}),
         operation(NdOp::INT_ADD, r(AddressRegister),
                   {r(AddressRegister), c(TableBase)}),
         operation(NdOp::LOAD, r(ValueRegister), {r(AddressRegister)}),
         operation(NdOp::INDIR_BR, {}, {r(ValueRegister)})});
  for (unsigned Lane = 0; Lane < 4; ++Lane) {
    const va_t Handler = HandlerBase + 0x20 * Lane;
    for (unsigned Byte = 0; Byte < 8; ++Byte)
      P.Image[TableBase + Lane * 8 + Byte] = Handler >> (Byte * 8);
    if (!Loop) {
      P.add(Handler, {operation(NdOp::LOAD, r(ValueRegister),
                                {r(PayloadAddressRegister)}),
                      operation(NdOp::INT_ADD, r(ResultRegister),
                                {r(ValueRegister), c(3 + Lane * 11)}),
                      ret()});
    } else {
      P.add(
          Handler,
          {operation(NdOp::LOAD, r(ValueRegister), {r(PayloadAddressRegister)}),
           operation(NdOp::INT_ADD, r(ValueRegister),
                     {r(ValueRegister), c(3 + Lane * 11)}),
           operation(NdOp::INT_ADD, r(ResultRegister),
                     {r(ResultRegister), r(ValueRegister)}),
           operation(NdOp::INT_ADD, r(SelectorRegister),
                     {r(SelectorRegister), c(1)}),
           operation(NdOp::INT_AND, r(SelectorRegister),
                     {r(SelectorRegister), c(3)}),
           operation(NdOp::STORE, {},
                     {r(SelectorAddressRegister), r(SelectorRegister)}),
           jump(Latch)});
    }
  }
  if (Loop) {
    P.add(Latch,
          {operation(NdOp::INT_ADD, r(CounterRegister),
                     {r(CounterRegister), c(1)}),
           operation(NdOp::INT_LESS, r(ConditionRegister, 1),
                     {r(CounterRegister), r(LimitRegister)}),
           operation(NdOp::COND_BR, {}, {c(Route), r(ConditionRegister, 1)})},
          Exit);
    P.add(Exit, {ret()});
  }
  return P;
}

SpecializationOptions bankOptions(bool SlotHint, bool RegisterHint) {
  SpecializationOptions Options;
  Options.FrameBaseRegister = symbolic::SymRegisterRange{FrameRegister, 8};
  if (SlotHint)
    Options.ControlFrameSlots = {{SelectorSlot, 8}};
  if (RegisterHint)
    Options.ControlRegisters = {{SelectorRegister, 8}};
  return Options;
}

constexpr unsigned MemoryCursorPositions = 17;
constexpr int64_t MemoryCursorSlot = -24;

BankProvider makeMemoryCursorProgram(bool UnboundedOffset = false) {
  BankProvider Provider;
  // The table's addresses vary, but every record selects the same safe slot.
  // Relations can therefore establish one affine write address until the
  // cursor's 17 positions exceed the ordinary immutable-read address limit.
  // The payload and accumulated return value remain runtime data throughout.
  constexpr uint64_t DataDisplacement = uint64_t{0} - 96;
  for (unsigned I = 0; I < MemoryCursorPositions; ++I)
    for (unsigned Byte = 0; Byte < 8; ++Byte)
      Provider.Image[TableBase + I * 8 + Byte] = DataDisplacement >> (8 * Byte);
  Provider.add(Entry,
               {operation(NdOp::INT_ADD, r(SelectorAddressRegister),
                          {r(FrameRegister), c(MemoryCursorSlot)}),
                operation(NdOp::COPY, r(SelectorRegister), {c(0)}),
                operation(NdOp::COPY, r(ResultRegister), {c(0)}),
                operation(NdOp::STORE, {},
                          {r(SelectorAddressRegister), r(SelectorRegister)}),
                jump(Route)});
  Provider.add(Route, {operation(NdOp::LOAD, r(SelectorRegister),
                                 {r(SelectorAddressRegister)}),
                       jump(Reload)});
  Provider.add(Reload, {jump(Dispatch)});
  Provider.add(
      Dispatch,
      {operation(NdOp::INT_MULT, r(AddressRegister),
                 {r(SelectorRegister), c(8)}),
       operation(NdOp::INT_ADD, r(AddressRegister),
                 {r(AddressRegister), c(TableBase)}),
       UnboundedOffset
           ? operation(NdOp::COPY, r(ValueRegister), {r(InputRegister)})
           : operation(NdOp::LOAD, r(ValueRegister), {r(AddressRegister)}),
       operation(NdOp::INT_ADD, r(PayloadAddressRegister),
                 {r(FrameRegister), r(ValueRegister)}),
       operation(NdOp::INT_ADD, r(ValueRegister),
                 {r(InputRegister), r(SelectorRegister)}),
       operation(NdOp::STORE, {},
                 {r(PayloadAddressRegister), r(ValueRegister)}),
       operation(NdOp::LOAD, r(ValueRegister), {r(PayloadAddressRegister)}),
       operation(NdOp::INT_ADD, r(ResultRegister),
                 {r(ResultRegister), r(ValueRegister)}),
       operation(NdOp::INT_ADD, r(SelectorRegister),
                 {r(SelectorRegister), c(1)}),
       operation(NdOp::STORE, {},
                 {r(SelectorAddressRegister), r(SelectorRegister)}),
       operation(NdOp::INT_LESS, r(ConditionRegister, 1),
                 {r(SelectorRegister), c(MemoryCursorPositions)}),
       operation(NdOp::COND_BR, {}, {c(Route), r(ConditionRegister, 1)})},
      Exit);
  Provider.add(Exit, {ret()});
  return Provider;
}

SpecializationOptions memoryCursorOptions(bool ManualCursor, bool Automatic) {
  auto Options = bankOptions(false, ManualCursor);
  Options.RequireRestoredFrameAtReturn = true;
  Options.DiscoverControlState = Automatic;
  return Options;
}

constexpr int64_t FirstMaskSlot = -48;
constexpr int64_t SecondMaskSlot = -40;
constexpr int64_t DerivedDisplacementSlot = -32;

BankProvider makeFiniteDependencyProgram() {
  BankProvider Provider;
  for (unsigned I = 0; I < 4; ++I) {
    const uint64_t Displacement = uint64_t{0} - 144 + I * 8;
    for (unsigned Byte = 0; Byte < 8; ++Byte)
      Provider.Image[TableBase + I * 8 + Byte] = Displacement >> (8 * Byte);
  }
  Provider.add(
      Entry,
      {operation(NdOp::INT_ADD, r(SelectorAddressRegister),
                 {r(FrameRegister), c(FirstMaskSlot)}),
       operation(NdOp::INT_ADD, r(PayloadAddressRegister),
                 {r(FrameRegister), c(SecondMaskSlot)}),
       operation(NdOp::INT_ADD, r(AddressRegister),
                 {r(FrameRegister), c(DerivedDisplacementSlot)}),
       operation(NdOp::INT_AND, r(ValueRegister), {r(InputRegister), c(1)}),
       operation(NdOp::STORE, {},
                 {r(SelectorAddressRegister), r(ValueRegister)}),
       operation(NdOp::INT_XOR, r(ValueRegister), {r(ValueRegister), c(1)}),
       operation(NdOp::STORE, {},
                 {r(PayloadAddressRegister), r(ValueRegister)}),
       jump(Route)});
  Provider.add(
      Route,
      {operation(NdOp::LOAD, r(ValueRegister), {r(SelectorAddressRegister)}),
       operation(NdOp::LOAD, r(CounterRegister), {r(PayloadAddressRegister)}),
       operation(NdOp::INT_AND, r(ValueRegister), {r(ValueRegister), c(1)}),
       operation(NdOp::INT_AND, r(CounterRegister), {r(CounterRegister), c(1)}),
       operation(NdOp::INT_ADD, r(ValueRegister),
                 {r(ValueRegister), r(CounterRegister)}),
       operation(NdOp::INT_SUB, r(ValueRegister), {r(ValueRegister), c(1)}),
       operation(NdOp::STORE, {}, {r(AddressRegister), r(ValueRegister)}),
       jump(Reload)});
  Provider.add(
      Reload,
      {operation(NdOp::LOAD, r(BankRegister), {r(AddressRegister)}),
       operation(NdOp::INT_AND, r(BankRegister), {r(BankRegister), c(7)}),
       jump(Dispatch)});
  Provider.add(Dispatch, {operation(NdOp::INT_AND, r(SelectorRegister),
                                    {r(BankRegister), c(3)}),
                          jump(HandlerBase)});
  Provider.add(
      HandlerBase,
      {operation(NdOp::INT_MULT, r(AddressRegister),
                 {r(SelectorRegister), c(8)}),
       operation(NdOp::INT_ADD, r(AddressRegister),
                 {r(AddressRegister), c(TableBase)}),
       operation(NdOp::LOAD, r(ValueRegister), {r(AddressRegister)}),
       operation(NdOp::INT_ADD, r(AddressRegister),
                 {r(FrameRegister), r(ValueRegister)}),
       operation(NdOp::INT_XOR, r(ValueRegister), {r(InputRegister), c(83)}),
       operation(NdOp::STORE, {}, {r(AddressRegister), r(ValueRegister)}),
       operation(NdOp::LOAD, r(ResultRegister), {r(AddressRegister)}), ret()});
  return Provider;
}

SpecializationOptions finiteDependencyOptions(bool Manual, bool Automatic) {
  auto Options = bankOptions(false, false);
  Options.RequireRestoredFrameAtReturn = true;
  Options.DiscoverControlState = Automatic;
  if (Manual) {
    Options.ControlRegisters = {{BankRegister, 8}, {SelectorRegister, 8}};
    Options.ControlFrameSlots = {
        {FirstMaskSlot, 8}, {SecondMaskSlot, 8}, {DerivedDisplacementSlot, 8}};
  }
  return Options;
}

// Small byte-based concrete interpreter for this corpus only. It deliberately
// avoids symbolic execution and rejects missing runtime bytes or opcodes.
std::optional<uint64_t>
run(const LowFunc &Function, uint64_t Input, uint64_t Limit,
    const std::map<uint64_t, uint64_t> &ExtraRegisters = {}) {
  using Key = std::pair<VnodeSpace, uint64_t>;
  std::map<Key, uint8_t> Values;
  std::map<uint64_t, uint8_t> Memory;
  std::map<int, const LowBlock *> Blocks;
  for (const auto &Block : Function.Blocks)
    Blocks.emplace(Block.Id, &Block);
  bool Valid = true;
  const auto Write = [&](NdVar Destination, uint64_t Value) {
    for (unsigned I = 0; I < Destination.Size; ++I)
      Values[{Destination.Space, Destination.Offset + I}] = Value >> (8 * I);
  };
  const auto Read = [&](NdVar Value) {
    uint64_t Result = Value.isConst() ? Value.Offset : 0;
    if (!Value.isConst())
      for (unsigned I = 0; I < Value.Size; ++I) {
        const auto At = Values.find({Value.Space, Value.Offset + I});
        if (At == Values.end()) {
          Valid = false;
          return uint64_t{0};
        }
        Result |= uint64_t{At->second} << (8 * I);
      }
    return Value.Size == 8 ? Result
                           : Result & ((uint64_t{1} << (Value.Size * 8)) - 1);
  };
  Write(r(FrameRegister), 0xa000);
  Write(r(InputRegister), Input);
  Write(r(LimitRegister), Limit);
  for (const auto &[Register, Value] : ExtraRegisters)
    Write(r(Register), Value);
  int Current = Function.Blocks.front().Id;
  for (unsigned Step = 0; Step < 10000; ++Step) {
    const auto Found = Blocks.find(Current);
    if (Found == Blocks.end())
      return std::nullopt;
    const auto &Block = *Found->second;
    std::optional<int> Next;
    for (const LowOp &Op : Block.Ops) {
      const auto A = [&] { return Read(Op.Inputs[0]); };
      const auto B = [&] { return Read(Op.Inputs[1]); };
      switch (Op.Opcode) {
      case NdOp::COPY:
      case NdOp::INT_ZEXT:
        Write(Op.Output, A());
        break;
      case NdOp::INT_ADD:
        Write(Op.Output, A() + B());
        break;
      case NdOp::INT_SUB:
        Write(Op.Output, A() - B());
        break;
      case NdOp::INT_MULT:
        Write(Op.Output, A() * B());
        break;
      case NdOp::INT_AND:
        Write(Op.Output, A() & B());
        break;
      case NdOp::INT_XOR:
        Write(Op.Output, A() ^ B());
        break;
      case NdOp::INT_EQUAL:
        Write(Op.Output, A() == B());
        break;
      case NdOp::INT_LESS:
        Write(Op.Output, A() < B());
        break;
      case NdOp::BOOL_AND:
        Write(Op.Output, A() && B());
        break;
      case NdOp::BOOL_OR:
        Write(Op.Output, A() || B());
        break;
      case NdOp::SELECT:
        Write(Op.Output, A() ? B() : Read(Op.Inputs[2]));
        break;
      case NdOp::LOAD: {
        const auto Access = lowMemoryOperands(Op);
        const uint64_t Address = Read(*Access.Address);
        uint64_t Value = 0;
        for (unsigned Byte = 0; Byte < Access.AccessSize; ++Byte) {
          const auto At = Memory.find(Address + Byte);
          if (At == Memory.end())
            return std::nullopt;
          Value |= uint64_t{At->second} << (8 * Byte);
        }
        Write(Op.Output, Value);
        break;
      }
      case NdOp::STORE: {
        const auto Access = lowMemoryOperands(Op);
        const auto Address = Read(*Access.Address);
        const auto Value = Read(*Access.StoredValue);
        for (unsigned Byte = 0; Byte < Access.AccessSize; ++Byte)
          Memory[Address + Byte] = Value >> (8 * Byte);
        break;
      }
      case NdOp::BRANCH:
      case NdOp::COND_BR: {
        const auto Target = A();
        const bool Taken = Op.Opcode == NdOp::BRANCH || B();
        for (int Successor : Block.Succs)
          if ((Blocks.at(Successor)->StartAddr == Target) == Taken)
            Next = Successor;
        if (!Next && Block.Succs.size() == 1)
          Next = Block.Succs.front();
        break;
      }
      case NdOp::RETURN: {
        const auto Result = Read(r(ResultRegister));
        return Valid ? std::optional(Result) : std::nullopt;
      }
      case NdOp::NOP:
        break;
      default:
        ADD_FAILURE() << "unexpected residual opcode " << ndOpName(Op.Opcode);
        return std::nullopt;
      }
      if (!Valid)
        return std::nullopt;
    }
    if (!Next)
      return std::nullopt;
    Current = *Next;
  }
  return std::nullopt;
}

void expectNoPublication(const SpecializationResult &Result) {
  EXPECT_EQ(Result.Status, SpecializationStatus::UnresolvedControl)
      << Result.Diagnostic;
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  EXPECT_TRUE(Result.Origins.empty());
  EXPECT_TRUE(Result.Reads.empty());
}

SpecializationOptions automaticBankOptions() {
  auto Options = bankOptions(false, false);
  Options.DiscoverControlState = true;
  return Options;
}

void expectBudgetRefusal(const SpecializationResult &Result) {
  EXPECT_EQ(Result.Status, SpecializationStatus::BudgetExceeded)
      << Result.Diagnostic;
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  EXPECT_TRUE(Result.Origins.empty());
  EXPECT_TRUE(Result.Reads.empty());
}
} // namespace

TEST(ControlStateRecovery, SpilledSelectorNeedsBothCurrentProjectionHints) {
  auto Provider = makeBankProgram(false);
  for (const auto [SlotHint, RegisterHint] :
       {std::pair{false, false}, std::pair{true, false},
        std::pair{false, true}})
    expectNoPublication(specializeInterpreter(
        Provider, {Entry}, bankOptions(SlotHint, RegisterHint)));

  auto Result =
      specializeInterpreter(Provider, {Entry}, bankOptions(true, true));
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(Result.Reads.size(), 4u);
  for (uint64_t Input : {0ULL, 1ULL, 2ULL, 3ULL, 19ULL, ~0ULL})
    EXPECT_EQ(run(Result.Residual, Input, 1),
              (Input ^ 45) + 3 + 11 * (Input & 3));
}

TEST(ControlStateRecovery,
     SpilledSelectorLoopKeepsBusinessCounterUnpartitioned) {
  auto Provider = makeBankProgram(true);
  expectNoPublication(
      specializeInterpreter(Provider, {Entry}, bankOptions(false, false)));
  auto Result =
      specializeInterpreter(Provider, {Entry}, bankOptions(true, true));
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_LT(Result.Contexts, 48u);
  EXPECT_LT(Result.NodeEvaluations, 192u);
  for (uint64_t Input : {0ULL, 1ULL, 7ULL, 22ULL, ~0ULL})
    for (uint64_t Limit : {1ULL, 2ULL, 5ULL, 19ULL}) {
      uint64_t Expected = 0;
      for (uint64_t Iteration = 0; Iteration < Limit; ++Iteration)
        Expected += (Input ^ 45) + 3 + 11 * ((Input + Iteration) & 3);
      EXPECT_EQ(run(Result.Residual, Input, Limit), Expected);
    }
}

TEST(ControlStateRecovery, HintsCannotInventBoundsForUnknownSelectorBytes) {
  for (auto Input :
       {SelectorInput::UnboundedScalar, SelectorInput::UnknownMemory}) {
    auto Provider = makeBankProgram(false, Input);
    expectNoPublication(
        specializeInterpreter(Provider, {Entry}, bankOptions(true, true)));
  }
}

TEST(ControlStateRecovery, DiscoversSpilledAndReloadedSelectorWithoutHints) {
  auto Provider = makeBankProgram(false);
  const auto Options = automaticBankOptions();
  const auto Result = specializeInterpreter(Provider, {Entry}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_GE(Result.DiscoveredControlFields, 2u);
  EXPECT_EQ(Result.DiscoveredContextFields, 0u);
  EXPECT_LE(Result.DiscoveredControlFields, Options.MaxControlFields);
  EXPECT_GT(Result.ControlRefinements, 0u);
  EXPECT_LE(Result.ControlRefinements, Options.MaxControlRefinements);
  EXPECT_GT(Result.DiscoveryVisits, 0u);
  EXPECT_LE(Result.DiscoveryVisits, Options.MaxDiscoveryVisits);
  for (uint64_t Input : {0ULL, 1ULL, 2ULL, 3ULL, 19ULL, ~0ULL})
    EXPECT_EQ(run(Result.Residual, Input, 1),
              (Input ^ 45) + 3 + 11 * (Input & 3));
}

TEST(ControlStateRecovery, AutomaticFieldsNeverPartitionLoopCounterContexts) {
  auto Provider = makeBankProgram(true);
  auto Options = automaticBankOptions();
  // The selector changes on every backedge and the business counter grows
  // without a static bound. Automatically retained fields must use relational
  // projection only: even two distinct contexts at one address are forbidden.
  Options.MaxContextsPerAddress = 1;
  const auto Result = specializeInterpreter(Provider, {Entry}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_GE(Result.DiscoveredControlFields, 2u);
  EXPECT_EQ(Result.DiscoveredContextFields, 0u);
  EXPECT_LE(Result.ControlRefinements, Options.MaxControlRefinements);
  EXPECT_LE(Result.DiscoveryVisits, Options.MaxDiscoveryVisits);
  for (uint64_t Input : {0ULL, 1ULL, 7ULL, ~0ULL})
    for (uint64_t Limit : {1ULL, 5ULL, 19ULL, 300ULL}) {
      uint64_t Expected = 0;
      for (uint64_t Iteration = 0; Iteration < Limit; ++Iteration)
        Expected += (Input ^ 45) + 3 + 11 * ((Input + Iteration) & 3);
      EXPECT_EQ(run(Result.Residual, Input, Limit), Expected);
    }
}

TEST(ControlStateRecovery, DiscoveryIgnoresUnrelatedControlRegisterReuse) {
  auto Provider = makeBankProgram(false);
  constexpr va_t BranchEntry = 0x400;
  constexpr va_t BusinessPhase = 0x420;
  constexpr va_t BypassPhase = 0x440;
  constexpr uint64_t FirstBusinessRegister = 0x1000;
  constexpr unsigned BusinessInputs = 17;
  Provider.add(BranchEntry,
               {operation(NdOp::INT_AND, r(ConditionRegister, 1),
                          {r(InputRegister, 1), c(1, 1)}),
                operation(NdOp::COND_BR, {},
                          {c(BusinessPhase), r(ConditionRegister, 1)})},
               BypassPhase);
  llvm::SmallVector<LowOp, 20> BusinessOps{
      operation(NdOp::COPY, r(SelectorRegister), {r(FirstBusinessRegister)})};
  for (unsigned I = 1; I < BusinessInputs; ++I)
    BusinessOps.push_back(
        operation(NdOp::INT_XOR, r(SelectorRegister),
                  {r(SelectorRegister), r(FirstBusinessRegister + I * 8)}));
  BusinessOps.push_back(jump(Entry));
  Provider.add(BusinessPhase, BusinessOps);
  Provider.add(BypassPhase, {operation(NdOp::COPY, r(SelectorRegister), {c(0)}),
                             jump(Entry)});

  auto Options = automaticBankOptions();
  Options.MaxControlFields = 4;
  // The business phase is visited before dispatch loses its selector. On a
  // retry the same register is tracked, but its 17-input value is overwritten
  // by Entry before any control use. Only demanded successor phases may ask
  // discovery to retain this register's producers.
  const auto Result = specializeInterpreter(Provider, {BranchEntry}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_GE(Result.DiscoveredControlFields, 2u);
  EXPECT_LE(Result.DiscoveredControlFields, Options.MaxControlFields);
  EXPECT_EQ(Result.DiscoveredContextFields, 0u);
  EXPECT_GT(Result.ControlRefinements, 0u);
  EXPECT_LE(Result.ControlRefinements, Options.MaxControlRefinements);
  EXPECT_LE(Result.DiscoveryVisits, Options.MaxDiscoveryVisits);
  for (uint64_t Seed : {0ULL, 71ULL, ~0ULL}) {
    std::map<uint64_t, uint64_t> ExtraRegisters;
    for (unsigned I = 0; I < BusinessInputs; ++I)
      ExtraRegisters.emplace(FirstBusinessRegister + I * 8,
                             (Seed + 19 * I) ^ (Seed >> I));
    for (uint64_t Input : {0ULL, 1ULL, 2ULL, 3ULL, 19ULL, ~0ULL})
      EXPECT_EQ(run(Result.Residual, Input, 1, ExtraRegisters),
                (Input ^ 45) + 3 + 11 * (Input & 3));
  }
}

TEST(ControlStateRecovery, AutomaticDiscoveryCannotInventUnboundedInputs) {
  for (auto Input :
       {SelectorInput::UnboundedScalar, SelectorInput::UnknownMemory}) {
    auto Provider = makeBankProgram(false, Input);
    expectNoPublication(
        specializeInterpreter(Provider, {Entry}, automaticBankOptions()));
  }
}

TEST(ControlStateRecovery, AutomaticRefinementBudgetsPublishNoPartialGraph) {
  for (unsigned Budget = 0; Budget < 4; ++Budget) {
    SCOPED_TRACE(Budget);
    auto Provider = makeBankProgram(false);
    auto Options = automaticBankOptions();
    switch (Budget) {
    case 0:
      Options.MaxControlRefinements = 0;
      break;
    case 1:
      Options.MaxDiscoveryVisits = 0;
      break;
    case 2:
      Options.MaxDiscoveryVisits = 1;
      break;
    case 3:
      Options.MaxControlFields = 1;
      break;
    }
    expectBudgetRefusal(specializeInterpreter(Provider, {Entry}, Options));
  }
}

TEST(ControlStateRecovery, ManualAndAutomaticFieldsShareOneFieldBudget) {
  auto Provider = makeBankProgram(false);
  auto Options = bankOptions(true, false);
  Options.DiscoverControlState = true;
  Options.MaxControlFields = 1;
  expectBudgetRefusal(specializeInterpreter(Provider, {Entry}, Options));
}

TEST(ControlStateRecovery, ZeroDiscoveryBudgetsPermitAlreadyProvedControl) {
  BankProvider Provider;
  Provider.add(Entry,
               {operation(NdOp::COPY, r(ResultRegister), {c(17)}), ret()});
  auto Options = automaticBankOptions();
  Options.MaxControlRefinements = 0;
  Options.MaxDiscoveryVisits = 0;
  const auto Result = specializeInterpreter(Provider, {Entry}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(Result.DiscoveredControlFields, 0u);
  EXPECT_EQ(Result.ControlRefinements, 0u);
  EXPECT_EQ(Result.DiscoveryVisits, 0u);
  EXPECT_EQ(run(Result.Residual, 0, 0), 17u);
}

TEST(ControlStateRecovery, RestartChargesEarlierOperationWorkToSameBudget) {
  auto Provider = makeBankProgram(false);
  const auto Initial =
      specializeInterpreter(Provider, {Entry}, bankOptions(false, false));
  ASSERT_FALSE(Initial.complete());
  ASSERT_GT(Initial.EvaluatedOperations, 0u);
  auto Options = automaticBankOptions();
  Options.MaxOperations = Initial.EvaluatedOperations;
  const auto Result = specializeInterpreter(Provider, {Entry}, Options);
  expectBudgetRefusal(Result);
}

TEST(ControlStateRecovery, ManualCursorContextProvesBoundedFrameWriteLoop) {
  auto Provider = makeMemoryCursorProgram();
  const auto WithoutHints = specializeInterpreter(
      Provider, {Entry}, memoryCursorOptions(false, false));
  EXPECT_EQ(WithoutHints.Status, SpecializationStatus::Unsupported)
      << WithoutHints.Diagnostic;
  EXPECT_TRUE(WithoutHints.Residual.Blocks.empty());
  const auto Result = specializeInterpreter(Provider, {Entry},
                                            memoryCursorOptions(true, false));
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(Result.Reads.size(), MemoryCursorPositions);
  for (uint64_t Input : {0ULL, 1ULL, 23ULL, ~0ULL})
    EXPECT_EQ(run(Result.Residual, Input, 0),
              MemoryCursorPositions * Input +
                  MemoryCursorPositions * (MemoryCursorPositions - 1) / 2);
}

TEST(ControlStateRecovery, MemoryPrecisionCanPromoteProvedCursorContexts) {
  auto Provider = makeMemoryCursorProgram();
  const auto Options = memoryCursorOptions(false, true);
  ASSERT_GT(MemoryCursorPositions, Options.MaxImmutableReadAddresses);
  const auto Result = specializeInterpreter(Provider, {Entry}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_GT(Result.DiscoveredControlFields, 0u);
  EXPECT_GT(Result.DiscoveredContextFields, 0u);
  EXPECT_LE(Result.DiscoveredContextFields, Result.DiscoveredControlFields);
  EXPECT_GT(Result.ControlRefinements, 0u);
  EXPECT_LE(Result.ControlRefinements, Options.MaxControlRefinements);
  EXPECT_LE(Result.DiscoveryVisits, Options.MaxDiscoveryVisits);
  EXPECT_EQ(Result.Reads.size(), MemoryCursorPositions);
  for (const auto &Block : Result.Residual.Blocks)
    for (const auto &Op : Block.Ops)
      EXPECT_NE(Op.Opcode, NdOp::INDIR_BR);
  for (uint64_t Input : {0ULL, 1ULL, 23ULL, 0x123456789abcdef0ULL, ~0ULL})
    EXPECT_EQ(run(Result.Residual, Input, 0),
              MemoryCursorPositions * Input +
                  MemoryCursorPositions * (MemoryCursorPositions - 1) / 2);
}

TEST(ControlStateRecovery, MemoryContextPromotionKeepsContextBudget) {
  auto Provider = makeMemoryCursorProgram();
  auto Options = memoryCursorOptions(false, true);
  Options.MaxContextsPerAddress = 1;
  expectBudgetRefusal(specializeInterpreter(Provider, {Entry}, Options));
}

TEST(ControlStateRecovery, MemoryDiscoveryCannotBoundInputDerivedFrameWrites) {
  auto Provider = makeMemoryCursorProgram(true);
  const auto Result = specializeInterpreter(Provider, {Entry},
                                            memoryCursorOptions(false, true));
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported)
      << Result.Diagnostic;
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  EXPECT_TRUE(Result.Origins.empty());
  EXPECT_TRUE(Result.Reads.empty());
}

TEST(ControlStateRecovery, ManualFieldsPreserveFiniteProducerCorrelation) {
  auto Provider = makeFiniteDependencyProgram();
  const auto WithoutHints = specializeInterpreter(
      Provider, {Entry}, finiteDependencyOptions(false, false));
  EXPECT_EQ(WithoutHints.Status, SpecializationStatus::Unsupported)
      << WithoutHints.Diagnostic;
  EXPECT_TRUE(WithoutHints.Residual.Blocks.empty());
  const auto Result = specializeInterpreter(
      Provider, {Entry}, finiteDependencyOptions(true, false));
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  ASSERT_EQ(Result.Reads.size(), 1u);
  EXPECT_EQ(Result.Reads.front().Address, TableBase);
  for (uint64_t Input : {0ULL, 1ULL, 6ULL, 17ULL, ~0ULL})
    EXPECT_EQ(run(Result.Residual, Input, 0), Input ^ 83);
}

TEST(ControlStateRecovery, MemoryDemandFollowsAlreadyFiniteProducerChain) {
  // A and B are correlated bits, so A+B-1 is zero. Losing either spill makes
  // the displacement and cursor finite but imprecise. Finite projection alone
  // must not stop the dependency chain before reaching those producer slots.
  auto Provider = makeFiniteDependencyProgram();
  const auto Options = finiteDependencyOptions(false, true);
  const auto Result = specializeInterpreter(Provider, {Entry}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_GE(Result.DiscoveredControlFields, 5u);
  EXPECT_GT(Result.ControlRefinements, 1u);
  EXPECT_LE(Result.ControlRefinements, Options.MaxControlRefinements);
  EXPECT_LE(Result.DiscoveryVisits, Options.MaxDiscoveryVisits);
  ASSERT_EQ(Result.Reads.size(), 1u);
  EXPECT_EQ(Result.Reads.front().Address, TableBase);
  for (uint64_t Input : {0ULL, 1ULL, 6ULL, 17ULL, 0xabcdef0123456789ULL, ~0ULL})
    EXPECT_EQ(run(Result.Residual, Input, 0), Input ^ 83);
}
