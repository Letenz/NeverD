//===- KernelModelMemory.h - Observe model reads of environment data -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Retain recovery dependencies for consuming reads of modeled kernel data.
///
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_KERNELMODELMEMORY_H
#define NEVERD_EMULATION_KERNELMODELMEMORY_H

#include "KernelExportRegistry.h"

#include "neverd/emulation/GuestMemory.h"

namespace neverd::emulation {
/// Forward the memory contract unchanged. Only successful consuming reads of
/// kernel data record an environment dependency; preflight and diagnostic
/// snapshots stay pure. This covers nested buffers read by any model owner,
/// without making access validators mutate recovery state.
class KernelModelMemory final : public GuestMemory {
  GuestMemory &Backing;
  std::function<void()> ObserveDataRead;

public:
  KernelModelMemory(GuestMemory &Backing, std::function<void()> ObserveDataRead)
      : Backing(Backing), ObserveDataRead(std::move(ObserveDataRead)) {}
  std::shared_ptr<AddressSpace> addressSpace() const override {
    return Backing.addressSpace();
  }
  llvm::Expected<MemoryView> pinBacking(uint64_t Address,
                                        uint64_t Size) const override {
    return Backing.pinBacking(Address, Size);
  }
  llvm::Error validatePinned(const MemoryView &View, uint64_t Offset,
                             uint64_t Size) const override {
    return Backing.validatePinned(View, Offset, Size);
  }
  llvm::Error readPinned(const MemoryView &View, uint64_t Offset,
                         llvm::MutableArrayRef<uint8_t> Bytes) override {
    return Backing.readPinned(View, Offset, Bytes);
  }
  llvm::Error writePinned(const MemoryView &View, uint64_t Offset,
                          llvm::ArrayRef<uint8_t> Bytes) override {
    return Backing.writePinned(View, Offset, Bytes);
  }
  llvm::Error map(uint64_t Address, uint64_t Size,
                  unsigned Permissions) override {
    return Backing.map(Address, Size, Permissions);
  }
  llvm::Error mapAlias(uint64_t Address, uint64_t Source, uint64_t Size,
                       unsigned Permissions) override {
    return Backing.mapAlias(Address, Source, Size, Permissions);
  }
  llvm::Error unmapAlias(uint64_t Address, uint64_t Size) override {
    return Backing.unmapAlias(Address, Size);
  }
  llvm::Error replaceAliases(llvm::ArrayRef<GuestAliasRange> Remove,
                             llvm::ArrayRef<GuestAliasMapping> Add) override {
    return Backing.replaceAliases(Remove, Add);
  }
  llvm::Error protect(uint64_t Address, uint64_t Size,
                      unsigned Permissions) override {
    return Backing.protect(Address, Size, Permissions);
  }
  llvm::Error mapMMIO(uint64_t Address, uint64_t Size,
                      GuestMMIOCallbacks Callbacks) override {
    return Backing.mapMMIO(Address, Size, std::move(Callbacks));
  }
  llvm::Error unmapMMIO(uint64_t Address, uint64_t Size) override {
    return Backing.unmapMMIO(Address, Size);
  }
  llvm::Error read(uint64_t Address,
                   llvm::MutableArrayRef<uint8_t> Bytes) override {
    if (auto E = Backing.read(Address, Bytes))
      return E;
    if (KernelExportRegistry::overlapsData(Address, Bytes.size()))
      ObserveDataRead();
    return llvm::Error::success();
  }
  llvm::Error write(uint64_t Address, llvm::ArrayRef<uint8_t> Bytes) override {
    return Backing.write(Address, Bytes);
  }
  llvm::Error validateBacking(uint64_t Address, uint64_t Size) const override {
    return Backing.validateBacking(Address, Size);
  }
  llvm::Expected<bool> canAccess(uint64_t Address, uint64_t Size,
                                 unsigned Permissions) const override {
    return Backing.canAccess(Address, Size, Permissions);
  }
  llvm::Error readBacking(uint64_t Address,
                          llvm::MutableArrayRef<uint8_t> Bytes) override {
    if (auto E = Backing.readBacking(Address, Bytes))
      return E;
    if (KernelExportRegistry::overlapsData(Address, Bytes.size()))
      ObserveDataRead();
    return llvm::Error::success();
  }
  llvm::Error writeBacking(uint64_t Address,
                           llvm::ArrayRef<uint8_t> Bytes) override {
    return Backing.writeBacking(Address, Bytes);
  }
  llvm::Error snapshotBacking(uint64_t Address,
                              llvm::MutableArrayRef<uint8_t> Bytes) override {
    return Backing.snapshotBacking(Address, Bytes);
  }
};
} // namespace neverd::emulation
#endif
