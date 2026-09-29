//===- CheckedBackend.cpp - Shared checked execution lifecycle -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "CheckedBackend.h"

#include "ExecutionDiagnostics.h"
#include "ExecutionExitBuilder.h"

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
  return Memory->mutableMemory();
}

llvm::Error
CheckedBackend::bindAddressSpace(std::shared_ptr<AddressSpace> Space) {
  if (auto E = mutableMemory())
    return E;
  return Memory->bind(std::move(Space), [this](uint64_t A, uint64_t N) {
    return canonicalRange(A, N);
  });
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
  auto Lock = Memory->lock();
  if (!Lock)
    return Lock.takeError();
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
  auto Lock = Memory->lock();
  if (!Lock)
    return Lock.takeError();
  if (auto E = access(A, B.size(), Execute))
    return E;
  return Memory->read(A, B, Execute);
}

llvm::Expected<bool> CheckedBackend::canAccess(uint64_t A, uint64_t N,
                                               unsigned P) const {
  if (FirstFault || (P & ~(Read | Write | Execute)))
    return error(diagnostic::Faulted);
  return Memory->addressSpace()->canAccess(A, N, P);
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
  return Memory->addressSpace()->snapshotBacking(A, B);
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

llvm::Expected<ExecutionExit> CheckedBackend::runUntilExit(uint64_t PC,
                                                           uint64_t Timeout) {
  bool Started = false, BackendFailed = false;
  auto E = runImpl(PC, Timeout, Started, BackendFailed);
  if (!Started) {
    if (E)
      return std::move(E);
    return error(diagnostic::MissingExecutionStart);
  }
  // A transport can fail while enforcing the deadline. Its terminal failure
  // takes precedence, but the elapsed budget must not disappear from the exit.
  if (BackendFailed && std::chrono::steady_clock::now() >= Deadline)
    TimedOut = true;
  return makeExecutionExit(
      std::move(E), {.Fault = FirstFault,
                     .Recoverable = RecoverableFault,
                     .BackendFailed = BackendFailed,
                     .InstructionRejected =
                         FirstFault && FirstFault->Kind ==
                                           BackendFaultKind::InvalidInstruction,
                     .StopRequested = StopRequested,
                     .DeadlineReached = TimedOut});
}

llvm::Error CheckedBackend::runImpl(uint64_t PC, uint64_t Timeout,
                                    bool &Started, bool &BackendFailed) {
  if (auto E = mutableMemory())
    return E;
  if (auto E = Memory->beginRun())
    return E;
  auto Release = llvm::scope_exit([&] { Memory->endRun(); });
  if (auto E = Memory->validateMappings(
          [this](uint64_t A, uint64_t N) { return canonicalRange(A, N); }))
    return E;
  setProgramCounter(PC);
  Running = true;
  Started = true;
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
          BackendFailed = !Unsupported;
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
    // A secondary observer failure must not replace the original guest fault.
    if (!FirstFault) {
      BackendFailed = true;
      FirstFault =
          BackendFault{BackendFaultKind::UnhandledException, programCounter()};
    }
    return error(diagnostic::Callback);
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
