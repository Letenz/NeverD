//===- KernelRegistry.cpp - Concrete NT registry semantics ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original implementation of the Microsoft WDK Zw*Key contracts. References:
/// https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nf-wdm-zwcreatekey
/// https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nf-wdm-zwqueryvaluekey
/// https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nf-wdm-zwdeletekey
/// https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/ne-wdm-_key_value_information_class
/// https://learn.microsoft.com/windows/win32/sysinfo/writing-and-deleting-registry-data
/// The explicit scenario tree grants the supported access-mask bits. This is
/// a concrete capability model, not an ACL, privilege, hive, or host OS model.
///
//===----------------------------------------------------------------------===//

#include "KernelRegistry.h"

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <set>

namespace neverd::emulation {
namespace {
#define NEVERD_REGISTRY_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "KernelRegistryValues.def"
#undef NEVERD_REGISTRY_VALUE
#define NEVERD_REGISTRY_STRING(Name, Value)                                    \
  constexpr llvm::StringLiteral Name(Value);
#include "KernelRegistryStrings.def"
#undef NEVERD_REGISTRY_STRING

enum class API {
  Unknown,
#define NEVERD_KERNEL_REGISTRY_API(Name, Count) Name,
#include "KernelRegistryAPIs.def"
#undef NEVERD_KERNEL_REGISTRY_API
};

struct APIDescriptor {
  API Function;
  unsigned ArgumentCount;
};

const APIDescriptor *lookupAPI(llvm::StringRef Name) {
  static const llvm::StringMap<APIDescriptor> APIs{
#define NEVERD_KERNEL_REGISTRY_API(Name, Count) {#Name, {API::Name, Count}},
#include "KernelRegistryAPIs.def"
#undef NEVERD_KERNEL_REGISTRY_API
  };
  auto I = APIs.find(Name);
  return I == APIs.end() ? nullptr : &I->second;
}

llvm::Error registryError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}

std::string folded(llvm::StringRef Text) {
  std::string Result = Text.str();
  for (char &C : Result)
    if (C >= 'A' && C <= 'Z')
      C += 'a' - 'A';
  return Result;
}

bool asciiName(llvm::StringRef Text) {
  return llvm::all_of(Text, [](char C) { return C && llvm::isASCII(C); });
}

bool atOrBelow(llvm::StringRef Path, llvm::StringRef Parent) {
  return Path == Parent ||
         (Path.starts_with(Parent) &&
          Path.drop_front(Parent.size()).starts_with(Separator));
}

bool validPath(llvm::StringRef Path) {
  const std::string Folded = folded(Path);
  return asciiName(Path) && !Path.ends_with(Separator) &&
         !Path.contains(EmptyComponent) &&
         (atOrBelow(Folded, MachineRoot) || atOrBelow(Folded, UserRoot));
}

std::string parentPath(llvm::StringRef Path) {
  return Path.take_front(Path.rfind(Separator)).str();
}

bool rootPath(llvm::StringRef Path) {
  return Path == MachineRoot || Path == UserRoot;
}

llvm::Error validateValue(const DriverRegistryValue &Value) {
  if (!asciiName(Value.Name) || Value.Name.size() > MaxRegistryValueNameLength)
    return registryError(
        "registry value name must be bounded ASCII without NUL");
  if (Value.Type > MaxValueType || Value.Type == LinkValueType)
    return registryError(
        "unsupported registry value type (symbolic links are not modeled)");
  if (Value.Data.size() > MaxRegistryValueBytes)
    return registryError("registry value data exceeds the byte limit");
  return llvm::Error::success();
}

llvm::Expected<std::vector<uint8_t>> readBytes(const KernelModel &Model,
                                               GuestMemory &Memory,
                                               uint64_t Address,
                                               uint32_t Size) {
  if (auto E = Model.validateGuestAccess(Address, Size, false))
    return E;
  std::vector<uint8_t> Bytes(Size);
  if (Size)
    if (auto E = Memory.read(Address, Bytes))
      return E;
  return Bytes;
}

llvm::Error validateOutput(const KernelModel &Model, GuestMemory &Memory,
                           uint64_t Address, uint32_t Size) {
  if (!Size)
    return llvm::Error::success();
  if (auto E = Model.validateGuestAccess(Address, Size, true))
    return E;
  auto Accessible = Memory.canAccess(Address, Size, Write);
  if (!Accessible)
    return Accessible.takeError();
  if (!*Accessible)
    return registryError("registry output requires writable ordinary RAM");
  return Memory.validateBacking(Address, Size);
}

llvm::Expected<std::string> readName(const KernelModel &Model,
                                     GuestMemory &Memory, uint64_t Address,
                                     size_t Limit) {
  if (!Address)
    return registryError("registry UNICODE_STRING pointer is null");
  auto Record = readBytes(Model, Memory, Address, UnicodeSize);
  if (!Record)
    return Record.takeError();
  const uint16_t Length = llvm::support::endian::read16le(Record->data());
  const uint16_t Maximum =
      llvm::support::endian::read16le(Record->data() + UnicodeMaximumOffset);
  const uint64_t Buffer =
      llvm::support::endian::read64le(Record->data() + UnicodeBufferOffset);
  if ((Length & 1) || (Maximum & 1) || Length > Maximum || Length / 2 > Limit ||
      (Length && !Buffer))
    return registryError("invalid or over-limit registry UNICODE_STRING");
  auto Bytes = readBytes(Model, Memory, Buffer, Length);
  if (!Bytes)
    return Bytes.takeError();
  std::string Result;
  for (size_t I = 0; I < Bytes->size(); I += 2) {
    if ((*Bytes)[I + 1] || !(*Bytes)[I] || !llvm::isASCII(char((*Bytes)[I])))
      return registryError(
          "registry names require ASCII; Unicode case folding is unsupported");
    Result.push_back((*Bytes)[I]);
  }
  return Result;
}

void store32(std::vector<uint8_t> &Bytes, size_t Offset, uint32_t Value) {
  llvm::support::endian::write32le(Bytes.data() + Offset, Value);
}

void storeName(std::vector<uint8_t> &Bytes, size_t Offset,
               llvm::StringRef Name) {
  for (size_t I = 0; I < Name.size(); ++I)
    Bytes[Offset + 2 * I] = Name[I];
}
} // namespace

