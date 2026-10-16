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
#include "os/windows/kernel/WindowsKernelLayout.h"

#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>

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

TEST(DriverFileSystemDeclaration, MetadataUsesDeclaredIdentityAndBoundedNames) {
  KernelFileSystem FS;
  auto V = inventory();
  V[0].Nodes[1].FileID = UINT64_MAX;
  ok(FS.initialize(V));
  ok(FS.bindDevice(0, 0x1000));
  const auto Open = take(FS.reserveOpen(0x1000, UINT64_MAX, 0x2000, 0x80, 7));
  ASSERT_EQ(Open.Status, 0u);
  for (auto [Class, Size] :
       {std::pair{4u, 40u}, {5u, 24u}, {6u, 8u}, {34u, 56u}, {35u, 8u}}) {
    for (uint32_t Length = 0; Length <= Size + 1; ++Length) {
      auto Result = take(FS.queryInformation(Open.Token, Class, Length));
      if (Length < Size) {
        EXPECT_EQ(Result.Status, windows::StatusInfoLengthMismatch);
        EXPECT_TRUE(Result.Bytes.empty());
        continue;
      }
      EXPECT_EQ(Result.Status, 0u);
      ASSERT_EQ(Result.Bytes.size(), Size);
      auto U64 = [&](unsigned Offset) {
        return llvm::support::endian::read64le(Result.Bytes.data() + Offset);
      };
      if (Class == 4 || Class == 34) {
        EXPECT_EQ(U64(0), 0u);
        EXPECT_EQ(U64(8), 1u);
        EXPECT_EQ(U64(16), 2u);
        EXPECT_EQ(U64(24), 3u);
      }
      if (Class == 5 || Class == 34) {
        EXPECT_EQ(U64(Class == 5 ? 0 : 32), 512u);
        EXPECT_EQ(U64(Class == 5 ? 8 : 40), 3u);
      }
      if (Class == 6)
        EXPECT_EQ(U64(0), UINT64_MAX);
    }
  }
  for (uint32_t Length = 0; Length <= 18; ++Length) {
    auto Result = take(FS.queryInformation(Open.Token, 9, Length));
    if (Length < 8) {
      EXPECT_EQ(Result.Status, windows::StatusInfoLengthMismatch);
      EXPECT_TRUE(Result.Bytes.empty());
      continue;
    }
    const uint32_t Copied = std::min(10u, (Length - 4) & ~1u);
    ASSERT_EQ(Result.Bytes.size(), 4u + Copied);
    EXPECT_EQ(Result.Status, Copied == 10 ? 0u : windows::StatusBufferOverflow);
    EXPECT_EQ(llvm::support::endian::read32le(Result.Bytes.data()), 10u);
    const std::array<uint8_t, 10> Name{'\\', 0, 'D', 0, 'a', 0, 't', 0, 'a', 0};
    EXPECT_TRUE(
        std::equal(Result.Bytes.begin() + 4, Result.Bytes.end(), Name.begin()));
  }
  auto Unsupported = FS.queryInformation(Open.Token, 59, 64);
  ASSERT_FALSE(bool(Unsupported));
  rejects(Unsupported.takeError(), "not implemented");
  ok(FS.cleanupOpen(Open.Token));
  EXPECT_EQ(take(FS.queryInformation(Open.Token, 6, 8)).Status, 0u);
  ok(FS.closeOpen(Open.Token));
  auto Closed = FS.queryInformation(Open.Token, 6, 8);
  ASSERT_FALSE(bool(Closed));
  rejects(Closed.takeError(), "retained provider open");
  const auto Directory = take(FS.reserveOpen(0x1000, 0, 0x3000, 0x80, 7));
  ASSERT_EQ(Directory.Status, 0u);
  EXPECT_EQ(take(FS.queryInformation(Directory.Token, 5, 24)).Bytes,
            (std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                  0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0}));
  EXPECT_EQ(take(FS.queryInformation(Directory.Token, 9, 8)).Bytes,
            (std::vector<uint8_t>{2, 0, 0, 0, '\\', 0}));
  EXPECT_EQ(take(FS.queryInformation(Directory.Token, 35, 8)).Bytes,
            (std::vector<uint8_t>{0x10, 0, 0, 0, 0, 0, 0, 0}));
  ok(FS.cleanupOpen(Directory.Token));
  ok(FS.closeOpen(Directory.Token));
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

