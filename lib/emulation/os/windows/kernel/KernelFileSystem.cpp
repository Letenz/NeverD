//===- KernelFileSystem.cpp - Declared filesystem validation and ownership ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate one closed namespace and retain its independent live state.

#include "KernelFileSystem.h"

#include "WindowsKernelLayout.h"

#include "neverd/emulation/DriverProfile.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
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
uint32_t sharingDemand(uint32_t Access) {
  // FILE_EXECUTE shares with readers; FILE_APPEND_DATA shares with writers.
  // Metadata-only access does not contribute to shared-access counts.
  return (Access & 0x21 ? 1u : 0u) | (Access & 0x6 ? 2u : 0u) |
         (Access & 0x10000 ? 4u : 0u);
}
} // namespace

llvm::Expected<KernelFileSystem::QueryResult>
KernelFileSystem::queryInformation(uint64_t Token, uint32_t Class,
                                   uint32_t Length) const {
  const auto *Node = nodeForOpen(Token);
  if (!Node)
    return invalid("metadata query requires a retained provider open");
  uint32_t Size = 0;
  switch (Class) {
  case 4:
    Size = 40;
    break; // FILE_BASIC_INFORMATION
  case 5:
    Size = 24;
    break; // FILE_STANDARD_INFORMATION
  case 6:
    Size = 8;
    break; // FILE_INTERNAL_INFORMATION
  case 9:
    Size = 8;
    break; // FILE_NAME_INFORMATION minimum
  case 34:
    Size = 56;
    break; // FILE_NETWORK_OPEN_INFORMATION
  case 35:
    Size = 8;
    break; // FILE_ATTRIBUTE_TAG_INFORMATION
  default:
    return invalid("metadata information class is not implemented");
  }
  if (Length < Size)
    return QueryResult{windows::StatusInfoLengthMismatch, {}};
  QueryResult Result;
  if (Class == 9) {
    // MS-FSA FileNameInformation: the fixed minimum includes one WCHAR and
    // ULONG alignment; overflow returns complete UTF-16 units and full length.
    // https://learn.microsoft.com/openspecs/windows_protocols/ms-fsa/a64d92b8-038b-470b-ad53-dd2218516507
    const uint32_t NameBytes = uint32_t(Node->Path.size() * 2);
    const uint32_t Copied = std::min(NameBytes, (Length - 4) & ~1u);
    Result.Bytes.resize(4 + Copied);
    llvm::support::endian::write32le(Result.Bytes.data(), NameBytes);
    for (uint32_t I = 0; I != Copied / 2; ++I)
      Result.Bytes[4 + I * 2] = uint8_t(Node->Path[I]);
    if (Copied != NameBytes)
      Result.Status = windows::StatusBufferOverflow;
    return Result;
  }
  Result.Bytes.resize(Size);
  auto Put32 = [&](uint32_t Offset, uint32_t Value) {
    llvm::support::endian::write32le(Result.Bytes.data() + Offset, Value);
  };
  auto Put64 = [&](uint32_t Offset, uint64_t Value) {
    llvm::support::endian::write64le(Result.Bytes.data() + Offset, Value);
  };
  const uint32_t Attributes = *Node->Attributes ? *Node->Attributes : 0x80u;
  if (Class == 4 || Class == 34) {
    Put64(0, *Node->CreationTime100ns);
    Put64(8, *Node->LastAccessTime100ns);
    Put64(16, *Node->LastWriteTime100ns);
    Put64(24, *Node->ChangeTime100ns);
    Put32(Class == 4 ? 32 : 48, Attributes);
  }
  if (Class == 5 || Class == 34) {
    const uint32_t Offset = Class == 5 ? 0 : 32;
    Put64(Offset, *Node->AllocationSize);
    Put64(Offset + 8, Node->Data ? Node->Data->size() : 0);
  }
  if (Class == 5) {
    // The admitted namespace has one link per node and no deletion yet.
    Put32(16, 1);
    Result.Bytes[21] = Node->Kind == DriverFileNodeKind::Directory;
  } else if (Class == 6) {
    Put64(0, *Node->FileID);
  } else if (Class == 35) {
    Put32(0, Attributes); // Reparse attributes are rejected at admission.
  }
  return Result;
}

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

