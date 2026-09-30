#ifndef NEVERD_SDK_CAPI_OBJCSYNCHRONIZEDSOURCE_H
#define NEVERD_SDK_CAPI_OBJCSYNCHRONIZEDSOURCE_H

#include "neverd/decode/Decoder.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/DarwinImportVeneer.h"
#include "neverd/loader/ObjC/ObjCCallHints.h"
#include "neverd/support/BranchEncoding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <capstone/arm64.h>
#include <cstdint>
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
  uint8_t UnprotectedReleases = 0;
  bool ReceiverIsSavedLocal = false;
  uint8_t UnprotectedRetains = 0;
  bool ReceiverHasStackCopy = false;
  uint8_t SuspendARC = 0; // release=1, retain=2; all such calls are unprotected
  uint8_t LandingPadPrelude = 0; // exact B to the five-instruction cleanup
};

struct ObjCSynchronizedSourceRegion {
  va_t Begin = 0;
  va_t End = 0;
  va_t Landing = 0;
  uint8_t LandingPadPrelude = 0;
};

inline bool objcSynchronizedSourceEHValid(const BinaryImage &Image,
                                          const HighFunc &Function) {
  if (Image.Arch != Arch::AArch64 || Image.Format != BinaryFormat::MachO ||
      Function.StructuredExceptionRegions ||
      Function.UnstructuredExceptionRegions || Function.Params.empty() ||
      Function.Params[0].Name != "objc_self" || !Function.ExceptionMetadata)
    return false;
  const auto &EH = *Function.ExceptionMetadata;
  return EH.CodeRange.Begin == Function.Entry && EH.Itanium && EH.ObjC &&
         EH.ParseStatus == ExceptionParseStatus::Complete &&
         EH.Personality == ExceptionPersonality::ObjCPersonalityV0 &&
         EH.Itanium->Actions.empty() && EH.Itanium->TypeTable.empty() &&
         EH.Itanium->ExceptionSpecs.empty() &&
         EH.Itanium->IsCallSiteAddressForm;
}

inline std::optional<ObjCSynchronizedSourceRegion>
objcSynchronizedSourceRegion(const BinaryImage &Image,
                             const HighFunc &Function) {
  if (!objcSynchronizedSourceEHValid(Image, Function))
    return std::nullopt;
  const auto &EH = *Function.ExceptionMetadata;
  if (EH.Itanium->CallSites.size() != 3 || EH.ObjC->LandingPads.size() != 1)
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

// A missing call-site-table interval is not a zero-landing-pad call site.
// Accept such an interval only when every instruction decodes and none is a
// call. The complete lifetime proof still checks its control flow and writes.
inline bool objcSynchronizedNonCallGap(const BinaryImage &Image, va_t Begin,
                                       va_t End) {
  if (Image.Arch != Arch::AArch64 || Begin >= End || End - Begin > 16384 ||
      (Begin | End) % 4)
    return false;
  Decoder Decoder;
  if (!Decoder.init(Arch::AArch64))
    return false;
  for (va_t Address = Begin; Address < End; Address += 4) {
    const uint8_t *Bytes = Image.readVA(Address, 4);
    DecodedInsn Instruction{};
    if (!Bytes || Decoder.decodeOne(Bytes, 4, Address, Instruction) != 4 ||
        !Instruction.Raw ||
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_CALL) ||
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_INT) ||
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_IRET))
      return false;
  }
  return true;
}

// LSDA ranges may share one cleanup, with unprotected instructions between
// them. Preserve those holes rather than treating their union as one protected
// interval. Every table entry and pad record must agree exactly.
inline std::optional<std::vector<ObjCSynchronizedSourceRegion>>
objcSynchronizedInterleavedRanges(const BinaryImage &Image,
                                  const HighFunc &Function) {
  if (!objcSynchronizedSourceEHValid(Image, Function))
    return std::nullopt;
  const auto &EH = *Function.ExceptionMetadata;
  const auto &Sites = EH.Itanium->CallSites;
  if (Sites.size() < 3 || Sites.size() > 129 || Sites.front().LandingPadVA ||
      Sites.back().LandingPadVA || EH.CodeRange.End < 20 ||
      Sites.front().GuardedRange.Begin != Function.Entry)
    return std::nullopt;
  const va_t Cleanup = EH.CodeRange.End - 20;
  va_t Landing = InvalidVA;
  for (const auto &Site : Sites)
    if (Site.LandingPadVA)
      Landing = std::min(Landing, Site.LandingPadVA);
  if (Landing < Function.Entry || Landing > Cleanup ||
      (Cleanup - Landing != 0 && Cleanup - Landing != 4) ||
      (Landing | Cleanup) % 4)
    return std::nullopt;
  const uint8_t Prelude = static_cast<uint8_t>(Cleanup - Landing);
  // Clang can give an early range a separate entry consisting solely of
  // B +4. Keep that address as exceptional too; all entries must reach the
  // same exact unlock/resume sequence, with no intervening effects.
  if (Prelude) {
    const uint8_t *Bytes = Image.readVA(Landing, 4);
    if (!Bytes || llvm::support::endian::read32le(Bytes) != 0x14000001U)
      return std::nullopt;
  }
  std::vector<ObjCSynchronizedSourceRegion> Regions;
  va_t Next = Function.Entry;
  for (const auto &Site : Sites) {
    const auto &Range = Site.GuardedRange;
    if (Range.Begin < Next || Range.End <= Range.Begin ||
        Range.End > EH.CodeRange.End || (Range.Begin | Range.End) % 4 ||
        Site.FirstActionOffset)
      return std::nullopt;
    if (Range.Begin != Next &&
        !objcSynchronizedNonCallGap(Image, Next, Range.Begin))
      return std::nullopt;
    Next = Range.End;
    if (!Site.LandingPadVA)
      continue;
    if (Site.LandingPadVA != Landing && Site.LandingPadVA != Cleanup)
      return std::nullopt;
    if (Range.End > Landing ||
        std::count_if(EH.ObjC->LandingPads.begin(), EH.ObjC->LandingPads.end(),
                      [&](const auto &Pad) {
                        const bool KindMatches =
                            Pad.PadVA == Cleanup
                                ? Pad.Kind == ObjCPadKind::SynchronizedExit
                                : Pad.Kind == ObjCPadKind::Cleanup ||
                                      Pad.Kind == ObjCPadKind::SynchronizedExit;
                        return Pad.PadVA == Site.LandingPadVA && KindMatches &&
                               Pad.Catches.empty() &&
                               Pad.GuardedRange.Begin == Range.Begin &&
                               Pad.GuardedRange.End == Range.End;
                      }) != 1)
      return std::nullopt;
    Regions.push_back({Range.Begin, Range.End, Landing, Prelude});
  }
  if (Regions.empty() || Regions.size() != EH.ObjC->LandingPads.size() ||
      Next != EH.CodeRange.End || !Landing)
    return std::nullopt;
  return Regions;
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
  if (!Instruction || !branch::A64BranchLink.matches(*Instruction))
    return 0;
  return branch::a64BranchTarget(*Instruction, Address).value_or(0);
}

inline bool objcSynchronizedCallIs(const BinaryImage &Image, va_t Address,
                                   llvm::StringRef Name) {
  const va_t Target = objcSynchronizedBranchTarget(Image, Address);
  return Target && llvm::StringRef(Image.getFunctionNameAt(Target)) == Name;
}