llvm::Error validateDriverRegistry(
    const std::optional<std::vector<DriverRegistryKey>> &Registry,
    std::optional<uint64_t> SystemTime100ns) {
  if (SystemTime100ns && (!Registry || *SystemTime100ns > INT64_MAX))
    return registryError("registry system time requires an explicit registry "
                         "and a nonnegative signed 64-bit Windows time");
  if (!Registry)
    return llvm::Error::success();
  if (Registry->size() > MaxRegistryKeys)
    return registryError("registry key count exceeds the limit");
  std::set<std::string> ExplicitPaths;
  std::set<std::string> AllPaths;
  size_t Values = 0;
  size_t Bytes = 0;
  for (const DriverRegistryKey &Key : *Registry) {
    if (Key.LastWriteTime100ns && *Key.LastWriteTime100ns > INT64_MAX)
      return registryError(
          "registry last-write time exceeds the signed 64-bit limit");
    if (Key.Path.size() > MaxRegistryPathLength || !validPath(Key.Path))
      return registryError(
          "registry path must be bounded ASCII under \\Registry\\Machine or "
          "\\Registry\\User without empty components");
    std::string Path = folded(Key.Path);
    if (!ExplicitPaths.insert(Path).second)
      return registryError("duplicate case-insensitive registry key path");
    while (true) {
      AllPaths.insert(Path);
      if (AllPaths.size() > MaxRegistryKeys)
        return registryError(
            "registry key count including ancestors exceeds the limit");
      if (rootPath(Path))
        break;
      Path = parentPath(Path);
    }
    std::set<std::string> Names;
    for (const DriverRegistryValue &Value : Key.Values) {
      if (auto E = validateValue(Value))
        return E;
      if (!Names.insert(folded(Value.Name)).second)
        return registryError("duplicate case-insensitive registry value name");
      if (++Values > MaxRegistryValues)
        return registryError("registry value count exceeds the limit");
      Bytes += Value.Data.size();
      if (Bytes > MaxRegistryTotalBytes)
        return registryError("registry total value bytes exceed the limit");
    }
  }
  return llvm::Error::success();
}

