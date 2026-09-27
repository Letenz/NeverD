#ifndef NEVERD_SDK_CAPI_OBJCSYNCHRONIZEDSOURCE_H
#define NEVERD_SDK_CAPI_OBJCSYNCHRONIZEDSOURCE_H

#include "neverd/decode/Decoder.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Endian.h"

#include <capstone/arm64.h>
#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd::sdk {

struct ObjCSynchronizedSourceProof {
  va_t EnterCall = 0;
  va_t GuardStopCall = 0;
  va_t ExitCall = 0;
  va_t LandingPad = 0;
  va_t ResumeTarget = 0;
  bool GuardStopsAtRelease = false;
};

struct ObjCSynchronizedSourceRegion {
  va_t Begin = 0;
  va_t End = 0;
  va_t Landing = 0;
};

inline std::optional<ObjCSynchronizedSourceRegion>
objcSynchronizedSourceRegion(const BinaryImage &Image,
                             const HighFunc &Function) {
  if (Image.Arch != Arch::AArch64 || Image.Format != BinaryFormat::MachO ||
      Function.StructuredExceptionRegions ||
      Function.UnstructuredExceptionRegions || Function.Params.empty() ||
      Function.Params[0].Name != "objc_self" || !Function.ExceptionMetadata)
    return std::nullopt;
  const auto &EH = *Function.ExceptionMetadata;
  if (EH.CodeRange.Begin != Function.Entry || !EH.Itanium || !EH.ObjC ||
      EH.ParseStatus != ExceptionParseStatus::Complete ||
      EH.Personality != ExceptionPersonality::ObjCPersonalityV0 ||
      EH.Itanium->Actions.size() || EH.Itanium->TypeTable.size() ||
      EH.Itanium->ExceptionSpecs.size() || !EH.Itanium->IsCallSiteAddressForm ||
      EH.Itanium->CallSites.size() != 3 || EH.ObjC->LandingPads.size() != 1)
    return std::nullopt;
  const auto &Sites = EH.Itanium->CallSites;
  const auto &Protected = Sites[1];
  const auto &Pad = EH.ObjC->LandingPads.front();
  if (Sites[0].LandingPadVA || Sites[2].LandingPadVA ||
      !Protected.LandingPadVA || Protected.FirstActionOffset ||
      Pad.PadVA != Protected.LandingPadVA ||
      Pad.Kind != ObjCPadKind::SynchronizedExit ||
      Pad.GuardedRange.Begin != Protected.GuardedRange.Begin ||
      Pad.GuardedRange.End != Protected.GuardedRange.End ||
      Sites[0].GuardedRange.Begin != Function.Entry ||
      Sites[0].GuardedRange.End != Protected.GuardedRange.Begin ||
      Sites[2].GuardedRange.Begin != Protected.GuardedRange.End ||
      Sites[2].GuardedRange.End != EH.CodeRange.End ||
      Protected.GuardedRange.Begin < Function.Entry ||
      Protected.GuardedRange.End < Protected.GuardedRange.Begin ||
      Protected.GuardedRange.End > Protected.LandingPadVA ||
      Protected.LandingPadVA > InvalidVA - 20 ||
      EH.CodeRange.End != Protected.LandingPadVA + 20)
    return std::nullopt;
  return ObjCSynchronizedSourceRegion{Protected.GuardedRange.Begin,
                                      Protected.GuardedRange.End,
                                      Protected.LandingPadVA};
}

inline std::optional<uint32_t> objcSynchronizedWord(const BinaryImage &Image,
                                                    va_t Address) {
  const uint8_t *Bytes = Image.readVA(Address, 4);
  if (!Bytes)
    return std::nullopt;
  return llvm::support::endian::read32le(Bytes);
}