inline std::optional<va_t>
objcSynchronizedDirectBranchTarget(const DecodedInsn &Instruction) {
  if (!Instruction.Raw || !Instruction.Raw->detail)
    return std::nullopt;
  switch (Instruction.Id) {
  case ARM64_INS_B:
  case ARM64_INS_CBZ:
  case ARM64_INS_CBNZ:
  case ARM64_INS_TBZ:
  case ARM64_INS_TBNZ:
    break;
  default:
    return std::nullopt;
  }
  const auto &Operands = Instruction.Raw->detail->aarch64;
  if (!Operands.op_count)
    return std::nullopt;
  const auto &Target = Operands.operands[Operands.op_count - 1];
  if (Target.type != AARCH64_OP_IMM || Target.imm < 0 || Target.imm % 4)
    return std::nullopt;
  return static_cast<va_t>(Target.imm);
}

inline bool objcSynchronizedForwardBranch(const DecodedInsn &Instruction,
                                          va_t LastTarget,
                                          va_t FirstTarget = 0) {
  const auto Target = objcSynchronizedDirectBranchTarget(Instruction);
  return Target && *Target > Instruction.Addr && *Target >= FirstTarget &&
         *Target <= LastTarget;
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

// Reuse the loader's ARC ABI catalog, including register-specific veneers.
// A matching symbol spelling alone cannot establish the runtime operation.
inline std::optional<SourceCallTypeHint>
objcSynchronizedRuntimeTarget(const BinaryImage &Image, va_t Target) {
  const auto Slot = darwinImportVeneerSlot(Image, Target);
  const auto Hint =
      Slot ? objcRuntimeSourceCallHint(Image, *Slot) : std::nullopt;
  if (!Hint || Hint->Signature.Parameters.size() != 1)
    return std::nullopt;
  const auto Bind = Image.DyldBindSlots.find(*Slot);
  if (Bind == Image.DyldBindSlots.end() ||
      Bind->second.Module != "/usr/lib/libobjc.A.dylib" ||
      std::find(Image.DynInfo.NeededLibs.begin(),
                Image.DynInfo.NeededLibs.end(),
                Bind->second.Module) == Image.DynInfo.NeededLibs.end())
    return std::nullopt;
  const auto &Location = Hint->Signature.Parameters.front().Location;
  if (Location.Kind != SourceABICarrierKind::IntegerRegister ||
      Location.ValueBytes != 8)
    return std::nullopt;
  return Hint;
}

inline bool objcSynchronizedRuntimeTargetIs(const BinaryImage &Image,
                                            va_t Target, llvm::StringRef Name,
                                            uint64_t ArgumentRegister) {
  const auto Hint = objcSynchronizedRuntimeTarget(Image, Target);
  return Hint && Hint->TargetName == Name &&
         Hint->Signature.Parameters.front().Location.RegisterOffset ==
             ArgumentRegister;
}

// Clang may synchronize on a retained getter result kept in a private stack
// slot, then call the getter again for the mutation. These objects need not
// be equal. Check the entire small frame so neither a stack alias nor a
// different result can replace the lock used by either exit.
inline std::optional<ObjCSynchronizedSourceProof>
proveObjCSynchronizedRetainedStackReceiverCleanup(const BinaryImage &Image,
                                                  const HighFunc &Function) {
  const auto Region = objcSynchronizedSourceRegion(Image, Function);
  const va_t Entry = Function.Entry;
  if (!Region || Function.Params.size() != 2 || Entry > InvalidVA - 0x74 ||
      Entry % 4 || Region->Begin != Entry + 0x28 ||
      Region->End != Entry + 0x40 || Region->Landing != Entry + 0x60)
    return std::nullopt;
  const auto Word = [&](unsigned Offset) {
    return objcSynchronizedWord(Image, Entry + Offset);
  };
  for (const auto &[Offset, Expected] :
       {std::pair<unsigned, uint32_t>{0x00, 0xd100c3ffU}, // sub sp, #0x30
        {0x04, 0xa9014ff4U},
        {0x08, 0xa9027bfdU},
        {0x0c, 0x910083fdU},
        {0x10, 0xaa0003f3U}, // save incoming self
        {0x18, 0xaa1d03fdU},
        {0x20, 0xf90007e0U}, // save retained lock
        {0x28, 0xaa1303e0U},
        {0x30, 0xaa1d03fdU},
        {0x38, 0xaa0003f3U}, // preserve the second getter's retained result
        {0x44, 0xf94007e0U},
        {0x4c, 0xf94007e0U}, // reload original lock
        {0x50, 0xa9427bfdU},
        {0x54, 0xa9414ff4U},
        {0x58, 0x9100c3ffU},
        {0x60, 0xaa0003f3U}, // save exception
        {0x64, 0xf94007e0U},
        {0x6c, 0xaa1303e0U}})
    if (Word(Offset) != Expected)
      return std::nullopt;
  const auto Target = [&](unsigned Offset) {
    return objcSynchronizedBranchTarget(Image, Entry + Offset);
  };
  const auto RuntimeCall = [&](unsigned Offset, llvm::StringRef Name,
                               uint64_t Argument) {
    return objcSynchronizedRuntimeTargetIs(Image, Target(Offset), Name,
                                           Argument);
  };
  const va_t Getter = Target(0x14);
  const va_t Mutation = Target(0x3c);
  const auto Tail = Word(0x5c);
  if (!Getter || Getter != Target(0x2c) || !Mutation ||
      !objcSelectorStubPreservesNonvolatileRegisters(Image, Getter) ||
      !objcSelectorStubPreservesNonvolatileRegisters(Image, Mutation) ||
      !RuntimeCall(0x1c, "objc_retainAutoreleasedReturnValue", a64reg::X0) ||
      !RuntimeCall(0x34, "objc_retainAutoreleasedReturnValue", a64reg::X0) ||
      !RuntimeCall(0x40, "objc_release", a64reg::X19) ||
      !objcSynchronizedCallIs(Image, Entry + 0x24, "_objc_sync_enter") ||
      !objcSynchronizedCallIs(Image, Entry + 0x48, "_objc_sync_exit") ||
      !objcSynchronizedCallIs(Image, Entry + 0x68, "_objc_sync_exit") ||
      !objcSynchronizedCallIs(Image, Entry + 0x70, "__Unwind_Resume") ||
      !Tail || !branch::A64Branch.matches(*Tail) ||
      !objcSynchronizedRuntimeTargetIs(
          Image, branch::a64BranchTarget(*Tail, Entry + 0x5c).value_or(0),
          "objc_release", a64reg::X0))
    return std::nullopt;
  return ObjCSynchronizedSourceProof{Entry + 0x24, Entry + 0x40,
                                     Entry + 0x48, Entry + 0x60,
                                     Target(0x70), 1,
                                     true,         0,
                                     true};
}

// A second clang shape keeps the receiver in a callee-saved register. Forward
// branches in the prefix must join before loading the enter argument; forward
// branches in the protected body must join by its end. No path may bypass the
// lock, reach its pad normally, or skip the first unprotected call. The C guard
// is cleared before that call, preserving the original LSDA boundary.
inline std::optional<ObjCSynchronizedSourceProof>
proveObjCSynchronizedRegisterReceiverCleanup(const BinaryImage &Image,
                                             const HighFunc &Function) {
  const auto Region = objcSynchronizedSourceRegion(Image, Function);
  if (!Region || Function.Entry > InvalidVA - 8 ||
      Region->Begin < Function.Entry + 8 || Region->Landing < 20 ||
      Region->End > Region->Landing - 20 || Region->Begin > Region->End ||
      (Function.Entry | Region->Begin | Region->End | Region->Landing) % 4 ||
      Region->End - Function.Entry > 16384)
    return std::nullopt;
  const auto Word = [&](va_t Address) {
    return objcSynchronizedWord(Image, Address);
  };
  const auto HasCall = [&](va_t Address, llvm::StringRef Name) {
    return objcSynchronizedCallIs(Image, Address, Name);
  };
  // LSDA ranges begin at potentially throwing instructions. Clang may put
  // straight-line non-call preparation between the enter and that range.
  va_t EnterCall = 0;
  for (va_t Address = Region->Begin - 4;
       Address >= Function.Entry + 4 && Region->Begin - Address <= 32;
       Address -= 4) {
    if (HasCall(Address, "_objc_sync_enter")) {
      EnterCall = Address;
      break;
    }
  }
  if (!EnterCall)
    return std::nullopt;
  const va_t Landing = Region->Landing;
  const auto SaveX0 = [](unsigned Register) { return 0xaa0003e0U | Register; };
  const auto LoadX0 = [](unsigned Register) {
    return 0xaa0003e0U | (Register << 16);
  };
  unsigned ReceiverRegister = 0;
  bool ReceiverFromSelf = false, ReceiverFromRetainResult = false;
  for (unsigned Register = 19; Register <= 22; ++Register) {
    if (Word(EnterCall - 4) == LoadX0(Register)) {
      ReceiverRegister = Register;
      ReceiverFromSelf = true;
    } else if (Word(EnterCall - 4) == SaveX0(Register) &&
               EnterCall >= Function.Entry + 8 &&
               HasCall(EnterCall - 8, "_objc_retainAutoreleasedReturnValue")) {
      ReceiverRegister = Register;
      ReceiverFromRetainResult = true;
    }
  }
  if (!ReceiverRegister)
    return std::nullopt;
  unsigned ExceptionRegister = 0;
  for (unsigned Register = 19; Register <= 22; ++Register)
    if (Register != ReceiverRegister && Word(Landing) == SaveX0(Register))
      ExceptionRegister = Register;
  if (!ExceptionRegister)
    return std::nullopt;
  va_t Normal = Region->End;
  if (Word(Normal) == SaveX0(20) || Word(Normal) == SaveX0(21) ||
      Word(Normal) == SaveX0(22))
    Normal += 4; // retain an already computed result across ARC releases
  va_t FirstRetain = 0;
  uint8_t UnprotectedRetains = 0;
  if ((Word(Normal) == 0xaa1403e0U || Word(Normal) == 0xaa1503e0U ||
       Word(Normal) == 0xaa1603e0U) &&
      HasCall(Normal + 4, "_objc_retain")) {
    FirstRetain = Normal + 4;
    UnprotectedRetains = 1;
    Normal += 8;
  }
  va_t FirstRelease = 0;
  uint8_t UnprotectedReleases = 0;
  while (UnprotectedReleases < 2 && Normal < Landing - 8 &&
         (Word(Normal) == 0xaa1403e0U || Word(Normal) == 0xaa1503e0U ||
          Word(Normal) == 0xaa1603e0U) &&
         HasCall(Normal + 4, "_objc_release")) {
    if (!FirstRelease)
      FirstRelease = Normal + 4;
    ++UnprotectedReleases;
    Normal += 8;
  }
  const va_t ExitCall = Normal + 4;
  if (!HasCall(EnterCall, "_objc_sync_enter") || Normal >= Landing - 4 ||
      Word(Normal) != LoadX0(ReceiverRegister) ||
      !HasCall(ExitCall, "_objc_sync_exit") ||
      Word(Landing + 4) != LoadX0(ReceiverRegister) ||
      !HasCall(Landing + 8, "_objc_sync_exit") ||
      Word(Landing + 12) != LoadX0(ExceptionRegister) ||
      !HasCall(Landing + 16, "__Unwind_Resume"))
    return std::nullopt;

  va_t SavedReceiver = ReceiverFromRetainResult ? EnterCall - 4 : 0;
  if (ReceiverFromSelf) {
    for (va_t Address = Function.Entry; Address + 4 < EnterCall; Address += 4) {
      if (Word(Address) != SaveX0(ReceiverRegister))
        continue;
      if (SavedReceiver || Address > Function.Entry + 32)
        return std::nullopt;
      SavedReceiver = Address;
    }
    if (!SavedReceiver)
      return std::nullopt;
  }

  Decoder Decoder;
  if (!Decoder.init(Arch::AArch64))
    return std::nullopt;
  const unsigned ReceiverXRegisters[] = {ARM64_REG_X19, ARM64_REG_X20,
                                         ARM64_REG_X21, ARM64_REG_X22};
  const unsigned ReceiverWRegisters[] = {ARM64_REG_W19, ARM64_REG_W20,
                                         ARM64_REG_W21, ARM64_REG_W22};
  const unsigned ReceiverX = ReceiverXRegisters[ReceiverRegister - 19];
  const unsigned ReceiverW = ReceiverWRegisters[ReceiverRegister - 19];
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
      if (ReceiverFromSelf && Address < SavedReceiver &&
          (Writes[I] == ARM64_REG_X0 || Writes[I] == ARM64_REG_W0))
        return std::nullopt;
      if (Address > SavedReceiver &&
          (Writes[I] == ReceiverX || Writes[I] == ReceiverW))
        return std::nullopt;
    }
    const bool IsJump =
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_JUMP);
    const bool IsCall =
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_CALL);
    if (cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_RET) ||
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_IRET) ||
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_INT))
      return std::nullopt;
    if (ReceiverFromSelf && Address < SavedReceiver && (IsJump || IsCall))
      return std::nullopt;
    if (IsJump) {
      if (Address < EnterCall) {
        const va_t Join = ReceiverFromSelf ? EnterCall - 4 : EnterCall - 8;
        if (!objcSynchronizedForwardBranch(Instruction, Join))
          return std::nullopt;
      } else if (Address < Region->Begin || Address >= Region->End ||
                 !objcSynchronizedForwardBranch(Instruction, Region->End))
        return std::nullopt;
    }
    if (Address > EnterCall && Address < Region->Begin && IsCall)
      return std::nullopt;
    if ((Address != EnterCall && HasCall(Address, "_objc_sync_enter")) ||
        HasCall(Address, "_objc_sync_exit"))
      return std::nullopt;
    if (UnprotectedReleases && Address > EnterCall && Address < Region->End &&
        HasCall(Address, "_objc_release"))
      return std::nullopt;
    if (UnprotectedRetains && Address > EnterCall && Address < Region->End &&
        HasCall(Address, "_objc_retain"))
      return std::nullopt;
  }
  return ObjCSynchronizedSourceProof{
      EnterCall,
      UnprotectedRetains    ? FirstRetain
      : UnprotectedReleases ? FirstRelease
                            : ExitCall,
      ExitCall,
      Landing,
      objcSynchronizedBranchTarget(Image, Landing + 16),
      UnprotectedReleases,
      ReceiverFromRetainResult,
      UnprotectedRetains};
}