llvm::Error KernelRegistry::initialize(
    const std::optional<std::vector<DriverRegistryKey>> &Registry,
    std::optional<uint64_t> InitialSystemTime100ns) {
  if (auto E = validateDriverRegistry(Registry, InitialSystemTime100ns))
    return E;
  Configured = Registry.has_value();
  Keys.clear();
  Handles.clear();
  NextHandle = HandleBase;
  ValueCount = 0;
  DataBytes = 0;
  SystemTime100ns = InitialSystemTime100ns;
  if (!Registry)
    return llvm::Error::success();
  for (const DriverRegistryKey &Input : *Registry) {
    std::string Path = Input.Path;
    while (true) {
      const std::string Folded = folded(Path);
      Keys.try_emplace(Folded,
                       Key{Path, {}, {}, false, false, SystemTime100ns});
      if (rootPath(Folded))
        break;
      Path = parentPath(Path);
    }
    Key &Entry = Keys.at(folded(Input.Path));
    // Prefer the explicit key spelling over an implicit ancestor spelling.
    Entry.Path = Input.Path;
    Entry.LastWriteTime100ns =
        Input.LastWriteTime100ns ? Input.LastWriteTime100ns : SystemTime100ns;
    for (const DriverRegistryValue &Value : Input.Values) {
      Entry.Values.emplace(folded(Value.Name), Value);
      Entry.ValueOrder.push_back(folded(Value.Name));
      Entry.MaxValueNameLength =
          std::max(Entry.MaxValueNameLength, uint32_t(Value.Name.size() * 2));
      Entry.MaxValueDataLength =
          std::max(Entry.MaxValueDataLength, uint32_t(Value.Data.size()));
      ++ValueCount;
      DataBytes += Value.Data.size();
    }
  }
  // Folded paths sort parents before children. Retain one spelling per
  // component, including when input paths spell an existing ancestor
  // differently.
  for (auto &[Path, Entry] : Keys) {
    auto Parent = Keys.find(parentPath(Path));
    if (Parent != Keys.end()) {
      Entry.Path =
          Parent->second.Path + Entry.Path.substr(Entry.Path.rfind(Separator));
      Parent->second.MaxNameLength =
          std::max(Parent->second.MaxNameLength,
                   uint32_t((Path.size() - Parent->first.size() - 1) * 2));
    }
  }
  return llvm::Error::success();
}

std::optional<std::vector<DriverRegistryKey>> KernelRegistry::snapshot() const {
  if (!Configured)
    return std::nullopt;
  std::vector<DriverRegistryKey> Result;
  for (const auto &[Path, Entry] : Keys) {
    if (Entry.Deleted)
      continue;
    DriverRegistryKey Output{Entry.Path, {}};
    Output.LastWriteTime100ns = Entry.LastWriteTime100ns;
    for (const auto &Name : Entry.ValueOrder)
      Output.Values.push_back(Entry.Values.at(Name));
    Result.push_back(std::move(Output));
  }
  return Result;
}

llvm::Expected<std::optional<uint64_t>>
KernelRegistry::mutationTime(const KernelModel &Model) const {
  if (!SystemTime100ns)
    return std::optional<uint64_t>();
  if (Model.now100ns() > uint64_t(INT64_MAX) - *SystemTime100ns)
    return registryError(
        "registry system time exceeds the signed 64-bit limit");
  return std::optional<uint64_t>(*SystemTime100ns + Model.now100ns());
}

