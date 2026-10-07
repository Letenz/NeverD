//===- WindowsProcessTime.cpp - Host-backed Windows time services --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsProcessModules.h"

#include "neverd/emulation/CPU.h"

#include <chrono>
#include <ratio>
#include <thread>

namespace neverd::emulation::windows_process {
namespace {
using namespace value;
using WindowsTime =
    std::chrono::duration<int64_t, std::ratio<1, PerformanceFrequency>>;
} // namespace

llvm::Expected<std::optional<uint64_t>>
Services::delay(const Service &S, const NativeCallEvent &Event) {
  const auto &A = Event.Arguments;
  auto Refuse =
      [&](const llvm::Twine &Why) -> llvm::Expected<std::optional<uint64_t>> {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic =
        std::string(text::ServiceArguments) + S.Name + ": " + Why.str();
    return std::nullopt;
  };
  if (!A[1])
    return Refuse(text::DelayArguments);
  // BOOLEAN occupies only the low byte; the caller may leave upper bits live.
  if (static_cast<uint8_t>(A[0]))
    return Refuse(text::DelayAlertable);
  auto Readable = access(A[1], PointerSize, Read);
  if (!Readable)
    return Readable.takeError();
  if (!*Readable)
    return failure(text::Access);
  auto Ticks = CPU.readInteger(A[1], PointerSize);
  if (!Ticks)
    return Ticks.takeError();
  const auto Interval = static_cast<int64_t>(*Ticks);
  if (Interval > 0)
    return Refuse(text::DelayAbsolute);
  if (Interval < 0) {
    // Form the magnitude without signed overflow, including INT64_MIN.
    const uint64_t Magnitude = uint64_t(-(Interval + 1)) + 1;
    constexpr auto Maximum = std::chrono::duration_cast<WindowsTime>(
        std::chrono::nanoseconds(DelayCapNanoseconds));
    if (Magnitude > uint64_t(Maximum.count()))
      return Refuse(text::DelayLimit);
    std::this_thread::sleep_for(WindowsTime(static_cast<int64_t>(Magnitude)));
  }
  return std::optional<uint64_t>(0);
}

llvm::Expected<std::optional<uint64_t>>
Services::clock(const Service &S, const NativeCallEvent &Event) {
  using namespace std::chrono;
  if (S.Kind == API::GetTickCount)
    return std::optional<uint64_t>(static_cast<uint32_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
            .count()));
  const auto &A = Event.Arguments;
  if (!A[0])
    return failure(text::Access);
  auto Writable = access(A[0], PointerSize, Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return failure(text::Access);
  uint64_t Value = 0;
  switch (S.Kind) {
  case API::GetSystemTimeAsFileTime:
    // C++20 system_clock uses the Unix epoch; FILETIME starts in 1601.
    Value = FileTimeEpoch +
            duration_cast<WindowsTime>(system_clock::now().time_since_epoch())
                .count();
    break;
  case API::QueryPerformanceCounter:
    Value = duration_cast<WindowsTime>(steady_clock::now().time_since_epoch())
                .count();
    break;
  case API::QueryPerformanceFrequency:
    Value = PerformanceFrequency;
    break;
  default:
    return unsupported(S);
  }
  if (auto E = CPU.writeInteger(A[0], Value, PointerSize))
    return std::move(E);
  return std::optional<uint64_t>(S.Kind == API::GetSystemTimeAsFileTime ? 0
                                                                        : 1);
}
} // namespace neverd::emulation::windows_process
