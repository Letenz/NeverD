//===- NeverDCAPIDisasm.cpp - C API: disassembly --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Disassembly functions: JSON array output and annotated text output.
///
//===----------------------------------------------------------------------===//

#include "JSONText.h"
#include "SessionImpl.h"

#include "neverd/evm/analysis/EVMAnalyzer.h"
#include "neverd/sbf/analysis/SBFAnalyzer.h"
#include "neverd/sbf/analysis/SBFFunctionBody.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/BitVector.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <optional>

using namespace neverd;
using namespace neverd::sdk;

namespace {

inline constexpr size_t kShortInstructionByteColumnWidth = 18;

std::string evmBytes(const evm::LowInstruction &Instruction) {
  std::string Bytes;
  for (uint8_t Byte : Instruction.Encoding)
    Bytes += llvm::utohexstr(Byte, /*LowerCase=*/true, evm::kHexDigitsPerByte);
  return Bytes;
}

std::string sbfBytes(const sbf::LowInstruction &Instruction) {
  std::string Bytes;
  for (uint8_t Byte : Instruction.Encoding)
    Bytes += llvm::utohexstr(Byte, /*LowerCase=*/true, evm::kHexDigitsPerByte);
  return Bytes;
}

llvm::BitVector functionSlots(const sbf::SBFProgram &Program,
                              const sbf::Function &Function) {
  llvm::BitVector Slots(Program.Low.Instructions.size());
  const sbf::FunctionBodyIndex FunctionBodies(Program);
  for (size_t BlockID : FunctionBodies.blocks(Function)) {
    if (BlockID >= Program.Low.Blocks.size())
      continue;
    const sbf::BasicBlock &Block = Program.Low.Blocks[BlockID];
    const size_t End =
        std::min(Block.EndSlot, static_cast<size_t>(Slots.size()));
    for (size_t Slot = Block.StartSlot; Slot < End; ++Slot)
      Slots.set(Slot);
  }
  return Slots;
}

/// Linear native decode from \p Addr, bounded by \p Span bytes when nonzero
/// and by \p Limit instructions.  Mode selection, segment bounds and ARM state
/// checks are those of the published disassembly; \p Visit returns false to
/// stop after an instruction.
template <typename VisitorT>
bool decodeNativeRange(const BinaryImage &Img, Decoder &Dec, va_t Addr,
                       uint64_t Span, uint64_t Limit, VisitorT Visit) {
  va_t Cur = Addr;
  std::optional<InstructionMode> PathMode = Img.instructionModeAt(Addr);
  for (uint64_t I = 0; I < Limit; ++I) {
    if (Cur < Addr)
      break;
    const Segment *Seg = Img.getSegmentFor(Cur);
    if (!Seg || !Seg->isExecutable())
      break;

    uint64_t Consumed = Cur - Addr;
    if (Span > 0 && Consumed >= Span)
      break;

    uint64_t Off64 = Cur - Seg->VA;
    if (Off64 >= Seg->Data.size())
      break;
    size_t Off = static_cast<size_t>(Off64);
    uint64_t Avail64 = std::min<uint64_t>(
        16, std::min<uint64_t>(Seg->Size - Off64, Seg->Data.size() - Off));
    if (Span > 0)
      Avail64 = std::min(Avail64, Span - Consumed);
    if (Avail64 == 0)
      break;
    const uint8_t *Bytes = Seg->Data.data() + Off;

    if (!Dec.selectMode(Img, Cur, PathMode))
      return false; // Unknown or conflicting instruction mode.
    PathMode = Dec.currentMode();
    DecodedInsn DI;
    int Sz = Dec.decodeOne(Bytes, static_cast<size_t>(Avail64), Cur, DI);
    if (Sz <= 0)
      break;
    if (Img.Arch == Arch::ARM &&
        Img.instructionModeAt(Cur + Sz - 1, Dec.currentMode()) !=
            Dec.currentMode())
      break;
    if (!Visit(DI, Bytes, Sz))
      break;
    if (static_cast<uint64_t>(Sz) > InvalidVA - Cur)
      break;
    Cur += Sz;
  }
  return true;
}

/// Control transfer and constant memory references of one decoded native
/// instruction.  They come from the instruction's own LowIR lift, so a listing
/// agrees with the operations the pipeline consumes.  Kind is empty for a
/// fall-through instruction.
struct InstructionFlow {
  llvm::StringRef Kind;
  va_t Target = InvalidVA;
  llvm::SmallVector<std::pair<va_t, llvm::StringRef>, 2> Refs;
};

InstructionFlow summarizeInstructionFlow(Decoder &Dec, const DecodedInsn &DI) {
  InstructionFlow Flow;
  std::vector<LowOp> Ops;
  // Each row is lifted independently of its neighbours.
  Dec.resetX86FpuState();
  Dec.liftToLow(DI, Ops);
  const auto ConstantInput = [](const LowOp &Op) -> std::optional<va_t> {
    if (Op.NumInputs == 0 || !Op.Inputs[0].isConst())
      return std::nullopt;
    return static_cast<va_t>(Op.Inputs[0].Offset);
  };
  for (const LowOp &Op : Ops) {
    switch (Op.Opcode) {
    case NdOp::CALL:
      if (auto Target = ConstantInput(Op)) {
        Flow.Kind = "call";
        Flow.Target = *Target;
      } else {
        Flow.Kind = "icall";
      }
      break;
    case NdOp::INDIR_CALL:
      Flow.Kind = "icall";
      if (auto Slot = ConstantInput(Op))
        Flow.Refs.push_back({*Slot, "read"});
      break;
    case NdOp::BRANCH:
    case NdOp::COND_BR:
      Flow.Kind = Op.Opcode == NdOp::BRANCH ? "jump" : "cjump";
      if (auto Target = ConstantInput(Op))
        Flow.Target = *Target;
      break;
    case NdOp::INDIR_BR:
      Flow.Kind = "ijump";
      if (auto Slot = ConstantInput(Op))
        Flow.Refs.push_back({*Slot, "read"});
      break;
    case NdOp::RETURN:
      Flow.Kind = "ret";
      break;
    case NdOp::LOAD:
    case NdOp::STORE: {
      const LowMemoryOperandView Memory = lowMemoryOperands(Op);
      if (Memory.Complete && Memory.Address && Memory.Address->isConst())
        Flow.Refs.push_back({static_cast<va_t>(Memory.Address->Offset),
                             Op.Opcode == NdOp::LOAD ? "read" : "write"});
      break;
    }
    default:
      break;
    }
  }
  if (va_t Address = Dec.pcRelCodeRefTarget(DI); Address != InvalidVA)
    Flow.Refs.push_back({Address, "offset"});
  return Flow;
}

} // namespace