llvm::Expected<uint64_t> KernelRegistry::open(const KernelModel &Model,
                                              llvm::ArrayRef<uint64_t> A,
                                              bool Create) {
  const uint32_t Access = A[1];
  if (Access & ~AllAccess)
    return registryError("unsupported registry access mask (generic, "
                         "maximum-allowed, or alternate-view access)");
  if (Create && (uint32_t(A[3]) || A[4] || (uint32_t(A[5]) & ~VolatileOption)))
    return registryError(
        "unsupported registry title, class, or creation options");
  auto Object = readBytes(Model, Memory, A[2], windows::ObjectAttributesSize);
  if (!Object)
    return Object.takeError();
  if (llvm::support::endian::read32le(Object->data()) !=
      windows::ObjectAttributesSize)
    return InvalidParameter;
  const uint64_t Root = llvm::support::endian::read64le(
      Object->data() + windows::ObjectRootOffset);
  const uint64_t Name = llvm::support::endian::read64le(
      Object->data() + windows::ObjectNameOffset);
  const uint32_t Flags = llvm::support::endian::read32le(
      Object->data() + windows::ObjectFlagsOffset);
  if ((Flags & ~(CaseInsensitive | windows::ObjectKernelHandle)) ||
      llvm::support::endian::read64le(Object->data() +
                                      windows::ObjectSecurityOffset) ||
      llvm::support::endian::read64le(Object->data() +
                                      windows::ObjectQualityOffset))
    return registryError(
        "unsupported registry object attributes or security policy");
  auto Text = readName(Model, Memory, Name, MaxRegistryPathLength);
  if (!Text)
    return Text.takeError();
  std::string Path = *Text;
  const Handle *RootHandle = nullptr;
  if (Root) {
    auto H = Handles.find(Root);
    if (H == Handles.end())
      return InvalidHandle;
    RootHandle = &H->second;
    const Key &Parent = Keys.at(H->second.Path);
    if (Parent.Deleted)
      return KeyDeleted;
    if (llvm::StringRef(Path).starts_with(Separator))
      return InvalidParameter;
    Path =
        Parent.Path + (Path.empty() ? std::string() : Separator.str() + Path);
  }
  if (Path.size() > MaxRegistryPathLength)
    return registryError("registry path exceeds the length limit");
  if (!validPath(Path))
    return registryError("unsupported registry path outside the configured "
                         "namespace or with empty components");
  // OBJ_CASE_INSENSITIVE opts into the same ASCII-insensitive behavior as
  // this concrete environment's default registry name comparison policy.
  const std::string Folded = folded(Path);
  auto It = Keys.find(Folded);
  const bool Exists = It != Keys.end();
  if (Exists && It->second.Deleted)
    return KeyDeleted;
  if (!Exists) {
    if (!Create)
      return ObjectNameNotFound;
    auto Parent = Keys.find(parentPath(Folded));
    if (Parent == Keys.end())
      return ObjectPathNotFound;
    if (Parent->second.Deleted)
      return KeyDeleted;
    if (RootHandle && !(RootHandle->Access & CreateSubKeyAccess))
      return AccessDenied;
    if (RootHandle && RootHandle->Path != Parent->first)
      return registryError(
          "relative registry creation requires an immediate parent handle");
    if (Parent->second.Volatile && !(uint32_t(A[5]) & VolatileOption))
      return ChildMustBeVolatile;
    if (Keys.size() >= MaxRegistryKeys)
      return registryError("registry key count exceeds the limit");
  }
  if (Handles.size() >= MaxRegistryHandles ||
      NextHandle > UINT64_MAX - HandleStride)
    return registryError("registry handle count exceeds the limit");
  std::optional<uint64_t> CreatedAt;
  if (!Exists) {
    auto Time = mutationTime(Model);
    if (!Time)
      return Time.takeError();
    CreatedAt = *Time;
  }
  // Validate every output before writes, and finish all guest writes before
  // publishing a key or handle. A guest fault cannot leak invisible state.
  if (auto E = Model.validateGuestAccess(A[0], 8, true))
    return E;
  if (Create && A[6])
    if (auto E = Model.validateGuestAccess(A[6], 4, true))
      return E;
  if (Create && A[6])
    if (auto E = Memory.writeInteger(
            A[6], Exists ? OpenedExistingKey : CreatedNewKey, 4))
      return E;
  if (auto E = Memory.writeInteger(A[0], NextHandle, 8))
    return E;
  if (!Exists) {
    Key &Parent = Keys.at(parentPath(Folded));
    Path = Parent.Path + Path.substr(Path.rfind(Separator));
    Keys.emplace(Folded, Key{Path,
                             {},
                             {},
                             false,
                             bool(uint32_t(A[5]) & VolatileOption),
                             CreatedAt});
    Parent.LastWriteTime100ns = CreatedAt;
    Parent.MaxNameLength =
        std::max(Parent.MaxNameLength,
                 uint32_t((Path.size() - parentPath(Folded).size() - 1) * 2));
  }
  Handles.emplace(NextHandle, Handle{Folded, Access});
  NextHandle += HandleStride;
  return Success;
}

