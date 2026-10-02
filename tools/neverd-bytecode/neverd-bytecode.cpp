//===- neverd-bytecode.cpp - External-profile source recovery -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

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
#include "neverd/support/AtomicOutput.h"

#include "llvm/ADT/StringSwitch.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

#include <map>

using namespace neverd;
using namespace neverd::analysis;

namespace {
llvm::cl::opt<std::string> Input(llvm::cl::Positional, llvm::cl::Required,
                                 llvm::cl::desc("<raw bytecode>"));
llvm::cl::opt<std::string>
    ProfilePath("profile", llvm::cl::Required,
                llvm::cl::desc("External decoder profile JSON"));
llvm::cl::opt<std::string>
    FunctionsPath("functions", llvm::cl::Required,
                  llvm::cl::desc("Function ranges JSON"));
llvm::cl::opt<std::string>
    BindingsPath("bindings", llvm::cl::init(""),
                 llvm::cl::desc("External state-call declarations JSON"));
llvm::cl::opt<std::string> Output("o", llvm::cl::init("-"),
                                  llvm::cl::desc("C output path"));
llvm::cl::opt<uint64_t>
    Base("base", llvm::cl::init(0),
         llvm::cl::desc("Logical address of the first byte"));
llvm::cl::opt<bool> LLVMRoute("llvm",
                              llvm::cl::desc("Use the LLVM-to-C route"));
llvm::cl::opt<bool> Optimize(
    "optimize",
    llvm::cl::desc("Optimize LLVM IR before C emission (requires --llvm)"));
llvm::cl::opt<bool> UnalignedPointers(
    "unaligned-pointers",
    llvm::cl::desc("Use Clang/GCC unaligned aliasing scalar pointers in C"));
llvm::cl::opt<bool>
    Check("check", llvm::cl::desc("Check CFG decoding without emitting C"));

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

llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>> read(llvm::StringRef Path) {
  auto Buffer = llvm::MemoryBuffer::getFile(Path);
  if (!Buffer)
    return llvm::errorCodeToError(Buffer.getError());
  if ((*Buffer)->getBufferSize() > 64 * 1024 * 1024)
    return invalid("input exceeds the 64 MiB limit");
  return std::move(*Buffer);
}

llvm::Error write(llvm::StringRef Text) {
  if (Output == "-") {
    llvm::outs() << Text;
    return llvm::Error::success();
  }
  auto File = llvm::sys::fs::TempFile::create(Output + ".%%%%%%.tmp");
  if (!File)
    return File.takeError();
  llvm::raw_fd_ostream Stream(File->FD, false);
  Stream << Text;
  Stream.flush();
  if (Stream.has_error()) {
    auto Error = llvm::errorCodeToError(Stream.error());
    Stream.clear_error();
    return llvm::joinErrors(
        std::move(Error),
        support::atomic_output::discardTemporaryOutput(*File));
  }
  return support::atomic_output::closeAndCommitTemporaryOutput(*File, Output);
}

llvm::Error run() {
  if (Optimize && (!LLVMRoute || Check))
    return invalid("--optimize requires --llvm source emission");
  auto ProfileBuffer = read(ProfilePath);
  if (!ProfileBuffer)
    return ProfileBuffer.takeError();
  auto P = readBytecodeProfile((*ProfileBuffer)->getBuffer());
  if (!P)
    return P.takeError();
  auto Decoder = BytecodeDecoder::create(std::move(*P));
  if (!Decoder)
    return Decoder.takeError();
  auto CodeBuffer = read(Input);
  if (!CodeBuffer)
    return CodeBuffer.takeError();
  auto Text = (*CodeBuffer)->getBuffer();
  llvm::ArrayRef<uint8_t> Code(reinterpret_cast<const uint8_t *>(Text.data()),
                               Text.size());
  auto FunctionsBuffer = read(FunctionsPath);
  if (!FunctionsBuffer)
    return FunctionsBuffer.takeError();
  auto Parsed = llvm::json::parse((*FunctionsBuffer)->getBuffer());
  if (!Parsed)
    return Parsed.takeError();
  const auto *Ranges = Parsed->getAsArray();
  if (!Ranges || Ranges->empty() || Ranges->size() > 100000)
    return invalid("functions must be a nonempty bounded JSON array");
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
  llvm::errs() << "Decoded " << Functions.size() << " functions, " << Blocks
               << " blocks, " << Bytes << " reachable bytes of " << Code.size()
               << " input bytes\n";
  if (Check)
    return llvm::Error::success();
  std::map<va_t, std::string> CallNames;
  std::vector<va_t> BoundCalls;
  for (const auto &F : Functions) {
    CallNames.emplace(F.Entry, F.Name);
    BoundCalls.push_back(F.Entry);
  }
  std::vector<va_t> ExternalCalls;
  if (!BindingsPath.empty()) {
    auto Buffer = read(BindingsPath);
    if (!Buffer)
      return Buffer.takeError();
    auto JSON = llvm::json::parse((*Buffer)->getBuffer());
    if (!JSON)
      return JSON.takeError();
    const auto *Bindings = JSON->getAsArray();
    if (!Bindings || Bindings->size() > 65536)
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
  for (auto &F : Functions) {
    auto Bound = lowerBytecodeState(F, (*Decoder)->profile().RegisterBytes,
                                    Arch::AArch64, 10000000, BoundCalls);
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
        Med.Params.size() != 1)
      return invalid(F.Name + ": source IR validation failed");
    MedFunctions.push_back(std::move(Med));
  }
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  Options.Format = BinaryFormat::ELF;
  Options.PreserveLLVMFunctionTypes = true;
  Options.UseUnalignedPointers = UnalignedPointers;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
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
  return write(Source);
}
} // namespace

int main(int Argc, char **Argv) {
  llvm::InitLLVM Init(Argc, Argv);
  llvm::cl::ParseCommandLineOptions(
      Argc, Argv, "NeverD external-profile bytecode source recovery\n");
  try {
    if (auto Error = run()) {
      llvm::errs() << llvm::toString(std::move(Error)) << '\n';
      return 1;
    }
  } catch (const std::exception &Error) {
    llvm::errs() << "bytecode source conversion failed: " << Error.what()
                 << '\n';
    return 1;
  }
  return 0;
}