// ===--------------------------------------------------------------------===//
// Disassembly (JSON array)
// ===--------------------------------------------------------------------===//

const char *neverd_disasm_json(neverd_session_t Sess, neverd_va_t Addr,
                               int MaxInsns) {
  return neverd_disasm_json_ex(Sess, Addr, MaxInsns, 0);
}

const char *neverd_disasm_json_ex(neverd_session_t Sess, neverd_va_t Addr,
                                  int MaxInsns, unsigned Options) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return dupStr(std::string("[]"));
  }

  if (S->Img.Arch == Arch::EVM) {
    if (!S->ensurePipeline() || !S->PipeResult.EVM)
      return dupStr(std::string("[]"));
    llvm::json::Array EVMInstructions;
    int Count = 0;
    for (const auto &Instruction : S->PipeResult.EVM->Low.Instructions) {
      if (Instruction.PC < Addr)
        continue;
      if (MaxInsns > 0 && Count >= MaxInsns)
        break;
      llvm::json::Object Object;
      Object["addr"] = vaHex(Instruction.PC);
      Object["size"] = static_cast<int64_t>(Instruction.Encoding.size());
      Object["mnemonic"] = std::string(Instruction.Info.Name);
      Object["op_str"] = evm::formatImmediate(Instruction);
      Object["decode_status"] =
          evm::opcodeDecodeStatusName(Instruction.DecodeStatus);
      Object["immediate_status"] =
          evm::immediateDecodeStatusName(Instruction.ImmediateStatus);
      Object["bytes"] = evmBytes(Instruction);
      EVMInstructions.push_back(std::move(Object));
      ++Count;
    }
    return dupStr(jsonToString(llvm::json::Value(std::move(EVMInstructions))));
  }

  if (S->Img.Arch == Arch::SBF) {
    if (!S->ensurePipeline() || !S->PipeResult.SBF)
      return dupStr(std::string("[]"));
    llvm::json::Array Instructions;
    int Count = 0;
    llvm::BitVector SelectedSlots;
    if (MaxInsns <= 0)
      for (const sbf::Function &Function : S->PipeResult.SBF->High.Functions)
        if (Function.Address == Addr) {
          SelectedSlots = functionSlots(*S->PipeResult.SBF, Function);
          break;
        }
    for (const auto &Instruction : S->PipeResult.SBF->Low.Instructions) {
      if (Instruction.Address < Addr || Instruction.IsContinuation)
        continue;
      if (!SelectedSlots.empty() && !SelectedSlots.test(Instruction.Slot))
        continue;
      if (MaxInsns > 0 && Count >= MaxInsns)
        break;
      llvm::json::Object Object;
      Object["addr"] = vaHex(Instruction.Address);
      Object["size"] =
          static_cast<int64_t>(Instruction.SlotWidth * sbf::kInstructionSize);
      Object["mnemonic"] =
          Instruction.Info ? Instruction.Info->Mnemonic.str() : ".byte";
      Object["op_str"] = jsonSafeText(sbf::formatInstruction(Instruction));
      Object["bytes"] = sbfBytes(Instruction);
      Instructions.push_back(std::move(Object));
      ++Count;
    }
    return dupStr(jsonToString(llvm::json::Value(std::move(Instructions))));
  }

  if (!S->synchronizeFunctions())
    return dupStr(std::string("[]"));

  llvm::json::Array Arr;
  uint64_t Span = 0;
  for (const auto &F : S->Functions) {
    if (F.Entry == Addr) {
      Span = F.Size;
      break;
    }
  }
  uint64_t Limit =
      MaxInsns > 0 ? static_cast<uint64_t>(MaxInsns) : (Span > 0 ? Span : 256);

  const bool ModeKnown = decodeNativeRange(
      S->Img, S->Dec, Addr, Span, Limit,
      [&](const DecodedInsn &DI, const uint8_t *Bytes, int Sz) {
        std::string BytesHex;
        for (int J = 0; J < Sz; ++J) {
          char Buf[4];
          snprintf(Buf, sizeof(Buf), "%02x", Bytes[J]);
          BytesHex += Buf;
        }

        llvm::json::Object Obj;
        Obj["addr"] = vaHex(DI.Addr);
        Obj["size"] = Sz;
        Obj["mnemonic"] = std::string(DI.Raw ? DI.Raw->mnemonic : "");
        Obj["op_str"] = std::string(DI.Raw ? DI.Raw->op_str : "");
        Obj["bytes"] = BytesHex;
        if (Options & NEVERD_DISASM_FLOW) {
          const InstructionFlow Flow = summarizeInstructionFlow(S->Dec, DI);
          if (!Flow.Kind.empty())
            Obj["flow"] = Flow.Kind.str();
          if (Flow.Target != InvalidVA)
            Obj["target"] = vaHex(Flow.Target);
          if (!Flow.Refs.empty()) {
            llvm::json::Array Refs;
            for (const auto &[To, Kind] : Flow.Refs)
              Refs.push_back(
                  llvm::json::Object{{"to", vaHex(To)}, {"kind", Kind.str()}});
            Obj["refs"] = std::move(Refs);
          }
        }
        Arr.push_back(std::move(Obj));
        return true;
      });
  if (!ModeKnown)
    S->setError("unknown or conflicting instruction mode at address");

  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

