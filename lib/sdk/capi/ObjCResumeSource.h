#ifndef NEVERD_SDK_CAPI_OBJCRESUMESOURCE_H
#define NEVERD_SDK_CAPI_OBJCRESUMESOURCE_H

#include "../../loader/MachO/DarwinRuntimeImport.h"
#include "ObjCSynchronizedSource.h"

#include "neverd/loader/MachO/DarwinRuntimeCalls.h"

namespace neverd::sdk {

struct ObjCResumeSourceProof {
  va_t Entry = 0;
  va_t LandingPad = 0;
  va_t ResumeTarget = 0;
};

inline bool objcResumeImportProvider(const BinaryImage &Image, va_t Slot,
                                     llvm::StringRef Providers) {
  const auto Bind = Image.DyldBindSlots.find(Slot);
  return Bind != Image.DyldBindSlots.end() &&
         darwinExportModuleMatches(Providers, Bind->second.Module) &&
         std::find(Image.DynInfo.NeededLibs.begin(),
                   Image.DynInfo.NeededLibs.end(),
                   Bind->second.Module) != Image.DynInfo.NeededLibs.end();
}

// A one-instruction resumption pad has no cleanup or dispatch to reproduce.
// Authenticate the actual runtime veneer, every LSDA entry, and every normal
// machine edge before omitting it from a source projection. The original
// loader/IR exception metadata remains intact outside that projection.
inline std::optional<ObjCResumeSourceProof>
proveObjCResumeOnlySource(const BinaryImage &Image, const HighFunc &Function) {
  if (!objcSynchronizedSourceEHValid(Image, Function) ||
      Image.Bits != Bitness::Bits64 || Image.IsRelocatable)
    return std::nullopt;
  const auto &EH = *Function.ExceptionMetadata;
  if (EH.SEH || EH.Cxx || EH.GSCookie || EH.ARMEHABI || EH.Registration ||
      EH.Delphi || EH.DelphiScopes || EH.Go || EH.Rust ||
      EH.ObjC->Runtime != ObjCRuntimeKind::AppleNonFragile ||
      EH.ObjC->UsesFragileSetjmp || EH.ObjC->UsesMSVCTables ||
      EH.CodeRange.End < 4 || EH.CodeRange.End - 4 <= Function.Entry ||
      EH.CodeRange.End - 4 - Function.Entry > 16384 ||
      (Function.Entry | EH.CodeRange.End) % 4 ||
      EH.Itanium->CallSites.empty() || EH.Itanium->CallSites.size() > 129)
    return std::nullopt;
  const va_t Pad = EH.CodeRange.End - 4;
  const va_t Resume = objcSynchronizedBranchTarget(Image, Pad);
  const auto Slot =
      Resume ? darwinImportVeneerSlot(Image, Resume) : std::nullopt;
  const auto Import = Slot ? darwinRuntimeImport(Image, *Slot) : std::nullopt;
  if (!Import || *Import != "__Unwind_Resume")
    return std::nullopt;
  if (!objcResumeImportProvider(
          Image, *Slot,
          "/usr/lib/libSystem.B.dylib|/usr/lib/system/libunwind.dylib"))
    return std::nullopt;
  for (const auto &Call : EH.ObjC->RuntimeCalls)
    if (Call.Kind == ObjCRuntimeCallKind::BeginCatch ||
        Call.Kind == ObjCRuntimeCallKind::EndCatch ||
        Call.Kind == ObjCRuntimeCallKind::Rethrow ||
        Call.Kind == ObjCRuntimeCallKind::FragileTry)
      return std::nullopt;

  va_t Next = Function.Entry;
  size_t Protected = 0;
  for (const auto &Site : EH.Itanium->CallSites) {
    const auto &Range = Site.GuardedRange;
    if (Range.Begin < Next || Range.End <= Range.Begin ||
        Range.End > EH.CodeRange.End || (Range.Begin | Range.End) % 4 ||
        Site.FirstActionOffset ||
        (Range.Begin != Next &&
         !objcSynchronizedNonCallGap(Image, Next, Range.Begin)))
      return std::nullopt;
    Next = Range.End;
    if (!Site.LandingPadVA)
      continue;
    if (Site.LandingPadVA != Pad || Range.End > Pad ||
        std::count_if(EH.ObjC->LandingPads.begin(), EH.ObjC->LandingPads.end(),
                      [&](const auto &Landing) {
                        return Landing.PadVA == Pad &&
                               Landing.Kind == ObjCPadKind::Cleanup &&
                               Landing.Catches.empty() &&
                               Landing.GuardedRange.Begin == Range.Begin &&
                               Landing.GuardedRange.End == Range.End;
                      }) != 1)
      return std::nullopt;
    ++Protected;
  }
  if (Next != EH.CodeRange.End || !Protected ||
      Protected != EH.ObjC->LandingPads.size())
    return std::nullopt;

  Decoder Decoder;
  if (!Decoder.init(Arch::AArch64))
    return std::nullopt;
  for (va_t Address = Function.Entry; Address < Pad; Address += 4) {
    const auto *Bytes = Image.readVA(Address, 4);
    DecodedInsn Instruction{};
    if (!Bytes || Decoder.decodeOne(Bytes, 4, Address, Instruction) != 4 ||
        !Instruction.Raw ||
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_INT) ||
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_IRET))
      return std::nullopt;
    const auto Word = objcSynchronizedWord(Image, Address);
    if (Word == 0xd65f03c0U)
      continue;
    if (cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_RET))
      return std::nullopt;
    if (cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_JUMP)) {
      const auto Target = objcSynchronizedDirectBranchTarget(Instruction);
      const bool Unconditional = Word && branch::A64Branch.matches(*Word);
      if (!Target || *Target == Pad ||
          (!Unconditional && (*Target < Function.Entry || *Target >= Pad)))
        return std::nullopt;
      if (Unconditional)
        continue;
    }
    // A final stack-check failure can precede the pad in address order. A
    // spelling or a cached hint cannot certify its lack of a normal edge.
    const va_t Callee = objcSynchronizedBranchTarget(Image, Address);
    const auto CalleeSlot =
        Callee ? darwinImportVeneerSlot(Image, Callee) : std::nullopt;
    if (CalleeSlot) {
      const auto Runtime = darwinRuntimeSourceCallHint(Image, *CalleeSlot);
      const auto ObjC = objcRuntimeSourceCallHint(Image, *CalleeSlot);
      if ((Runtime && Runtime->DoesNotReturn &&
           Runtime->TargetName == "__stack_chk_fail" &&
           objcResumeImportProvider(Image, *CalleeSlot,
                                    "/usr/lib/libSystem.B.dylib|/usr/lib/"
                                    "system/libsystem_c.dylib")) ||
          (ObjC && ObjC->DoesNotReturn &&
           ObjC->TargetName == "objc_exception_throw" &&
           objcResumeImportProvider(Image, *CalleeSlot,
                                    "/usr/lib/libobjc.A.dylib")))
        continue;
    }
    if (Address + 4 == Pad)
      return std::nullopt;
  }
  return ObjCResumeSourceProof{Function.Entry, Pad, Resume};
}

