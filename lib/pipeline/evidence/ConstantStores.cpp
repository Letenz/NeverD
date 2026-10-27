//===- ConstantStores.cpp - LowIR constant-store projection --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Selected instruction boundaries, full-width values and atomic refusal.
///
//===----------------------------------------------------------------------===//

#include "neverd/pipeline/ConstantStores.h"

#include "neverd/decode/Decoder.h"
#include "neverd/decode/InstructionRelocations.h"
#include "neverd/pipeline/Limits.h"

#include "llvm/Support/Errc.h"

#include <algorithm>
#include <map>
#include <optional>
#include <stdexcept>

namespace neverd {
namespace {
struct Refusal : std::runtime_error {
  using std::runtime_error::runtime_error;
};
struct Value {
  bool Constant = false;
  uint64_t Bits = 0, Base = 0;
  int64_t Offset = 0;
  ConstantAddressProvenance Provenance = ConstantAddressProvenance::Unknown;
};
struct Context {
  const BinaryImage &Image;
  Decoder Decode;
  NativeConstantStores Result;
  std::map<std::pair<VnodeSpace, uint64_t>, Value> Values;

  void step(uint64_t Count = 1) {
    if (Count >
        ::neverd::limits::pipeline::kMaxNativeConstantStoreSteps - Result.Steps)
      throw Refusal("native_store_work_budget_exceeded");
    Result.Steps += Count;
  }
  DecodedInsn instruction(va_t PC) {
    step();
    const auto *Segment = Image.getSegmentFor(PC);
    if (!Segment || !Segment->isExecutable() || PC < Segment->VA ||
        Segment->Size > UINT64_MAX - Segment->VA ||
        PC - Segment->VA >= Segment->Data.size() ||
        !Decode.selectMode(Image, PC))
      throw Refusal("native_store_invalid_instruction_range");
    const auto Offset = PC - Segment->VA;
    const auto Size =
        std::min<uint64_t>(16, std::min<uint64_t>(Segment->Data.size() - Offset,
                                                  Segment->Size - Offset));
    DecodedInsn I{};
    if (Decode.decodeOne(Segment->Data.data() + Offset, Size, PC, I) <= 0)
      throw Refusal("native_store_decode_failed");
    return I;
  }
  Value value(const NdVar &V) {
    if (V.Size != 8)
      throw Refusal("native_store_width_unavailable");
    if (V.isConst())
      return {true, V.Offset, 0, 0, V.Provenance};
    if (const auto Found = Values.find({V.Space, V.Offset});
        Found != Values.end())
      return Found->second;
    if (V.isReg())
      return {false, 0, V.Offset, 0, ConstantAddressProvenance::Unknown};
    throw Refusal("native_store_value_unavailable");
  }
  Value arithmetic(const LowOp &Op) {
    if (Op.NumInputs != 2)
      throw Refusal("native_store_operation_unavailable");
    const auto L = value(Op.Inputs[0]), R = value(Op.Inputs[1]);
    const bool Add = Op.Opcode == NdOp::INT_ADD;
    if (L.Constant && R.Constant) {
      auto Result = L;
      Result.Bits = Add ? L.Bits + R.Bits : L.Bits - R.Bits;
      Result.Provenance = ConstantAddressProvenance::Scalar;
      // Only transport an already exact address, without wrapping or
      // subtracting an address. Numeric equality never grants provenance.
      if (isExactAddressProvenance(L.Provenance) &&
          !isAddressProvenance(R.Provenance) &&
          (Add ? Result.Bits >= L.Bits : L.Bits >= R.Bits))
        Result.Provenance = L.Provenance;
      else if (Add && isExactAddressProvenance(R.Provenance) &&
               !isAddressProvenance(L.Provenance) && Result.Bits >= R.Bits)
        Result.Provenance = R.Provenance;
      return Result;
    }
    const auto Base = !L.Constant ? L : R;
    const auto Number = L.Constant ? L : R;
    if (Base.Constant || !Number.Constant || (!Add && L.Constant) ||
        isAddressProvenance(Number.Provenance))
      throw Refusal("native_store_address_unavailable");
    const auto Delta = int64_t(Number.Bits);
    if (Delta < -::neverd::limits::pipeline::kMaxConstantStoreDisplacement ||
        Delta > ::neverd::limits::pipeline::kMaxConstantStoreDisplacement)
      throw Refusal("native_store_offset_unavailable");
    auto Result = Base;
    Result.Offset += Add ? Delta : -Delta;
    if (Result.Offset <
            -::neverd::limits::pipeline::kMaxConstantStoreDisplacement ||
        Result.Offset >
            ::neverd::limits::pipeline::kMaxConstantStoreDisplacement)
      throw Refusal("native_store_offset_unavailable");
    return Result;
  }
  void apply(const LowOp &Op) {
    step();
    if (Op.MemoryOrdering != NdMemoryOrdering::None ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      throw Refusal("native_store_memory_semantics_unavailable");
    if (Op.Opcode == NdOp::STORE && Op.NumInputs == 2 && !Op.Output.Size) {
      const auto Address = value(Op.Inputs[0]), Data = value(Op.Inputs[1]);
      if (Address.Constant || !Data.Constant)
        throw Refusal("native_store_destination_unavailable");
      if (Result.Stores.empty()) {
        Result.BaseRegister = Address.Base;
        Result.BaseOffset = Address.Offset;
      } else if (Address.Base != Result.BaseRegister ||
                 Address.Offset != Result.BaseOffset + 8 * Result.Stores.size())
        throw Refusal("native_store_nonconsecutive_fields");
      Result.Stores.push_back({Op.Addr, Data.Bits, Data.Provenance});
      return;
    }
    if (Op.Output.Size != 8 || (!Op.Output.isReg() && !Op.Output.isTemp()))
      throw Refusal("native_store_effect_unavailable");
    std::optional<Value> V;
    if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
      V = value(Op.Inputs[0]);
    else if (Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB)
      V = arithmetic(Op);
    else
      throw Refusal("native_store_effect_unavailable");
    if (!Result.Stores.empty() && Op.Output.isReg() &&
        Op.Output.Offset == Result.BaseRegister)
      throw Refusal("native_store_destination_base_written");
    Values[{Op.Output.Space, Op.Output.Offset}] = *V;
  }
};
} // namespace

llvm::Expected<NativeConstantStores>
projectNativeConstantStores(const BinaryImage &Image, va_t Start,
                            uint32_t Count) {
  try {
    if (Image.Arch != Arch::X64 || Image.Bits != Bitness::Bits64 || !Count ||
        Count > 16)
      throw Refusal("native_store_profile_unavailable");
    Context C{Image};
    if (!C.Decode.init(Image))
      throw Refusal("native_store_decoder_unavailable");
    C.Decode.setStrict(true);
    std::optional<va_t> Entry;
    std::optional<va_t> NextEntry;
    if (Image.Symbols.size() >
        ::neverd::limits::pipeline::
            kConstantStoresProjectNativeConstantStoresImageSymbolsSizeLimit)
      throw Refusal("native_store_function_budget_exceeded");
    for (const auto &Symbol : Image.Symbols) {
      C.step();
      if (Symbol.IsFunc && !Symbol.IsBoundaryGuess && Symbol.Addr <= Start &&
          (!Entry || Symbol.Addr > *Entry))
        Entry = Symbol.Addr;
      if (Symbol.IsFunc && !Symbol.IsBoundaryGuess && Symbol.Addr > Start &&
          (!NextEntry || Symbol.Addr < *NextEntry))
        NextEntry = Symbol.Addr;
    }
    if (!Entry ||
        Start - *Entry >
            ::neverd::limits::pipeline::kMaxNativeConstantStoreEntryDistance)
      throw Refusal("native_store_function_entry_unavailable");
    va_t PC = *Entry;
    while (PC < Start) {
      const auto I = C.instruction(PC);
      if (I.Size > Start - PC)
        throw Refusal("native_store_instruction_boundary_required");
      PC += I.Size;
    }
    C.Result.Start = Start;
    C.Result.FunctionEntry = *Entry;
    while (
        C.Result.Instructions <
            ::neverd::limits::pipeline::
                kConstantStoresProjectNativeConstantStoresCResultInstructionsLimit &&
        PC - Start <
            ::neverd::limits::pipeline::
                kConstantStoresProjectNativeConstantStoresPCStartLimit) {
      if (NextEntry && PC >= *NextEntry)
        throw Refusal("native_store_function_boundary_crossed");
      const auto I = C.instruction(PC);
      if (I.Size >
              ::neverd::limits::pipeline::
                      kConstantStoresProjectNativeConstantStoresPCStartLimit -
                  (PC - Start) ||
          !I.Size)
        throw Refusal("native_store_instruction_budget_exceeded");
      if (NextEntry && I.Size > *NextEntry - PC)
        throw Refusal("native_store_function_boundary_crossed");
      std::vector<LowOp> Ops;
      const auto Relocations = instructionRelocations(Image, I);
      C.Decode.liftToLow(I, Ops, Relocations.Addresses, Relocations.Scalars);
      if (Ops.empty() ||
          Ops.size() >
              ::neverd::limits::pipeline::
                  kConstantStoresProjectNativeConstantStoresOpsSizeLimit)
        throw Refusal("native_store_lift_unavailable");
      // LowIR temporaries are instruction-local. Registers alone transport
      // values across the selected straight-line initializer.
      for (auto It = C.Values.begin(); It != C.Values.end();)
        It = It->first.first == VnodeSpace::TEMP ? C.Values.erase(It)
                                                 : std::next(It);
      for (const auto &Op : Ops) {
        C.apply(Op);
        if (C.Result.Stores.size() > Count)
          throw Refusal("native_store_extra_field");
      }
      ++C.Result.Instructions;
      PC += I.Size;
      if (C.Result.Stores.size() == Count) {
        C.Result.End = PC;
        return std::move(C.Result);
      }
    }
    throw Refusal("native_store_instruction_budget_exceeded");
  } catch (const Refusal &E) {
    return llvm::createStringError(llvm::errc::invalid_argument, "%s",
                                   E.what());
  } catch (const UnliftedInstruction &) {
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "native_store_instruction_unavailable");
  }
}
} // namespace neverd
