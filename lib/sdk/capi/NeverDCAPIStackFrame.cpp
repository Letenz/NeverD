#include "../../backend/c/LLVMC/LLVMCFrameLayout.h"
#include "../../pipeline/PipelineLLVMFrame.h"
#include "SessionImpl.h"
#include "SourceEdits.h"

#include "neverd/ir/SourceStackFrame.h"

#include "llvm/ADT/StringExtras.h"

#include <cctype>

using namespace neverd;
using namespace neverd::sdk;

void neverd::sdk::preserveAnalystStackObjects(llvm::Module &M) {
  Pipeline::promoteScaffoldingAllocas(M);
  canonicalizePrivateFrameAccesses(M);
  for (auto &F : M) {
    bool HasFrame = false;
    for (auto &B : F)
      for (auto &I : B)
        if (auto *A = llvm::dyn_cast<llvm::AllocaInst>(&I))
          if (auto Base =
                  llvmc::syntheticFrameBaseOffset(*A, M.getDataLayout())) {
            HasFrame = true;
            A->setMetadata(
                llvmc::CapturedFrameBase,
                llvm::MDNode::get(
                    M.getContext(),
                    llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                        llvm::Type::getInt64Ty(M.getContext()), *Base))));
          }
    // A user named the physical storage, so scalar replacement must retain
    // it. This optimization barrier belongs only to the private C projection;
    // it adds no machine instruction, memory effect or native ABI authority.
    if (HasFrame) {
      F.removeFnAttr(llvm::Attribute::AlwaysInline);
      F.addFnAttr(llvm::Attribute::NoInline);
      F.addFnAttr(llvm::Attribute::OptimizeNone);
    }
  }
}

std::string neverd::sdk::sourceStackSnapshot(const LowFunc &F) {
  std::string Text = sourceCallSnapshot(F);
  if (Text.empty())
    return {};
  llvm::raw_string_ostream OS(Text);
  OS << ":stack-v1:";
  for (auto Root : F.ModuleAnalysisRoots)
    OS << 'R' << Root;
  for (auto Root : F.OrdinaryModuleAnalysisRoots)
    OS << 'O' << Root;
  for (const auto &T : F.FunctionTemporaries)
    OS << 'T' << T.Offset << ':' << T.Bytes;
  for (const auto &B : F.Blocks) {
    for (const auto &E : B.ExceptionalSuccs)
      OS << 'E' << E.BlockId << ':' << E.TargetVA << ':' << unsigned(E.Kind)
         << ':' << E.SourceVA << ':' << E.RegionIndex << ':' << E.State;
    for (const auto &I : B.InstructionBoundaries)
      OS << 'I' << I.Address << ':' << I.Size << ':' << unsigned(I.Mode) << ':'
         << unsigned(I.Control) << ':' << I.FirstOp << ':' << I.OpCount;
  }
  return sourceVariableSnapshot(Text);
}

SourceStackFrame
neverd::sdk::readSourceStackFrame(const llvm::json::Object &O) {
  SourceStackFrame F;
  F.Snapshot = O.getString("snapshot").value_or("").str();
  if (const auto *Points = O.getArray("points")) {
    if (Points->size() > 256)
      throw std::invalid_argument("Too many SP corrections");
    for (const auto &P : *Points) {
      const auto *Row = P.getAsObject();
      const auto PC = Row ? Row->getString("address") : std::nullopt;
      const auto After = Row ? Row->getInteger("after") : std::nullopt;
      uint64_t Address;
      if (!PC || PC->getAsInteger(0, Address) || !After ||
          !F.Points.emplace(Address, *After).second)
        throw std::invalid_argument("Invalid or repeated SP correction");
    }
  } else if (O.get("points"))
    throw std::invalid_argument("points must be an array");
  if (const auto *Variables = O.getArray("variables")) {
    if (Variables->size() > 256)
      throw std::invalid_argument("Too many stack variables");
    for (const auto &V : *Variables) {
      const auto *Row = V.getAsObject();
      const auto At = Row ? Row->getInteger("offset") : std::nullopt;
      const auto Bytes = Row ? Row->getInteger("bytes") : std::nullopt;
      const auto Name = Row ? Row->getString("name") : std::nullopt;
      if (!At || !Bytes || *Bytes < 1 || *Bytes > 16 || !Name ||
          Name->empty() || Name->size() > 128)
        throw std::invalid_argument("Invalid stack variable");
      F.Variables.push_back({*At, static_cast<uint16_t>(*Bytes), Name->str(),
                             Row->getString("type").value_or("").str()});
    }
  } else if (O.get("variables"))
    throw std::invalid_argument("variables must be an array");
  if (!F.empty() && (F.Snapshot.empty() || F.Snapshot.size() > 128))
    throw std::invalid_argument(
        "A stack edit requires an original graph snapshot");
  return F;
}

