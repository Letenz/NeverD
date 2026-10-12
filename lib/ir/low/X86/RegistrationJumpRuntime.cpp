//===- RegistrationJumpRuntime.cpp - Checked PE32 jump runtime binding ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Match the original runtime import and physical veneer for nonlocal jumps.

#include "RegistrationJumpRuntime.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

namespace neverd::registration_state {

std::optional<JumpRuntimeBinding>
checkedJumpRuntime(const BinaryImage &Image, va_t Target, bool Indirect) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || Target > UINT32_MAX)
    return std::nullopt;
  va_t Slot = Target;
  if (!Indirect) {
    if (Target > UINT32_MAX - 6)
      return std::nullopt;
    const auto Bytes =
        readImmutablePE32CodeBytes(Image, Target, 6, {Target + 2});
    if (!Bytes || (*Bytes)[0] != 0xff || (*Bytes)[1] != 0x25)
      return std::nullopt;
    Slot = readLE<uint32_t>(Bytes->data() + 2);
  }
  const auto *Imported = Image.findImportAt(Slot);
  if (!Imported || Imported->IATAddr != Slot || Imported->IsDelayImport ||
      !isFileBackedPEImportSlot(Image, Slot))
    return std::nullopt;
  const llvm::StringRef Module = Imported->Module;
  if (!Module.equals_insensitive("msvcrt.dll") &&
      !Module.equals_insensitive("vcruntime140.dll") &&
      !Module.equals_insensitive("vcruntime140d.dll"))
    return std::nullopt;
  JumpRuntimeBinding Binding;
  if (Imported->Name == "_setjmp3")
    Binding.Routine = JumpRuntimeBinding::Kind::Capture;
  else if (Imported->Name == "_longjmpex")
    Binding.Routine = JumpRuntimeBinding::Kind::Resume;
  else if (Imported->Name == "_seh_longjmp_unwind")
    Binding.Routine = JumpRuntimeBinding::Kind::EH3Unwind;
  else
    return std::nullopt;
  Binding.Target = Target;
  Binding.ImportSlot = Slot;
  Binding.Indirect = Indirect;
  return Binding;
}

} // namespace neverd::registration_state
