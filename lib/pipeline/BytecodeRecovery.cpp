//===- BytecodeRecovery.cpp - Shared external-profile source recovery ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/pipeline/BytecodeRecovery.h"

#include "neverd/analysis/BytecodeDecoder.h"
#include "neverd/analysis/BytecodeSource.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/high/HighSourceFlow.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/StringSwitch.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/JSON.h"

#include <map>

using namespace neverd::analysis;

namespace neverd {
namespace {
llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, Message);
}

bool identifier(llvm::StringRef Name) {
  if (Name.empty() || Name.size() > 128 || !llvm::isAlpha(Name.front()) ||
      !llvm::all_of(Name, [](char C) { return llvm::isAlnum(C) || C == '_'; }))
    return false;
  return !llvm::StringSwitch<bool>(Name)
              .Cases(
                  {"auto",         "break",         "case",         "char",
                   "const",        "continue",      "default",      "do",
                   "double",       "else",          "enum",         "extern",
                   "float",        "for",           "goto",         "if",
                   "inline",       "int",           "long",         "register",
                   "restrict",     "return",        "short",        "signed",
                   "sizeof",       "static",        "struct",       "switch",
                   "typedef",      "union",         "unsigned",     "void",
                   "volatile",     "while",         "alignas",      "alignof",
                   "bool",         "constexpr",     "nullptr",      "false",
                   "true",         "static_assert", "thread_local", "typeof",
                   "typeof_unqual"},
                  true)
              .Default(false);
}

} // namespace

llvm::Expected<BytecodeRecoveryResult>
recoverBytecode(llvm::ArrayRef<uint8_t> Code, llvm::StringRef RequestJSON) {
  return recoverBytecode(Code, RequestJSON, {});
}

