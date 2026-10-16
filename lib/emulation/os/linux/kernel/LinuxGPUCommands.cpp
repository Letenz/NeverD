//===- LinuxGPUCommands.cpp - Explicit synchronous A3xx command subset ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Executes admitted PM4 memory packets against retained guest RAM. This is
/// an opt-in command processor, not a shader, display, cache or timing model.
//===----------------------------------------------------------------------===//
#include "LinuxFiles.h"
#include "LinuxKGSL.h"
#include "LinuxMemory.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::linux_model {
llvm::Expected<std::optional<uint64_t>>
LinuxFiles::gpuIoctl(LinuxDeviceState &Device, uint32_t Request,
                     uint64_t Argument, ProcessResult &Result) {
  using Outcome = std::optional<uint64_t>;
  constexpr uint32_t Map = kgsl::MapUserMemory, Free = kgsl::FreeMemory;
  constexpr uint32_t Submit = kgsl::SubmitCommands, Wait = kgsl::WaitTimestamp;
  if (!Memory || !Device.GPU)
    return unsupported(Result,
                       "GPU commands require owned memory and context state");
  const unsigned Size = Request == Map      ? 48
                        : Request == Free   ? 8
                        : Request == Submit ? 32
                                            : 12;
  if (Argument > Layout.UserLimit || Size > Layout.UserLimit - Argument)
    return Outcome(uint64_t(0) - BadAddress);
  auto Readable = CPU.canAccess(Argument, Size, Read | UserAccessible);
  if (!Readable)
    return Readable.takeError();
  if (!*Readable)
    return Outcome(uint64_t(0) - BadAddress);
  uint8_t Data[48]{};
  if (auto E = CPU.read(Argument, llvm::MutableArrayRef(Data, Size)))
    return std::move(E);
  auto Get32 = [&](unsigned At) {
    return llvm::support::endian::read32le(Data + At);
  };
  auto Get64 = [&](unsigned At) {
    return llvm::support::endian::read64le(Data + At);
  };
  auto &GPU = *Device.GPU;
  if (Request == Free) {
    return Outcome(
        GPU.Allocations.erase(Get64(0)) ? 0 : uint64_t(0) - InvalidArgument);
  }
  if (Request == Wait) {
    if (!Device.Contexts.contains(Get32(0)))
      return Outcome(uint64_t(0) - InvalidArgument);
    const uint32_t Current = Device.QueuedTimestamps.at(Get32(0));
    if (int32_t(Current - Get32(4)) >= 0)
      return Outcome(0);
    return unsupported(
        Result, "GPU wait requires work outside the owned synchronous queue");
  }
  auto Writable = CPU.canAccess(Argument, Size, Write | UserAccessible);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return unsupported(Result, "GPU ioctl result is not fully writable");
  if (Request == Map) {
    const uint64_t Host = Get64(32), Length = Get64(16);
    const uint32_t Flags = Get32(44);
    if (Get32(40) != 2 || (Flags & ~0x01000000u))
      return unsupported(
          Result, "GPU mapping requires ordinary user RAM and admitted flags");
    if (!Host || !Length || Host % 4096 || Length % 4096 || Get64(24))
      return Outcome(uint64_t(0) - InvalidArgument);
    const auto GPUAddress =
        kgsl::allocationAddress(*Device.Options, GPU.NextAddress, Length);
    if (Length > kgsl::MaximumAllocation ||
        GPU.Allocations.size() >= kgsl::MaximumAllocations || !GPUAddress)
      return unsupported(Result, "GPU pinned memory budget exhausted");
    if (Host > Layout.UserLimit || Length > Layout.UserLimit - Host)
      return Outcome(uint64_t(0) - BadAddress);
    const bool ReadOnly = Flags & 0x01000000;
    auto Allowed = CPU.canAccess(
        Host, Length, Read | UserAccessible | (ReadOnly ? 0 : Write));
    if (!Allowed)
      return Allowed.takeError();
    if (!*Allowed)
      return Outcome(uint64_t(0) - BadAddress);
    auto Backing = CPU.pinBacking(Host, Length);
    if (!Backing)
      return Backing.takeError();
    const uint64_t Address = *GPUAddress;
    llvm::support::endian::write64le(Data + 8, Address);
    llvm::support::endian::write32le(Data + 44, Flags | 0x60); // USERMEM_ADDR
    if (auto E = CPU.write(Argument, Data))
      return std::move(E);
    GPU.Allocations.emplace(Address,
                            GPUAllocation{std::move(*Backing), ReadOnly});
    GPU.NextAddress = Address + Length + 4096;
    return Outcome(0);
  }
  const uint32_t ID = Get32(0), Words = Get32(16);
  if (!Device.Contexts.contains(ID) || !Words)
    return Outcome(uint64_t(0) - InvalidArgument);
  if (Get32(24))
    return unsupported(
        Result, "GPU command flags or IB lists are outside the legacy subset");
  if (Words > 4096)
    return unsupported(Result, "GPU command word budget exhausted");
  auto Resolve = [&](uint64_t Address, uint64_t Length,
                     bool Write) -> llvm::Expected<std::optional<MemoryView>> {
    auto I = GPU.Allocations.upper_bound(Address);
    if (I == GPU.Allocations.begin())
      return std::optional<MemoryView>();
    --I;
    if (Address - I->first > I->second.Bytes.size() ||
        Length > I->second.Bytes.size() - (Address - I->first) ||
        (Write && I->second.ReadOnly))
      return std::optional<MemoryView>();
    auto View = I->second.Bytes.subview(Address - I->first, Length);
    if (!View)
      return View.takeError();
    return std::optional<MemoryView>(std::move(*View));
  };
  if (Get64(8) % 4)
    return unsupported(Result, "GPU command buffer is not dword aligned");
  auto Program = Resolve(Get64(8), uint64_t(Words) * 4, false);
  if (!Program)
    return Program.takeError();
  if (!*Program)
    return unsupported(Result, "GPU command range is not registered RAM");
  std::vector<uint8_t> Code(Words * 4);
  if (auto E = (*Program)->read(0, Code))
    return std::move(E);
  struct Store {
    MemoryView Target;
    uint32_t Value;
  };
  std::vector<Store> Stores;
  for (size_t At = 0; At < Words;) {
    const uint32_t Header =
        llvm::support::endian::read32le(Code.data() + At * 4);
    const unsigned Count = ((Header >> 16) & 0x3fff) + 1;
    const unsigned Opcode = (Header >> 8) & 255;
    if ((Header & 0xc00000ff) != 0xc0000000 || Count > Words - At - 1)
      return unsupported(Result,
                         "GPU packet type, predicate or extent is unsupported");
    if (Opcode == 0x10) { // CP_NOP: its declared payload is not executed.
      At += 1 + Count;
      continue;
    }
    if (Opcode != 0x3d || Count != 2)
      return unsupported(Result,
                         "GPU opcode is outside CP_NOP/one-word CP_MEM_WRITE");
    const uint32_t Address =
        llvm::support::endian::read32le(Code.data() + (At + 1) * 4);
    if (Address % 4)
      return unsupported(Result, "GPU memory write is not dword aligned");
    auto Target = Resolve(Address, 4, true);
    if (!Target)
      return Target.takeError();
    if (!*Target)
      return unsupported(Result,
                         "GPU write range is not registered writable RAM");
    if ((*Target)->overlaps(**Program))
      return unsupported(Result,
                         "GPU self-modifying command fetch is unmodeled");
    Stores.push_back({std::move(**Target), llvm::support::endian::read32le(
                                               Code.data() + (At + 2) * 4)});
    At += 3;
  }
  // Admission precedes all effects. Unknown packets cannot become successful
  // NOPs, partial execution or fabricated retirement. This profile drains the
  // admitted finite queue at this stopped CPU boundary.
  for (const auto &Store : Stores) {
    uint8_t Bytes[4];
    llvm::support::endian::write32le(Bytes, Store.Value);
    if (auto E = Store.Target.write(0, Bytes))
      return std::move(E);
  }
  const uint32_t Timestamp = Device.QueuedTimestamps.at(ID) + 1;
  const auto *Memstore = kgsl::memstore(*Device.Options);
  uint8_t Stamp[4];
  llvm::support::endian::write32le(Stamp, Timestamp);
  for (unsigned Field : {0u, 8u})
    if (auto E = Memory->writeDeviceBytes(
            *Memstore, uint64_t(ID) * kgsl::SlotBytes + Field, Stamp))
      return std::move(E);
  llvm::support::endian::write32le(Data + 20, Timestamp);
  if (auto E = CPU.write(Argument, llvm::ArrayRef(Data, Size)))
    return std::move(E);
  Device.QueuedTimestamps.at(ID) = Timestamp;
  return Outcome(0);
}
} // namespace neverd::emulation::linux_model