llvm::Expected<uint64_t> KernelRegistry::query(const KernelModel &Model,
                                               Key &Entry,
                                               llvm::ArrayRef<uint64_t> A,
                                               bool Enumerate) {
  const uint32_t Class = A[2];
  if (Class >= MaxInformationClass)
    return InvalidParameter;
  if (Class == LayerInformation)
    return registryError("registry layer information is unsupported");
  if ((Class == FullInformationAlign64 || Class == PartialInformationAlign64) &&
      (A[3] % FullDataAlignment64))
    return DatatypeMisalignment;
  std::string LookupName;
  if (Enumerate) {
    const uint32_t Index = A[1];
    if (Index >= Entry.ValueOrder.size())
      return NoMoreEntries;
    LookupName = Entry.ValueOrder[Index];
  } else {
    auto Name = readName(Model, Memory, A[1], MaxRegistryValueNameLength);
    if (!Name)
      return Name.takeError();
    LookupName = folded(*Name);
  }
  auto V = Entry.Values.find(LookupName);
  if (V == Entry.Values.end())
    return ObjectNameNotFound;
  const DriverRegistryValue &Value = V->second;
  const uint32_t NameBytes = Value.Name.size() * 2;
  const uint32_t DataSize = Value.Data.size();
  uint32_t Header;
  uint32_t Required;
  uint32_t DataStart = 0;
  switch (Class) {
  case BasicInformation:
    Header = BasicHeaderSize;
    Required = Header + NameBytes;
    break;
  case FullInformation:
  case FullInformationAlign64: {
    Header = FullHeaderSize;
    const uint32_t Alignment = Class == FullInformationAlign64
                                   ? FullDataAlignment64
                                   : FullDataAlignment;
    DataStart = DataSize
                    ? (Header + NameBytes + Alignment - 1) & ~(Alignment - 1)
                    : UINT32_MAX;
    Required = DataSize ? DataStart + DataSize : Header + NameBytes;
    break;
  }
  default:
    Header =
        Class == PartialInformation ? PartialHeaderSize : Partial64HeaderSize;
    DataStart = Header;
    Required = Header + DataSize;
    break;
  }
  const uint32_t Length = A[4];
  const uint32_t Written = Length < Header ? 0 : std::min(Length, Required);
  if (auto E = validateOutput(Model, Memory, A[5], 4))
    return E;
  if (auto E = validateOutput(Model, Memory, A[3], Written))
    return E;
  if (!Written) {
    if (auto E = Memory.writeInteger(A[5], Required, 4))
      return E;
    return BufferTooSmall;
  }
  std::vector<uint8_t> Bytes(Header, 0);
  store32(Bytes,
          Class == PartialInformationAlign64 ? Partial64TypeOffset : TypeOffset,
          Value.Type);
  switch (Class) {
  case BasicInformation:
    store32(Bytes, BasicNameLengthOffset, NameBytes);
    break;
  case FullInformation:
  case FullInformationAlign64:
    store32(Bytes, FullDataOffset, DataStart);
    store32(Bytes, FullDataLengthOffset, DataSize);
    store32(Bytes, FullNameLengthOffset, NameBytes);
    break;
  default:
    store32(Bytes,
            Class == PartialInformation ? PartialDataLengthOffset
                                        : Partial64DataLengthOffset,
            DataSize);
    break;
  }
  if (auto E = Memory.write(A[3], Bytes))
    return E;
  // The x64 kernel aligns Full data to eight bytes for both Full classes.
  // With no data it uses DataOffset=-1 and no alignment tail. ASCII names
  // occupy compressed hive names: truncation writes complete UTF-16 units.
  // Padding and any final half-unit retain the caller's bytes.
  if (Class == BasicInformation || Class == FullInformation ||
      Class == FullInformationAlign64) {
    std::vector<uint8_t> NameBytesOut(std::min(NameBytes, Written - Header) &
                                      ~1u);
    storeName(NameBytesOut, 0,
              llvm::StringRef(Value.Name).take_front(NameBytesOut.size() / 2));
    if (!NameBytesOut.empty())
      if (auto E = Memory.write(A[3] + Header, NameBytesOut))
        return E;
  }
  if (Class != BasicInformation && DataSize && Written > DataStart)
    if (auto E = Memory.write(A[3] + DataStart,
                              llvm::ArrayRef<uint8_t>(Value.Data)
                                  .take_front(Written - DataStart)))
      return E;
  // NtQueryValueKey/NtEnumerateValueKey publish ResultLength after the payload,
  // including when it aliases part of that payload.
  if (auto E = Memory.writeInteger(A[5], Required, 4))
    return E;
  return Length < Required ? BufferOverflow : Success;
}

