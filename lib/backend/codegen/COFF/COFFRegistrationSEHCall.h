//===- COFFRegistrationSEHCall.h - Dynamic call receipts ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_COFFREGISTRATIONSEHCALL_H
#define NEVERD_COFFREGISTRATIONSEHCALL_H

namespace llvm {
class CallBase;
class CallInst;
} // namespace llvm

namespace neverd::coff_registration {
bool isDynamicSEHCall(const llvm::CallBase &Call);

/// Bind a call receipt to the adjacent source call. Scope and source replay
/// validation separately authenticate its enclosing dynamic execution segment.
const llvm::CallInst *dynamicSEHCall(const llvm::CallInst &Anchor);
} // namespace neverd::coff_registration

#endif // NEVERD_COFFREGISTRATIONSEHCALL_H