// The token mutators skip the lock entirely for a null argument. On the
// non-null path clang keeps that argument in x19, the retained lock in x20,
// and the receiver in x21. Only the exact forward skip and one protected,
// straight-line mutation may use this cleanup source guard.
inline std::optional<ObjCSynchronizedSourceProof>
proveObjCSynchronizedBranchedLocalReceiverCleanup(const BinaryImage &Image,
                                                  const HighFunc &Function) {
  const auto Region = objcSynchronizedSourceRegion(Image, Function);
  if (!Region || Function.Entry > InvalidVA - 0xbc ||
      Region->Begin != Function.Entry + 0x4c ||
      Region->End != Function.Entry + 0x68 ||
      Region->Landing != Function.Entry + 0xa8)
    return std::nullopt;
  const va_t Entry = Function.Entry;
  const va_t End = Region->End;
  const va_t Landing = Region->Landing;
  const auto Word = [&](va_t Address) {
    return objcSynchronizedWord(Image, Address);
  };
  const auto HasCall = [&](va_t Address, llvm::StringRef Name) {
    return objcSynchronizedCallIs(Image, Address, Name);
  };
  const auto Tail = Word(End + 60);
  if (!Tail || !branch::A64Branch.matches(*Tail))
    return std::nullopt;
  const va_t ReleaseTarget = objcSynchronizedBranchTarget(Image, End + 4);
  if (!ReleaseTarget ||
      branch::a64BranchTarget(*Tail, End + 60) != ReleaseTarget)
    return std::nullopt;
  if (Word(Entry) != 0xa9bd57f6U || Word(Entry + 4) != 0xa9014ff4U ||
      Word(Entry + 8) != 0xa9027bfdU || Word(Entry + 12) != 0x910083fdU ||
      Word(Entry + 16) != 0xaa0203f3U || // save argument in x19
      Word(Entry + 20) != 0xaa0003f5U || // save objc_self in x21
      Word(Entry + 24) != 0xaa0203e0U ||
      !HasCall(Entry + 0x1c, "_objc_retain") ||
      Word(Entry + 0x30) != 0xb40002b3U || // cbz x19, after unlock
      Word(Entry + 0x34) != 0xaa1503e0U ||
      !HasCall(Entry + 0x38, "_objc_msgSend$runningTokens") ||
      Word(Entry + 0x3c) != 0xaa1d03fdU ||
      !HasCall(Entry + 0x40, "_objc_retainAutoreleasedReturnValue") ||
      Word(Entry + 0x44) != 0xaa0003f4U || // retained lock in x20
      !HasCall(Entry + 0x48, "_objc_sync_enter") ||
      Word(Entry + 0x4c) != 0xaa1503e0U ||
      !HasCall(Entry + 0x50, "_objc_msgSend$runningTokens") ||
      !HasCall(Entry + 0x58, "_objc_retainAutoreleasedReturnValue") ||
      Word(Entry + 0x5c) != 0xaa0003f5U || // retained mutation receiver
      Word(Entry + 0x60) != 0xaa1303e2U ||
      (!HasCall(Entry + 0x64, "_objc_msgSend$addObject:") &&
       !HasCall(Entry + 0x64, "_objc_msgSend$removeObject:")) ||
      Word(End) != 0xaa1503e0U || !HasCall(End + 4, "_objc_release") ||
      Word(End + 8) != 0xaa1403e0U || !HasCall(End + 12, "_objc_sync_exit") ||
      Word(End + 16) != 0xaa1403e0U || !HasCall(End + 20, "_objc_release") ||
      Word(End + 24) != 0x14000005U || // normal path joins after skip
      Word(End + 44) != 0xaa1303e0U || Word(End + 48) != 0xa9427bfdU ||
      Word(End + 52) != 0xa9414ff4U || Word(End + 56) != 0xa8c357f6U ||
      Word(Landing) != 0xaa0003f3U || // preserve exception in x19
      Word(Landing + 4) != 0xaa1403e0U ||
      !HasCall(Landing + 8, "_objc_sync_exit") ||
      Word(Landing + 12) != 0xaa1303e0U ||
      !HasCall(Landing + 16, "__Unwind_Resume"))
    return std::nullopt;

  Decoder Decoder;
  if (!Decoder.init(Arch::AArch64))
    return std::nullopt;
  for (va_t Address = Entry; Address < End + 44; Address += 4) {
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
      if ((Address > Entry + 16 &&
           (Writes[I] == ARM64_REG_X19 || Writes[I] == ARM64_REG_W19)) ||
          (((Address > Entry + 20 && Address < Entry + 0x4c) ||
            (Address > Entry + 0x5c)) &&
           (Writes[I] == ARM64_REG_X21 || Writes[I] == ARM64_REG_W21)) ||
          (Address > Entry + 0x44 &&
           (Writes[I] == ARM64_REG_X20 || Writes[I] == ARM64_REG_W20)))
        return std::nullopt;
    }
    if (cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_RET))
      return std::nullopt;
    if (cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_JUMP) &&
        Address != Entry + 0x30 && Address != End + 24)
      return std::nullopt;
    if (Address >= End + 28 &&
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_CALL))
      return std::nullopt;
  }
  return ObjCSynchronizedSourceProof{
      Entry + 0x48,
      End + 4,
      End + 12,
      Landing,
      objcSynchronizedBranchTarget(Image, Landing + 16),
      1,
      true};
}