// ===--------------------------------------------------------------------===//
// Direct references (JSON object)
// ===--------------------------------------------------------------------===//

const char *neverd_code_refs_json(neverd_session_t Sess, neverd_va_t FirstEntry,
                                  int MaxFunctions) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return nullptr;
  }
  if (S->Img.Arch == Arch::EVM || S->Img.Arch == Arch::SBF) {
    S->setError("direct references are only published for native images");
    return nullptr;
  }
  if (!S->synchronizeFunctions())
    return nullptr;
  constexpr int MaxFunctionsPerQuery = 4096;
  // A function without a recorded size is decoded only up to its first
  // terminator; this bounds a corrupt or missing extent.
  constexpr uint64_t MaxUnsizedInstructions = 65536;
  std::vector<const FuncInfo *> Ordered;
  Ordered.reserve(S->Functions.size());
  for (const FuncInfo &F : S->Functions)
    if (F.Entry >= FirstEntry)
      Ordered.push_back(&F);
  std::sort(Ordered.begin(), Ordered.end(),
            [](const FuncInfo *Left, const FuncInfo *Right) {
              return Left->Entry < Right->Entry;
            });
  Ordered.erase(std::unique(Ordered.begin(), Ordered.end(),
                            [](const FuncInfo *Left, const FuncInfo *Right) {
                              return Left->Entry == Right->Entry;
                            }),
                Ordered.end());
  const size_t End = std::min(
      Ordered.size(),
      static_cast<size_t>(std::clamp(MaxFunctions, 1, MaxFunctionsPerQuery)));

  // Functions decode independently: each thread owns its decoder and writes
  // only its function's slot, and slots merge in entry order, so the result
  // is identical for any thread count.
  struct Ref {
    va_t From, To;
    llvm::StringRef Kind;
  };
  std::vector<std::vector<Ref>> Slots(End);
  std::atomic<bool> DecoderFailed{false};
  parallelForEach(End, [&](auto Claim, size_t Total) {
    Decoder Local;
    if (!Local.init(S->Img)) {
      DecoderFailed = true;
      return;
    }
    for (size_t Index = Claim(); Index < Total; Index = Claim()) {
      const FuncInfo &F = *Ordered[Index];
      const uint64_t Limit = F.Size > 0 ? F.Size : MaxUnsizedInstructions;
      auto &Out = Slots[Index];
      (void)decodeNativeRange(
          S->Img, Local, F.Entry, F.Size, Limit,
          [&](const DecodedInsn &DI, const uint8_t *, int) {
            const InstructionFlow Flow = summarizeInstructionFlow(Local, DI);
            if (Flow.Target != InvalidVA && !Flow.Kind.empty())
              Out.push_back({DI.Addr, Flow.Target, Flow.Kind});
            for (const auto &[To, Kind] : Flow.Refs)
              Out.push_back({DI.Addr, To, Kind});
            return F.Size > 0 || !Local.isFunctionTerminator(DI);
          });
    }
  });
  if (DecoderFailed) {
    S->setError("failed to initialize a decoder for the image");
    return nullptr;
  }
  llvm::json::Array Refs;
  for (const auto &Slot : Slots)
    for (const Ref &R : Slot)
      Refs.push_back(
          llvm::json::Array{vaHex(R.From), vaHex(R.To), R.Kind.str()});
  llvm::json::Object Result;
  Result["refs"] = std::move(Refs);
  Result["next_entry"] = End < Ordered.size()
                             ? llvm::json::Value(vaHex(Ordered[End]->Entry))
                             : nullptr;
  Result["function_count"] = static_cast<int64_t>(S->Functions.size());
  return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
}

