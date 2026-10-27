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

#include "neverd/emulation/Limits.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/FormatVariadic.h"

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
                            GPUAllocation{std::move(*Backing), Flags | 0x60});
    GPU.NextAddress = Address + Length + 4096;
    return Outcome(0);
  }
  const uint32_t ID = Get32(0), Words = Get32(16);
  if (!Device.Contexts.contains(ID) || !Words)
    return Outcome(uint64_t(0) - InvalidArgument);
  if (Get32(24))
    return unsupported(
        Result, "GPU command flags or IB lists are outside the legacy subset");
  if (Words > ::neverd::limits::emulation::kMaxGPUCommandWords)
    return unsupported(Result, "GPU command word budget exhausted");
  auto Resolve = [&](uint64_t Address, uint64_t Length,
                     bool Write) -> llvm::Expected<std::optional<MemoryView>> {
    auto I = GPU.Allocations.upper_bound(Address);
    if (I == GPU.Allocations.begin())
      return std::optional<MemoryView>();
    --I;
    if (Address - I->first > I->second.Bytes.size() ||
        Length > I->second.Bytes.size() - (Address - I->first) ||
        (Write && (I->second.Flags & kgsl::GPUReadOnly)))
      return std::optional<MemoryView>();
    auto View = I->second.Bytes.subview(Address - I->first, Length);
    if (!View)
      return View.takeError();
    return std::optional<MemoryView>(std::move(*View));
  };
  struct Location {
    uint64_t Buffer;
    unsigned Word;
    std::optional<uint32_t> Header;
  };
  auto Reject = [&](const Location &At, const char *Reason) {
    auto Message =
        llvm::formatv("GPU PM4 buffer {0:x} word {1}", At.Buffer, At.Word)
            .str();
    if (At.Header)
      Message += llvm::formatv(" header {0:x}", *At.Header).str();
    Message += ": ";
    Message += Reason;
    unsupported(Result, Message.c_str());
    return false;
  };
  struct Store {
    MemoryView Target;
    uint32_t Value;
    Location At;
  };
  std::vector<Store> Stores;
  std::vector<MemoryView> Programs, Active;
  unsigned RemainingWords =
      ::neverd::limits::emulation::kLinuxGPUCommandsRemainingWords;
  auto Admit = [&](auto &&Self, uint64_t Address, unsigned Length,
                   const Location &From) -> llvm::Expected<bool> {
    if (Address % 4 || !Length)
      return Reject(From, "GPU command buffer needs aligned, nonempty words");
    if (Length > RemainingWords)
      return Reject(From, "GPU aggregate command word budget exhausted");
    auto Program = Resolve(Address, uint64_t(Length) * 4, false);
    if (!Program)
      return Program.takeError();
    if (!*Program)
      return Reject(From, "GPU command range is not registered RAM");
    for (const auto &Parent : Active)
      if (Parent.overlaps(**Program))
        return Reject(From,
                      "GPU recursive or overlapping active IB is unmodeled");
    // Explicit profile limit: one submitted IB and one indirect level. Repeated
    // sibling calls execute again and charge the aggregate budget each time.
    if (Active.size() == 2)
      return Reject(From, "GPU indirect buffer depth budget exhausted");
    RemainingWords -= Length;
    Programs.push_back(**Program);
    Active.push_back(**Program);
    std::vector<uint8_t> Code(uint64_t(Length) * 4);
    if (auto E = (*Program)->read(0, Code))
      return std::move(E);
    auto Word = [&](unsigned At) {
      return llvm::support::endian::read32le(Code.data() + uint64_t(At) * 4);
    };
    for (unsigned At = 0; At < Length;) {
      const uint32_t Header = Word(At);
      const Location Here{Address, At, Header};
      const unsigned Count = ((Header >> 16) & 0x3fff) + 1;
      const unsigned Opcode = (Header >> 8) & 255;
      if ((Header & 0xc00000ff) != 0xc0000000 || Count > Length - At - 1)
        return Reject(Here,
                      "GPU packet type, predicate or extent is unsupported");
      if (Opcode == 0x10) { // CP_NOP payload is never decoded as commands.
        At += 1 + Count;
        continue;
      }
      if (Count != 2 || (Opcode != 0x3d && Opcode != 0x3f && Opcode != 0x37))
        return Reject(Here,
                      "GPU opcode is outside CP_NOP, one-word CP_MEM_WRITE "
                      "and CP_INDIRECT_BUFFER_PFE/PFD");
      if (Opcode == 0x3f || Opcode == 0x37) {
        auto Child = Self(Self, Word(At + 1), Word(At + 2), Here);
        if (!Child)
          return Child.takeError();
        if (!*Child)
          return false;
      } else {
        if (Word(At + 1) % 4)
          return Reject(Here, "GPU memory write is not dword aligned");
        auto Target = Resolve(Word(At + 1), 4, true);
        if (!Target)
          return Target.takeError();
        if (!*Target)
          return Reject(Here, "GPU write range is not registered writable RAM");
        Stores.push_back({std::move(**Target), Word(At + 2), Here});
      }
      At += 3;
    }
    Active.pop_back();
    return true;
  };
  auto Admitted = Admit(Admit, Get64(8), Words, {Get64(8), 0, std::nullopt});
  if (!Admitted)
    return Admitted.takeError();
  if (!*Admitted)
    return Outcome();
  // A parent may write a child before that child is discovered. Check every
  // retained physical fetch against every store after traversing the entire
  // program. Distinct GPU addresses can alias the same underlying RAM.
  for (const auto &Store : Stores)
    for (const auto &Program : Programs)
      if (Store.Target.overlaps(Program)) {
        Reject(Store.At, "GPU self-modifying command fetch is unmodeled");
        return Outcome();
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
