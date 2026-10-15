//===- DriverFileSystemTests.cpp - Explicit filesystem admission ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Test required metadata, closed trees, transport limits and owner snapshots.

#include "gtest/gtest.h"
#include "os/windows/driver/DriverFileSystemJSON.h"
#include "os/windows/kernel/KernelFileSystem.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
using Kind = DriverFileNodeKind;
using Inventory = std::vector<DriverFileSystemVolume>;
DriverFileSystemNode node(std::string Path, uint64_t ID, Kind NodeKind) {
  DriverFileSystemNode Node;
  Node.Path = std::move(Path);
  Node.Kind = NodeKind;
  Node.FileID = ID;
  Node.Attributes = NodeKind == Kind::Directory ? 0x10 : 0x80;
  Node.AllocationSize = 0;
  Node.CreationTime100ns = 0;
  Node.LastAccessTime100ns = 1;
  Node.LastWriteTime100ns = 2;
  Node.ChangeTime100ns = 3;
  if (NodeKind == Kind::File)
    Node.Data.emplace();
  return Node;
}
Inventory inventory() {
  DriverFileSystemVolume Volume;
  Volume.DeviceName = "\\Device\\DeclaredVolume";
  Volume.AccessPolicy = DriverFileAccessPolicy::FullAccess;
  Volume.SectorSize = 512;
  Volume.AlignmentMask = 0;
  Volume.Nodes = {node("\\", 0, Kind::Directory),
                  node("\\Data", 5, Kind::File)};
  Volume.Nodes[1].Data = std::vector<uint8_t>{0x41, 0, 0xff};
  Volume.Nodes[1].AllocationSize = 512;
  return {Volume};
}
void ok(llvm::Error E) {
  if (E)
    ADD_FAILURE() << llvm::toString(std::move(E));
}
template <class T> T take(llvm::Expected<T> Value) {
  if (!Value) {
    ADD_FAILURE() << llvm::toString(Value.takeError());
    return {};
  }
  return std::move(*Value);
}
void rejects(llvm::Error E, llvm::StringRef Fragment = {}) {
  ASSERT_TRUE(bool(E));
  auto Text = llvm::toString(std::move(E));
  EXPECT_NE(Text.find(Fragment.str()), std::string::npos) << Text;
}

TEST(DriverFileSystemDeclaration,
     OmissionEmptyInventoryAndRootOnlyStayDistinct) {
  ok(validateDriverFileSystem(std::nullopt));
  ok(validateDriverFileSystem(Inventory{}));
  auto V = inventory();
  V[0].Nodes.resize(1);
  ok(validateDriverFileSystem(V));
  V[0].Nodes.clear();
  rejects(validateDriverFileSystem(V), "root directory");
  DriverOptions Base;
  Base.FileSystem = inventory();
  EXPECT_EQ(take(driverOptionsFromScenarioJSON("{}", Base)).FileSystem->size(),
            1u);
  EXPECT_TRUE(take(driverOptionsFromScenarioJSON("{\"filesystem\":[]}", Base))
                  .FileSystem->empty());
  auto Null = driverOptionsFromScenarioJSON("{\"filesystem\":null}", Base);
  ASSERT_FALSE(bool(Null));
  rejects(Null.takeError(), "array");
}

TEST(DriverFileSystemDeclaration, EveryRequiredZeroHasPresence) {
  auto V = inventory();
  V[0].Nodes[1] = node("\\Data", 1, Kind::File);
  ok(validateDriverFileSystem(V));
  for (auto Field :
       {&DriverFileSystemNode::FileID, &DriverFileSystemNode::AllocationSize,
        &DriverFileSystemNode::CreationTime100ns,
        &DriverFileSystemNode::LastAccessTime100ns,
        &DriverFileSystemNode::LastWriteTime100ns,
        &DriverFileSystemNode::ChangeTime100ns}) {
    auto Missing = V;
    (Missing[0].Nodes[1].*Field).reset();
    rejects(validateDriverFileSystem(Missing));
  }
  for (unsigned Field = 0; Field != 6; ++Field) {
    auto Missing = V;
    switch (Field) {
    case 0:
      Missing[0].Nodes[1].Kind.reset();
      break;
    case 1:
      Missing[0].Nodes[1].Attributes.reset();
      break;
    case 2:
      Missing[0].Nodes[1].Data.reset();
      break;
    case 3:
      Missing[0].AccessPolicy.reset();
      break;
    case 4:
      Missing[0].SectorSize.reset();
      break;
    case 5:
      Missing[0].AlignmentMask.reset();
      break;
    }
    rejects(validateDriverFileSystem(Missing));
  }
  V[0].Nodes[0].Data.emplace();
  rejects(validateDriverFileSystem(V), "directories");
}

