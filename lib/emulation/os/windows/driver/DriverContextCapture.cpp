//===- DriverContextCapture.cpp - Checked caller-context CPU bridge ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Collect live checked CPU values without consulting host OS state.
//===----------------------------------------------------------------------===//
#include "DriverContextCapture.h"

#include "../exception/WindowsX64Context.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation {
llvm::Expected<KernelCapturedContext>
collectDriverContext(ExecutionBackend &CPU) {
  KernelCapturedContext State;
  if (auto E = windows_context::collectX64IntegerContext(CPU, State.Bytes))
    return E;
  using namespace llvm::support::endian;
  windows_context::writeFlatDataSelectors(
      State.Bytes, read16le(State.Bytes.data() + seh::ContextSSOffset));
  auto FPCW = CPU.readRegister(CPURegister::X64FPCW);
  if (!FPCW)
    return FPCW.takeError();
  write16le(State.Bytes.data() + seh::ContextFPOffset, (*FPCW)[0]);
  write32le(State.Bytes.data() + seh::ContextFXMXCSROffset,
            read32le(State.Bytes.data() + seh::ContextMXCSROffset));
  for (unsigned I = 0; I < 16; ++I) {
    auto Value = CPU.xmm(I);
    if (!Value)
      return Value.takeError();
    auto *Output = State.Bytes.data() + seh::ContextXmmOffset + I * 16;
    write64le(Output, (*Value)[0]);
    write64le(Output + 8, (*Value)[1]);
  }
  return State;
}
} // namespace neverd::emulation
