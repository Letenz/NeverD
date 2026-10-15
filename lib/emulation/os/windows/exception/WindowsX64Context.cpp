//===- WindowsX64Context.cpp - Shared Windows x64 context collection -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Collect shared Windows x64 integer fields and logical data selectors.
//===----------------------------------------------------------------------===//
#include "WindowsX64Context.h"

#include "../../../arch/x86_64/X64Machine.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::windows_context {
llvm::Error collectX64IntegerContext(ExecutionBackend &CPU,
                                     llvm::MutableArrayRef<uint8_t> Bytes,
                                     bool HardwareFault) {
  if (CPU.architecture() != GuestArchitecture::X64 ||
      Bytes.size() != seh::ContextSize)
    return llvm::createStringError("invalid Windows x64 context collection");
  struct Field {
    CPURegister Register;
    unsigned Offset, Size;
  };
  constexpr Field Fields[] = {
#define NEVERD_WINDOWS_CONTEXT_X64(Name, Offset, Size, Mask)                   \
  {CPURegister::X64##Name, Offset, Size},
#include "WindowsX64Context.def"
#undef NEVERD_WINDOWS_CONTEXT_X64
  };
  for (const auto &F : Fields) {
    auto Value = CPU.readRegister(F.Register);
    if (!Value)
      return Value.takeError();
    if (HardwareFault && F.Register == CPURegister::X64FLAGS)
      (*Value)[0] |= x64::ResumeFlag;
    for (unsigned I = 0; I < F.Size; ++I)
      Bytes[F.Offset + I] = (*Value)[0] >> (I * 8);
  }
  return llvm::Error::success();
}

void writeFlatDataSelectors(llvm::MutableArrayRef<uint8_t> Bytes,
                            uint16_t Selector) {
  assert(Bytes.size() == seh::ContextSize);
#define NEVERD_WINDOWS_CAPTURE_DATA_SEGMENT(Offset)                            \
  llvm::support::endian::write16le(Bytes.data() + Offset, Selector);
#include "WindowsX64Context.def"
#undef NEVERD_WINDOWS_CAPTURE_DATA_SEGMENT
}
} // namespace neverd::emulation::windows_context