inline bool omitProvenObjCResumeOnlyPad(HighFunc &Function,
                                        const ObjCResumeSourceProof &Proof) {
  if (!Function.ExceptionMetadata || Function.Entry != Proof.Entry ||
      !Proof.ResumeTarget || !Proof.LandingPad ||
      Function.ExceptionMetadata->CodeRange.Begin != Function.Entry ||
      Function.ExceptionMetadata->CodeRange.End < 4 ||
      Function.ExceptionMetadata->CodeRange.End - 4 != Proof.LandingPad ||
      Function.Body.empty())
    return false;
  std::vector<std::pair<const HighStmt *, bool>> Pending;
  for (const auto &Statement : Function.Body)
    Pending.emplace_back(&Statement, false);
  size_t Budget = 4096;
  while (!Pending.empty()) {
    if (!Budget--)
      return false;
    const auto [Statement, Nested] = Pending.back();
    Pending.pop_back();
    if ((Nested && Statement->Addr == Proof.LandingPad) ||
        (Statement->Kind == StmtKind::Goto &&
         Statement->GotoTarget == Proof.LandingPad))
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
  const auto Marker =
      std::find_if(Body.begin(), Body.end(),
                   [&](const auto &S) { return S.Addr == Proof.LandingPad; });
  if (Marker != Body.end()) {
    bool SawResume = false;
    for (auto It = Marker; It != Body.end(); ++It) {
      if (!It->Body.empty() || !It->ElseBody.empty() || !It->Cases.empty() ||
          !It->DefaultBody.empty() || !It->EHClauseBodies.empty() ||
          It->MemoryOrdering != NdMemoryOrdering::None ||
          It->MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return false;
      const auto &Call = It->Val;
      if (!SawResume && It->Addr == Proof.LandingPad &&
          (It->Kind == StmtKind::Assign || It->Kind == StmtKind::ExprStmt) &&
          Call && Call->Kind == ExprKind::Call && !Call->IsIndirectCall &&
          Call->CallAddr == Proof.ResumeTarget && Call->Operands.size() == 1 &&
          Call->Operands[0] && Call->Operands[0]->Kind == ExprKind::Var &&
          Call->Operands[0]->Var.Kind == MedVar::EHException) {
        SawResume = true;
        continue;
      }
      if (SawResume && It + 1 == Body.end() && !It->Addr &&
          It->Kind == StmtKind::Return &&
          (!It->RetVal || (It->RetVal->Operands.empty() &&
                           (It->RetVal->Kind == ExprKind::Var ||
                            It->RetVal->Kind == ExprKind::Const ||
                            It->RetVal->Kind == ExprKind::Undef))))
        continue;
      return false;
    }
    if (!SawResume)
      return false;
    Body.erase(Marker, Body.end());
  }
  // Ordinary source calls propagate the same exception. Requiring
  // -fexceptions below keeps compiler-generated frame unwinding available.
  Function.ExceptionMetadata.reset();
  return true;
}

inline constexpr char ObjCResumeSourceRequirements[] =
    "#ifndef __EXCEPTIONS\n"
    "#error \"This source requires -fexceptions for native exception "
    "propagation\"\n"
    "#endif\n";

} // namespace neverd::sdk
#endif