// ===--------------------------------------------------------------------===//
// Disassembly (annotated text)
// ===--------------------------------------------------------------------===//

const char *neverd_disasm_text(neverd_session_t Sess,
                               const char *FuncNameOrAddr, int Annotate) {
  auto *S = static_cast<Session *>(Sess);
  if (!S || !S->Loaded)
    return nullptr;

  if (S->Img.Arch == Arch::EVM) {
    if (!S->ensurePipeline() || !S->PipeResult.EVM)
      return nullptr;
    std::string Buffer;
    llvm::raw_string_ostream OS(Buffer);
    OS << "; " << kEVMEntrySymbolName << " (0x"
       << llvm::utohexstr(evm::kEntryPC) << ", " << S->Img.Raw.size()
       << " bytes)\n";
    for (const auto &Instruction : S->PipeResult.EVM->Low.Instructions) {
      OS << "  0x" << llvm::utohexstr(Instruction.PC) << "  ";
      const std::string Bytes = evmBytes(Instruction);
      OS << Bytes;
      if (Bytes.size() < kShortInstructionByteColumnWidth)
        OS.indent(kShortInstructionByteColumnWidth - Bytes.size());
      OS << " " << Instruction.Info.Name;
      const std::string Immediate = evm::formatImmediate(Instruction);
      if (!Immediate.empty())
        OS << " " << Immediate;
      const std::string Annotation = evm::formatDecodeAnnotation(Instruction);
      if (!Annotation.empty())
        OS << " ; " << Annotation;
      OS << "\n";
    }
    return dupStr(Buffer);
  }

  if (S->Img.Arch == Arch::SBF) {
    if (!S->ensurePipeline() || !S->PipeResult.SBF)
      return nullptr;
    std::string Buffer;
    llvm::raw_string_ostream OS(Buffer);
    const auto &Program = *S->PipeResult.SBF;
    const llvm::StringRef Identifier = FuncNameOrAddr ? FuncNameOrAddr : "";
    const sbf::Function *Function = sbf::findFunction(Program, Identifier);
    if (!Function) {
      S->setError("SBF function not found: " + Identifier.str());
      return nullptr;
    }
    const llvm::BitVector SelectedSlots = functionSlots(Program, *Function);
    OS << "; " << Function->Name << " (0x" << llvm::utohexstr(Function->Address)
       << ", " << SelectedSlots.count() * sbf::kInstructionSize << " bytes, "
       << sbf::versionDisplayName(Program.Low.TheVersion) << ")\n";
    for (const auto &Instruction : Program.Low.Instructions) {
      if (Instruction.IsContinuation || !SelectedSlots.test(Instruction.Slot))
        continue;
      OS << "  0x" << llvm::utohexstr(Instruction.Address) << "  ";
      const std::string Bytes = sbfBytes(Instruction);
      OS << Bytes;
      if (Bytes.size() < kShortInstructionByteColumnWidth)
        OS.indent(kShortInstructionByteColumnWidth - Bytes.size());
      OS << " " << sbf::formatInstruction(Instruction) << "\n";
    }
    return dupStr(Buffer);
  }

  if (!S->synchronizeFunctions())
    return nullptr;

  std::optional<FuncInfo> Function;
  const Symbol *Sym = nullptr;
  std::string FN(FuncNameOrAddr ? FuncNameOrAddr : "");
  if (FN.size() > 2 && (FN.substr(0, 2) == "0x" || FN.substr(0, 2) == "0X")) {
    llvm::StringRef Ref(FN);
    Ref = Ref.drop_front(2);
    va_t Addr = 0;
    if (!Ref.getAsInteger(16, Addr)) {
      for (const auto &F : S->Functions) {
        if (F.Entry == Addr) {
          Function = F;
          break;
        }
      }
      if (!Function)
        Sym = S->Img.findSymbolAt(Addr);
    }
  } else {
    for (const auto &F : S->Functions) {
      if (F.Name == FN) {
        Function = F;
        break;
      }
    }
    if (!Function)
      Sym = S->Img.findSymbol(FN);
  }
  if (!Function) {
    if (!Sym)
      return nullptr;
    Function = FuncInfo{Sym->Addr, Sym->Size, Sym->Name};
  }

  const Segment *Seg = nullptr;
  for (const auto &Sg : S->Img.Segments)
    if (Sg.contains(Function->Entry)) {
      Seg = &Sg;
      break;
    }
  if (!Seg)
    return nullptr;

  if (!S->Dec.init(S->Img))
    return nullptr;

  std::string Buf;
  llvm::raw_string_ostream OS(Buf);
  OS << "; " << Function->Name << " (0x" << llvm::utohexstr(Function->Entry)
     << ", " << Function->Size << " bytes)\n";

  va_t Addr = Function->Entry;
  std::optional<InstructionMode> PathMode = S->Img.instructionModeAt(Addr);
  uint64_t Span = Function->Size > 0 ? Function->Size : 0x100;
  while (Addr >= Function->Entry && Addr - Function->Entry < Span &&
         Seg->contains(Addr)) {
    uint64_t Off64 = Addr - Seg->VA;
    // contains() only checks the VA span (Seg->Size), which can exceed the
    // materialized bytes; guard so Remain does not underflow into a huge
    // length.
    if (Off64 >= Seg->Data.size())
      break;
    size_t Off = static_cast<size_t>(Off64);
    const uint8_t *Bytes = Seg->Data.data() + Off;
    size_t Remain = static_cast<size_t>(std::min<uint64_t>(
        Seg->Size - Off64,
        std::min<uint64_t>(Seg->Data.size() - Off,
                           Span - (Addr - Function->Entry))));
    if (!S->Dec.selectMode(S->Img, Addr, PathMode)) {
      S->setError("unknown or conflicting instruction mode at address");
      break;
    }
    PathMode = S->Dec.currentMode();
    DecodedInsn Insn;
    int Sz = S->Dec.decodeOne(Bytes, Remain, Addr, Insn);
    if (Sz <= 0)
      break;
    if (S->Img.Arch == Arch::ARM &&
        S->Img.instructionModeAt(Addr + Sz - 1, S->Dec.currentMode()) !=
            S->Dec.currentMode())
      break;
    OS << "  0x" << llvm::utohexstr(Addr) << "  ";
    for (int I = 0; I < Sz && I < 8; ++I)
      OS << llvm::format("%02x ", Bytes[I]);
    for (int I = Sz; I < 8; ++I)
      OS << "   ";
    OS << " " << (Insn.Raw ? Insn.Raw->mnemonic : "");
    if (Insn.Raw && Insn.Raw->op_str[0])
      OS << "\t" << Insn.Raw->op_str;

    if (Annotate && Insn.Raw && Insn.Raw->op_str[0]) {
      std::string OpStr(Insn.Raw->op_str);
      auto HexPos = OpStr.find("0x");
      if (HexPos != std::string::npos) {
        size_t HexBegin = HexPos + 2;
        size_t HexEnd = HexBegin;
        while (HexEnd < OpStr.size() &&
               std::isxdigit(static_cast<unsigned char>(OpStr[HexEnd])))
          ++HexEnd;
        va_t OpAddr = 0;
        llvm::StringRef HexDigits(OpStr.data() + HexBegin, HexEnd - HexBegin);
        if (HexDigits.empty() || HexDigits.getAsInteger(16, OpAddr))
          OpAddr = 0;
        if (OpAddr > 0 && OpAddr != Addr) {
          const char *Ref = neverd_resolve_addr(Sess, OpAddr);
          if (Ref) {
            auto Parsed = llvm::json::parse(Ref);
            if (Parsed) {
              if (auto *RObj = Parsed->getAsObject()) {
                auto Type = RObj->getString("type").value_or("");
                if (Type == "function")
                  OS << " ; " << RObj->getString("name").value_or("");
                else if (Type == "import")
                  OS << " ; [import] " << RObj->getString("name").value_or("");
                else if (Type == "export")
                  OS << " ; [export] " << RObj->getString("name").value_or("");
                else if (Type == "string")
                  OS << " ; \"" << RObj->getString("value").value_or("")
                     << "\"";
              }
            }
            neverd_free_string(Ref);
          }
        }
      }
    }
    OS << "\n";
    if (static_cast<uint64_t>(Sz) > InvalidVA - Addr)
      break;
    Addr += Sz;
  }
  return dupStr(Buf);
}
