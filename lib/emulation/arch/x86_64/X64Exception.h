//===- X64Exception.h - Native architectural exception results -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_ARCH_X64EXCEPTION_H
#define NEVERD_EMULATION_ARCH_X64EXCEPTION_H

#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>

namespace neverd::emulation {
namespace x64 {
#define NEVERD_X64_EXCEPTION_VALUE(Name, Value)                                \
  inline constexpr uint64_t Name = Value;
#include "X64Exceptions.def"
#undef NEVERD_X64_EXCEPTION_VALUE
enum class ExceptionVector : unsigned {
#define NEVERD_X64_EXCEPTION(Name, Vector, Error, Trap) Name = Vector,
#include "X64Exceptions.def"
#undef NEVERD_X64_EXCEPTION
};
inline constexpr uint64_t ExceptionExitBitmap = 0
#define NEVERD_X64_INTERCEPT_EXCEPTION(Name)                                   \
  | (uint64_t(1) << unsigned(ExceptionVector::Name))
#include "X64Exceptions.def"
#undef NEVERD_X64_INTERCEPT_EXCEPTION
    ;
inline bool exceptionHasError(unsigned Vector) {
  switch (Vector) {
#define NEVERD_X64_EXCEPTION(Name, Vector, Error, Trap)                        \
  case Vector:                                                                 \
    return Error;
#include "X64Exceptions.def"
#undef NEVERD_X64_EXCEPTION
  default:
    return false;
  }
}
inline bool exceptionIsTrap(unsigned Vector) {
  switch (Vector) {
#define NEVERD_X64_EXCEPTION(Name, Vector, Error, Trap)                        \
  case Vector:                                                                 \
    return Trap;
#include "X64Exceptions.def"
#undef NEVERD_X64_EXCEPTION
  default:
    return false;
  }
}
inline bool isExceptionVector(unsigned Vector) {
  switch (Vector) {
#define NEVERD_X64_EXCEPTION(Name, Vector, Error, Trap) case Vector:
#include "X64Exceptions.def"
#undef NEVERD_X64_EXCEPTION
    return true;
  default:
    return false;
  }
}
namespace exceptiontext {
#define NEVERD_X64_EXCEPTION_TEXT(Name, Text)                                  \
  inline constexpr char Name[] = Text;
#include "X64Exceptions.def"
#undef NEVERD_X64_EXCEPTION_TEXT
} // namespace exceptiontext
} // namespace x64

struct X64Exception {
  unsigned Vector;
  std::optional<uint64_t> ErrorCode;
  /// CR2 is meaningful only for #PF, never for another vector.
  std::optional<uint64_t> FaultAddress;
};

/// A processor exception is an execution outcome, not a transport failure.
/// The machine restores the original architectural exception boundary before
/// returning this error; private gateway registers never escape to its owner.
class X64ExceptionError final : public llvm::ErrorInfo<X64ExceptionError> {
public:
  static inline char ID = 0;
  explicit X64ExceptionError(X64Exception Exception) : Exception(Exception) {}
  const X64Exception &exception() const { return Exception; }
  void log(llvm::raw_ostream &OS) const override {
    OS << x64::exceptiontext::Exception << Exception.Vector;
  }
  std::error_code convertToErrorCode() const override {
    return llvm::inconvertibleErrorCode();
  }

private:
  X64Exception Exception;
};
} // namespace neverd::emulation
#endif