// Normal cleanup tails can have a shared epilogue before a bypass block in
// address order. Prove the complete suffix graph is acyclic and every edge
// stays after the normal unlock or terminates with a return/external tail.
// This permits backward joins without permitting a loop, pad entry or relock.
inline bool objcSynchronizedNormalSuffixValid(const BinaryImage &Image,
                                              va_t Entry, va_t Begin,
                                              va_t Landing,
                                              uint8_t LandingPadPrelude = 0) {
  if (Begin < Entry || Begin >= Landing || Landing - Entry > 16384 ||
      (LandingPadPrelude != 0 && LandingPadPrelude != 4) ||
      Landing > InvalidVA - 20 - LandingPadPrelude || (Begin | Landing) % 4)
    return false;
  Decoder Decoder;
  if (!Decoder.init(Arch::AArch64))
    return false;
  const size_t Count = (Landing - Begin) / 4;
  std::vector<std::vector<size_t>> Edges(Count);
  std::vector<unsigned> Incoming(Count, 0);
  const auto AddEdge = [&](va_t Address, va_t Target) {
    if (Target < Begin || Target >= Landing || Target % 4)
      return false;
    const size_t Next = (Target - Begin) / 4;
    Edges[(Address - Begin) / 4].push_back(Next);
    ++Incoming[Next];
    return true;
  };
  for (va_t Address = Begin; Address < Landing; Address += 4) {
    const uint8_t *Bytes = Image.readVA(Address, 4);
    DecodedInsn Instruction{};
    if (!Bytes || Decoder.decodeOne(Bytes, 4, Address, Instruction) != 4 ||
        !Instruction.Raw ||
        objcSynchronizedCallIs(Image, Address, "_objc_sync_enter") ||
        objcSynchronizedCallIs(Image, Address, "_objc_sync_exit") ||
        objcSynchronizedCallIs(Image, Address, "__Unwind_Resume"))
      return false;
    const auto Word = objcSynchronizedWord(Image, Address);
    if (Word == 0xd65f03c0U)
      continue;
    if (cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_RET) ||
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_IRET) ||
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_INT))
      return false;
    if (cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_JUMP)) {
      const auto Target = objcSynchronizedDirectBranchTarget(Instruction);
      if (!Target)
        return false;
      const bool Unconditional = Word && branch::A64Branch.matches(*Word);
      if (Unconditional &&
          (*Target < Entry || *Target >= Landing + 20 + LandingPadPrelude))
        continue;
      if (!AddEdge(Address, *Target))
        return false;
      if (Unconditional)
        continue;
    }
    if (!AddEdge(Address, Address + 4))
      return false;
  }
  std::vector<size_t> Ready;
  for (size_t I = 0; I < Count; ++I)
    if (!Incoming[I])
      Ready.push_back(I);
  for (size_t I = 0; I < Ready.size(); ++I)
    for (size_t Next : Edges[Ready[I]])
      if (!--Incoming[Next])
        Ready.push_back(Next);
  return Ready.size() == Count;
}

