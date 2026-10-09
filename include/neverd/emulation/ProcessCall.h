//===- ProcessCall.h - Named guest process service evidence -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_PROCESSCALL_H
#define NEVERD_EMULATION_PROCESSCALL_H
#include <array>
#include <cstdint>
#include <optional>
#include <string>
namespace neverd::emulation {
namespace process_call {
#define NEVERD_PROCESS_CALL_VALUE(Name, Value)                                 \
  inline constexpr unsigned Name = Value;
#include "neverd/emulation/ProcessProfile.def"
#undef NEVERD_PROCESS_CALL_VALUE
} // namespace process_call
struct NativeCallEvent {
  uint64_t PC;
  std::string Name;
  std::array<uint64_t, process_call::MaxArguments> Arguments{};
  std::optional<uint64_t> Result;
  /// Explicit provider/argument count for Windows named imports. Android's
  /// existing Bionic report keeps its compatible register-bank representation.
  std::string Module;
  unsigned ArgumentCount = 0;
  /// Android resolver request, or the explicit provider of a dynamically
  /// obtained call.
  std::string Library;
  std::string Symbol;
  /// Present for explicitly enabled Android guest-thread scheduling.
  std::optional<uint64_t> ThreadID;
  /// Guest return address at the call, when the ABI keeps one. A tail call
  /// leaves the caller's return address here.
  std::optional<uint64_t> ReturnAddress;
  /// A Windows model service number used outside an authenticated export
  /// boundary. The generated provider defines this identity; replay on a
  /// different native system requires a separate binding proof.
  std::optional<uint64_t> DirectServiceNumber;
};
} // namespace neverd::emulation
#endif