TEST(DriverFileSystemDeclaration,
     CanonicalIdentityRequiresParentsAndUniqueIDs) {
  auto V = inventory();
  V[0].Nodes.push_back(node("\\Folder", 3, Kind::Directory));
  V[0].Nodes.push_back(node("\\Folder\\Child", UINT64_MAX, Kind::File));
  std::reverse(V[0].Nodes.begin(), V[0].Nodes.end());
  ok(validateDriverFileSystem(V));
  auto Duplicate = V;
  Duplicate[0].Nodes.push_back(node("\\folder\\cHILD", 7, Kind::File));
  rejects(validateDriverFileSystem(Duplicate), "duplicate node path");
  Duplicate = V;
  Duplicate[0].Nodes[0].FileID = 0;
  rejects(validateDriverFileSystem(Duplicate), "file_id");
  auto Missing = V;
  Missing[0].Nodes.erase(Missing[0].Nodes.begin() + 1);
  rejects(validateDriverFileSystem(Missing), "parent");
  V.push_back(V.front());
  V.back().DeviceName = "\\Device\\OtherVolume";
  ok(validateDriverFileSystem(V)); // IDs belong to a volume, not the session.
  V.back().DeviceName = "\\device\\declaredvolume";
  rejects(validateDriverFileSystem(V), "duplicate device");
}

TEST(DriverFileSystemDeclaration, UnsupportedMetadataAndPathShapesReject) {
  for (std::string Path :
       {"relative", "\\\\two", "\\trailing\\", "\\.", "\\..", "\\file:stream",
        "\\bad/name", "\\*.bin", "\\x\x7f"}) {
    auto V = inventory();
    V[0].Nodes[1].Path = Path;
    rejects(validateDriverFileSystem(V), "path");
  }
  for (uint32_t Attributes : {0x10u, 0x81u, 0x200u, 0x400u, 0x800u, 0x4000u}) {
    auto V = inventory();
    V[0].Nodes[1].Attributes = Attributes;
    rejects(validateDriverFileSystem(V), "attributes");
  }
  for (uint64_t Allocation : {0, 3, 511, 513}) {
    auto V = inventory();
    V[0].Nodes[1].AllocationSize = Allocation;
    rejects(validateDriverFileSystem(V), "allocation");
  }
  auto V = inventory();
  V[0].Nodes[1].CreationTime100ns = uint64_t(INT64_MAX) + 1;
  rejects(validateDriverFileSystem(V), "timestamps");
  V = inventory();
  V[0].SectorSize = 513;
  rejects(validateDriverFileSystem(V), "sector_size");
  V = inventory();
  V[0].AlignmentMask = 2;
  rejects(validateDriverFileSystem(V), "alignment_mask");
}

TEST(DriverFileSystemDeclaration,
     NativeContentAndAllocationCapsAreIndependent) {
  auto V = inventory();
  V[0].Nodes[1].Data->resize(MaxFileSystemFileBytes, 0x5a);
  V[0].Nodes[1].AllocationSize = MaxFileSystemAllocationBytes;
  ok(validateDriverFileSystem(V));
  V[0].Nodes[1].Data->push_back(0);
  rejects(validateDriverFileSystem(V), "bounded data");
  V[0].Nodes[1].Data->pop_back();
  *V[0].Nodes[1].AllocationSize += 512;
  rejects(validateDriverFileSystem(V), "inventory exceeds");
  V = inventory();
  V[0].Nodes.resize(1);
  for (size_t I = 0; I != MaxFileSystemContentBytes / MaxFileSystemFileBytes;
       ++I) {
    auto File = node("\\File" + std::to_string(I), I + 1, Kind::File);
    File.Data->resize(MaxFileSystemFileBytes, uint8_t(I));
    File.AllocationSize = MaxFileSystemFileBytes;
    V[0].Nodes.push_back(std::move(File));
  }
  ok(validateDriverFileSystem(V));
  auto Extra = node("\\Extra", 100, Kind::File);
  Extra.Data = std::vector<uint8_t>{1};
  Extra.AllocationSize = 512;
  V[0].Nodes.push_back(Extra);
  rejects(validateDriverFileSystem(V), "inventory exceeds");
}