inline va_t objcSynchronizedBranchTarget(const BinaryImage &Image,
                                         va_t Address) {
  const auto Instruction = objcSynchronizedWord(Image, Address);
  if (!Instruction || (*Instruction & 0xfc000000U) != 0x94000000U)
    return 0;
  const int64_t Displacement =
      static_cast<int64_t>(static_cast<int32_t>(*Instruction << 6) >> 6) * 4;
  if (Displacement >= 0)
    return Address <= InvalidVA - static_cast<uint64_t>(Displacement)
               ? Address + static_cast<uint64_t>(Displacement)
               : 0;
  return Address >= static_cast<uint64_t>(-Displacement)
             ? Address - static_cast<uint64_t>(-Displacement)
             : 0;
}

inline bool objcSynchronizedCallIs(const BinaryImage &Image, va_t Address,
                                   llvm::StringRef Name) {
  const va_t Target = objcSynchronizedBranchTarget(Image, Address);
  return Target && llvm::StringRef(Image.getFunctionNameAt(Target)) == Name;
}

// A narrowly recognized clang @synchronized cleanup: the Itanium call-site
// table names one unconditional pad, and that pad does nothing except unlock
// the saved receiver and resume the original exception. Its source equivalent
// is a C cleanup variable compiled with -fexceptions. In particular, do not
// accept a pad which also releases an ARC object or executes a finally body.
inline std::optional<ObjCSynchronizedSourceProof>
proveObjCSynchronizedStackReceiverCleanup(const BinaryImage &Image,
                                          const HighFunc &Function) {
  const auto Region = objcSynchronizedSourceRegion(Image, Function);
  if (!Region)
    return std::nullopt;

  const auto Word = [&](va_t Address) {
    return objcSynchronizedWord(Image, Address);
  };
  const auto HasCall = [&](va_t Address, llvm::StringRef Name) {
    return objcSynchronizedCallIs(Image, Address, Name);
  };
  const va_t Landing = Region->Landing;
  if (Word(Landing) != 0xaa0003f3U || Word(Landing + 4) != 0xf94007e0U ||
      !HasCall(Landing + 8, "_objc_sync_exit") ||
      Word(Landing + 12) != 0xaa1303e0U ||
      !HasCall(Landing + 16, "__Unwind_Resume"))
    return std::nullopt;

  // The source guard uses objc_self. Prove the original enter, normal exit,
  // and exceptional exit all load that same saved receiver, and that no
  // later instruction stores a new value into its stack slot.
  if (Word(Function.Entry + 16) != 0xf90007e0U ||
      Word(Function.Entry + 24) != 0xf94007e0U)
    return std::nullopt;
  const va_t EnterCall = Function.Entry + 28;
  if (!HasCall(EnterCall, "_objc_sync_enter") || Region->Begin != EnterCall + 4)
    return std::nullopt;
  // The one call after the LSDA range is dispatch_group_enter. Clear the C
  // cleanup guard before it: the original unwind table does not cover it.
  const va_t Unprotected = Region->End;
  const va_t DispatchCall = Unprotected + 8;
  const va_t ExitCall = Unprotected + 16;
  if (Unprotected > InvalidVA - 40 || Landing != Unprotected + 40 ||
      Word(Unprotected) != 0xf94007e0U ||
      Word(Unprotected + 4) != 0xf9400400U ||
      !HasCall(DispatchCall, "_dispatch_group_enter") ||
      Word(Unprotected + 12) != 0xf94007e0U ||
      !HasCall(ExitCall, "_objc_sync_exit"))
    return std::nullopt;
  for (va_t Address = Function.Entry + 20; Address < Landing; Address += 4)
    if (Word(Address) == 0xf90007e0U)
      return std::nullopt;
  return ObjCSynchronizedSourceProof{
      EnterCall, DispatchCall, ExitCall, Landing,
      objcSynchronizedBranchTarget(Image, Landing + 16)};
}

