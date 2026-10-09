//===- LowUndefinedEffects.cpp - Exact operation identities --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/LowUndefinedEffects.h"

#include "neverd/support/SHA256.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/SHA256.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>

namespace neverd {

namespace {

constexpr llvm::StringLiteral DigestDomain("neverd-low-undefined-ops-v1");
constexpr size_t MaxBufferedBytes = 64 * 1024;
constexpr size_t HeaderBytes = DigestDomain.size() + sizeof(uint64_t);
constexpr size_t InputSlots = std::size(LowOp{}.Inputs);
static_assert(InputSlots == 6, "changing the input slots changes digest v1");
constexpr size_t WordsPerOperation = 6 + 5 * (1 + InputSlots);
constexpr size_t BytesPerOperation = WordsPerOperation * sizeof(uint64_t);

// Both storage paths enumerate the same words. Unused input slots, provenance
// and source coordinates remain part of the existing sidecar identity.
template <typename EmitWord>
void operationWords(llvm::ArrayRef<LowOp> Ops, EmitWord Number) {
  const auto Variable = [&](const NdVar &V) {
    Number(static_cast<unsigned>(V.Space));
    Number(V.Offset);
    Number(V.Size);
    Number(static_cast<unsigned>(V.Provenance));
    Number(V.AddressOwnerVA);
  };
  Number(Ops.size());
  for (const auto &Op : Ops) {
    Number(static_cast<unsigned>(Op.Opcode));
    Number(static_cast<unsigned>(Op.MemoryOrdering));
    Number(static_cast<unsigned>(Op.MemoryAddressSpace));
    Variable(Op.Output);
    Number(Op.NumInputs);
    for (const auto &Input : Op.Inputs)
      Variable(Input);
    Number(Op.Addr);
    Number(static_cast<uint64_t>(Op.Seq));
  }
}

} // namespace

std::string lowUndefinedOperationDigest(llvm::ArrayRef<LowOp> Ops) {
  // Check the ceiling before multiplying or allocating. Larger spans retain
  // the original streaming implementation and its exact digest behavior.
  if (Ops.size() <= (MaxBufferedBytes - HeaderBytes) / BytesPerOperation) {
    llvm::SmallVector<uint8_t, 1024> Bytes;
    Bytes.resize_for_overwrite(HeaderBytes + Ops.size() * BytesPerOperation);
    std::memcpy(Bytes.data(), DigestDomain.data(), DigestDomain.size());
    uint8_t *Next = Bytes.data() + DigestDomain.size();
    operationWords(Ops, [&](uint64_t Value) {
      llvm::support::endian::write64le(Next, Value);
      Next += sizeof(Value);
    });
    return llvm::toHex(sha256(Bytes), true);
  }

  llvm::SHA256 Hash;
  Hash.update(DigestDomain);
  operationWords(Ops, [&](uint64_t Value) {
    uint8_t Bytes[8];
    for (unsigned I = 0; I != 8; ++I)
      Bytes[I] = static_cast<uint8_t>(Value >> (I * 8));
    Hash.update(llvm::ArrayRef<uint8_t>(Bytes));
  });
  return llvm::toHex(Hash.final(), true);
}

} // namespace neverd
