//===- KernelFileSystem.cpp - Declared filesystem validation and ownership ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate one closed namespace and retain its independent live state.

#include "KernelFileSystem.h"

#include "neverd/emulation/DriverProfile.h"

#include "llvm/ADT/StringRef.h"

#include <map>
#include <set>

namespace neverd::emulation {
namespace {
llvm::Error invalid(const llvm::Twine &Text) {
  return llvm::createStringError("driver filesystem: " + Text);
}
std::string key(llvm::StringRef Text) {
  std::string Result = Text.str();
  for (char &C : Result)
    if (C >= 'a' && C <= 'z')
      C -= 'a' - 'A';
  return Result;
}
bool component(llvm::StringRef Text) {
  if (Text.empty() || Text == "." || Text == "..")
    return false;
  for (unsigned char C : Text)
    if (C < ' ' || C > '~' || llvm::StringRef("\\/:*?\"<>|").contains(C))
      return false;
  return true;
}
bool path(llvm::StringRef Text) {
  if (Text.empty() || Text.size() > MaxFileSystemPathBytes ||
      Text.front() != '\\')
    return false;
  if (Text == "\\")
    return true;
  Text = Text.drop_front();
  while (!Text.empty()) {
    auto [Head, Tail] = Text.split('\\');
    if (!component(Head) || (Tail.empty() && Text.back() == '\\'))
      return false;
    Text = Tail;
  }
  return true;
}
bool accumulate(uint64_t &Total, uint64_t Amount, uint64_t Limit) {
  if (Total > Limit || Amount > Limit - Total)
    return false;
  Total += Amount;
  return true;
}
} // namespace

llvm::Error validateDriverFileSystem(
    const std::optional<std::vector<DriverFileSystemVolume>> &Volumes) {
  if (!Volumes)
    return llvm::Error::success();
  if (Volumes->size() > MaxFileSystemVolumes)
    return invalid("volume count exceeds the limit");
  uint64_t Nodes = 0, Names = 0, Content = 0, Allocation = 0, Report = 0;
  std::set<std::string> Devices;
  for (const auto &Volume : *Volumes) {
    const auto Device = key(Volume.DeviceName);
    constexpr llvm::StringLiteral Prefix = "\\DEVICE\\";
    if (Device.size() > profile::MaxDeviceNameSize ||
        !llvm::StringRef(Device).starts_with(Prefix) ||
        !component(llvm::StringRef(Device).drop_front(Prefix.size())))
      return invalid("device_name requires one bounded ASCII device component");
    if (!Devices.insert(Device).second)
      return invalid("duplicate device name");
    if (Volume.AccessPolicy != DriverFileAccessPolicy::FullAccess)
      return invalid("explicit full_access policy is required");
    if (!Volume.SectorSize || *Volume.SectorSize < 512 ||
        *Volume.SectorSize > 4096 ||
        (*Volume.SectorSize & (*Volume.SectorSize - 1)))
      return invalid("sector_size must be a power of two from 512 to 4096");
    if (!Volume.AlignmentMask || *Volume.AlignmentMask >= *Volume.SectorSize ||
        (*Volume.AlignmentMask & (*Volume.AlignmentMask + 1)))
      return invalid("alignment_mask must be 2^n-1 below sector_size");
    if (!accumulate(Nodes, Volume.Nodes.size(), MaxFileSystemNodes) ||
        !accumulate(Names, Device.size(), MaxFileSystemNameBytes))
      return invalid("node or name inventory exceeds its limit");
    std::map<std::string, DriverFileNodeKind> Paths;
    std::set<uint64_t> IDs;
    for (const auto &Node : Volume.Nodes) {
      if (!path(Node.Path))
        return invalid("node path must be a bounded rooted ASCII path");
      if (!Node.Kind || (*Node.Kind != DriverFileNodeKind::File &&
                         *Node.Kind != DriverFileNodeKind::Directory))
        return invalid("node kind must be file or directory");
      const bool Directory = *Node.Kind == DriverFileNodeKind::Directory;
      if (!Paths.emplace(key(Node.Path), *Node.Kind).second)
        return invalid("duplicate node path");
      if (!Node.FileID || !IDs.insert(*Node.FileID).second)
        return invalid(
            "node file_id must be explicitly unique within its volume");
      // FILE_BASIC_INFORMATION permits NORMAL only without other attributes.
      constexpr uint32_t DirectoryAttribute = 0x10, NormalAttribute = 0x80;
      constexpr uint32_t AdmittedAttributes =
          0x1 | 0x2 | 0x4 | 0x10 | 0x20 | 0x80;
      if (!Node.Attributes || (*Node.Attributes & ~AdmittedAttributes) ||
          bool(*Node.Attributes & DirectoryAttribute) != Directory ||
          ((*Node.Attributes & NormalAttribute) &&
           *Node.Attributes != NormalAttribute))
        return invalid("node attributes disagree with kind or require "
                       "unsupported semantics");
      for (const auto &Time : {Node.CreationTime100ns, Node.LastAccessTime100ns,
                               Node.LastWriteTime100ns, Node.ChangeTime100ns})
        if (!Time || *Time > INT64_MAX)
          return invalid("all four node timestamps must be explicit "
                         "nonnegative signed64 values");
      if (!Node.AllocationSize)
        return invalid("allocation_size is required");
      if (Directory) {
        if (Node.Data || *Node.AllocationSize)
          return invalid(
              "directories require no data and zero allocation_size");
      } else {
        if (!Node.Data || Node.Data->size() > MaxFileSystemFileBytes ||
            *Node.AllocationSize < Node.Data->size() ||
            *Node.AllocationSize % *Volume.SectorSize)
          return invalid("file requires bounded data and sector-aligned "
                         "allocation covering EOF");
      }
      if (!accumulate(Names, Node.Path.size(), MaxFileSystemNameBytes) ||
          !accumulate(Content, Node.Data ? Node.Data->size() : 0,
                      MaxFileSystemContentBytes) ||
          !accumulate(Allocation, *Node.AllocationSize,
                      MaxFileSystemAllocationBytes))
        return invalid(
            "name, content or allocation inventory exceeds its limit");
    }
    const auto Root = Paths.find("\\");
    if (Root == Paths.end() || Root->second != DriverFileNodeKind::Directory)
      return invalid("volume requires an explicit root directory");
    for (const auto &[Path, Kind] : Paths) {
      if (Path == "\\")
        continue;
      const auto Last = Path.rfind('\\');
      const auto Parent = Paths.find(Last ? Path.substr(0, Last) : "\\");
      if (Parent == Paths.end() ||
          Parent->second != DriverFileNodeKind::Directory)
        return invalid("node parent must be an explicitly declared directory");
    }
  }
  // Hex bytes and JSON-escaped ASCII names appear in configuration and final
  // state. Fixed allowances cover both metadata records and device addresses.
  // Bounds above make multiplication safe; subtraction checks the independent
  // filesystem contribution allowance without bounding unrelated event data.
  if (!accumulate(Report, 4 * Content, MaxFileSystemReportBytes) ||
      !accumulate(Report, 4 * Names, MaxFileSystemReportBytes) ||
      !accumulate(Report, 2048 * Nodes, MaxFileSystemReportBytes) ||
      !accumulate(Report, 4096 * Volumes->size(), MaxFileSystemReportBytes))
    return invalid("filesystem report contribution exceeds its limit");
  return llvm::Error::success();
}

llvm::Error KernelFileSystem::initialize(
    const std::optional<std::vector<DriverFileSystemVolume>> &Input) {
  if (Initialized)
    return invalid("owner cannot be initialized twice");
  if (auto E = validateDriverFileSystem(Input))
    return E;
  if (Input) {
    Volumes.emplace();
    for (const auto &Volume : *Input)
      Volumes->push_back({Volume, 0});
  }
  Initialized = true;
  return llvm::Error::success();
}

llvm::Error KernelFileSystem::bindDevice(size_t Volume, uint64_t DeviceObject) {
  if (!Initialized || !Volumes || Volume >= Volumes->size() || !DeviceObject ||
      (*Volumes)[Volume].DeviceObject || ownsDevice(DeviceObject))
    return invalid("volume requires one distinct canonical device binding");
  (*Volumes)[Volume].DeviceObject = DeviceObject;
  return llvm::Error::success();
}
const DriverFileSystemVolume *
KernelFileSystem::volumeForDevice(uint64_t DeviceObject) const {
  if (Volumes && DeviceObject)
    for (const auto &Volume : *Volumes)
      if (Volume.DeviceObject == DeviceObject)
        return &Volume.Volume;
  return nullptr;
}
bool KernelFileSystem::ownsDevice(uint64_t DeviceObject) const {
  return volumeForDevice(DeviceObject) != nullptr;
}
llvm::Expected<std::optional<std::vector<DriverFileSystemVolumeState>>>
KernelFileSystem::snapshot() const {
  if (!Initialized)
    return invalid("cannot snapshot an uninitialized owner");
  if (Volumes)
    for (const auto &Volume : *Volumes)
      if (!Volume.DeviceObject)
        return invalid("cannot publish an unbound volume device");
  return Volumes;
}
} // namespace neverd::emulation
