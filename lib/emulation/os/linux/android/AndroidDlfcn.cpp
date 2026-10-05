//===- AndroidDlfcn.cpp - Bionic dynamic library lifetime ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation::android_model {
BionicResult Bionic::linkerError(llvm::StringRef Message,
                                 uint64_t ReturnValue) {
  const uint64_t ErrorAddress = tlsAddress() + LinkerErrorAddress - TLSAddress;
  const uint64_t ErrorSlot = tlsAddress() + LinkerErrorSlot - TLSAddress;
  // A fixed guest buffer, separate from errno, TLS ABI slots and constructor
  // argv/envp. A later error may replace its contents; dlerror consumes it
  // once.
  if (Message.size() >= 1024)
    return failure(diagnostic::LinkerErrorLimit);
  std::vector<uint8_t> Bytes(Message.bytes_begin(), Message.bytes_end());
  Bytes.push_back(0);
  if (auto E = access(ErrorAddress, Bytes.size(), Write))
    return std::move(E);
  if (auto E = CPU.write(ErrorAddress, Bytes))
    return std::move(E);
  uint8_t Pointer[8];
  llvm::support::endian::write64le(Pointer, ErrorAddress);
  if (auto E = access(ErrorSlot, sizeof(Pointer), Write))
    return std::move(E);
  if (auto E = CPU.write(ErrorSlot, Pointer))
    return std::move(E);
  return value(ReturnValue);
}
bool Bionic::isResident(const std::string &Library) const {
  const auto &Scope = Options.Android->DefaultScope;
  return Scope &&
         std::find(Scope->begin(), Scope->end(), Library) != Scope->end();
}
BionicResult Bionic::unsupportedLinking(llvm::StringRef Detail) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = diagnostic::DynamicLinkingPrefix + Detail.str();
  return std::optional<BionicValue>();
}

BionicResult Bionic::getLinkerError(NativeCallEvent &) {
  const uint64_t ErrorSlot = tlsAddress() + LinkerErrorSlot - TLSAddress;
  uint8_t Pointer[8], Empty[8]{};
  if (auto E = access(ErrorSlot, sizeof(Pointer), Read | Write))
    return std::move(E);
  if (auto E = CPU.read(ErrorSlot, Pointer))
    return std::move(E);
  uint64_t Address = llvm::support::endian::read64le(Pointer);
  if (auto E = CPU.write(ErrorSlot, Empty))
    return std::move(E);
  return value(Address);
}

BionicResult Bionic::openLibrary(NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  if (!A[0])
    return unsupportedLinking(diagnostic::NullOpenScope);
  auto Library = string(A[0]);
  if (!Library)
    return Library.takeError();
  if (Library->size() > 1024)
    return failure(diagnostic::LibraryNameLimit);
  Call.Library = *Library;
  // API 28 LP64 accepts any combination of these bits, including zero
  // and NOLOAD alone. Scope promotion and NODELETE need a fuller loader.
  uint32_t Flags = static_cast<uint32_t>(A[1]);
  if (Flags & ~0x1107u)
    return linkerError(diagnostic::OpenFlags);
  if (Flags & 0x1100u)
    return unsupportedLinking(diagnostic::OpenScopeFlags);
  if (!Linked.Libraries.count(*Library))
    return linkerError(diagnostic::LibraryAbsent);
  auto &State = OpenLibraries[*Library];
  if (!State.References && !isResident(*Library) && (Flags & 4u))
    return linkerError(diagnostic::LibraryNotOpen);
  if (!State.Handle) {
    if (NextHandle > UINT64_MAX - 2)
      return failure(diagnostic::LibraryHandleLimit);
    State.Handle = NextHandle;
    NextHandle += 2;
    Handles.emplace(State.Handle, *Library);
  }
  if (State.References == UINT64_MAX)
    return failure(diagnostic::LibraryReferenceOverflow);
  ++State.References;
  return value(State.Handle);
}

BionicResult Bionic::lookupSymbol(NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  if (!A[1])
    return linkerError(diagnostic::NullSymbol);
  auto Symbol = string(A[1]);
  if (!Symbol)
    return Symbol.takeError();
  if (Symbol->size() > 1024)
    return failure(diagnostic::SymbolNameLimit);
  Call.Symbol = *Symbol;
  if (!A[0]) {
    const auto &Scope = Options.Android->DefaultScope;
    if (!Scope)
      return unsupportedLinking(diagnostic::DefaultScope);
    for (const auto &Library : *Scope) {
      const auto &Symbols = Linked.Libraries.at(Library);
      auto I = Symbols.find(*Symbol);
      if (I != Symbols.end()) {
        Call.Library = Library;
        return value(I->second);
      }
    }
    return linkerError(diagnostic::DefaultSymbolAbsent);
  }
  if (A[0] == UINT64_MAX)
    return unsupportedLinking(diagnostic::NextScope);
  auto Handle = Handles.find(A[0]);
  if (Handle == Handles.end())
    return linkerError(diagnostic::LibraryHandle);
  Call.Library = Handle->second;
  const auto &Symbols = Linked.Libraries.at(Handle->second);
  auto I = Symbols.find(*Symbol);
  if (I == Symbols.end())
    return linkerError(diagnostic::LibrarySymbolAbsent);
  return value(I->second);
}

BionicResult Bionic::closeLibrary(NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  auto Handle = Handles.find(A[0]);
  if (Handle == Handles.end())
    return linkerError(diagnostic::LibraryHandle, UINT64_MAX);
  Call.Library = Handle->second;
  auto &State = OpenLibraries.at(Handle->second);
  if (!State.References)
    return linkerError(diagnostic::LibraryReferences, UINT64_MAX);
  if (!--State.References && !isResident(Handle->second)) {
    Handles.erase(Handle);
    State.Handle = 0;
  }
  return value(0);
}

} // namespace neverd::emulation::android_model