void neverd::sdk::validateSourceStackFrame(Session &S, const LowFunc &F,
                                           const SourceStackFrame &Policy) {
  if (!Policy.empty() && Policy.Snapshot != sourceStackSnapshot(F))
    throw std::invalid_argument(
        "The stack analysis changed; reset or reapply the correction");
  const auto Flow = analyzeSourceStackFrame(F, S.Img.Arch, Policy);
  if (!Flow.Error.empty())
    throw std::invalid_argument(Flow.Error);
  std::string Error;
  if (!projectSourceStackFrame(F, S.Img.Arch, Policy, Error))
    throw std::invalid_argument(Error);
  std::set<std::string> Names;
  std::vector<std::pair<int64_t, int64_t>> Ranges;
  for (const auto &V : Policy.Variables) {
    if (V.Offset < -(1 << 20) || V.Offset >= 0 || V.Offset + V.Bytes > 0 ||
        !(std::isalpha(static_cast<unsigned char>(V.Name.front())) ||
          V.Name.front() == '_') ||
        !std::all_of(
            V.Name.begin(), V.Name.end(),
            [](unsigned char C) { return std::isalnum(C) || C == '_'; }) ||
        !Names.insert(V.Name).second)
      throw std::invalid_argument(
          "Use distinct C identifiers for bounded local stack slots");
    // Let the C parser reject keywords, reserved declarations and invalid
    // types.
    const auto *Library = S.readLocalTypes();
    if (!Library)
      throw std::invalid_argument(S.LastError);
    auto Decl = types::parseFunctionDeclaration(
        "void __frame(" + (V.Type.empty() ? "uint64_t" : V.Type) + " " +
            V.Name + ")",
        *Library);
    if (!Decl.ok())
      throw std::invalid_argument("Invalid C stack variable name or type");
    bool Observed = false;
    for (const auto &Access : Flow.Accesses) {
      if (Access.Offset == V.Offset && Access.Bytes == V.Bytes)
        Observed = true;
      else if (Access.Offset < V.Offset + V.Bytes &&
               V.Offset < Access.Offset + Access.Bytes)
        throw std::invalid_argument("Overlapping stack accesses must retain "
                                    "their shared backing storage");
    }
    if (!Observed)
      throw std::invalid_argument("Name an observed stack access of the same "
                                  "width; repair unknown SP first");
    for (const auto &[Begin, End] : Ranges)
      if (Begin < V.Offset + V.Bytes && V.Offset < End)
        throw std::invalid_argument("Manual stack variables overlap");
    Ranges.emplace_back(V.Offset, V.Offset + V.Bytes);
    if (!V.Type.empty()) {
      auto Type = parseSourceVariableType(S, V.Type, V.Bytes, Error);
      if (!Type || Type->Kind == NdTypeKind::Float)
        throw std::invalid_argument(
            Error.empty()
                ? "Stack bindings require an exact-width integer or pointer"
                : Error);
    }
  }
}

const char *neverd_stack_frame_json(neverd_session_t Sess, neverd_va_t Entry,
                                    const char *OptionsJSON) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  try {
    llvm::StringRef Text = OptionsJSON ? OptionsJSON : "{}";
    if (Text.size() > 128 * 1024)
      throw std::invalid_argument("Stack policy exceeds 128 KiB");
    auto Parsed = llvm::json::parse(Text);
    if (!Parsed) {
      llvm::consumeError(Parsed.takeError());
      throw std::invalid_argument("Invalid stack policy JSON");
    }
    const auto *O = Parsed->getAsObject();
    if (!O)
      throw std::invalid_argument("Stack policy must be an object");
    const bool Prepared = neverd_prepare_function_view(Sess, Entry, "low") != 0;
    if (!Prepared && !S->findLowFunc(Entry))
      return nullptr;
    S->clearError();
    const auto *F = S->findLowFunc(Entry);
    if (!F)
      throw std::invalid_argument("Original LowIR is unavailable");
    const auto Policy = readSourceStackFrame(*O);
    validateSourceStackFrame(*S, *F, Policy);
    const auto Flow = analyzeSourceStackFrame(*F, S->Img.Arch, Policy);
    llvm::json::Array Instructions, Accesses;
    for (const auto &[PC, I] : Flow.Instructions) {
      llvm::json::Object Row{{"address", vaHex(PC)},
                             {"correctable", I.Correctable}};
      Row["before"] =
          I.Before ? llvm::json::Value(*I.Before) : llvm::json::Value(nullptr);
      Row["after"] =
          I.After ? llvm::json::Value(*I.After) : llvm::json::Value(nullptr);
      Instructions.push_back(std::move(Row));
    }
    for (const auto &A : Flow.Accesses)
      Accesses.push_back(llvm::json::Object{{"address", vaHex(A.Instruction)},
                                            {"sequence", A.Sequence},
                                            {"offset", A.Offset},
                                            {"bytes", A.Bytes},
                                            {"write", A.Write}});
    llvm::json::Value Result =
        llvm::json::Object{{"snapshot", sourceStackSnapshot(*F)},
                           {"instructions", std::move(Instructions)},
                           {"accesses", std::move(Accesses)},
                           {"valid", true}};
    std::string Out;
    llvm::raw_string_ostream OS(Out);
    OS << Result;
    return dupStr(Out);
  } catch (const std::exception &E) {
    S->setError(E.what());
    return nullptr;
  }
}