// A second clang shape keeps objc_self in callee-saved x19. Its protected
// body is straight-line and the first instruction after the LSDA range loads
// that same receiver for the normal unlock. There can be no intervening call
// which the C cleanup would incorrectly cover.
inline std::optional<ObjCSynchronizedSourceProof>
proveObjCSynchronizedRegisterReceiverCleanup(const BinaryImage &Image,
                                             const HighFunc &Function) {
  const auto Region = objcSynchronizedSourceRegion(Image, Function);
  if (!Region || Function.Entry > InvalidVA - 8 ||
      Region->Begin < Function.Entry + 8 || Region->Landing < 20 ||
      Region->End > Region->Landing - 20 || Region->Begin > Region->End)
    return std::nullopt;
  const auto Word = [&](va_t Address) {
    return objcSynchronizedWord(Image, Address);
  };
  const auto HasCall = [&](va_t Address, llvm::StringRef Name) {
    return objcSynchronizedCallIs(Image, Address, Name);
  };
  const va_t EnterCall = Region->Begin - 4;
  const va_t Landing = Region->Landing;
  const bool ExitImmediately =
      Word(Region->End) == 0xaa1303e0U &&
      HasCall(Region->End + 4, "_objc_sync_exit");
  const bool ReleaseBeforeExit =
      Word(Region->End) == 0xaa0003f4U &&
      Word(Region->End + 4) == 0xaa1503e0U &&
      HasCall(Region->End + 8, "_objc_release") &&
      Word(Region->End + 12) == 0xaa1303e0U &&
      HasCall(Region->End + 16, "_objc_sync_exit");
  const va_t ExitCall = Region->End + (ReleaseBeforeExit ? 16 : 4);
  if (Word(EnterCall - 4) != 0xaa1303e0U ||
      !HasCall(EnterCall, "_objc_sync_enter") ||
      (!ExitImmediately && !ReleaseBeforeExit) ||
      Word(Landing) != 0xaa0003f4U ||
      Word(Landing + 4) != 0xaa1303e0U ||
      !HasCall(Landing + 8, "_objc_sync_exit") ||
      Word(Landing + 12) != 0xaa1403e0U ||
      !HasCall(Landing + 16, "__Unwind_Resume"))
    return std::nullopt;

  va_t SavedSelf = 0;
  for (va_t Address = Function.Entry; Address + 4 < EnterCall; Address += 4) {
    if (Word(Address) != 0xaa0003f3U)
      continue;
    if (SavedSelf || Address > Function.Entry + 32)
      return std::nullopt;
    SavedSelf = Address;
  }
  if (!SavedSelf)
    return std::nullopt;

  Decoder Decoder;
  if (!Decoder.init(Arch::AArch64))
    return std::nullopt;
  for (va_t Address = Function.Entry; Address < ExitCall; Address += 4) {
    const uint8_t *Bytes = Image.readVA(Address, 4);
    DecodedInsn Instruction{};
    if (!Bytes || Decoder.decodeOne(Bytes, 4, Address, Instruction) != 4 ||
        !Instruction.Raw)
      return std::nullopt;
    cs_regs Reads{}, Writes{};
    uint8_t ReadCount = 0, WriteCount = 0;
    if (cs_regs_access(Decoder.getHandle(), Instruction.Raw, Reads, &ReadCount,
                       Writes, &WriteCount) != CS_ERR_OK)
      return std::nullopt;
    for (uint8_t I = 0; I < WriteCount; ++I) {
      if (Address < SavedSelf &&
          (Writes[I] == ARM64_REG_X0 || Writes[I] == ARM64_REG_W0))
        return std::nullopt;
      if (Address > SavedSelf &&
          (Writes[I] == ARM64_REG_X19 || Writes[I] == ARM64_REG_W19))
        return std::nullopt;
    }
    // The lock and normal unlock must be reached in one straight-line path.
    if (Address < EnterCall &&
        (cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_JUMP) ||
         cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_RET) ||
         (Address < SavedSelf &&
          cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_CALL))))
      return std::nullopt;
    if (Address > EnterCall &&
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_JUMP))
      return std::nullopt;
    if (ReleaseBeforeExit && Address > EnterCall &&
        Address < Region->End &&
        HasCall(Address, "_objc_release"))
      return std::nullopt;
  }
  return ObjCSynchronizedSourceProof{
      EnterCall, ReleaseBeforeExit ? Region->End + 8 : ExitCall, ExitCall,
      Landing, objcSynchronizedBranchTarget(Image, Landing + 16),
      ReleaseBeforeExit};
}

