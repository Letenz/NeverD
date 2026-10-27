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
#include "llvm/Support/Base64.h"

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
llvm::Expected<std::vector<uint8_t>>
mappingBytes(const llvm::json::Object &Object, uint64_t &Total) {
  const bool Hex = Object.get(field::Bytes);
  const bool Base64 = Object.get(field::DeviceBytesBase64);
  if (Hex == Base64)
    return invalid();
  if (Hex)
    return bytes(Object, field::Bytes, Total);
  auto Text = Object.getString(field::DeviceBytesBase64);
  const uint64_t Remaining = linux_model::FileByteLimit - Total;
  // Bound decoder allocation before reading the payload. Canonical padded
  // RFC 4648 encoding admits no whitespace, alternate alphabet or spare bits.
  if (!Text || Text->size() % 4 || Text->size() / 4 > (Remaining + 2) / 3)
    return invalid();
  std::vector<char> Decoded;
  if (auto E = llvm::decodeBase64(*Text, Decoded)) {
    llvm::consumeError(std::move(E));
    return invalid();
  }
  if (Decoded.size() > Remaining || llvm::encodeBase64(Decoded) != *Text)
    return invalid();
  Total += Decoded.size();
  return std::vector<uint8_t>(Decoded.begin(), Decoded.end());
}
} // namespace

llvm::Expected<LinuxDeviceOptions>
linuxDeviceOptionsFromJSON(const llvm::json::Object &Value,
                           uint64_t &PayloadBytes, uint64_t &Transactions) {
  auto Mode = number<uint32_t>(Value, field::DeviceAccessMode);
  auto Error = number<uint32_t>(Value, field::DeviceOpenErrno);
  const auto *Ioctls = Value.getArray(field::DeviceIoctls);
  const bool HasMappings = Value.get(field::DeviceReadOnlyMappings);
  const auto *Mappings = Value.getArray(field::DeviceReadOnlyMappings);
  const bool HasKGSL = Value.get(field::DeviceKGSL);
  const auto *KGSL = Value.getObject(field::DeviceKGSL);
  const bool HasBinder = Value.get(field::DeviceBinder);
  auto Binder = Value.getBoolean(field::DeviceBinder);
  if (Value.size() !=
          4 + unsigned(HasMappings) + unsigned(HasKGSL) + unsigned(HasBinder) ||
      (HasBinder && !Binder) || (HasKGSL && !KGSL) ||
      (HasMappings &&
       (!Mappings || Mappings->size() > linux_model::DeviceMappingLimit)) ||
      !Value.getString(field::Path) || !Mode || !Error || !Ioctls ||
      Ioctls->size() > linux_model::DeviceIoctlLimit - Transactions)
    return invalid();
  Transactions += Ioctls->size();
  LinuxDeviceOptions Out{*Mode, *Error, {}};
  Out.Binder = Binder.value_or(false);
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
  if (Mappings)
    for (const auto &V : *Mappings) {
      const auto *Object = V.getAsObject();
      if (!Object)
        return invalid();
      const bool HasUpdates = Object->get(field::DeviceUpdates);
      const auto *Updates = Object->getArray(field::DeviceUpdates);
      if (Object->size() != 2 + unsigned(HasUpdates) ||
          (HasUpdates &&
           (!Updates ||
            Updates->size() > linux_model::DeviceIoctlLimit - Transactions)))
        return invalid();
      auto Offset = number<uint64_t>(*Object, field::DeviceOffset);
      if (!Offset)
        return invalid();
      auto Data = mappingBytes(*Object, PayloadBytes);
      if (!Data)
        return Data.takeError();
      Out.ReadOnlyMappings.push_back({*Offset, std::move(*Data)});
      if (Updates) {
        Transactions += Updates->size();
        for (const auto &V : *Updates) {
          const auto *Update = V.getAsObject();
          if (!Update || Update->size() != 3)
            return invalid();
          auto Time = number<uint64_t>(*Update, field::DeviceUpdateTime);
          auto Offset = number<uint64_t>(*Update, field::DeviceOffset);
          if (!Time || !Offset)
            return invalid();
          auto Data = bytes(*Update, field::Bytes, PayloadBytes);
          if (!Data)
            return Data.takeError();
          Out.ReadOnlyMappings.back().Updates.push_back(
              {*Time, *Offset, std::move(*Data)});
        }
      }
    }
  if (KGSL) {
    auto Offset = number<uint64_t>(*KGSL, field::DeviceMemstoreOffset);
    auto Limit = number<uint32_t>(*KGSL, field::DeviceContextLimit);
    const bool HasCommands = KGSL->get(field::DeviceSynchronousA3xx);
    auto Commands = KGSL->getBoolean(field::DeviceSynchronousA3xx);
    const bool HasObjects = KGSL->get(field::DeviceMemoryObjects);
    auto Objects = KGSL->getBoolean(field::DeviceMemoryObjects);
    if (KGSL->size() != 2 + unsigned(HasCommands) + unsigned(HasObjects) ||
        !Offset || !Limit || (HasCommands && !Commands) ||
        (HasObjects && !Objects))
      return invalid();
    Out.KGSL = LinuxKGSLContexts{*Offset, *Limit, Commands.value_or(false),
                                 Objects.value_or(false)};
  }
  return Out;
}
} // namespace neverd::emulation