inline std::optional<ObjCSynchronizedSourceProof>
proveObjCSynchronizedInterleavedCleanup(const BinaryImage &Image,
                                        const HighFunc &Function) {
  const auto Regions = objcSynchronizedInterleavedRanges(Image, Function);
  if (!Regions || Function.Entry > InvalidVA - 8 ||
      Regions->front().Begin < Function.Entry + 8 ||
      Regions->back().End > Regions->front().Landing ||
      Regions->front().Landing - Function.Entry > 16384)
    return std::nullopt;
  const va_t Landing = Regions->front().Landing;
  const uint8_t Prelude = Regions->front().LandingPadPrelude;
  const va_t Cleanup = Landing + Prelude;
  const auto Word = [&](va_t Address) {
    return objcSynchronizedWord(Image, Address);
  };
  const auto HasCall = [&](va_t Address, llvm::StringRef Name) {
    return objcSynchronizedCallIs(Image, Address, Name);
  };
  va_t EnterCall = 0;
  for (va_t Address = Regions->front().Begin - 4;
       Address >= Function.Entry + 4 && Regions->front().Begin - Address <= 128;
       Address -= 4)
    if (HasCall(Address, "_objc_sync_enter")) {
      EnterCall = Address;
      break;
    }
  if (!EnterCall)
    return std::nullopt;
  const auto SaveX0 = [](unsigned Register) { return 0xaa0003e0U | Register; };
  const auto LoadX0 = [](unsigned Register) {
    return 0xaa0003e0U | (Register << 16);
  };
  unsigned ReceiverRegister = 0, ExceptionRegister = 0;
  bool ReceiverFromRetainResult = false;
  for (unsigned Register = 19; Register <= 22; ++Register)
    if (Word(EnterCall - 4) == LoadX0(Register))
      ReceiverRegister = Register;
    else if (EnterCall >= Function.Entry + 8 &&
             Word(EnterCall - 4) == SaveX0(Register) &&
             objcSynchronizedRuntimeTargetIs(
                 Image, objcSynchronizedBranchTarget(Image, EnterCall - 8),
                 "objc_retainAutoreleasedReturnValue", a64reg::X0)) {
      ReceiverRegister = Register;
      ReceiverFromRetainResult = true;
    }
  for (unsigned Register = 19; Register <= 22; ++Register)
    if (Register != ReceiverRegister && Word(Cleanup) == SaveX0(Register))
      ExceptionRegister = Register;
  if (!ReceiverRegister || !ExceptionRegister ||
      Word(Cleanup + 4) != LoadX0(ReceiverRegister) ||
      !HasCall(Cleanup + 8, "_objc_sync_exit") ||
      Word(Cleanup + 12) != LoadX0(ExceptionRegister) ||
      !HasCall(Cleanup + 16, "__Unwind_Resume"))
    return std::nullopt;
  va_t SavedReceiver = ReceiverFromRetainResult ? EnterCall - 4 : 0;
  va_t ExitCall = 0;
  for (va_t Address = Function.Entry; Address < Landing; Address += 4) {
    if (!ReceiverFromRetainResult && Address + 4 < EnterCall &&
        Word(Address) == SaveX0(ReceiverRegister)) {
      if (SavedReceiver || Address > Function.Entry + 32)
        return std::nullopt;
      SavedReceiver = Address;
    }
    if (HasCall(Address, "_objc_sync_exit")) {
      if (ExitCall || Address < Regions->back().End ||
          Word(Address - 4) != LoadX0(ReceiverRegister))
        return std::nullopt;
      ExitCall = Address;
    }
  }
  if (!SavedReceiver || !ExitCall)
    return std::nullopt;

  Decoder Decoder;
  if (!Decoder.init(Arch::AArch64))
    return std::nullopt;
  const unsigned ReceiverXRegisters[] = {ARM64_REG_X19, ARM64_REG_X20,
                                         ARM64_REG_X21, ARM64_REG_X22};
  const unsigned ReceiverWRegisters[] = {ARM64_REG_W19, ARM64_REG_W20,
                                         ARM64_REG_W21, ARM64_REG_W22};
  uint8_t UnprotectedARC = 0, ProtectedARC = 0;
  bool BypassesLock = false;
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
    for (uint8_t I = 0; I < WriteCount; ++I)
      if ((!ReceiverFromRetainResult && Address < SavedReceiver &&
           (Writes[I] == ARM64_REG_X0 || Writes[I] == ARM64_REG_W0)) ||
          (Address > SavedReceiver &&
           (Writes[I] == ReceiverXRegisters[ReceiverRegister - 19] ||
            Writes[I] == ReceiverWRegisters[ReceiverRegister - 19])))
        return std::nullopt;
    const bool IsJump =
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_JUMP);
    const bool IsCall =
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_CALL);
    if (cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_RET) ||
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_IRET) ||
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_INT) ||
        (!ReceiverFromRetainResult && Address < SavedReceiver &&
         (IsJump || IsCall)))
      return std::nullopt;
    if (IsJump) {
      const bool BeforeEnter = Address < EnterCall;
      const bool JoinsLockedPath = objcSynchronizedForwardBranch(
          Instruction, BeforeEnter
                           ? EnterCall - (ReceiverFromRetainResult ? 8 : 4)
                           : ExitCall - 4);
      // A pre-acquisition branch can skip the whole lock lifetime and join
      // the verified normal suffix. The source guard remains zero on that
      // path; entering the body, unlock or exceptional pad is still rejected.
      const bool SkipsLock =
          BeforeEnter &&
          objcSynchronizedForwardBranch(Instruction, Landing - 4, ExitCall + 4);
      if (!JoinsLockedPath && !SkipsLock)
        return std::nullopt;
      BypassesLock |= SkipsLock;
    }
    if ((Address != EnterCall && HasCall(Address, "_objc_sync_enter")) ||
        HasCall(Address, "_objc_sync_exit"))
      return std::nullopt;
    if (Address <= EnterCall || !IsCall)
      continue;
    const bool Protected =
        std::any_of(Regions->begin(), Regions->end(), [&](const auto &Region) {
          return Address >= Region.Begin && Address + 4 <= Region.End;
        });
    const auto Hint = objcSynchronizedRuntimeTarget(
        Image, objcSynchronizedBranchTarget(Image, Address));
    const uint8_t ARC = !Hint                                ? 0
                        : Hint->TargetName == "objc_release" ? 1
                        : Hint->TargetName == "objc_retain"  ? 2
                                                             : 0;
    if (Protected)
      ProtectedARC |= ARC;
    else {
      if (!ARC)
        return std::nullopt;
      UnprotectedARC |= ARC;
    }
  }
  // The rendered calls have canonical ARC names, even for register veneers.
  // Only suspend an operation when every occurrence in the locked body is
  // outside the LSDA ranges. Otherwise its source identity is insufficient.
  // The legacy proofs own ordinary single-range lifetimes. This proof also
  // handles a single range when a verified prefix path bypasses the lock;
  // no ARC suspension is needed if every locked-body call is protected.
  if ((Regions->size() == 1 && !BypassesLock && !Prelude) ||
      (Regions->size() > 1 && !UnprotectedARC) ||
      (UnprotectedARC & ProtectedARC))
    return std::nullopt;
  if (!objcSynchronizedNormalSuffixValid(Image, Function.Entry, ExitCall + 4,
                                         Landing, Prelude))
    return std::nullopt;
  ObjCSynchronizedSourceProof Proof{
      EnterCall, ExitCall, ExitCall, Landing,
      objcSynchronizedBranchTarget(Image, Cleanup + 16)};
  Proof.ReceiverIsSavedLocal = ReceiverFromRetainResult;
  Proof.SuspendARC = UnprotectedARC;
  Proof.LandingPadPrelude = Prelude;
  return Proof;
}