inline std::optional<ObjCSynchronizedSourceProof>
proveObjCSynchronizedReceiverCleanup(const BinaryImage &Image,
                                     const HighFunc &Function) {
  if (auto Proof = proveObjCSynchronizedStackReceiverCleanup(Image, Function))
    return Proof;
  return proveObjCSynchronizedRegisterReceiverCleanup(Image, Function);
}

// The proved pad is an exceptional entry, not a normal source path. A full
// image analysis can retain it as an unreachable suffix after the ordinary
// return. Remove that suffix only when its label and statement addresses
// match the five proved machine instructions, with no source edge into it.
// The cleanup variable below supplies the same exceptional unlock.
inline bool omitProvenObjCSynchronizedLandingPad(
    HighFunc &Function, const ObjCSynchronizedSourceProof &Proof) {
  if (!Proof.LandingPad || !Proof.ResumeTarget ||
      Proof.LandingPad > InvalidVA - 20 ||
      Function.Body.empty())
    return false;
  const va_t Landing = Proof.LandingPad;
  const auto InPad = [&](va_t Address) {
    return Address >= Landing && Address < Landing + 20;
  };
  std::vector<std::pair<const HighStmt *, bool>> Pending;
  for (const auto &Statement : Function.Body)
    Pending.emplace_back(&Statement, false);
  size_t Budget = 4096;
  while (!Pending.empty()) {
    if (!Budget--)
      return false;
    const auto [Statement, Nested] = Pending.back();
    Pending.pop_back();
    if (Nested && InPad(Statement->Addr))
      return false;
    if (Statement->Kind == StmtKind::Goto && InPad(Statement->GotoTarget))
      return false;
    for (const auto &Child : Statement->Body)
      Pending.emplace_back(&Child, true);
    for (const auto &Child : Statement->ElseBody)
      Pending.emplace_back(&Child, true);
    for (const auto &Case : Statement->Cases)
      for (const auto &Child : Case.Body)
        Pending.emplace_back(&Child, true);
    for (const auto &Child : Statement->DefaultBody)
      Pending.emplace_back(&Child, true);
    for (const auto &Clause : Statement->EHClauseBodies)
      for (const auto &Child : Clause)
        Pending.emplace_back(&Child, true);
  }
  auto &Body = Function.Body;
  const auto Marker = std::find_if(Body.begin(), Body.end(), [&](const auto &S) {
    return InPad(S.Addr);
  });
  if (Marker == Body.end())
    return Body.back().Kind == StmtKind::Return;
  if (Marker == Body.begin() || (Marker - 1)->Kind != StmtKind::Return ||
      Marker->Addr != Landing || Marker->Kind != StmtKind::Block ||
      !Marker->Body.empty() || !Marker->ElseBody.empty() || Marker->Dst ||
      Marker->Val || Marker->Cond || Marker->RetVal || Marker->StoreAddr ||
      Marker->StoreVal || Marker->CallExpr || Marker->SwitchExpr ||
      !Marker->Cases.empty() || !Marker->DefaultBody.empty() ||
      Marker->GotoTarget || !Marker->EHClauseBodies.empty() ||
      !Marker->EHClauses.empty() || Marker->MemoryOrdering !=
                                        NdMemoryOrdering::None ||
      Marker->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Marker->EHRange.Begin || Marker->EHRange.End)
    return false;
  bool SawResume = false;
  for (auto It = Marker + 1; It != Body.end(); ++It) {
    if (It->Addr == Landing + 16 && It->Kind == StmtKind::Assign &&
        It->Val && It->Val->Kind == ExprKind::Call &&
        It->Val->CallAddr == Proof.ResumeTarget)
      SawResume = true;
    else if (InPad(It->Addr) && It->Kind == StmtKind::Assign &&
             (It->Addr == Landing + 4 || It->Addr == Landing + 8))
      continue;
    else if (It + 1 == Body.end() && It->Addr == 0 &&
             It->Kind == StmtKind::Return &&
             (!It->RetVal ||
              (It->RetVal->Operands.empty() &&
               (It->RetVal->Kind == ExprKind::Var ||
                It->RetVal->Kind == ExprKind::Undef))))
      continue;
    else
      return false;
    if (!It->Body.empty() || !It->ElseBody.empty() || !It->Cases.empty() ||
        !It->DefaultBody.empty() || !It->EHClauseBodies.empty())
      return false;
  }
  if (!SawResume)
    return false;
  Body.erase(Marker, Body.end());
  return true;
}

