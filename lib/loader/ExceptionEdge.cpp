//===- ExceptionEdge.cpp - Immutable exceptional contexts --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/ExceptionEdge.h"

#include "llvm/Support/SHA256.h"

#include <stdexcept>

namespace neverd {

ExceptionalEdgeContextDomain::ExceptionalEdgeContextDomain(
    std::vector<ExceptionalEdgeContext> Contexts)
    : Values(Contexts) {
  // Detach even from a moved vector: its caller may have retained mutable
  // element references. The cached digest must describe privately owned data.
  if (Values.empty())
    throw std::invalid_argument("an exceptional edge context domain is empty");
  std::sort(Values.begin(), Values.end());
  Values.erase(std::unique(Values.begin(), Values.end()), Values.end());
  llvm::SHA256 Hash;
  const auto Word = [&](uint64_t Value) {
    std::array<uint8_t, 8> Bytes;
    for (unsigned I = 0; I != Bytes.size(); ++I)
      Bytes[I] = uint8_t(Value >> (I * 8));
    Hash.update(Bytes);
  };
  Word(Values.size());
  for (const auto &Context : Values) {
    Word(Context.RegionIndex);
    Word(uint64_t(int64_t(Context.State)));
  }
  Digest = Hash.final();
}

} // namespace neverd