llvm::Expected<uint64_t>
KernelRegistry::queryKey(const KernelModel &Model, const Key &Entry,
                         uint32_t Class, uint64_t Buffer, uint32_t Length,
                         uint64_t ResultLength) {
  const llvm::StringRef Path(Entry.Path);
  const auto Name = Class == KeyNameInformation
                        ? Path
                        : Path.drop_front(Path.rfind(Separator) + 1);
  uint32_t Header = 0, NameOffset = 0;
  switch (Class) {
  case KeyBasicInformation:
    Header = NameOffset = KeyBasicHeaderSize;
    break;
  case KeyNodeInformation:
    Header = NameOffset = KeyNodeHeaderSize;
    break;
  case KeyFullInformation:
    Header = KeyFullHeaderSize;
    break;
  case KeyNameInformation:
    Header = NameOffset = KeyNameHeaderSize;
    break;
  case KeyCachedInformation:
    Header = KeyCachedSize;
    break;
  default:
    return registryError("registry key information class is unsupported");
  }
  const uint32_t Required = Header + (NameOffset ? Name.size() * 2 : 0);
  const uint32_t Written = Length < Header ? 0 : std::min(Length, Required);
  if (Written && Class != KeyNameInformation && !Entry.LastWriteTime100ns)
    return registryError("registry last-write time is unspecified; supply "
                         "last_write_time_100ns or registry_system_time_100ns");
  if (auto E = validateOutput(Model, Memory, ResultLength, 4))
    return E;
  if (auto E = validateOutput(Model, Memory, Buffer, Written))
    return E;
  if (!Written) {
    if (auto E = Memory.writeInteger(ResultLength, Required, 4))
      return E;
    return BufferTooSmall;
  }
  // CachedInformation is sizeof=40 on x64; its four tail padding bytes are
  // not fields. Basic/Node store complete compressed-ASCII name units, while
  // NameInformation copies the constructed full path byte by byte.
  std::vector<uint8_t> Bytes(Class == KeyCachedInformation ? 36 : Header, 0);
  if (Class == KeyNameInformation)
    store32(Bytes, 0, Name.size() * 2);
  else
    llvm::support::endian::write64le(Bytes.data(), *Entry.LastWriteTime100ns);
  if (Class == KeyBasicInformation)
    store32(Bytes, 12, Name.size() * 2);
  if (Class == KeyNodeInformation) {
    store32(Bytes, 12, UINT32_MAX); // No key class in this registry profile.
    store32(Bytes, 20, Name.size() * 2);
  }
  if (Class == KeyFullInformation || Class == KeyCachedInformation) {
    uint32_t Subkeys = 0;
    const std::string Parent = folded(Entry.Path);
    for (const auto &[ChildPath, Child] : Keys) {
      if (Child.Deleted || parentPath(ChildPath) != Parent)
        continue;
      ++Subkeys;
    }
    if (Class == KeyFullInformation) {
      store32(Bytes, 12, UINT32_MAX);
      store32(Bytes, 20, Subkeys);
      store32(Bytes, 24, Entry.MaxNameLength);
      store32(Bytes, 32, Entry.Values.size());
      store32(Bytes, 36, Entry.MaxValueNameLength);
      store32(Bytes, 40, Entry.MaxValueDataLength);
    } else {
      store32(Bytes, 12, Subkeys);
      store32(Bytes, 16, Entry.MaxNameLength);
      store32(Bytes, 20, Entry.Values.size());
      store32(Bytes, 24, Entry.MaxValueNameLength);
      store32(Bytes, 28, Entry.MaxValueDataLength);
      store32(Bytes, 32, Name.size() * 2);
    }
  }
  if (auto E = Memory.write(Buffer, Bytes))
    return E;
  if (NameOffset) {
    std::vector<uint8_t> NameBytes(Name.size() * 2, 0);
    storeName(NameBytes, 0, Name);
    uint32_t Count = Written - NameOffset;
    if (Class != KeyNameInformation)
      Count &= ~1u;
    if (Count)
      if (auto E = Memory.write(
              Buffer + NameOffset,
              llvm::ArrayRef<uint8_t>(NameBytes).take_front(Count)))
        return E;
  }
  if (auto E = Memory.writeInteger(ResultLength, Required, 4))
    return E;
  if (Length < Required)
    return BufferOverflow;
  // NameInformation still copies the retained path for a deleted handle.
  return Entry.Deleted ? KeyDeleted : Success;
}

