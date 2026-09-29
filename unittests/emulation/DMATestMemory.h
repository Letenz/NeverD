//===- DMATestMemory.h - Retained DMA RAM with injected validation holes
//---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNITTESTS_EMULATION_DMATESTMEMORY_H
#define NEVERD_UNITTESTS_EMULATION_DMATESTMEMORY_H

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/DriverDMA.h"

#include <optional>

namespace neverd::emulation::dma_test {
#define NEVERD_DMA_TEST_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_DMA_TEST_MESSAGE(Name, Message) constexpr char Name[] = Message;
#include "DMATestMemory.def"
#undef NEVERD_DMA_TEST_MESSAGE
#undef NEVERD_DMA_TEST_VALUE

class Memory final : public GuestMemory {
public:
  Memory()
      : Space(llvm::cantFail(AddressSpace::create(
            llvm::cantFail(PhysicalMemory::create(Capacity)), Capacity))) {
    llvm::cantFail(Space->map(Base, Capacity, Read | Write));
  }
  std::optional<uint64_t> Hole;
  unsigned DeviceReads = 0, DeviceWrites = 0;

  std::vector<uint8_t> bytes() const {
    std::vector<uint8_t> Bytes(Capacity);
    llvm::cantFail(Space->readBacking(Base, Bytes));
    return Bytes;
  }
  std::shared_ptr<AddressSpace> addressSpace() const override { return Space; }
  llvm::Error map(uint64_t, uint64_t, unsigned) override {
    return error(AlreadyMapped);
  }
  llvm::Error protect(uint64_t Address, uint64_t Size,
                      unsigned Permissions) override {
    return Space->protect(Address, Size, Permissions);
  }
  llvm::Error validateBacking(uint64_t Address, uint64_t Size) const override {
    if (auto E = Space->validateBacking(Address, Size))
      return E;
    if (Hole && Address <= *Hole && Size > *Hole - Address)
      return error(BackingHole);
    return llvm::Error::success();
  }
  llvm::Expected<MemoryView> pinBacking(uint64_t Address,
                                        uint64_t Size) const override {
    if (auto E = validateBacking(Address, Size))
      return E;
    return Space->pinBacking(Address, Size);
  }
  llvm::Error validatePinned(const MemoryView &View, uint64_t Offset,
                             uint64_t Size) const override {
    if (auto E = Space->validatePinned(View, Offset, Size))
      return E;
    if (Hole) {
      auto Invalid = Space->pinBacking(*Hole, 1);
      if (!Invalid)
        return Invalid.takeError();
      if (llvm::cantFail(View.subview(Offset, Size)).overlaps(*Invalid))
        return error(BackingHole);
    }
    return llvm::Error::success();
  }
  llvm::Error read(uint64_t Address,
                   llvm::MutableArrayRef<uint8_t> Bytes) override {
    if (auto E = validateBacking(Address, Bytes.size()))
      return E;
    return Space->read(Address, Bytes);
  }
  llvm::Error write(uint64_t Address, llvm::ArrayRef<uint8_t> Bytes) override {
    if (auto E = validateBacking(Address, Bytes.size()))
      return E;
    return Space->write(Address, Bytes);
  }
  llvm::Error readBacking(uint64_t Address,
                          llvm::MutableArrayRef<uint8_t> Bytes) override {
    if (auto E = validateBacking(Address, Bytes.size()))
      return E;
    ++DeviceReads;
    return Space->readBacking(Address, Bytes);
  }
  llvm::Error writeBacking(uint64_t Address,
                           llvm::ArrayRef<uint8_t> Bytes) override {
    if (auto E = validateBacking(Address, Bytes.size()))
      return E;
    ++DeviceWrites;
    return Space->writeBacking(Address, Bytes);
  }
  llvm::Error readPinned(const MemoryView &View, uint64_t Offset,
                         llvm::MutableArrayRef<uint8_t> Bytes) override {
    if (auto E = validatePinned(View, Offset, Bytes.size()))
      return E;
    ++DeviceReads;
    return Space->readPinned(View, Offset, Bytes);
  }
  llvm::Error writePinned(const MemoryView &View, uint64_t Offset,
                          llvm::ArrayRef<uint8_t> Bytes) override {
    if (auto E = validatePinned(View, Offset, Bytes.size()))
      return E;
    ++DeviceWrites;
    return Space->writePinned(View, Offset, Bytes);
  }

private:
  static llvm::Error error(llvm::StringRef Message) {
    return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
  }
  std::shared_ptr<AddressSpace> Space;
};
} // namespace neverd::emulation::dma_test
#endif
