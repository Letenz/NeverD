//===- AndroidProperties.cpp - Explicit Android property snapshots -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Implement read-only API 28 properties and real guest callback continuations.

#include "AndroidInternal.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation::android_model {
llvm::Expected<PropertyTable>
initializeProperties(AddressSpace &Space, const ProcessOptions &Options,
                     const ExecutionBudget &Budget) {
  PropertyTable Out;
  uint64_t Size = 0;
  const uint64_t Limit = std::min(Options.MemoryLimit, property_abi::Capacity);
  // Validate aggregate size before constructing or publishing any guest bytes.
  for (const auto &[Name, Value] : Options.Android->Properties) {
    if (!Budget.remainingMicroseconds())
      return failure(diagnostic::CallTimeout);
    if (Name.empty() || Name.find('\0') != std::string::npos ||
        Value.find('\0') != std::string::npos ||
        Value.size() >= property_abi::ValueBytes || Name.size() > Limit)
      return failure(diagnostic::Property);
    const uint64_t RecordSize =
        (property_abi::NameOffset + Name.size() + 4) & ~uint64_t(3);
    if (RecordSize > Limit - Size)
      return failure(diagnostic::PropertyArena);
    Size += RecordSize;
  }
  if (!Size)
    return Out;
  const uint64_t MappedSize = (Size + PageSize - 1) & ~(PageSize - 1);
  if (MappedSize > Limit)
    return failure(diagnostic::PropertyArena);
  if (auto E = Space.map(property_abi::Base, MappedSize, Read | Write))
    return std::move(E);
  uint64_t Address = property_abi::Base;
  for (const auto &[Name, Value] : Options.Android->Properties) {
    if (!Budget.remainingMicroseconds())
      return failure(diagnostic::CallTimeout);
    // A newly constructed, immutable short-value record has generation zero.
    // This is snapshot policy, not an observed device update counter.
    const uint32_t Serial = static_cast<uint32_t>(Value.size()) << 24;
    const uint64_t RecordSize =
        (property_abi::NameOffset + Name.size() + 4) & ~uint64_t(3);
    PropertyRecord Record{Address, Name, Value, Serial,
                          std::vector<uint8_t>(RecordSize)};
    llvm::support::endian::write32le(Record.Bytes.data(), Serial);
    std::copy(Value.begin(), Value.end(),
              Record.Bytes.begin() + property_abi::ValueOffset);
    std::copy(Name.begin(), Name.end(),
              Record.Bytes.begin() + property_abi::NameOffset);
    if (auto E = Space.write(Address, Record.Bytes))
      return std::move(E);
    Out.Names.emplace(Name, Out.Records.size());
    Out.Records.push_back(std::move(Record));
    Address += RecordSize;
  }
  if (auto E =
          Space.protect(property_abi::Base, MappedSize, Read | UserAccessible))
    return std::move(E);
  return Out;
}

llvm::Expected<const PropertyRecord *> Bionic::propertyAt(uint64_t Address) {
  const auto &Records = Linked.Properties.Records;
  auto I = std::lower_bound(
      Records.begin(), Records.end(), Address,
      [](const PropertyRecord &P, uint64_t A) { return P.Address < A; });
  if (I == Records.end() || I->Address != Address)
    return failure(diagnostic::PropertyHandle);
  if (auto E = access(Address, I->Bytes.size(), Read))
    return std::move(E);
  std::vector<uint8_t> Current(I->Bytes.size());
  if (auto E = CPU.read(Address, Current))
    return std::move(E);
  // An unmap, replacement or guest mutation cannot silently fall back to the
  // original host-side dictionary. Direct guest reads see this same image.
  if (Current != I->Bytes)
    return failure(diagnostic::PropertyChanged);
  return &*I;
}

llvm::Expected<const PropertyRecord *>
Bionic::propertyNamed(llvm::StringRef Name) {
  auto I = Linked.Properties.Names.find(Name.str());
  if (I == Linked.Properties.Names.end())
    return nullptr;
  return propertyAt(Linked.Properties.Records[I->second].Address);
}

