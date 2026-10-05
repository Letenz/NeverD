//===- AndroidArguments.cpp - Bionic AAPCS64 integer varargs
//---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidArguments.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

#include <array>

namespace neverd::emulation::android_model {
llvm::Error Bionic::ArgumentReader::read(uint64_t Address,
                                         llvm::MutableArrayRef<uint8_t> Bytes) {
  if (Bytes.size() > Remaining)
    return failure(diagnostic::ArgumentInputLimit);
  Remaining -= Bytes.size();
  if (auto E = Model.access(Address, Bytes.size(), Read))
    return E;
  Reads.push_back({Address, static_cast<unsigned>(Bytes.size())});
  return Model.CPU.read(Address, Bytes);
}

llvm::Error Bionic::ArgumentReader::initializeVAList(uint64_t Address) {
  if (Address % 8)
    return failure(diagnostic::ArgumentVAListAlignment);
  std::array<uint8_t, 32> Bytes{};
  if (auto E = read(Address, Bytes))
    return E;
  Stack = llvm::support::endian::read64le(Bytes.data());
  GRTop = llvm::support::endian::read64le(Bytes.data() + 8);
  GROffset =
      static_cast<int32_t>(llvm::support::endian::read32le(Bytes.data() + 24));
  if (GROffset < -64 || GROffset % 8)
    return failure(diagnostic::ArgumentRegisterOffset);
  HasVAList = StackLoaded = true;
  // vr_top/vr_offs are irrelevant to the supported GP-only conversions.
  return llvm::Error::success();
}

llvm::Expected<uint64_t> Bionic::ArgumentReader::next(unsigned Size) {
  assert((Size == 4 || Size == 8) && "unsupported GP argument width");
  // Narrow scalar arguments are promoted to int. Their other four bytes are
  // unspecified, but both widths consume one eight-byte argument slot.
  if (!HasVAList && NextRegister < Call.Arguments.size()) {
    uint64_t Value = Call.Arguments[NextRegister++];
    return Size == 4 ? static_cast<uint32_t>(Value) : Value;
  }
  uint64_t Address;
  if (HasVAList && GROffset < 0) {
    const uint64_t Offset = -int64_t(GROffset);
    if (GRTop % 8 || GRTop < Offset)
      return failure(diagnostic::ArgumentRegisterSaveArea);
    Address = GRTop - Offset;
    GROffset += 8;
  } else {
    if (!StackLoaded) {
      auto SP = Model.CPU.readRegister(CPURegister::AArch64SP);
      if (!SP)
        return SP.takeError();
      Stack = (*SP)[0];
      StackLoaded = true;
      if (Stack % 16)
        return failure(diagnostic::ArgumentCallStackAlignment);
    }
    if (Stack % 8 || Stack > UINT64_MAX - 8)
      return failure(diagnostic::ArgumentStack);
    Address = Stack;
    Stack += 8;
  }
  std::array<uint8_t, 8> Bytes{};
  if (auto E = read(Address, llvm::MutableArrayRef(Bytes).take_front(Size)))
    return std::move(E);
  return llvm::support::endian::read64le(Bytes.data());
}

bool Bionic::ArgumentReader::overlaps(uint64_t Address, unsigned Size) const {
  for (const auto &Range : Reads)
    if (Address <= Range.Address ? Range.Address - Address < Size
                                 : Address - Range.Address < Range.Size)
      return true;
  return false;
}
} // namespace neverd::emulation::android_model