inline std::optional<ObjCSynchronizedSourceProof>
proveObjCSynchronizedReceiverCleanup(const BinaryImage &Image,
                                     const HighFunc &Function) {
  if (auto Proof =
          proveObjCSynchronizedRetainedStackReceiverCleanup(Image, Function))
    return Proof;
  if (auto Proof = proveObjCSynchronizedStackReceiverCleanup(Image, Function))
    return Proof;
  if (auto Proof =
          proveObjCSynchronizedRegisterReceiverCleanup(Image, Function))
    return Proof;
  if (auto Proof =
          proveObjCSynchronizedBranchedLocalReceiverCleanup(Image, Function))
    return Proof;
  return proveObjCSynchronizedInterleavedCleanup(Image, Function);
}

// The proved pad is an exceptional entry, not a normal source path. A full
// image analysis can retain it as an unreachable suffix after the ordinary
// return. Remove that suffix only when its label and statement addresses
// match the proved branch entry and five cleanup instructions, with no source
// edge into either exceptional entry.
// The cleanup variable below supplies the same exceptional unlock.
inline bool
omitProvenObjCSynchronizedLandingPad(HighFunc &Function,
                                     const ObjCSynchronizedSourceProof &Proof) {
  if (!Proof.LandingPad || !Proof.ResumeTarget ||
      (Proof.LandingPadPrelude != 0 && Proof.LandingPadPrelude != 4) ||
      Proof.LandingPad > InvalidVA - 20 - Proof.LandingPadPrelude ||
      Function.Body.empty())
    return false;
  const va_t Landing = Proof.LandingPad;
  const va_t Cleanup = Landing + Proof.LandingPadPrelude;
  const auto InPad = [&](va_t Address) {
    return Address >= Landing && Address < Cleanup + 20;
  };
  const auto EmptyLabel = [](const HighStmt &S) {
    return S.Kind == StmtKind::Block && S.Body.empty() && S.ElseBody.empty() &&
           !S.Dst && !S.Val && !S.Cond && !S.RetVal && !S.StoreAddr &&
           !S.StoreVal && !S.CallExpr && !S.SwitchExpr && S.Cases.empty() &&
           S.DefaultBody.empty() && !S.GotoTarget && S.EHClauseBodies.empty() &&
           S.EHClauses.empty() && S.MemoryOrdering == NdMemoryOrdering::None &&
           S.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
           !S.EHRange.Begin && !S.EHRange.End;
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
  const auto Marker = std::find_if(
      Body.begin(), Body.end(), [&](const auto &S) { return InPad(S.Addr); });
  if (Marker == Body.end())
    return Body.back().Kind == StmtKind::Return;
  if (Marker == Body.begin() || (Marker - 1)->Kind != StmtKind::Return ||
      Marker->Addr != Landing || !EmptyLabel(*Marker))
    return false;
  bool SawResume = false;
  for (auto It = Marker + 1; It != Body.end(); ++It) {
    if (Proof.LandingPadPrelude && It == Marker + 1 && It->Addr == Cleanup &&
        EmptyLabel(*It))
      continue;
    if (It->Addr == Cleanup + 16 && It->Kind == StmtKind::Assign && It->Val &&
        It->Val->Kind == ExprKind::Call &&
        It->Val->CallAddr == Proof.ResumeTarget)
      SawResume = true;
    else if (InPad(It->Addr) && It->Kind == StmtKind::Assign &&
             (It->Addr == Cleanup + 4 || It->Addr == Cleanup + 8))
      continue;
    else if (It + 1 == Body.end() && It->Addr == 0 &&
             It->Kind == StmtKind::Return &&
             (!It->RetVal || (It->RetVal->Operands.empty() &&
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

inline std::optional<std::string>
objcSynchronizedSavedLocalArgument(llvm::StringRef Source, size_t Call,
                                   llvm::StringRef Name) {
  if (Call == std::string::npos)
    return std::nullopt;
  const size_t Begin = Call + Name.size();
  unsigned Depth = 0;
  size_t End = Begin;
  for (; End < Source.size() && End - Begin < 128; ++End) {
    const char Character = Source[End];
    if (Character == '(')
      ++Depth;
    else if (Character == ')') {
      if (!Depth)
        break;
      --Depth;
    } else if (Character == '\n' || Character == ';' || Character == ',' ||
               Character == '"' || Character == '\'')
      return std::nullopt;
  }
  if (End == Source.size() || End - Begin >= 128)
    return std::nullopt;
  const llvm::StringRef Argument = Source.slice(Begin, End);
  const llvm::StringRef Prefix = "(void*)(uintptr_t)(";
  if (!Argument.starts_with(Prefix) || !Argument.ends_with(')'))
    return std::nullopt;
  const llvm::StringRef NameOnly =
      Argument.drop_front(Prefix.size()).drop_back();
  const auto Alpha = [](char C) {
    return (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || C == '_';
  };
  const auto Digit = [](char C) { return C >= '0' && C <= '9'; };
  if (NameOnly.empty() || NameOnly == "objc_self" || !Alpha(NameOnly.front()))
    return std::nullopt;
  for (char Character : NameOnly)
    if (!Alpha(Character) && !Digit(Character))
      return std::nullopt;
  return Argument.str();
}

// Locate the first emitted method definition, ignoring prototypes, comments,
// and braces in literals. Cleanup edits must never reach appended helpers.
inline std::optional<std::pair<size_t, size_t>>
objcSynchronizedSourceBody(llvm::StringRef Source) {
  enum class Lexical { Normal, String, Character, LineComment, BlockComment };
  Lexical State = Lexical::Normal;
  unsigned Braces = 0, Parameters = 0;
  bool Candidate = false;
  size_t Open = std::string::npos;
  const auto Identifier = [](char C) {
    return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') ||
           (C >= '0' && C <= '9') || C == '_';
  };
  const size_t Limit = std::min<size_t>(Source.size(), 16 * 1024 * 1024);
  for (size_t I = 0; I < Limit; ++I) {
    const char C = Source[I];
    const char Next = I + 1 < Limit ? Source[I + 1] : 0;
    if (State == Lexical::String || State == Lexical::Character) {
      if (C == '\\')
        ++I;
      else if ((State == Lexical::String && C == '"') ||
               (State == Lexical::Character && C == '\''))
        State = Lexical::Normal;
      continue;
    }
    if (State == Lexical::LineComment) {
      if (C == '\n')
        State = Lexical::Normal;
      continue;
    }
    if (State == Lexical::BlockComment) {
      if (C == '*' && Next == '/') {
        State = Lexical::Normal;
        ++I;
      }
      continue;
    }
    if (C == '"' || C == '\'') {
      State = C == '"' ? Lexical::String : Lexical::Character;
      continue;
    }
    if (C == '/' && (Next == '/' || Next == '*')) {
      State = Next == '/' ? Lexical::LineComment : Lexical::BlockComment;
      ++I;
      continue;
    }
    if (Open == std::string::npos && !Braces && !Candidate && Identifier(C)) {
      size_t End = I + 1;
      while (End < Limit && Identifier(Source[End]))
        ++End;
      if (Source.slice(I, End).starts_with("neverd_objc_imp_")) {
        size_t NextToken = End;
        while (NextToken < Limit &&
               (Source[NextToken] == ' ' || Source[NextToken] == '\t' ||
                Source[NextToken] == '\n'))
          ++NextToken;
        Candidate = NextToken < Limit && Source[NextToken] == '(';
        Parameters = 0;
      }
      I = End - 1;
      continue;
    }
    if (Candidate) {
      if (C == '(')
        ++Parameters;
      else if (C == ')') {
        if (!Parameters)
          Candidate = false;
        else
          --Parameters;
      } else if (C == ';' && !Parameters)
        Candidate = false;
      else if (C == '{' && !Parameters) {
        Open = I;
        Candidate = false;
      }
    }
    if (C == '{')
      ++Braces;
    else if (C == '}') {
      if (!Braces)
        return std::nullopt;
      if (!--Braces && Open != std::string::npos)
        return std::pair{Open, I + 1};
    }
  }
  return std::nullopt;
}

// The emitter has already rendered and checked the method. This constrained
// edit adds only the exceptional unlock around the rendered runtime calls;
// every other statement retains its existing source binding.
inline bool objcSynchronizedSelfArgument(llvm::StringRef Source, size_t At,
                                         llvm::StringRef Name) {
  if (At == std::string::npos || At + Name.size() > Source.size())
    return false;
  for (llvm::StringRef Argument :
       {"objc_self)", "(void*)(uintptr_t)(objc_self))",
        "(void*)(uintptr_t)((uintptr_t)objc_self))"})
    if (Source.drop_front(At + Name.size()).starts_with(Argument))
      return true;
  return false;
}

inline std::optional<std::string>
objcSynchronizedSuspendARC(llvm::StringRef Source, size_t Begin, size_t End,
                           uint8_t Operations, llvm::StringRef Receiver) {
  if (!Operations || Operations > 3 || Begin >= End || End > Source.size())
    return std::nullopt;
  std::vector<std::pair<size_t, std::string>> Calls;
  std::string Helpers;
  for (const auto &[Operation, Name] :
       {std::pair<uint8_t, llvm::StringRef>{1, "objc_release"},
        {2, "objc_retain"}}) {
    if (!(Operations & Operation))
      continue;
    const std::string Call = (Name + "(").str();
    size_t Count = 0;
    for (size_t At = Source.find(Call, Begin);
         At != std::string::npos && At < End;
         At = Source.find(Call, At + Call.size())) {
      if (++Count > 256)
        return std::nullopt;
      const size_t Line = Source.rfind('\n', At);
      if (Line == std::string::npos)
        return std::nullopt;
      auto Prefix = Source.slice(Line + 1, At).trim();
      if (Operation == 1) {
        if (!Prefix.empty())
          return std::nullopt;
      } else {
        const size_t Assign = Prefix.find('=');
        if (Assign != std::string::npos) {
          const auto Local = Prefix.take_front(Assign).trim();
          if (Local.empty() || llvm::isDigit(Local.front()) ||
              !std::all_of(Local.begin(), Local.end(),
                           [](char C) { return llvm::isAlnum(C) || C == '_'; }))
            return std::nullopt;
          Prefix = Prefix.drop_front(Assign + 1).trim();
        }
        if (Prefix != "(uint64_t)(" && Prefix != "(uint64_t)(uintptr_t)(" &&
            Prefix != "(uint64_t)(uintptr_t)")
          return std::nullopt;
      }
      Calls.emplace_back(At, ("neverd_objc_sync_" + Name.drop_front(5) +
                              "(&neverd_objc_sync_guard, " + Receiver + ", ")
                                 .str());
    }
    if (!Count)
      return std::nullopt;
    if (Operation == 1)
      Helpers +=
          "extern void objc_release(void*);\n"
          "static void neverd_objc_sync_release(void **guard, void *lock, "
          "void *object) {\n"
          "    void *active = *guard ? lock : 0;\n"
          "    *guard = 0;\n"
          "    objc_release(object);\n"
          "    *guard = active;\n"
          "}\n";
    else
      Helpers +=
          "extern void *objc_retain(void*);\n"
          "static void *neverd_objc_sync_retain(void **guard, void *lock, "
          "void *object) {\n"
          "    void *active = *guard ? lock : 0;\n"
          "    *guard = 0;\n"
          "    void *result = objc_retain(object);\n"
          "    *guard = active;\n"
          "    return result;\n"
          "}\n";
  }
  std::sort(Calls.begin(), Calls.end(),
            [](const auto &A, const auto &B) { return A.first > B.first; });
  std::string Result = Source.str();
  for (const auto &[At, Replacement] : Calls) {
    const size_t Length = Source.find('(', At) - At + 1;
    Result.replace(At, Length, Replacement);
  }
  return Helpers + Result;
}

// The emitter may expand a shared unlock into early-return tails. Before
// accepting another rendered unlock, require the preceding one to terminate
// its path with an unconditional return. Ignore literals and comments; reject
// intervening control flow, labels and nested scopes instead of guessing it.
inline bool objcSynchronizedUnlockReturns(llvm::StringRef Source, size_t Exit,
                                          size_t NextExit) {
  const size_t End = Source.find(';', Exit);
  if (End == std::string::npos || End >= NextExit || NextExit > Source.size() ||
      NextExit - End > 16 * 1024 * 1024)
    return false;
  enum class Lexical { Normal, String, Character, LineComment, BlockComment };
  Lexical State = Lexical::Normal;
  bool StatementStart = true, Returning = false;
  unsigned Parentheses = 0;
  for (size_t I = End + 1; I < NextExit; ++I) {
    const char C = Source[I], Next = I + 1 < NextExit ? Source[I + 1] : 0;
    if (State == Lexical::String || State == Lexical::Character) {
      if (C == '\\')
        ++I;
      else if (C == (State == Lexical::String ? '"' : '\''))
        State = Lexical::Normal;
      continue;
    }
    if (State == Lexical::LineComment) {
      if (C == '\n')
        State = Lexical::Normal;
      continue;
    }
    if (State == Lexical::BlockComment) {
      if (C == '*' && Next == '/') {
        State = Lexical::Normal;
        ++I;
      }
      continue;
    }
    if (C == '/' && (Next == '/' || Next == '*')) {
      State = Next == '/' ? Lexical::LineComment : Lexical::BlockComment;
      ++I;
      continue;
    }
    if (llvm::isSpace(C))
      continue;
    if (C == '"' || C == '\'') {
      State = C == '"' ? Lexical::String : Lexical::Character;
      StatementStart = false;
      continue;
    }
    if (C == '{' || C == '}' || C == '#' || (C == ':' && !Parentheses))
      return false;
    if (llvm::isAlpha(C) || C == '_') {
      const size_t Begin = I;
      while (I + 1 < NextExit &&
             (llvm::isAlnum(Source[I + 1]) || Source[I + 1] == '_'))
        ++I;
      const auto Token = Source.slice(Begin, I + 1);
      if (Token == "if" || Token == "switch" || Token == "for" ||
          Token == "while" || Token == "do" || Token == "goto" ||
          Token == "break" || Token == "continue")
        return false;
      if (Token == "return") {
        if (!StatementStart || Parentheses || Returning)
          return false;
        Returning = true;
      }
      StatementStart = false;
    } else if (C == '(') {
      ++Parentheses;
      StatementStart = false;
    } else if (C == ')') {
      if (!Parentheses)
        return false;
      --Parentheses;
    } else if (C == ';' && !Parentheses) {
      if (Returning)
        return true;
      StatementStart = true;
    } else
      StatementStart = false;
  }
  return false;
}

inline std::optional<std::string>
addObjCSynchronizedReceiverCleanup(llvm::StringRef Source,
                                   const ObjCSynchronizedSourceProof &Proof) {
  const std::string EnterName = "neverd_darwin_objc_sync_enter(";
  const std::string DispatchName = "neverd_darwin_dispatch_group_enter(";
  const std::string ExitName = "neverd_darwin_objc_sync_exit(";
  const std::string ReleaseName = "objc_release(";
  const std::string RetainName = "objc_retain(";
  const auto Body = objcSynchronizedSourceBody(Source);
  if (!Body)
    return std::nullopt;
  const auto Scoped = Source.take_front(Body->second);
  const size_t Open = Body->first;
  const size_t Enter = Scoped.find(EnterName, Open);
  const size_t Dispatch = Scoped.find(DispatchName, Open);
  const size_t Exit = Scoped.find(ExitName, Open);
  std::vector<size_t> Exits;
  for (size_t At = Exit; At != std::string::npos;
       At = Scoped.find(ExitName, At + 1)) {
    if (Exits.size() == 256)
      return std::nullopt;
    Exits.push_back(At);
  }
  const bool StopAtExit = Proof.GuardStopCall == Proof.ExitCall;
  const size_t Release = Proof.UnprotectedReleases
                             ? Scoped.find(ReleaseName, Enter)
                             : std::string::npos;
  const size_t Retain = Proof.UnprotectedRetains
                            ? Scoped.find(RetainName, Enter)
                            : std::string::npos;
  const size_t Stop = StopAtExit                  ? Exit
                      : Proof.UnprotectedRetains  ? Retain
                      : Proof.UnprotectedReleases ? Release
                                                  : Dispatch;
  if (Enter == std::string::npos || Stop == std::string::npos ||
      Exit == std::string::npos ||
      Scoped.find(EnterName, Enter + 1) != std::string::npos ||
      (Exits.size() > 1 && (!StopAtExit || Proof.ReceiverIsSavedLocal)) ||
      (Proof.UnprotectedReleases && (Release <= Enter || Release >= Exit)) ||
      (Proof.UnprotectedRetains &&
       (Proof.UnprotectedRetains != 1 || Retain <= Enter || Retain >= Exit ||
        (Proof.UnprotectedReleases && Retain >= Release) ||
        Scoped.find(RetainName, Retain + 1) < Exit)) ||
      (!StopAtExit && !Proof.UnprotectedReleases && !Proof.UnprotectedRetains &&
       Scoped.find(DispatchName, Dispatch + 1) != std::string::npos) ||
      Enter >= Stop || Stop > Exit)
    return std::nullopt;
  if (Proof.UnprotectedReleases) {
    size_t Count = 0;
    for (size_t At = Release; At != std::string::npos && At < Exit;
         At = Scoped.find(ReleaseName, At + 1))
      ++Count;
    if (Count != Proof.UnprotectedReleases)
      return std::nullopt;
  }
  const size_t EnterEnd = Source.find(';', Enter);
  if (EnterEnd == std::string::npos)
    return std::nullopt;
  if (Proof.SuspendARC) {
    if (!StopAtExit || Proof.UnprotectedReleases || Proof.UnprotectedRetains ||
        (!Proof.ReceiverIsSavedLocal &&
         (!objcSynchronizedSelfArgument(Scoped, Enter, EnterName) ||
          !objcSynchronizedSelfArgument(Scoped, Exit, ExitName))))
      return std::nullopt;
  }
  std::string Receiver = "objc_self";
  if (Proof.ReceiverIsSavedLocal) {
    const auto EnterArg =
        objcSynchronizedSavedLocalArgument(Source, Enter, EnterName);
    const auto ExitArg =
        objcSynchronizedSavedLocalArgument(Source, Exit, ExitName);
    if (!EnterArg || !ExitArg)
      return std::nullopt;
    const auto LocalName = [](llvm::StringRef Argument) {
      return Argument.drop_front(llvm::StringRef("(void*)(uintptr_t)(").size())
          .drop_back();
    };
    const auto EnterLocal = LocalName(*EnterArg);
    const auto ExitLocal = LocalName(*ExitArg);
    if (*EnterArg != *ExitArg) {
      if (!Proof.ReceiverHasStackCopy)
        return std::nullopt;
      const std::string Copy = (ExitLocal + " = " + EnterLocal + ";").str();
      const size_t At = Scoped.find(Copy, Open);
      const size_t Line =
          At == std::string::npos ? std::string::npos : Scoped.rfind('\n', At);
      const size_t LineEnd =
          At == std::string::npos ? std::string::npos : Scoped.find('\n', At);
      if (At == std::string::npos || Line == std::string::npos ||
          LineEnd == std::string::npos || LineEnd >= Enter ||
          Scoped.slice(Line + 1, LineEnd).trim() != Copy ||
          Scoped.slice(At + Copy.size(), Enter).contains(EnterLocal) ||
          Scoped.slice(At + Copy.size(), Enter).contains(ExitLocal))
        return std::nullopt;
    }
    if (Source.slice(EnterEnd + 1, Exit).contains(EnterLocal) ||
        Source.slice(EnterEnd + 1, Exit).contains(ExitLocal))
      return std::nullopt;
    Receiver = *EnterArg;
  }
  const size_t StopLine = Source.rfind('\n', Stop);
  const size_t ExitLine = Source.rfind('\n', Exit);
  if (EnterEnd == std::string::npos || StopLine == std::string::npos ||
      ExitLine == std::string::npos || EnterEnd >= StopLine ||
      (!StopAtExit && StopLine >= ExitLine) ||
      (!StopAtExit &&
       Source.substr(StopLine + 1, Stop - StopLine - 1).trim() != "" &&
       (!Proof.UnprotectedRetains ||
        (Source.substr(StopLine + 1, Stop - StopLine - 1).trim() !=
             "(uint64_t)(" &&
         Source.substr(StopLine + 1, Stop - StopLine - 1).trim() !=
             "(uint64_t)(uintptr_t)("))) ||
      Source.substr(ExitLine + 1, Exit - ExitLine - 1).trim() != "(uint32_t)(")
    return std::nullopt;
  std::string Result = Source.str();
  if (Exits.size() > 1) {
    // HighC can expand a shared unlock tail into mutually exclusive returns.
    // Each occurrence must still use the proved self receiver and be a whole
    // statement. Clear the guard on every rendered normal-unlock path.
    if (!objcSynchronizedSelfArgument(Scoped, Enter, EnterName))
      return std::nullopt;
    for (size_t I = 1; I < Exits.size(); ++I)
      if (!objcSynchronizedUnlockReturns(Scoped, Exits[I - 1], Exits[I]))
        return std::nullopt;
    for (auto It = Exits.rbegin(); It != Exits.rend(); ++It) {
      const size_t Line = Source.rfind('\n', *It);
      if (*It <= EnterEnd || Line == std::string::npos ||
          Scoped.slice(Line + 1, *It).trim() != "(uint32_t)(" ||
          !objcSynchronizedSelfArgument(Scoped, *It, ExitName))
        return std::nullopt;
      size_t IndentEnd = Line + 1;
      while (IndentEnd < *It &&
             (Source[IndentEnd] == ' ' || Source[IndentEnd] == '\t'))
        ++IndentEnd;
      Result.insert(Line + 1, Source.slice(Line + 1, IndentEnd).str() +
                                  "neverd_objc_sync_guard = 0;\n");
    }
  } else
    Result.insert(StopLine + 1, "    neverd_objc_sync_guard = 0;\n");
  Result.insert(EnterEnd + 1,
                "\n    neverd_objc_sync_guard = " + Receiver + ";");
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
  if (Proof.SuspendARC) {
    const auto RenderedBody = objcSynchronizedSourceBody(Result);
    if (!RenderedBody)
      return std::nullopt;
    const size_t RenderedEnter = Result.find(EnterName, RenderedBody->first);
    // A shared machine unlock can be duplicated into early HighC returns.
    // ARC calls after an earlier rendered unlock must leave its guard zero;
    // suspend every occurrence through the last normal unlock, preserving
    // whether that path still owns the lock.
    const size_t RenderedExit = Result.rfind(ExitName, RenderedBody->second);
    const size_t RenderedEnterEnd = Result.find(';', RenderedEnter);
    return objcSynchronizedSuspendARC(Result, RenderedEnterEnd + 1,
                                      RenderedExit, Proof.SuspendARC, Receiver);
  }
  return Result;
}

} // namespace neverd::sdk

#endif