struct OpenOwner {
  KernelFileSystem Owner;
  OpenOwner() {
    auto Volumes = inventory();
    Volumes[0].Nodes.push_back(node("\\Other", 6, Kind::File));
    Volumes.push_back(Volumes.front());
    Volumes.back().DeviceName = "\\Device\\SecondVolume";
    ok(Owner.initialize(Volumes));
    ok(Owner.bindDevice(0, 0x1000));
    ok(Owner.bindDevice(1, 0x2000));
  }
  KernelFileSystem::OpenResult reserve(uint64_t Body, uint32_t Access,
                                       uint32_t Share, uint64_t ID = 5,
                                       uint64_t Device = 0x1000) {
    return take(Owner.reserveOpen(Device, ID, Body, Access, Share));
  }
  void retire(uint64_t Token) {
    ok(Owner.cleanupOpen(Token));
    ok(Owner.closeOpen(Token));
  }
};

TEST(DriverFileSystemOpen, NoDeclarationOrBindingCannotInventAnOpen) {
  for (unsigned State = 0; State != 4; ++State) {
    KernelFileSystem Owner;
    if (State == 0)
      rejects(Owner.validateOpenBindings({}), "uninitialized");
    else if (State == 1)
      ok(Owner.initialize(std::nullopt));
    else if (State == 2)
      ok(Owner.initialize(Inventory{}));
    else
      ok(Owner.initialize(inventory()));
    auto Open = Owner.reserveOpen(0x1000, 5, 0x3000, 1, 7);
    ASSERT_FALSE(bool(Open));
    rejects(Open.takeError(), "bound volume");
    EXPECT_EQ(Owner.openCount(), 0u);
    if (State)
      ok(Owner.validateOpenBindings({}));
  }
}

TEST(DriverFileSystemOpen, SharingChecksExistingAndIncomingAccess) {
  // The three access categories and all sharing combinations. Expected
  // results use SHARE_ACCESS-style counts, independently of token storage.
  // The same 4096 pairs were executed through the original public kernel
  // share helpers; execute/append and metadata rights have separate cases.
  constexpr std::array<uint32_t, 8> Access = {
      0, 1, 2, 3, 0x10000, 0x10001, 0x10002, 0x10003};
  OpenOwner S;
  for (unsigned Existing = 0; Existing != 8; ++Existing)
    for (unsigned Incoming = 0; Incoming != 8; ++Incoming)
      for (unsigned OldShare = 0; OldShare != 8; ++OldShare)
        for (unsigned NewShare = 0; NewShare != 8; ++NewShare) {
          SCOPED_TRACE(::testing::Message()
                       << Existing << ',' << Incoming << ',' << OldShare << ','
                       << NewShare);
          const auto First = S.reserve(0x3000, Access[Existing], OldShare);
          ASSERT_EQ(First.Status, windows::StatusSuccess);
          ASSERT_NE(First.Token, 0u);
          bool Compatible = true;
          if (Existing && Incoming)
            for (unsigned Bit = 1; Bit <= 4; Bit <<= 1) {
              const unsigned Users = bool(Existing & Bit);
              const unsigned Sharers = bool(OldShare & Bit);
              if ((Incoming & Bit) && Sharers != 1)
                Compatible = false;
              if (Users && !(NewShare & Bit))
                Compatible = false;
            }
          const auto Second = S.reserve(0x4000, Access[Incoming], NewShare);
          EXPECT_EQ(Second.Status, Compatible
                                       ? windows::StatusSuccess
                                       : windows::StatusSharingViolation);
          EXPECT_EQ(Second.Token != 0, Compatible);
          EXPECT_EQ(S.Owner.openCount(), Compatible ? 2u : 1u);
          if (Second.Token)
            S.retire(Second.Token);
          S.retire(First.Token);
          EXPECT_EQ(S.Owner.openCount(), 0u);
        }
}