llvm::Expected<uint64_t>
KernelRegistry::enumerateKey(const KernelModel &Model, llvm::StringRef Path,
                             llvm::ArrayRef<uint64_t> A) {
  std::vector<const Key *> Children;
  for (const auto &[ChildPath, Child] : Keys)
    if (!Child.Deleted && parentPath(ChildPath) == Path)
      Children.push_back(&Child);
  // Registry subkeys use the uppercase name collation, unlike value indices.
  llvm::sort(Children, [](const Key *L, const Key *R) {
    return std::lexicographical_compare(
        L->Path.begin(), L->Path.end(), R->Path.begin(), R->Path.end(),
        [](unsigned char A, unsigned char B) {
          return llvm::toUpper(A) < llvm::toUpper(B);
        });
  });
  const uint32_t Index = A[1];
  if (Index >= Children.size())
    return NoMoreEntries;
  return queryKey(Model, *Children[Index], uint32_t(A[2]), A[3], uint32_t(A[4]),
                  A[5]);
}

llvm::Expected<uint64_t> KernelRegistry::set(const KernelModel &Model,
                                             Key &Entry,
                                             llvm::ArrayRef<uint64_t> A) {
  if (uint32_t(A[2]))
    return registryError("registry value TitleIndex must be zero");
  DriverRegistryValue Value;
  if (A[1]) {
    auto Name = readName(Model, Memory, A[1], MaxRegistryValueNameLength);
    if (!Name)
      return Name.takeError();
    Value.Name = *Name;
  }
  Value.Type = uint32_t(A[3]);
  if (auto E = validateValue(Value))
    return E;
  const uint32_t Size = A[5];
  if (Size > MaxRegistryValueBytes)
    return registryError("registry value data exceeds the byte limit");
  const std::string Name = folded(Value.Name);
  auto Old = Entry.Values.find(Name);
  const size_t OldBytes =
      Old == Entry.Values.end() ? 0 : Old->second.Data.size();
  if (Old == Entry.Values.end() && ValueCount >= MaxRegistryValues)
    return registryError("registry value count exceeds the limit");
  if (DataBytes - OldBytes + Size > MaxRegistryTotalBytes)
    return registryError("registry total value bytes exceed the limit");
  auto Bytes = readBytes(Model, Memory, A[4], Size);
  if (!Bytes)
    return Bytes.takeError();
  // The unchanged-value fast path does not update the kernel's timestamp.
  if (Old != Entry.Values.end() && Old->second.Type == Value.Type &&
      Old->second.Data == *Bytes)
    return Success;
  auto Time = mutationTime(Model);
  if (!Time)
    return Time.takeError();
  Value.Data = std::move(*Bytes);
  if (Old != Entry.Values.end())
    Value.Name = Old->second.Name;
  else {
    ++ValueCount;
    Entry.ValueOrder.push_back(Name);
  }
  DataBytes = DataBytes - OldBytes + Size;
  Entry.Values.insert_or_assign(Name, std::move(Value));
  Entry.MaxValueNameLength =
      std::max(Entry.MaxValueNameLength, uint32_t(Name.size() * 2));
  Entry.MaxValueDataLength = std::max(Entry.MaxValueDataLength, Size);
  Entry.LastWriteTime100ns = *Time;
  return Success;
}

