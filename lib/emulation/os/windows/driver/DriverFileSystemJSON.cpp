//===- DriverFileSystemJSON.cpp - Declared filesystem transport -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Parse bounded declarations and serialize owner snapshots independently.

#include "DriverFileSystemJSON.h"

#include "llvm/ADT/StringExtras.h"

#include <algorithm>

namespace neverd::emulation::driver_filesystem {
namespace {
llvm::Error invalid(const llvm::Twine &Text) {
  return llvm::createStringError("driver filesystem: " + Text);
}
llvm::Error fields(const llvm::json::Object &Object,
                   llvm::ArrayRef<llvm::StringRef> Names) {
  for (const auto &[Name, Value] : Object)
    if (std::find(Names.begin(), Names.end(), llvm::StringRef(Name)) ==
        Names.end())
      return invalid("unknown field " + Name.str());
  return llvm::Error::success();
}
llvm::Expected<std::string> text(const llvm::json::Object &Object,
                                 llvm::StringRef Name) {
  auto Value = Object.getString(Name);
  if (!Value)
    return invalid(Name + " must be a string");
  return Value->str();
}
llvm::Expected<uint64_t> number(const llvm::json::Object &Object,
                                llvm::StringRef Name, uint64_t Maximum) {
  const auto *Field = Object.get(Name);
  auto Value = Field ? Field->getAsUINT64() : std::nullopt;
  if (!Value || *Value > Maximum)
    return invalid(Name + " must be a bounded unsigned integer");
  return *Value;
}
llvm::Expected<DriverFileSystemNode> node(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object)
    return invalid("node must be an object");
  if (auto E = fields(
          *Object, {"path", "kind", "file_id", "attributes", "allocation_size",
                    "creation_time_100ns", "last_access_time_100ns",
                    "last_write_time_100ns", "change_time_100ns", "data"}))
    return E;
  DriverFileSystemNode Node;
  auto Path = text(*Object, "path");
  if (!Path)
    return Path.takeError();
  Node.Path = std::move(*Path);
  auto Kind = text(*Object, "kind");
  if (!Kind)
    return Kind.takeError();
  if (*Kind == "file")
    Node.Kind = DriverFileNodeKind::File;
  else if (*Kind == "directory")
    Node.Kind = DriverFileNodeKind::Directory;
  else
    return invalid("unknown node kind");
  for (auto [Name, Destination] :
       {std::pair{"file_id", &Node.FileID},
        std::pair{"allocation_size", &Node.AllocationSize},
        std::pair{"creation_time_100ns", &Node.CreationTime100ns},
        std::pair{"last_access_time_100ns", &Node.LastAccessTime100ns},
        std::pair{"last_write_time_100ns", &Node.LastWriteTime100ns},
        std::pair{"change_time_100ns", &Node.ChangeTime100ns}}) {
    auto Number = number(*Object, Name, UINT64_MAX);
    if (!Number)
      return Number.takeError();
    *Destination = *Number;
  }
  auto Attributes = number(*Object, "attributes", UINT32_MAX);
  if (!Attributes)
    return Attributes.takeError();
  Node.Attributes = uint32_t(*Attributes);
  if (const auto *Data = Object->get("data")) {
    auto Hex = Data->getAsString();
    if (!Hex || Hex->size() % 2 || Hex->size() / 2 > MaxFileSystemFileBytes)
      return invalid("data must be a bounded even-length hexadecimal string");
    Node.Data.emplace();
    for (size_t I = 0; I < Hex->size(); I += 2) {
      if (!llvm::isHexDigit((*Hex)[I]) || !llvm::isHexDigit((*Hex)[I + 1]))
        return invalid("data contains a non-hexadecimal byte");
      Node.Data->push_back(uint8_t((llvm::hexDigitValue((*Hex)[I]) << 4) |
                                   llvm::hexDigitValue((*Hex)[I + 1])));
    }
  }
  return Node;
}
template <typename T> llvm::json::Value number(const std::optional<T> &Value) {
  return Value ? llvm::json::Value(*Value) : llvm::json::Value(nullptr);
}
llvm::json::Object volumeJSON(const DriverFileSystemVolume &Volume,
                              bool Observed) {
  llvm::json::Array Nodes;
  for (const auto &Node : Volume.Nodes) {
    llvm::json::Object Item{
        {"path", Node.Path},
        {"kind", Node.Kind == DriverFileNodeKind::Directory
                     ? llvm::json::Value("directory")
                 : Node.Kind == DriverFileNodeKind::File
                     ? llvm::json::Value("file")
                     : llvm::json::Value(nullptr)},
        {"file_id", number(Node.FileID)},
        {"attributes", number(Node.Attributes)},
        {"allocation_size", number(Node.AllocationSize)},
        {"creation_time_100ns", number(Node.CreationTime100ns)},
        {"last_access_time_100ns", number(Node.LastAccessTime100ns)},
        {"last_write_time_100ns", number(Node.LastWriteTime100ns)},
        {"change_time_100ns", number(Node.ChangeTime100ns)}};
    if (Node.Data)
      Item["data"] = llvm::toHex(llvm::ArrayRef(*Node.Data), true);
    if (Observed)
      Item["end_of_file"] = Node.Data ? uint64_t(Node.Data->size()) : 0;
    Nodes.push_back(std::move(Item));
  }
  return llvm::json::Object{
      {"device_name", Volume.DeviceName},
      {"access_policy",
       Volume.AccessPolicy == DriverFileAccessPolicy::FullAccess
           ? llvm::json::Value("full_access")
           : llvm::json::Value(nullptr)},
      {"sector_size", number(Volume.SectorSize)},
      {"alignment_mask", number(Volume.AlignmentMask)},
      {"nodes", std::move(Nodes)}};
}
} // namespace

