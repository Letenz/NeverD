#ifndef NEVERD_SDK_CAPI_OBJCSYNCHRONIZEDSOURCE_H
#define NEVERD_SDK_CAPI_OBJCSYNCHRONIZEDSOURCE_H

#include "neverd/ir/high/HighIR.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Endian.h"

#include <optional>
#include <string>

namespace neverd::sdk {

struct ObjCSynchronizedSourceProof {
  va_t EnterCall = 0;
  va_t FirstUnprotectedCall = 0;
  va_t ExitCall = 0;
};

// A narrowly recognized clang @synchronized cleanup: the Itanium call-site
// table names one unconditional pad, and that pad does nothing except unlock
// the saved receiver and resume the original exception. Its source equivalent
// is a C cleanup variable compiled with -fexceptions. In particular, do not
// accept a pad which also releases an ARC object or executes a finally body.
inline std::optional<ObjCSynchronizedSourceProof>
proveObjCSynchronizedReceiverCleanup(const BinaryImage &Image,
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
      Protected.GuardedRange.Begin < Function.Entry ||
      Protected.GuardedRange.End > Protected.LandingPadVA ||
      EH.CodeRange.End != Protected.LandingPadVA + 20)
    return std::nullopt;

  const auto Word = [&](va_t Address) -> std::optional<uint32_t> {
    const uint8_t *Bytes = Image.readVA(Address, 4);
    if (!Bytes)
      return std::nullopt;
    return llvm::support::endian::read32le(Bytes);
  };
  const auto BranchTarget = [&](va_t Address) -> va_t {
    const auto Instruction = Word(Address);
    if (!Instruction || (*Instruction & 0xfc000000U) != 0x94000000U)
      return 0;
    const int64_t Displacement =
        static_cast<int64_t>(static_cast<int32_t>(*Instruction << 6) >> 6) * 4;
    return Displacement >= 0 ? Address + static_cast<uint64_t>(Displacement)
                             : Address - static_cast<uint64_t>(-Displacement);
  };
  const auto HasName = [&](va_t Address, llvm::StringRef Name) {
    return Address && llvm::StringRef(Image.getFunctionNameAt(Address)) == Name;
  };
  const va_t Landing = Protected.LandingPadVA;
  if (Word(Landing) != 0xaa0003f3U || Word(Landing + 4) != 0xf94007e0U ||
      !HasName(BranchTarget(Landing + 8), "_objc_sync_exit") ||
      Word(Landing + 12) != 0xaa1303e0U ||
      !HasName(BranchTarget(Landing + 16), "__Unwind_Resume"))
    return std::nullopt;

  // The source guard uses objc_self. Prove the original enter, normal exit,
  // and exceptional exit all load that same saved receiver, and that no
  // later instruction stores a new value into its stack slot.
  if (Word(Function.Entry + 16) != 0xf90007e0U ||
      Word(Function.Entry + 24) != 0xf94007e0U)
    return std::nullopt;
  const va_t EnterCall = Function.Entry + 28;
  if (!HasName(BranchTarget(EnterCall), "_objc_sync_enter") ||
      Protected.GuardedRange.Begin != EnterCall + 4)
    return std::nullopt;
  // The one call after the LSDA range is dispatch_group_enter. Clear the C
  // cleanup guard before it: the original unwind table does not cover it.
  const va_t Unprotected = Protected.GuardedRange.End;
  const va_t DispatchCall = Unprotected + 8;
  const va_t ExitCall = Unprotected + 16;
  if (Landing != Unprotected + 40 || Word(Unprotected) != 0xf94007e0U ||
      Word(Unprotected + 4) != 0xf9400400U ||
      !HasName(BranchTarget(DispatchCall), "_dispatch_group_enter") ||
      Word(Unprotected + 12) != 0xf94007e0U ||
      !HasName(BranchTarget(ExitCall), "_objc_sync_exit"))
    return std::nullopt;
  for (va_t Address = Function.Entry + 20; Address < Landing; Address += 4)
    if (Word(Address) == 0xf90007e0U)
      return std::nullopt;
  return ObjCSynchronizedSourceProof{EnterCall, DispatchCall, ExitCall};
}

// The emitter has already rendered and checked the method. This constrained
// edit adds only the exceptional unlock around the two uniquely rendered
// runtime calls; every other statement retains its existing source binding.
inline std::optional<std::string>
addObjCSynchronizedReceiverCleanup(llvm::StringRef Source) {
  const std::string EnterName = "neverd_darwin_objc_sync_enter(";
  const std::string DispatchName = "neverd_darwin_dispatch_group_enter(";
  const std::string ExitName = "neverd_darwin_objc_sync_exit(";
  const size_t Method = Source.find("neverd_objc_imp_");
  const size_t Open = Method == std::string::npos ? std::string::npos
                                                  : Source.find('{', Method);
  if (Open == std::string::npos)
    return std::nullopt;
  const size_t Enter = Source.find(EnterName, Open);
  const size_t Dispatch = Source.find(DispatchName, Open);
  const size_t Exit = Source.find(ExitName, Open);
  if (Enter == std::string::npos || Dispatch == std::string::npos ||
      Exit == std::string::npos ||
      Source.find(EnterName, Enter + 1) != std::string::npos ||
      Source.find(DispatchName, Dispatch + 1) != std::string::npos ||
      Source.find(ExitName, Exit + 1) != std::string::npos ||
      Enter >= Dispatch || Dispatch >= Exit)
    return std::nullopt;
  const size_t EnterEnd = Source.find(';', Enter);
  const size_t DispatchLine = Source.rfind('\n', Dispatch);
  const size_t ExitLine = Source.rfind('\n', Exit);
  if (EnterEnd == std::string::npos || DispatchLine == std::string::npos ||
      ExitLine == std::string::npos || EnterEnd >= DispatchLine ||
      DispatchLine >= ExitLine ||
      Source.substr(DispatchLine + 1, Dispatch - DispatchLine - 1).trim() !=
          "" ||
      Source.substr(ExitLine + 1, Exit - ExitLine - 1).trim() != "(uint32_t)(")
    return std::nullopt;
  std::string Result = Source.str();
  Result.insert(DispatchLine + 1, "    neverd_objc_sync_guard = 0;\n");
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