llvm::Error Bionic::propertyString(uint64_t Address, llvm::StringRef Text) {
  if (auto E = access(Address, Text.size() + 1, Write))
    return E;
  if (!Linked.Properties.Records.empty()) {
    const auto &Last = Linked.Properties.Records.back();
    const uint64_t End =
        (Last.Address + Last.Bytes.size() + PageSize - 1) & ~(PageSize - 1);
    // Even if guest mprotect made these pages writable, a property output
    // cannot mutate the immutable input used by a later copy in this call.
    if (Address < End && (Address >= property_abi::Base ||
                          Text.size() + 1 > property_abi::Base - Address))
      return failure(diagnostic::PropertyOutput);
  }
  std::vector<uint8_t> Bytes(Text.begin(), Text.end());
  Bytes.push_back(0);
  return CPU.write(Address, Bytes);
}

BionicResult Bionic::getProperty(const NativeCallEvent &Call) {
  auto Key = string(Call.Arguments[0]);
  if (!Key)
    return Key.takeError();
  auto Property = propertyNamed(*Key);
  if (!Property)
    return Property.takeError();
  llvm::StringRef Text = *Property ? (*Property)->Value : llvm::StringRef();
  if (auto E = propertyString(Call.Arguments[1], Text))
    return std::move(E);
  return value(Text.size());
}

BionicResult Bionic::findProperty(const NativeCallEvent &Call) {
  auto Key = string(Call.Arguments[0]);
  if (!Key)
    return Key.takeError();
  auto Property = propertyNamed(*Key);
  if (!Property)
    return Property.takeError();
  return value(*Property ? (*Property)->Address : 0);
}

BionicResult Bionic::findPropertyNth(const NativeCallEvent &Call) {
  // unsigned, not size_t: unspecified upper argument bits are ignored.
  const uint32_t Index = Call.Arguments[0];
  if (Index >= Linked.Properties.Records.size())
    return value(0);
  auto Property = propertyAt(Linked.Properties.Records[Index].Address);
  if (!Property)
    return Property.takeError();
  return value((*Property)->Address);
}

BionicResult Bionic::readProperty(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  auto Property = propertyAt(A[0]);
  if (!Property)
    return Property.takeError();
  const auto &P = **Property;
  // API 28 writes value first, then an optional, legacy 32-byte name. Keep
  // this order when output objects overlap, including their trailing bytes.
  if (auto E = propertyString(A[2], P.Value))
    return std::move(E);
  if (A[1])
    if (auto E = propertyString(A[1], llvm::StringRef(P.Name).take_front(
                                          property_abi::LegacyNameBytes - 1)))
      return std::move(E);
  return value(P.Value.size());
}

BionicResult Bionic::propertySerial(const NativeCallEvent &Call) {
  auto Property = propertyAt(Call.Arguments[0]);
  if (!Property)
    return Property.takeError();
  return value((*Property)->Serial);
}

llvm::Error Bionic::propertyCallbackEntry(uint64_t Entry) {
  if (Entry % 4)
    return failure(diagnostic::PropertyCallbackAlignment);
  return access(Entry, 4, Execute);
}

BionicResult Bionic::readPropertyCallback(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  auto Property = propertyAt(A[0]);
  if (!Property)
    return Property.takeError();
  if (auto E = propertyCallbackEntry(A[1]))
    return std::move(E);
  const auto &P = **Property;
  // Snapshot strings are const guest storage. No host callback runs here.
  return BionicValue(
      GuestCallback{A[1],
                    {A[2], P.Address + property_abi::NameOffset,
                     P.Address + property_abi::ValueOffset, P.Serial},
                    PropertyReadCallback{}});
}

BionicResult Bionic::nextProperty(const PropertyCallback &Callback) {
  if (Callback.Next == Linked.Properties.Records.size())
    return value(0);
  auto Property = propertyAt(Linked.Properties.Records[Callback.Next].Address);
  if (!Property)
    return Property.takeError();
  if (auto E = propertyCallbackEntry(Callback.Entry))
    return std::move(E);
  return BionicValue(GuestCallback{
      Callback.Entry,
      {(*Property)->Address, Callback.Cookie},
      PropertyCallback{Callback.Next + 1, Callback.Entry, Callback.Cookie}});
}

BionicResult Bionic::forEachProperty(const NativeCallEvent &Call) {
  return nextProperty({0, Call.Arguments[0], Call.Arguments[1]});
}
} // namespace neverd::emulation::android_model