llvm::Expected<const DriverFileSystemNode *>
KernelFileSystem::lookupNode(uint64_t DeviceObject, llvm::StringRef Path,
                             bool FoldCase) const {
  const auto *Volume = volumeForDevice(DeviceObject);
  if (!Volume)
    return invalid("node lookup requires a bound volume");
  if (!path(Path))
    return invalid("file name requires a bounded rooted ASCII path");
  for (const auto &Node : Volume->Nodes)
    if (FoldCase ? Path.equals_insensitive(Node.Path) : Path == Node.Path)
      return &Node;
  return nullptr;
}

llvm::Expected<KernelFileSystem::OpenResult>
KernelFileSystem::reserveOpen(uint64_t DeviceObject, uint64_t FileID,
                              uint64_t FileObject, uint32_t GrantedAccess,
                              uint32_t ShareAccess) {
  const auto *Volume = volumeForDevice(DeviceObject);
  if (!Initialized || !Volume || !FileObject)
    return invalid("open requires a bound volume and a nonzero FILE_OBJECT");
  if (GrantedAccess & ~windows::FileAllAccess || ShareAccess & ~7u)
    return invalid("open requires mapped file rights and supported sharing");
  if (std::none_of(Volume->Nodes.begin(), Volume->Nodes.end(),
                   [&](const auto &Node) { return Node.FileID == FileID; }))
    return invalid("open requires an existing node in its bound volume");
  for (const auto &[Token, Existing] : Opens)
    if (Existing.FileObject == FileObject)
      return invalid("FILE_OBJECT already owns a provider open");

  const auto Demand = sharingDemand(GrantedAccess);
  if (Demand)
    for (const auto &[Token, Existing] : Opens) {
      if (Existing.Cleaned || Existing.DeviceObject != DeviceObject ||
          Existing.FileID != FileID)
        continue;
      const auto ExistingDemand = sharingDemand(Existing.GrantedAccess);
      if (ExistingDemand &&
          ((Demand & ~Existing.ShareAccess) || (ExistingDemand & ~ShareAccess)))
        return OpenResult{windows::StatusSharingViolation, 0};
    }
  if (Opens.size() >= MaxFileSystemOpens || NextOpen == UINT64_MAX)
    return invalid("provider open inventory or token space exhausted");
  const auto Token = NextOpen++;
  Opens.emplace(Token, Open{Token, FileObject, DeviceObject, FileID,
                            GrantedAccess, ShareAccess, Demand, false});
  return OpenResult{windows::StatusSuccess, Token};
}

const KernelFileSystem::Open *KernelFileSystem::findOpen(uint64_t Token) const {
  const auto Found = Opens.find(Token);
  return Found == Opens.end() ? nullptr : &Found->second;
}

const DriverFileSystemNode *
KernelFileSystem::nodeForOpen(uint64_t Token) const {
  const auto *Open = findOpen(Token);
  if (Open)
    if (const auto *Volume = volumeForDevice(Open->DeviceObject))
      for (const auto &Node : Volume->Nodes)
        if (Node.FileID == Open->FileID)
          return &Node;
  return nullptr;
}

llvm::Error KernelFileSystem::cleanupOpen(uint64_t Token) {
  const auto Found = Opens.find(Token);
  if (Found == Opens.end())
    return invalid("CLEANUP requires a live provider token");
  // Closing a reopened handle can repeat CLEANUP. Its old reservation remains
  // released even while pointer references or handles retain this body.
  Found->second.Cleaned = true;
  return llvm::Error::success();
}

llvm::Error KernelFileSystem::closeOpen(uint64_t Token) {
  const auto Found = Opens.find(Token);
  if (Found == Opens.end() || !Found->second.Cleaned)
    return invalid("CLOSE requires a cleaned live provider token");
  Opens.erase(Found);
  return llvm::Error::success();
}

llvm::Error KernelFileSystem::validateOpenBindings(
    llvm::ArrayRef<OpenBinding> Bindings) const {
  if (!Initialized)
    return invalid("cannot validate opens on an uninitialized owner");
  if (Bindings.size() != Opens.size())
    return invalid("provider tokens and FILE_OBJECT bindings differ");
  std::set<uint64_t> Seen;
  for (const auto &Binding : Bindings) {
    const auto *Open = findOpen(Binding.Token);
    if (!Open || Open->FileObject != Binding.FileObject ||
        !Seen.insert(Binding.Token).second)
      return invalid("provider token has a missing, duplicate or wrong "
                     "FILE_OBJECT binding");
  }
  return llvm::Error::success();
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