llvm::Expected<std::vector<DriverFileSystemVolume>>
parse(const llvm::json::Value &Value) {
  const auto *Array = Value.getAsArray();
  if (!Array || Array->size() > MaxFileSystemVolumes)
    return invalid("declaration must be a bounded volume array");
  std::vector<DriverFileSystemVolume> Volumes;
  size_t Count = 0;
  for (const auto &Value : *Array) {
    const auto *Object = Value.getAsObject();
    if (!Object)
      return invalid("volume must be an object");
    if (auto E = fields(*Object, {"device_name", "access_policy", "sector_size",
                                  "alignment_mask", "nodes"}))
      return E;
    DriverFileSystemVolume Volume;
    auto Device = text(*Object, "device_name");
    if (!Device)
      return Device.takeError();
    Volume.DeviceName = std::move(*Device);
    auto Policy = text(*Object, "access_policy");
    if (!Policy)
      return Policy.takeError();
    if (*Policy != "full_access")
      return invalid("unknown access_policy");
    Volume.AccessPolicy = DriverFileAccessPolicy::FullAccess;
    auto Sector = number(*Object, "sector_size", UINT32_MAX);
    if (!Sector)
      return Sector.takeError();
    Volume.SectorSize = uint32_t(*Sector);
    auto Alignment = number(*Object, "alignment_mask", UINT32_MAX);
    if (!Alignment)
      return Alignment.takeError();
    Volume.AlignmentMask = uint32_t(*Alignment);
    const auto *Nodes = Object->getArray("nodes");
    if (!Nodes || Nodes->size() > MaxFileSystemNodes - Count)
      return invalid("nodes must be a bounded array");
    Count += Nodes->size();
    for (const auto &Value : *Nodes) {
      auto Node = node(Value);
      if (!Node)
        return Node.takeError();
      Volume.Nodes.push_back(std::move(*Node));
    }
    Volumes.push_back(std::move(Volume));
  }
  if (auto E = validateDriverFileSystem(Volumes))
    return std::move(E);
  return Volumes;
}
llvm::json::Value configurationJSON(
    const std::optional<std::vector<DriverFileSystemVolume>> &Volumes) {
  if (!Volumes)
    return nullptr;
  llvm::json::Array Result;
  for (const auto &Volume : *Volumes)
    Result.push_back(volumeJSON(Volume, false));
  return Result;
}
llvm::json::Value stateJSON(
    const std::optional<std::vector<DriverFileSystemVolumeState>> &Volumes) {
  if (!Volumes)
    return nullptr;
  llvm::json::Array Result;
  for (const auto &State : *Volumes) {
    auto Volume = volumeJSON(State.Volume, true);
    Volume["device_object"] = "0x" + llvm::utohexstr(State.DeviceObject);
    Result.push_back(std::move(Volume));
  }
  return Result;
}
} // namespace neverd::emulation::driver_filesystem
