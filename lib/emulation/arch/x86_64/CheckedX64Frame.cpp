//===- CheckedX64Frame.cpp - Ordered frame entry effects ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../core/RAMTransaction.h"
#include "CheckedX64Backend.h"
#include "X64Exception.h"

#include "llvm/ADT/STLExtras.h"

#include <climits>
#include <map>

namespace neverd::emulation {
llvm::Error CheckedX64Backend::executeEnter(const cs_insn &I) {
  const auto &X = I.detail->x86;
  if (X.op_count != 2 || X.operands[0].type != X86_OP_IMM ||
      X.operands[1].type != X86_OP_IMM || I.size < x64::EnterInstructionBytes)
    return llvm::make_error<UnsupportedExecutionError>();
  // Immediate bytes may equal prefix encodings. Only inspect the actual
  // prefix span; Capstone can omit ignored REP prefixes from its metadata.
  if (llvm::any_of(
          llvm::ArrayRef(I.bytes, I.size).drop_back(x64::EnterInstructionBytes),
          [](uint8_t B) {
            if ((B & x64::RexPrefixMask) == x64::RexPrefixBase)
              return false;
            switch (B) {
            case X86_PREFIX_OPSIZE:
            case X86_PREFIX_ADDRSIZE:
            case X86_PREFIX_CS:
            case X86_PREFIX_SS:
            case X86_PREFIX_DS:
            case X86_PREFIX_ES:
            case X86_PREFIX_FS:
            case X86_PREFIX_GS:
              return false;
            default:
              return true;
            }
          }))
    return llvm::make_error<UnsupportedExecutionError>();

  const unsigned Width = implicitStackWidth(X);
  // The decoder can sign-extend the nesting immediate. ENTER interprets its
  // encoded byte modulo 32 and the allocation as an unsigned 16-bit value.
  const auto *Immediate = I.bytes + I.size - x64::EnterImmediateBytes;
  const unsigned Allocate = Immediate[0] | (unsigned(Immediate[1]) << CHAR_BIT);
  const unsigned Level = Immediate[2] & x64::EnterNestingMask;
  const uint64_t SP = CPU.reg(X64Register::SP);
  const uint64_t BP = CPU.reg(X64Register::BP);
  const uint64_t Frame = SP - Width;
  const uint64_t Final = Frame - Level * Width - Allocate;
  const uint64_t Mask = UINT64_MAX >> (x64::WordBits - Width * CHAR_BIT);
  struct FrameAccess {
    uint64_t Address;
    unsigned Permission;
    uint64_t Value;
    bool Copy = false, Probe = false;
  };
  std::vector<FrameAccess> Accesses{{Frame, Write, BP & Mask}};
  for (unsigned N = 1; N < Level; ++N) {
    Accesses.push_back({BP - N * Width, Read, 0});
    Accesses.push_back({Frame - N * Width, Write, 0, true});
  }
  if (Level)
    Accesses.push_back({Frame - Level * Width, Write, Frame & Mask});
  // Intel ENTER checks write permission over the operand width at final RSP.
  // This is not a store and must produce neither a data write nor an observer.
  Accesses.push_back({Final, Write, 0, false, true});

  // Device frames cannot enter an ordinary-RAM transaction. Inspect the
  // complete bounded sequence before callbacks or any architectural effects.
  for (const auto &A : Accesses) {
    if (deviceAt(A.Address) || (Width - 1 <= UINT64_MAX - A.Address &&
                                deviceAt(A.Address + Width - 1)))
      return llvm::make_error<UnsupportedExecutionError>();
  }
  if (auto E = Memory->prepareWrite())
    return E;

  // Plan values in architectural access order without changing backing RAM.
  // ENTER may read its earlier writes, even through a different virtual alias.
  // Physical identity is therefore the authority for this small overlay.
  std::map<uint64_t, uint8_t> Pending;
  std::vector<RAMWriteRange> Writes;
  auto Physical = [&](uint64_t A) {
    const auto &P = Memory->mappings().at(A & ~(x64::PageSize - 1));
    return P.Physical + A % x64::PageSize;
  };
  size_t Completed = 0;
  uint64_t Copied = 0;
  for (auto &A : Accesses) {
    if (Memory->firstAccessFailure(A.Address, Width,
                                   executionPermissions(A.Permission)))
      break;
    if (A.Permission == Read) {
      Copied = 0;
      for (unsigned N = 0; N < Width; ++N) {
        const uint64_t P = Physical(A.Address + N);
        const auto Previous = Pending.find(P);
        const uint8_t Byte = Previous == Pending.end()
                                 ? *Memory->physicalPointer(P)
                                 : Previous->second;
        Copied |= uint64_t(Byte) << (N * CHAR_BIT);
      }
    } else if (!A.Probe) {
      if (A.Copy)
        A.Value = Copied;
      Writes.push_back({A.Address, Width});
      for (unsigned N = 0; N < Width; ++N)
        Pending[Physical(A.Address + N)] = uint8_t(A.Value >> (N * CHAR_BIT));
    }
    ++Completed;
  }
  for (const auto &A : llvm::ArrayRef(Accesses).take_front(Completed)) {
    if (StopRequested || FirstFault)
      return llvm::Error::success();
    if (A.Permission == Read && Hooks.Read)
      Hooks.Read(A.Address, Width);
    if (A.Permission == Write && !A.Probe && Hooks.Write)
      Hooks.Write(A.Address, Width, A.Value);
  }
  if (StopRequested || FirstFault)
    return llvm::Error::success();

  if (Completed != Accesses.size()) {
    // A guest fault commits only stores preceding the first failing access.
    // RSP/RBP/PC retain their entry values; repair and restart must consume
    // the resulting RAM, which may differ from the first attempt's input.
    for (const auto &[P, Byte] : Pending) {
      Memory->recordRAMWrite(P, sizeof(Byte));
      *Memory->physicalPointer(P) = Byte;
    }
    const auto &A = Accesses[Completed];
    return access(A.Address, Width, A.Permission, true, true);
  }

  // The successful path executes the original instruction on the selected
  // processor. The overlay only predicts observations and fault-prefix stores;
  // it never substitutes for the processor's successful register/RAM result.
  auto Root = buildX64PageTables(*Memory, UserMode,
                                 Machine->requiresExceptionMonitor());
  if (!Root)
    return Root.takeError();
  auto Transaction = RAMTransaction::create(
      *Memory, Writes, execution_limits::InstructionRAMWriteBytes,
      executionPermissions(Write));
  if (!Transaction)
    return Transaction.takeError();
  auto Next = CPU;
  std::vector<RAMWriteRange> Inputs{{I.address, I.size}};
  for (const auto &A : Accesses)
    if (!A.Probe)
      Inputs.push_back({A.Address, Width});
  if (auto E = (*Transaction)
                   ->execute(
                       [&] {
                         return Machine->step(Next, *Root,
                                              {Deadline, &StopRequested});
                       },
                       Inputs)) {
    return llvm::handleErrors(
        std::move(E), [&](const X64ExceptionError &E) -> llvm::Error {
          if (auto Stage = (*Transaction)->stage())
            return Stage;
          if (StopRequested || FirstFault)
            return llvm::Error::success();
          if (auto Commit = (*Transaction)->commit())
            return Commit;
          CPU = Next;
          BackendFault Fault{BackendFaultKind::Interrupt, I.address};
          Fault.Interrupt = E.exception().Vector;
          Fault.Address = E.exception().FaultAddress;
          Fault.ErrorCode = E.exception().ErrorCode;
          return raiseFault(Fault, true);
        });
  }
  if (auto E = (*Transaction)->stage())
    return E;
  if (StopRequested || FirstFault)
    return llvm::Error::success();
  if (auto E = (*Transaction)->commit())
    return E;
  CPU = Next;
  return llvm::Error::success();
}
} // namespace neverd::emulation
