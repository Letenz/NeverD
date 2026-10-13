//===- ProcessLinuxDevicesJSON.cpp - Bounded device input transport -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Decodes explicit transaction inputs; shared Linux validation owns policy.
//===----------------------------------------------------------------------===//
#include "../os/linux/kernel/LinuxFiles.h"
#include "ProcessJSONInteger.h"
#include "ProcessLinuxFilesJSON.h"

#include "neverd/emulation/ProcessReportFields.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd::emulation {
namespace {
namespace field = process_report;
llvm::Error invalid() {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 linux_model::DeviceOptions);
}
template <typename T>
std::optional<T> number(const llvm::json::Object &Object, llvm::StringRef Key) {
  const auto *V = Object.get(Key);
  return V ? process_json::integer<T>(*V) : std::nullopt;
}
llvm::Expected<std::vector<uint8_t>>
bytes(const llvm::json::Object &Object, llvm::StringRef Key, uint64_t &Total) {
  auto Hex = Object.getString(Key);
  if (!Hex || Hex->size() % 2 ||
      Hex->size() / 2 > linux_model::FileByteLimit - Total ||
      !llvm::all_of(*Hex, llvm::isHexDigit))
    return invalid();
  Total += Hex->size() / 2;
  std::string Data = llvm::fromHex(*Hex);
  return std::vector<uint8_t>(Data.begin(), Data.end());
}
} // namespace

llvm::Expected<LinuxDeviceOptions>
linuxDeviceOptionsFromJSON(const llvm::json::Object &Value,
                           uint64_t &PayloadBytes, uint64_t &Transactions) {
  auto Mode = number<uint32_t>(Value, field::DeviceAccessMode);
  auto Error = number<uint32_t>(Value, field::DeviceOpenErrno);
  const auto *Ioctls = Value.getArray(field::DeviceIoctls);
  if (Value.size() != 4 || !Value.getString(field::Path) || !Mode || !Error ||
      !Ioctls || Ioctls->size() > linux_model::DeviceIoctlLimit - Transactions)
    return invalid();
  Transactions += Ioctls->size();
  LinuxDeviceOptions Out{*Mode, *Error, {}};
  for (const auto &V : *Ioctls) {
    const auto *Object = V.getAsObject();
    if (!Object)
      return invalid();
    auto Request = number<uint32_t>(*Object, field::DeviceRequest);
    auto Result = number<int64_t>(*Object, field::Result);
    const auto *Memory = Object->getArray(field::Memory);
    bool HasArgument = Object->get(field::DeviceArgument);
    auto Argument = number<uint64_t>(*Object, field::DeviceArgument);
    if (Object->size() != 3 + unsigned(HasArgument) || !Request || !Result ||
        !Memory || Memory->size() > linux_model::DeviceMemoryLimit ||
        (HasArgument && !Argument))
      return invalid();
    LinuxDeviceIoctl Call{*Request, *Result, Argument, {}};
    for (const auto &M : *Memory) {
      const auto *Object = M.getAsObject();
      if (!Object)
        return invalid();
      auto Offset = number<uint64_t>(*Object, field::DeviceOffset);
      bool HasPointer = Object->get(field::DevicePointerOffset);
      auto Pointer = number<uint64_t>(*Object, field::DevicePointerOffset);
      if (Object->size() != 3 + unsigned(HasPointer) || !Offset ||
          (HasPointer && !Pointer))
        return invalid();
      auto Expected = bytes(*Object, field::DeviceExpected, PayloadBytes);
      if (!Expected)
        return Expected.takeError();
      auto Output = bytes(*Object, field::DeviceOutput, PayloadBytes);
      if (!Output)
        return Output.takeError();
      Call.Memory.push_back(
          {*Offset, Pointer, std::move(*Expected), std::move(*Output)});
    }
    Out.Ioctls.push_back(std::move(Call));
  }
  return Out;
}
} // namespace neverd::emulation