TEST(DriverFileSystemDeclaration,
     JSONRoundTripRejectsUnknownAndDuplicateFields) {
  auto V = inventory();
  auto JSON = driver_filesystem::configurationJSON(V);
  auto Parsed = take(driver_filesystem::parse(JSON));
  ASSERT_EQ(Parsed.size(), 1u);
  EXPECT_EQ(Parsed[0].Nodes[1].Data, V[0].Nodes[1].Data);
  EXPECT_EQ(Parsed[0].Nodes[0].FileID, 0u);
  EXPECT_FALSE(Parsed[0].Nodes[0].Data);
  EXPECT_EQ(Parsed[0].AlignmentMask, 0u);
  auto &Volume = *JSON.getAsArray()->front().getAsObject();
  Volume["aliases"] = llvm::json::Array{};
  auto Bad = driver_filesystem::parse(JSON);
  ASSERT_FALSE(bool(Bad));
  rejects(Bad.takeError(), "unknown field");
  auto Duplicate = driverOptionsFromScenarioJSON(
      R"({"filesystem":[{"device_name":"x","device_\u006eame":"y"}]})");
  ASSERT_FALSE(bool(Duplicate));
  rejects(Duplicate.takeError(), "duplicate field");
}

TEST(DriverFileSystemDeclaration, ExactNameNodeVolumeAndPathLimits) {
  auto V = inventory();
  V[0].DeviceName =
      "\\Device\\" + std::string(profile::MaxDeviceNameSize - 8, 'x');
  V[0].Nodes[1].Path = "\\" + std::string(MaxFileSystemPathBytes - 1, 'x');
  ok(validateDriverFileSystem(V));
  V[0].Nodes[1].Path.push_back('x');
  rejects(validateDriverFileSystem(V), "path");
  V[0].Nodes[1].Path.pop_back();
  V[0].DeviceName.push_back('x');
  rejects(validateDriverFileSystem(V), "device_name");
  V = inventory();
  V[0].Nodes.resize(1);
  while (V.size() != MaxFileSystemVolumes) {
    auto Volume = V.front();
    Volume.DeviceName += std::to_string(V.size());
    V.push_back(std::move(Volume));
  }
  ok(validateDriverFileSystem(V));
  V.push_back(V.front());
  rejects(validateDriverFileSystem(V), "volume count");
  V = inventory();
  V[0].Nodes.resize(1);
  while (V[0].Nodes.size() != MaxFileSystemNodes) {
    const auto I = V[0].Nodes.size();
    V[0].Nodes.push_back(node("\\" + std::to_string(I), I, Kind::File));
  }
  ok(validateDriverFileSystem(V));
  V[0].Nodes.push_back(node("\\extra", MaxFileSystemNodes, Kind::File));
  rejects(validateDriverFileSystem(V), "node or name");
  V = inventory();
  V[0].Nodes.resize(1);
  size_t Names = V[0].DeviceName.size() + 1;
  while (Names < MaxFileSystemNameBytes) {
    const auto I = V[0].Nodes.size();
    const auto Length =
        std::min(MaxFileSystemPathBytes, MaxFileSystemNameBytes - Names);
    auto Path = "\\" + std::to_string(I);
    ASSERT_LE(Path.size(), Length);
    Path.resize(Length, 'x');
    V[0].Nodes.push_back(node(Path, I, Kind::File));
    Names += Length;
  }
  ok(validateDriverFileSystem(V));
  V[0].Nodes.back().Path.push_back('x');
  rejects(validateDriverFileSystem(V), "inventory exceeds");
}