TEST(DriverFileSystemOpen, ExecuteAppendAndMetadataUseCorrectShareCategories) {
  for (const auto [Access, Sharing] :
       {std::pair{0x20u, 1u}, std::pair{4u, 2u}}) {
    for (bool IncomingAlias : {false, true}) {
      OpenOwner S;
      auto First = S.reserve(0x3000, IncomingAlias ? Sharing : Access, 0);
      auto Second = S.reserve(0x4000, IncomingAlias ? Access : Sharing, 7);
      EXPECT_EQ(Second.Status, windows::StatusSharingViolation);
      EXPECT_EQ(Second.Token, 0u);
      S.retire(First.Token);
    }
  }
  for (uint32_t Metadata : {0u, 8u, 0x10u, 0x80u, 0x100u, 0x20000u, 0x40000u,
                            0x80000u, 0x100000u}) {
    OpenOwner S;
    const auto First = S.reserve(0x3000, Metadata, 0);
    const auto Second = S.reserve(0x4000, 0x10023, 0);
    const auto Third = S.reserve(0x5000, Metadata, 0);
    EXPECT_EQ(First.Status, 0u);
    EXPECT_EQ(Second.Status, 0u);
    EXPECT_EQ(Third.Status, 0u);
    ok(S.Owner.validateOpenBindings({{0x3000, First.Token},
                                     {0x4000, Second.Token},
                                     {0x5000, Third.Token}}));
    S.retire(Second.Token);
    S.retire(First.Token);
    S.retire(Third.Token);
  }
}

TEST(DriverFileSystemOpen, CleanupReleasesSharingWhileCloseRetiresIdentity) {
  OpenOwner S;
  const auto Before = driver_filesystem::stateJSON(take(S.Owner.snapshot()));
  const auto First = S.reserve(0x3000, 1, 0);
  ASSERT_NE(First.Token, 0u);
  auto Failure = S.reserve(0x4000, 2, 7);
  EXPECT_EQ(Failure.Status, windows::StatusSharingViolation);
  EXPECT_EQ(Failure.Token, 0u);
  rejects(S.Owner.closeOpen(First.Token), "cleaned");
  EXPECT_EQ(S.Owner.openCount(), 1u);
  ok(S.Owner.cleanupOpen(First.Token));
  ASSERT_NE(S.Owner.findOpen(First.Token), nullptr);
  EXPECT_TRUE(S.Owner.findOpen(First.Token)->Cleaned);
  EXPECT_EQ(S.Owner.findOpen(First.Token)->FileID, 5u);
  const auto Second = S.reserve(0x4000, 2, 0);
  EXPECT_EQ(Second.Status, 0u);
  EXPECT_EQ(Second.Token, First.Token + 1);
  // Repeated CLEANUP on a retained/reopened body never cancels another open's
  // reservation and does not recreate the old reservation.
  ok(S.Owner.cleanupOpen(First.Token));
  Failure = S.reserve(0x5000, 1, 7);
  EXPECT_EQ(Failure.Status, windows::StatusSharingViolation);
  ok(S.Owner.closeOpen(First.Token));
  EXPECT_EQ(S.Owner.findOpen(First.Token), nullptr);
  rejects(S.Owner.cleanupOpen(First.Token), "live");
  rejects(S.Owner.closeOpen(First.Token), "live");
  ok(S.Owner.validateOpenBindings({{0x4000, Second.Token}}));
  S.retire(Second.Token);
  const auto ReusedBody = S.reserve(0x3000, 1, 7);
  EXPECT_GT(ReusedBody.Token, Second.Token);
  S.retire(ReusedBody.Token);
  EXPECT_EQ(driver_filesystem::stateJSON(take(S.Owner.snapshot())), Before);
}

TEST(DriverFileSystemOpen, FileIdentityIncludesVolumeAndAcceptsZeroID) {
  OpenOwner S;
  const auto First = S.reserve(0x3000, 1, 0, 0);
  const auto OtherVolume = S.reserve(0x4000, 2, 0, 0, 0x2000);
  const auto OtherNode = S.reserve(0x5000, 2, 0, 6);
  EXPECT_EQ(First.Status, 0u);
  EXPECT_EQ(OtherVolume.Status, 0u);
  EXPECT_EQ(OtherNode.Status, 0u);
  const auto Conflict = S.reserve(0x6000, 2, 7, 0);
  EXPECT_EQ(Conflict.Status, windows::StatusSharingViolation);
  const auto *Record = S.Owner.findOpen(First.Token);
  ASSERT_NE(Record, nullptr);
  EXPECT_EQ(Record->FileID, 0u);
  EXPECT_EQ(Record->DeviceObject, 0x1000u);
  EXPECT_EQ(Record->GrantedAccess, 1u);
  EXPECT_EQ(Record->ShareAccess, 0u);
  S.retire(First.Token);
  S.retire(OtherVolume.Token);
  S.retire(OtherNode.Token);
}