llvm::Expected<BytecodeRecoveryResult>
recoverBytecode(llvm::ArrayRef<uint8_t> Code, llvm::StringRef RequestJSON,
                BytecodeDecodeCallback Decode) {
  if (Code.size() > BytecodeRecoveryInputLimit ||
      RequestJSON.size() > BytecodeRecoveryInputLimit)
    return invalid("input exceeds the 64 MiB limit");
  auto JSON = llvm::json::parse(RequestJSON);
  if (!JSON)
    return JSON.takeError();
  const auto *Root = JSON->getAsObject();
  if (!Root || Root->getInteger("schemaVersion") != 1)
    return invalid("bytecode recovery requires schemaVersion 1");
  for (const auto &[Key, Value] : *Root)
    if (Key != "schemaVersion" && Key != "profile" && Key != "layout" &&
        Key != "functions" && Key != "bindings" && Key != "base" &&
        Key != "output" && Key != "optimize" && Key != "unaligned_pointers" &&
        Key != "with_context")
      return invalid(llvm::Twine("unknown bytecode recovery key: ") +
                     llvm::StringRef(Key));
  if (Root->get(Decode ? "profile" : "layout"))
    return invalid(
        "static profile and external decoder layout are mutually exclusive");
  const auto *Profile = Root->getObject(Decode ? "layout" : "profile");
  const auto *Ranges = Root->getArray("functions");
  if (!Profile)
    return invalid(Decode
                       ? "layout must be an external decoder layout object"
                       : "profile must be an external decoder profile object");
  if (!Ranges || Ranges->empty() || Ranges->size() > 100000)
    return invalid("functions must be a nonempty bounded JSON array");
  if (Root->get("bindings") && !Root->getArray("bindings"))
    return invalid("bindings must be a bounded JSON array");
  uint64_t Base = 0;
  if (const auto *Value = Root->get("base")) {
    auto Address = Value->getAsUINT64();
    if (!Address)
      return invalid("base must be an unsigned 64-bit integer");
    Base = *Address;
  }
  llvm::StringRef Output = "highc";
  if (Root->get("output")) {
    auto Name = Root->getString("output");
    if (!Name)
      return invalid("output must be check, highc or llvmc");
    Output = *Name;
  }
  if (Output != "check" && Output != "highc" && Output != "llvmc")
    return invalid("output must be check, highc or llvmc");
  for (llvm::StringRef Key : {"optimize", "unaligned_pointers", "with_context"})
    if (Root->get(Key) && !Root->getBoolean(Key))
      return invalid(Key + " must be a boolean");
  bool LLVMRoute = Output == "llvmc", Check = Output == "check";
  bool Optimize = Root->getBoolean("optimize").value_or(false);
  bool UnalignedPointers =
      Root->getBoolean("unaligned_pointers").value_or(false);
  bool WithContext = Root->getBoolean("with_context").value_or(false);
  if (Optimize && !LLVMRoute)
    return invalid("optimize requires llvmc source emission");
  std::string ProfileJSON;
  llvm::raw_string_ostream(ProfileJSON)
      << *Root->get(Decode ? "layout" : "profile");
  auto P = Decode ? readBytecodeLayout(ProfileJSON)
                  : readBytecodeProfile(ProfileJSON);
  if (!P)
    return P.takeError();
  auto Decoder =
      Decode ? BytecodeDecoder::createExternal(std::move(*P), std::move(Decode))
             : BytecodeDecoder::create(std::move(*P));
  if (!Decoder)
    return Decoder.takeError();
  std::vector<LowFunc> Functions;
  std::map<va_t, SourceFunctionTypeHint> Hints;
  std::vector<MedFunc> MedFunctions;
  std::set<std::string> Names;
  std::map<uint64_t, uint64_t> Intervals;
  uint64_t Bytes = 0;
  uint64_t Blocks = 0;
  uint64_t RemainingOperations = 10000000;
  uint32_t RemainingInstructions = 1000000;
  for (const auto &Item : *Ranges) {
    const auto *O = Item.getAsObject();
    if (!O || O->size() != 3)
      return invalid("each function needs exactly entry, end and name");
    auto Entry = O->getInteger("entry");
    auto End = O->getInteger("end");
    auto Name = O->getString("name");
    if (!Entry || *Entry < 0 || !End || *End <= *Entry || !Name ||
        !identifier(*Name) || !Names.insert(Name->str()).second)
      return invalid("invalid or duplicate function declaration");
    auto Next = Intervals.lower_bound(*Entry);
    if ((Next != Intervals.end() && uint64_t(*End) > Next->first) ||
        (Next != Intervals.begin() &&
         std::prev(Next)->second > uint64_t(*Entry)))
      return invalid("overlapping function declarations");
    Intervals.emplace(*Entry, *End);
    BytecodeDecodeLimits Limits;
    Limits.MaxInstructions =
        std::min(Limits.MaxInstructions, RemainingInstructions);
    Limits.MaxOperations = std::min(Limits.MaxOperations, RemainingOperations);
    auto F = (*Decoder)->function(Code, Base, *Entry, *End, *Name, Limits);
    if (!F)
      return invalid(*Name + ": " + llvm::toString(F.takeError()));
    Bytes += F->DecodedBytes;
    Blocks += F->Function.Blocks.size();
    RemainingInstructions -= F->Function.DecodedInstructionCount;
    for (const auto &Block : F->Function.Blocks)
      RemainingOperations -= Block.Ops.size();
    Functions.push_back(std::move(F->Function));
  }
  std::map<va_t, std::string> CallNames;
  std::vector<va_t> BoundCalls;
  for (const auto &F : Functions) {
    CallNames.emplace(F.Entry, F.Name);
    BoundCalls.push_back(F.Entry);
  }
  std::vector<va_t> ExternalCalls;
  if (const auto *Bindings = Root->getArray("bindings")) {
    if (Bindings->size() > 65536)
      return invalid("bindings must be a bounded JSON array");
    for (const auto &Item : *Bindings) {
      const auto *O = Item.getAsObject();
      if (!O || O->size() != 2)
        return invalid("each binding needs exactly address and name");
      auto Address = O->getInteger("address");
      auto Name = O->getString("name");
      if (!Address || *Address < 0 || !Name || !identifier(*Name) ||
          !Names.insert(Name->str()).second ||
          !CallNames.emplace(*Address, Name->str()).second)
        return invalid("invalid or duplicate external call binding");
      BoundCalls.push_back(*Address);
      ExternalCalls.push_back(*Address);
    }
  }
  BytecodeRecoveryResult Result;
  Result.Functions = Functions.size();
  Result.Blocks = Blocks;
  Result.DecodedBytes = Bytes;
  Result.DecodedInstructions = 1000000 - RemainingInstructions;
  Result.CheckedOnly = Check;
  Result.WithContext = WithContext;
  if (Check)
    return Result;
  for (auto &F : Functions) {
    auto Bound =
        lowerBytecodeState(F, (*Decoder)->profile().RegisterBytes,
                           Arch::AArch64, 10000000, BoundCalls, WithContext);
    if (!Bound)
      return invalid(F.Name + ": " + llvm::toString(Bound.takeError()));
    Hints.emplace(F.Entry, Bound->SourceABI);
    F = std::move(Bound->Function);
  }
  for (va_t Address : ExternalCalls)
    Hints.emplace(Address, Hints.begin()->second);
  for (const auto &F : Functions) {
    LowToMedConverter Converter;
    Converter.setSourceCallHintsEnabled(true);
    Converter.setSourceEntryTypeHints(&Hints);
    Converter.setSourceCalleeTypeHints(&Hints);
    auto Med = Converter.convert(F, Arch::AArch64);
    Med.SourceTypeHint = Hints.at(F.Entry);
    inferMedTypes(Med, Arch::AArch64);
    for (auto &Block : Med.Blocks)
      for (auto &Op : Block.Ops)
        if (Op.SourceCallHint &&
            CallNames.count(Op.SourceCallHint->TargetAddress)) {
          auto Hint = std::make_shared<SourceCallTypeHint>(*Op.SourceCallHint);
          Hint->TargetName = CallNames.at(Hint->TargetAddress);
          Op.SourceCallHint = std::move(Hint);
        }
    recoverCallAbi(Med, Arch::AArch64, CallNames);
    if (Med.SkippedSSA || !verifyMedFunc(Med, "bytecode-source") ||
        Med.Params.size() != (WithContext ? 2u : 1u))
      return invalid(F.Name + ": source IR validation failed");
    MedFunctions.push_back(std::move(Med));
  }
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  Options.Format = BinaryFormat::ELF;
  Options.PreserveLLVMFunctionTypes = true;
  Options.UseUnalignedPointers = UnalignedPointers;
  llvm::raw_string_ostream OS(Result.Source);
  if (LLVMRoute) {
    llvm::LLVMContext Context;
    std::vector<char> Bodies(MedFunctions.size(), 1);
    for (va_t Address : ExternalCalls) {
      // Declare the exact same state ABI as internal functions. The existing
      // body mask keeps imports as declarations, with no invented stub body.
      MedFunc Declaration;
      Declaration.Entry = Address;
      Declaration.Name = CallNames.at(Address);
      Declaration.Params = MedFunctions.front().Params;
      Declaration.TypedParams = MedFunctions.front().TypedParams;
      Declaration.ReturnType = Hints.at(Address).ReturnType;
      MedFunctions.push_back(std::move(Declaration));
      Bodies.push_back(0);
    }
    auto Module =
        MedLLVMEmitter().emit(MedFunctions, Context, "bytecode", Arch::AArch64,
                              {}, nullptr, BinaryFormat::ELF, false, &Bodies);
    if (!Module || llvm::verifyModule(*Module, &llvm::errs()))
      return invalid("LLVM verification failed");
    if (Optimize) {
      Pipeline::OptimizationOptions Policy;
      // Source recovery benefits from scalar simplification while retaining
      // function boundaries. Use the shared thin policy rather than a second
      // pass list or module-wide inlining/speculative loop transforms.
      Policy.Strength = Pipeline::OptStrength::Thin;
      auto Result = Pipeline::optimizeModule(*Module, Policy);
      if (Result.Stop == OptimizationStopReason::InputInvalid ||
          Result.Stop == OptimizationStopReason::VerificationFailed)
        return invalid(llvm::Twine("LLVM source optimization failed: ") +
                       optimizationStopReasonName(Result.Stop));
    } else {
      Pipeline::promoteScaffoldingAllocas(*Module);
    }
    if (llvm::verifyModule(*Module, &llvm::errs()))
      return invalid("LLVM source transformation failed verification");
    if (!LLVMCEmitter().emit(*Module, OS, Options))
      return invalid("LLVM C emission failed");
  } else {
    std::vector<HighFunc> HighFunctions;
    MedToHighConverter Converter;
    Converter.setFuncNames(&CallNames);
    for (const auto &Med : MedFunctions) {
      auto High = Converter.convert(Med, Arch::AArch64);
      auto Flow = analyzeHighSourceFlow(High, true);
      if (!Flow.Complete)
        return invalid(
            Med.Name + ": incomplete source flow" +
            (Flow.Items.empty() ? "" : ": " + Flow.Items.front().Reason));
      HighFunctions.push_back(std::move(High));
    }
    if (!HighCEmitter().emit(HighFunctions, OS, Options))
      return invalid("structured C emission failed");
  }
  OS.flush();
  return Result;
}
} // namespace neverd
