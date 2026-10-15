//===- KernelModelVaShadow.cpp - Bounded mitigation query output ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded mitigation query output.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "KernelVaShadowQuery.h"

namespace neverd::emulation {
llvm::Expected<uint64_t>
KernelModel::queryKernelVaShadow(llvm::ArrayRef<uint64_t> A) {
  using namespace kernel_va_shadow_query;
  constexpr llvm::StringLiteral API = "kernel VA shadow query";
  if (Contract != ExecutionContract::CheckedX64)
    return llvm::createStringError(API + " requires checked-x64 execution");
  if (!KernelVaShadowQuery)
    return llvm::createStringError(API +
                                   " kernel_va_shadow_query is not declared");
  const uint32_t Length = A[2];
  if (A[3])
    if (auto E = validateQueryBuffer(A[3], ResultSize, true, API))
      return E;
  if (Length >= ResultSize) {
    // The bounded result routine updates bitfields in place. Admit its read
    // and write footprint even though all resulting bits are declared.
    if (auto E = validateQueryBuffer(A[1], ResultSize, false, API))
      return E;
    if (auto E = validateQueryBuffer(A[1], ResultSize, true, API))
      return E;
    if (A[3] && (A[1] < A[3] + ResultSize && A[3] < A[1] + ResultSize))
      return llvm::createStringError(API + " output and return length overlap");
    // Complete admission of both outputs precedes every write. No byte of
    // the supplied tail is accessed, including an unmapped following page.
    if (auto E =
            Memory.writeInteger(A[1], *KernelVaShadowQuery->Flags, ResultSize))
      return E;
  }
  if (A[3])
    if (auto E = Memory.writeInteger(A[3], ResultSize, ResultSize))
      return E;
  return Length < ResultSize ? uint64_t(0xc0000004u) : uint64_t(0);
}
} // namespace neverd::emulation