// The emitter has already rendered and checked the method. This constrained
// edit adds only the exceptional unlock around the two uniquely rendered
// runtime calls; every other statement retains its existing source binding.
inline std::optional<std::string>
addObjCSynchronizedReceiverCleanup(llvm::StringRef Source,
                                   const ObjCSynchronizedSourceProof &Proof) {
  const std::string EnterName = "neverd_darwin_objc_sync_enter(";
  const std::string DispatchName = "neverd_darwin_dispatch_group_enter(";
  const std::string ExitName = "neverd_darwin_objc_sync_exit(";
  const std::string ReleaseName = "objc_release(";
  const size_t Method = Source.find("neverd_objc_imp_");
  const size_t Open = Method == std::string::npos ? std::string::npos
                                                  : Source.find('{', Method);
  if (Open == std::string::npos)
    return std::nullopt;
  const size_t Enter = Source.find(EnterName, Open);
  const size_t Dispatch = Source.find(DispatchName, Open);
  const size_t Exit = Source.find(ExitName, Open);
  const bool StopAtExit = Proof.GuardStopCall == Proof.ExitCall;
  const size_t Release = Proof.GuardStopsAtRelease
                             ? Source.find(ReleaseName, Open)
                             : std::string::npos;
  const size_t Stop = StopAtExit ? Exit
                      : Proof.GuardStopsAtRelease ? Release
                                                   : Dispatch;
  if (Enter == std::string::npos || Stop == std::string::npos ||
      Exit == std::string::npos ||
      Source.find(EnterName, Enter + 1) != std::string::npos ||
      Source.find(ExitName, Exit + 1) != std::string::npos ||
      (Proof.GuardStopsAtRelease &&
       (Release <= Enter || Release >= Exit ||
        Source.find(ReleaseName, Release + 1) <= Exit)) ||
      (!StopAtExit &&
       Source.find(DispatchName, Dispatch + 1) != std::string::npos) ||
      Enter >= Stop || Stop > Exit)
    return std::nullopt;
  const size_t EnterEnd = Source.find(';', Enter);
  const size_t StopLine = Source.rfind('\n', Stop);
  const size_t ExitLine = Source.rfind('\n', Exit);
  if (EnterEnd == std::string::npos || StopLine == std::string::npos ||
      ExitLine == std::string::npos || EnterEnd >= StopLine ||
      (!StopAtExit && StopLine >= ExitLine) ||
      (!StopAtExit &&
       Source.substr(StopLine + 1, Stop - StopLine - 1).trim() != "") ||
      Source.substr(ExitLine + 1, Exit - ExitLine - 1).trim() != "(uint32_t)(")
    return std::nullopt;
  std::string Result = Source.str();
  Result.insert(StopLine + 1, "    neverd_objc_sync_guard = 0;\n");
  Result.insert(EnterEnd + 1, "\n    neverd_objc_sync_guard = objc_self;");
  Result.insert(Open + 1,
                "\n    void *neverd_objc_sync_guard "
                "__attribute__((cleanup(neverd_objc_sync_cleanup))) = 0;");
  Result.insert(0,
                "#include <stdint.h>\n"
                "#ifndef __EXCEPTIONS\n"
                "#error \"This source requires -fexceptions for Objective-C "
                "synchronization cleanup\"\n"
                "#endif\n"
                "extern int32_t neverd_darwin_objc_sync_exit(void*) "
                "__asm__(\"_objc_sync_exit\");\n"
                "static void neverd_objc_sync_cleanup(void **guard) {\n"
                "    if (*guard) (void)neverd_darwin_objc_sync_exit(*guard);\n"
                "}\n");
  return Result;
}

} // namespace neverd::sdk

#endif