TEST(DriverFileSystemDeclaration, NativeAndJSONShareMetadataAdmission) {
  for (unsigned Change = 0; Change != 9; ++Change) {
    auto V = inventory();
    switch (Change) {
    case 0:
      V[0].Nodes[1].Attributes = 0x400;
      break;
    case 1:
      V[0].Nodes[1].AllocationSize = 513;
      break;
    case 2:
      V[0].Nodes[1].FileID = 0;
      break;
    case 3:
      V[0].Nodes[1].Path = "\\Missing\\Child";
      break;
    case 4:
      V[0].SectorSize = 8192;
      break;
    case 5:
      V[0].AlignmentMask = 512;
      break;
    case 6:
      V[0].Nodes[0].Data.emplace();
      break;
    case 7:
      V[0].Nodes[1].LastWriteTime100ns = UINT64_MAX;
      break;
    case 8:
      V[0].Nodes[1].Data.reset();
      break;
    }
    rejects(validateDriverFileSystem(V));
    auto Parsed =
        driver_filesystem::parse(driver_filesystem::configurationJSON(V));
    ASSERT_FALSE(bool(Parsed));
    rejects(Parsed.takeError());
  }
  auto JSON = driver_filesystem::configurationJSON(inventory());
  auto &File =
      *(*JSON.getAsArray()->front().getAsObject()->getArray("nodes"))[1]
           .getAsObject();
  for (const char *Data : {"0", "xy", "0g"}) {
    File["data"] = Data;
    auto Parsed = driver_filesystem::parse(JSON);
    ASSERT_FALSE(bool(Parsed));
    rejects(Parsed.takeError(), "data");
  }
  File["data"] = "";
  File["file_id"] = UINT64_MAX;
  EXPECT_EQ(take(driver_filesystem::parse(JSON))[0].Nodes[1].FileID,
            UINT64_MAX);
  File["unknown"] = 0;
  auto Parsed = driver_filesystem::parse(JSON);
  ASSERT_FALSE(bool(Parsed));
  rejects(Parsed.takeError(), "unknown field");
}

TEST(DriverFileSystemDeclaration,
     PartialAndDuplicateBindingsNeverPublishVolumes) {
  auto Input = inventory();
  Input.push_back(Input.front());
  Input.back().DeviceName = "\\Device\\SecondVolume";
  KernelFileSystem Owner;
  ok(Owner.initialize(Input));
  ok(Owner.bindDevice(0, 0x1000));
  auto Partial = Owner.snapshot();
  ASSERT_FALSE(bool(Partial));
  rejects(Partial.takeError(), "unbound");
  rejects(Owner.bindDevice(1, 0x1000), "distinct");
  EXPECT_EQ(Owner.volumeForDevice(0x1000)->DeviceName,
            Input.front().DeviceName);
  ok(Owner.bindDevice(1, 0x2000));
  auto State = take(Owner.snapshot());
  ASSERT_TRUE(State);
  ASSERT_EQ(State->size(), 2u);
  EXPECT_EQ((*State)[0].DeviceObject, 0x1000u);
  EXPECT_EQ((*State)[1].DeviceObject, 0x2000u);
}

TEST(DriverFileSystemDeclaration,
     OwnerSnapshotRequiresBoundDevicesAndCopiesState) {
  auto Input = inventory();
  KernelFileSystem Owner;
  ok(Owner.initialize(Input));
  auto Before = Owner.snapshot();
  ASSERT_FALSE(bool(Before));
  rejects(Before.takeError(), "unbound");
  ok(Owner.bindDevice(0, 0x12345000));
  Input[0].Nodes[1].Data->clear();
  auto State = take(Owner.snapshot());
  ASSERT_TRUE(State);
  ASSERT_EQ(State->size(), 1u);
  EXPECT_EQ(State->front().DeviceObject, 0x12345000u);
  EXPECT_EQ(*State->front().Volume.Nodes[1].Data,
            (std::vector<uint8_t>{0x41, 0, 0xff}));
  EXPECT_TRUE(Owner.ownsDevice(0x12345000));
  EXPECT_FALSE(Owner.ownsDevice(0));
  rejects(Owner.bindDevice(0, 0x12346000));
  rejects(Owner.initialize(Input), "twice");
}
} // namespace
} // namespace neverd::emulation
