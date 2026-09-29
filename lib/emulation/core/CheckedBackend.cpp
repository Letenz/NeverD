//===- CheckedBackend.cpp - Shared checked execution lifecycle -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "CheckedBackend.h"

#include "ExecutionDiagnostics.h"

#include "llvm/ADT/ScopeExit.h"

namespace neverd::emulation {
using diagnostic::error;
llvm::Error CheckedBackend::initializeDecoder(cs_arch Arch, cs_mode Mode) {
  if (cs_open(Arch, Mode, &Decoder) != CS_ERR_OK ||
      cs_option(Decoder, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK)
    return error(diagnostic::Decode);
  return llvm::Error::success();
}
CheckedBackend::~CheckedBackend() {
  if (Decoder)
    cs_close(&Decoder);
}

llvm::Error CheckedBackend::mutableMemory() const {
  if (FirstFault || RecoverableFault)
    return error(diagnostic::Faulted);
  if (Running)
    return error(diagnostic::Running);
  return llvm::Error::success();
}

llvm::Error CheckedBackend::map(uint64_t A, uint64_t N, unsigned P) {
  if (auto E = mutableMemory())
    return E;
  if (!canonicalRange(A, N))
    return error(diagnostic::InvalidMapping);
  return Memory->map(A, N, P);
}

llvm::Error CheckedBackend::mapAlias(uint64_t A, uint64_t S, uint64_t N,
                                     unsigned P) {
  return replaceAliases({}, {GuestAliasMapping{A, S, N, P}});
}

llvm::Error CheckedBackend::unmapAlias(uint64_t A, uint64_t N) {
  return replaceAliases({GuestAliasRange{A, N}}, {});
}

llvm::Error
CheckedBackend::replaceAliases(llvm::ArrayRef<GuestAliasRange> Remove,
                               llvm::ArrayRef<GuestAliasMapping> Add) {
  if (auto E = mutableMemory())
    return E;
  for (const auto &R : Add)
    if (!canonicalRange(R.Address, R.Size) || !canonicalRange(R.Source, R.Size))
      return error(diagnostic::InvalidMapping);
  return Memory->aliases(Remove, Add);
}

llvm::Error CheckedBackend::protect(uint64_t A, uint64_t N, unsigned P) {
  if (auto E = mutableMemory())
    return E;
  return Memory->protect(A, N, P);
}

llvm::Error CheckedBackend::access(uint64_t A, uint64_t N, unsigned P,
                                   bool Recoverable) {
  if (FirstFault)
    return error(diagnostic::Faulted);
  if (auto Kind = Memory->check(A, N, P)) {
    auto Access = P == Execute ? BackendAccessKind::Execute
                  : P == Write ? BackendAccessKind::Write
                               : BackendAccessKind::Read;
    BackendFault F{*Kind, programCounter(), A, N, Access, std::nullopt};
    if (Recoverable && P != Execute && Hooks.RecoverableFault &&
        Hooks.RecoverableFault(F)) {
      RecoverableFault = F;
      StopRequested = true;
      return llvm::Error::success();
    }
    FirstFault = F;
    if (Hooks.Fault)
      Hooks.Fault(A, N, backendAccessKindName(Access));
    return error(diagnostic::MemoryAccess);
  }
  return llvm::Error::success();
}

llvm::Error CheckedBackend::read(uint64_t A, llvm::MutableArrayRef<uint8_t> B) {
  if (auto E = access(A, B.size(), Read))
    return E;
  return Memory->read(A, B);
}

llvm::Error CheckedBackend::write(uint64_t A, llvm::ArrayRef<uint8_t> B) {
  if (auto E = mutableMemory())
    return E;
  if (auto E = access(A, B.size(), Write))
    return E;
  return Memory->write(A, B);
}

llvm::Error CheckedBackend::fetch(uint64_t A,
                                  llvm::MutableArrayRef<uint8_t> B) {
  if (auto E = access(A, B.size(), Execute))
    return E;
  return Memory->read(A, B, Execute);
}

llvm::Expected<bool> CheckedBackend::canAccess(uint64_t A, uint64_t N,
                                               unsigned P) const {
  if (FirstFault || (P & ~(Read | Write | Execute)))
    return error(diagnostic::Faulted);
  return !Memory->check(A, N, P);
}

llvm::Error CheckedBackend::validateBacking(uint64_t A, uint64_t N) const {
  if (auto E = mutableMemory())
    return E;
  if (Memory->check(A, N, 0))
    return error(diagnostic::MemoryAccess);
  return llvm::Error::success();
}

llvm::Error CheckedBackend::readBacking(uint64_t A,
                                        llvm::MutableArrayRef<uint8_t> B) {
  if (auto E = validateBacking(A, B.size()))
    return E;
  return Memory->read(A, B, 0);
}

llvm::Error CheckedBackend::writeBacking(uint64_t A,
                                         llvm::ArrayRef<uint8_t> B) {
  if (auto E = validateBacking(A, B.size()))
    return E;
  return Memory->write(A, B, 0);
}

llvm::Error CheckedBackend::snapshotBacking(uint64_t A,
                                            llvm::MutableArrayRef<uint8_t> B) {
  if (Running)
    return error(diagnostic::Running);
  return Memory->read(A, B, 0);
}

llvm::Error CheckedBackend::installHooks(BackendHooks H) {
  if (auto E = mutableMemory())
    return E;
  Hooks = std::move(H);
  return llvm::Error::success();
}

std::optional<BackendFault> CheckedBackend::takeRecoverableFault() {
  return std::exchange(RecoverableFault, std::nullopt);
}

llvm::Error CheckedBackend::run(uint64_t PC, uint64_t Timeout) {
  if (auto E = mutableMemory())
    return E;
  setProgramCounter(PC);
  Running = true;
  TimedOut = false;
  StopRequested = false;
  auto Reset = llvm::scope_exit([&] { Running = false; });
  Deadline =
      std::chrono::steady_clock::now() + std::chrono::microseconds(Timeout);
  try {
    while (!StopRequested) {
      if (std::chrono::steady_clock::now() >= Deadline) {
        TimedOut = true;
        break;
      }
      PC = programCounter();
      if (PC % InstructionAlignment) {
        FirstFault = BackendFault{BackendFaultKind::InvalidInstruction, PC};
        if (Hooks.InvalidInstruction)
          Hooks.InvalidInstruction();
        return llvm::make_error<UnsupportedExecutionError>();
      }
      std::vector<uint8_t> Bytes(MaxInstructionBytes);
      size_t Count = 0;
      for (; Count < Bytes.size() && Count <= UINT64_MAX - PC; ++Count) {
        if (Memory->check(PC + Count, 1, Execute))
          break;
        if (auto E = Memory->read(
                PC + Count, llvm::MutableArrayRef<uint8_t>(&Bytes[Count], 1),
                Execute))
          return E;
      }
      cs_insn *Decoded = nullptr;
      if (!Count)
        return access(PC, 1, Execute);
      if (!cs_disasm(Decoder, Bytes.data(), Count, PC, 1, &Decoded)) {
        FirstFault = BackendFault{BackendFaultKind::InvalidInstruction, PC};
        if (Hooks.InvalidInstruction)
          Hooks.InvalidInstruction();
        return llvm::make_error<UnsupportedExecutionError>();
      }
      auto Free = llvm::scope_exit([&] { cs_free(Decoded, 1); });
      if (Hooks.Instruction)
        Hooks.Instruction(PC, Decoded->size);
      if (FirstFault)
        return error(diagnostic::Faulted);
      if (StopRequested)
        break;
      if (auto E = execute(*Decoded)) {
        if (!FirstFault) {
          const bool Unsupported = E.isA<UnsupportedExecutionError>();
          FirstFault =
              BackendFault{Unsupported ? BackendFaultKind::InvalidInstruction
                                       : BackendFaultKind::UnhandledException,
                           PC};
          if (Unsupported && Hooks.InvalidInstruction)
            Hooks.InvalidInstruction();
        }
        return E;
      }
      if (FirstFault)
        return error(diagnostic::Faulted);
    }
  } catch (...) {
    FirstFault =
        BackendFault{BackendFaultKind::UnhandledException, programCounter()};
    return error(diagnostic::Callback);
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
