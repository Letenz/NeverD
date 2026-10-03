//===- WindowsProcessExceptions.h - User exception continuations ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WINDOWS_PROCESS_EXCEPTIONS_H
#define NEVERD_EMULATION_WINDOWS_PROCESS_EXCEPTIONS_H
#include "WindowsProcess.h"

#include "neverd/emulation/CPU.h"

#include <list>

namespace neverd::emulation::windows_process {
class VectoredExceptions final {
public:
  using Exception = ServiceOutcome::Exception;
  struct Transfer {
    uint64_t PC;
    std::optional<size_t> CompletedEvent;
  };
  VectoredExceptions(ExecutionBackend &CPU, const IntegerABI &ABI,
                     uint64_t StackBase)
      : CPU(CPU), ABI(ABI), StackBase(StackBase) {}
  llvm::Expected<uint64_t> add(bool First, uint64_t Handler);
  uint64_t remove(uint64_t Handle);
  static bool recoverable(GuestArchitecture Architecture,
                          const BackendFault &Fault);
  bool accepts(const BackendFault &Fault) const;
  static Exception exception(const BackendFault &Fault);
  llvm::Expected<Transfer> begin(Exception Raised, uint64_t StackPointer,
                                 size_t LoaderDepth,
                                 std::optional<size_t> Event = std::nullopt);
  bool activeAt(size_t LoaderDepth) const;
  bool returning(uint64_t PC, uint64_t SP, size_t LoaderDepth) const;
  llvm::Expected<Transfer> returned(uint32_t Disposition);
  void abandon() { Frames.clear(); }

private:
  struct Handler {
    uint64_t Handle, PC;
    bool Live = true;
  };
  struct Frame {
    std::unique_ptr<BackendContext> Snapshot;
    std::vector<uint8_t> Context;
    std::list<Handler>::iterator Current;
    uint64_t Top, Payload, ExpectedSP;
    size_t LoaderDepth;
    uint32_t Flags;
    std::optional<size_t> Event;
  };
  llvm::Expected<Transfer> callNext();
  void collect();
  ExecutionBackend &CPU;
  IntegerABI ABI;
  uint64_t StackBase, NextHandle = value::ExceptionHandleBase;
  std::list<Handler> Handlers;
  std::vector<Frame> Frames;
};
} // namespace neverd::emulation::windows_process
#endif