llvm::Expected<uint64_t> KernelRegistry::call(const KernelModel &Model,
                                              llvm::StringRef Name,
                                              llvm::ArrayRef<uint64_t> A) {
  const auto *Descriptor = lookupAPI(Name);
  if (!Descriptor || A.size() != Descriptor->ArgumentCount)
    return registryError("unknown registry API or incorrect argument count");
  const API Function = Descriptor->Function;
  if (!Configured)
    return registryError("registry availability is unspecified; supply an "
                         "explicit registry scenario");
  if (Function == API::ZwEnumerateValueKey &&
      uint32_t(A[2]) > PartialInformation)
    return InvalidParameter;
  if (Function == API::ZwEnumerateKey && uint32_t(A[2]) > KeyFullInformation)
    return InvalidParameter;
  if (Function == API::ZwQueryKey && uint32_t(A[1]) >= KeyMaxInformationClass)
    return InvalidParameter;
  if (Function == API::ZwOpenKey || Function == API::ZwCreateKey)
    return open(Model, A, Function == API::ZwCreateKey);
  auto H = Handles.find(A[0]);
  if (H == Handles.end())
    return InvalidHandle;
  const std::string Path = H->second.Path;
  Key &Entry = Keys.at(Path);
  if (Function == API::ZwClose) {
    Handles.erase(H);
    if (Entry.Deleted && llvm::none_of(Handles, [&](const auto &Other) {
          return Other.second.Path == Path;
        }))
      Keys.erase(Path);
    return Success;
  }
  if (Entry.Deleted &&
      !(Function == API::ZwQueryKey && uint32_t(A[1]) == KeyNameInformation))
    return KeyDeleted;
  uint32_t Required =
      (Function == API::ZwQueryValueKey ||
       Function == API::ZwEnumerateValueKey || Function == API::ZwQueryKey)
          ? QueryValueAccess
          : SetValueAccess;
  if (Function == API::ZwDeleteKey)
    Required = DeleteAccess;
  if (Function == API::ZwEnumerateKey)
    Required = EnumerateSubKeysAccess;
  if (Function == API::ZwQueryKey && uint32_t(A[1]) == KeyNameInformation)
    Required = AllAccess; // Any granted key access suffices for its name.
  if (!(H->second.Access & Required))
    return AccessDenied;
  switch (Function) {
  case API::ZwQueryKey:
    return queryKey(Model, Entry, uint32_t(A[1]), A[2], uint32_t(A[3]), A[4]);
  case API::ZwEnumerateKey:
    return enumerateKey(Model, Path, A);
  case API::ZwQueryValueKey:
  case API::ZwEnumerateValueKey:
    return query(Model, Entry, A, Function == API::ZwEnumerateValueKey);
  case API::ZwSetValueKey:
    return set(Model, Entry, A);
  case API::ZwDeleteValueKey: {
    auto ValueName = readName(Model, Memory, A[1], MaxRegistryValueNameLength);
    if (!ValueName)
      return ValueName.takeError();
    auto Value = Entry.Values.find(folded(*ValueName));
    if (Value == Entry.Values.end())
      return ObjectNameNotFound;
    auto Time = mutationTime(Model);
    if (!Time)
      return Time.takeError();
    DataBytes -= Value->second.Data.size();
    --ValueCount;
    std::erase(Entry.ValueOrder, Value->first);
    Entry.Values.erase(Value);
    if (Entry.Values.empty()) {
      Entry.MaxValueNameLength = 0;
      Entry.MaxValueDataLength = 0;
    }
    Entry.LastWriteTime100ns = *Time;
    return Success;
  }
  case API::ZwDeleteKey: {
    if (rootPath(Path))
      return CannotDelete;
    for (const auto &[OtherPath, Other] : Keys)
      if (!Other.Deleted && OtherPath != Path && atOrBelow(OtherPath, Path))
        return CannotDelete;
    auto Time = mutationTime(Model);
    if (!Time)
      return Time.takeError();
    for (const auto &[ValueName, Value] : Entry.Values) {
      --ValueCount;
      DataBytes -= Value.Data.size();
    }
    Entry.Values.clear();
    Entry.ValueOrder.clear();
    Entry.Deleted = true;
    const auto ParentPath = parentPath(Path);
    Key &Parent = Keys.at(ParentPath);
    Parent.LastWriteTime100ns = *Time;
    if (llvm::none_of(Keys, [&](const auto &Other) {
          return !Other.second.Deleted && parentPath(Other.first) == ParentPath;
        }))
      Parent.MaxNameLength = 0;
    return Success;
  }
  default:
    llvm_unreachable("handled registry API");
  }
}
} // namespace neverd::emulation