TEST(DriverFileSystemOpen, InvalidBindingsAndRightsCannotReserveOrReviveOpen) {
  OpenOwner S;
  const auto First = S.reserve(0x3000, 0, 0);
  for (const auto &Arguments : {std::array<uint64_t, 5>{0, 5, 0x4000, 0, 7},
                                {0x1000, 999, 0x4000, 0, 7},
                                {0x1000, 5, 0, 0, 7},
                                {0x1000, 5, 0x3000, 0, 7},
                                {0x1000, 5, 0x4000, 0x80000000, 7},
                                {0x1000, 5, 0x4000, 0x2000000, 7},
                                {0x1000, 5, 0x4000, 0, 8}}) {
    auto Invalid = S.Owner.reserveOpen(Arguments[0], Arguments[1], Arguments[2],
                                       Arguments[3], Arguments[4]);
    ASSERT_FALSE(bool(Invalid));
    rejects(Invalid.takeError());
    EXPECT_EQ(S.Owner.openCount(), 1u);
  }
  ok(S.Owner.cleanupOpen(First.Token));
  auto Reopen = S.Owner.reserveOpen(0x1000, 5, 0x3000, 1, 7);
  ASSERT_FALSE(bool(Reopen));
  rejects(Reopen.takeError(), "already owns");
  const auto Second = S.reserve(0x4000, 0, 0);
  EXPECT_EQ(Second.Token, First.Token + 1);
  ok(S.Owner.validateOpenBindings(
      {{0x3000, First.Token}, {0x4000, Second.Token}}));
  rejects(S.Owner.validateOpenBindings({}), "differ");
  rejects(S.Owner.validateOpenBindings({{0x3000, First.Token}}), "differ");
  rejects(S.Owner.validateOpenBindings(
              {{0x3000, First.Token}, {0x3000, First.Token}}),
          "duplicate");
  rejects(S.Owner.validateOpenBindings(
              {{0x3000, Second.Token}, {0x4000, First.Token}}),
          "wrong");
  S.retire(First.Token);
  rejects(S.Owner.validateOpenBindings({{0x3000, First.Token}}), "missing");
  S.retire(Second.Token);
  ok(S.Owner.validateOpenBindings({}));
}

TEST(DriverFileSystemOpen, CleanedReferencesRetainBoundedCapacityUntilClose) {
  OpenOwner S;
  std::vector<KernelFileSystem::OpenBinding> Bindings;
  for (size_t I = 0; I != MaxFileSystemOpens; ++I) {
    const uint64_t Body = 0x3000 + I * 0x100;
    const auto Open = S.reserve(Body, 0, 0);
    ASSERT_NE(Open.Token, 0u);
    Bindings.push_back({Body, Open.Token});
    ok(S.Owner.cleanupOpen(Open.Token));
  }
  ok(S.Owner.validateOpenBindings(Bindings));
  auto Exhausted = S.Owner.reserveOpen(0x1000, 5, 0x1000000, 0, 0);
  ASSERT_FALSE(bool(Exhausted));
  rejects(Exhausted.takeError(), "exhausted");
  EXPECT_EQ(S.Owner.openCount(), MaxFileSystemOpens);
  ok(S.Owner.closeOpen(Bindings.front().Token));
  const auto New = S.reserve(0x1000000, 0, 0);
  EXPECT_GT(New.Token, Bindings.back().Token);
  EXPECT_EQ(S.Owner.openCount(), MaxFileSystemOpens);
  Bindings.front() = {0x1000000, New.Token};
  ok(S.Owner.validateOpenBindings(Bindings));
  for (const auto &Binding : Bindings)
    S.retire(Binding.Token);
  ok(S.Owner.validateOpenBindings({}));
}
} // namespace
} // namespace neverd::emulation
