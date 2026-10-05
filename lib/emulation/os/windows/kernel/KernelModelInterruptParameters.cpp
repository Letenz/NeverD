//===- KernelModelInterruptParameters.cpp - WDK interrupt arguments -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Keep fixed argument decoding and versioned guest records separate from
/// interrupt registration and ownership.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
llvm::Error apiError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "interrupt API: " + Message);
}
} // namespace

llvm::Expected<uint64_t>
KernelModel::connectInterrupt(llvm::ArrayRef<uint64_t> A) {
  InterruptParameters Params;
  Params.Output = A[0];
  Params.Routine = A[1];
  Params.Context = A[2];
  Params.SpinLock = A[3];
  Params.Vector = uint32_t(A[4]);
  Params.IRQL = uint8_t(A[5]);
  Params.Synchronize = uint8_t(A[6]);
  Params.Mode = uint32_t(A[7]);
  Params.Share = uint8_t(A[8]);
  Params.Affinity = A[9];
  Params.Floating = uint8_t(A[10]);
  return registerInterrupt(Params);
}

llvm::Expected<uint64_t> KernelModel::connectInterruptEx(uint64_t Record) {
  InterruptParameters Params;
  Params.Record = Record;
  if (auto E = validateGuestAccess(Record, 4, false))
    return E;
  auto VersionField = Memory.readInteger(Record, 4);
  if (!VersionField)
    return VersionField.takeError();
  Params.Version = uint32_t(*VersionField);
  if (Params.Version != interrupts::FullySpecified &&
      Params.Version != interrupts::FullySpecifiedGroup &&
      Params.Version != interrupts::LineBased &&
      Params.Version != interrupts::MessageBased &&
      Params.Version != interrupts::MessageBasedPassive)
    return apiError("unsupported Ex connect version");
  Params.LineBased = Params.Version == interrupts::LineBased;
  Params.MessageBased = Params.Version == interrupts::MessageBased ||
                        Params.Version == interrupts::MessageBasedPassive;
  Params.Passive = Params.Version == interrupts::MessageBasedPassive;
  struct Field {
    uint64_t Offset;
    unsigned Size;
    uint64_t *Value;
  };
  std::vector<Field> Fields{
      {interrupts::PDOOffset, 8, &Params.PDO},
      {interrupts::OutputOffset, 8, &Params.Output},
      {interrupts::RoutineOffset, 8, &Params.Routine},
      {interrupts::ContextOffset, 8, &Params.Context},
      {interrupts::SpinLockOffset, 8, &Params.SpinLock},
      {interrupts::SynchronizeIRQL, 1, &Params.Synchronize},
      {interrupts::FloatingSave, 1, &Params.Floating}};
  if (Params.MessageBased)
    Fields.push_back({interrupts::FallbackRoutine, 8, &Params.Fallback});
  if (!Params.LineBased && !Params.MessageBased) {
    Fields.push_back({interrupts::ShareVector, 1, &Params.Share});
    Fields.push_back({interrupts::Vector, 4, &Params.Vector});
    Fields.push_back({interrupts::IRQL, 1, &Params.IRQL});
    Fields.push_back({interrupts::Mode, 4, &Params.Mode});
    Fields.push_back({interrupts::Affinity, 8, &Params.Affinity});
    if (Params.Version == interrupts::FullySpecifiedGroup)
      Fields.push_back({interrupts::Group, 2, &Params.Group});
  }
  for (const Field &Field : Fields) {
    if (Field.Offset > UINT64_MAX - Record)
      return apiError("Ex parameter field address overflows");
    if (auto E = validateGuestAccess(Record + Field.Offset, Field.Size, false))
      return E;
    auto Value = Memory.readInteger(Record + Field.Offset, Field.Size);
    if (!Value)
      return Value.takeError();
    *Field.Value = *Value;
  }
  if (!Params.PDO || !isProviderDevice(Params.PDO))
    return apiError("Ex registration requires its configured PDO");
  return registerInterrupt(Params);
}

llvm::Expected<uint64_t> KernelModel::disconnectInterruptEx(uint64_t Record) {
  uint64_t Object = 0;
  uint32_t Version = 0;
  if (auto E = validateGuestAccess(Record, interrupts::DisconnectSize, false))
    return E;
  auto VersionField = Memory.readInteger(Record + interrupts::VersionOffset, 4);
  auto Context = Memory.readInteger(Record + interrupts::DisconnectContext, 8);
  if (!VersionField || !Context)
    return llvm::joinErrors(VersionField.takeError(), Context.takeError());
  Version = uint32_t(*VersionField);
  if (Version != interrupts::FullySpecified &&
      Version != interrupts::FullySpecifiedGroup &&
      Version != interrupts::LineBased && Version != interrupts::MessageBased &&
      Version != interrupts::MessageBasedPassive)
    return apiError("unsupported Ex disconnect version");
  Object = *Context;
  return disconnectInterrupt(Object, Version);
}
} // namespace neverd::emulation
