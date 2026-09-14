#include "ObjCMethodLists.h"

#include "ObjCRuntimeData.h"

namespace neverd::objc {
std::optional<std::vector<MethodRecord>>
readMethodList(const BinaryImage &Image, va_t Address, size_t &Remaining,
               std::string &Diagnostic) {
  Diagnostic.clear();
  const RuntimeData Data(Image);
  const auto Flags = Data.u32(Address);
  const auto Count =
      Address <= InvalidVA - 4 ? Data.u32(Address + 4) : std::nullopt;
  if (!Flags || !Count) {
    Diagnostic = "Truncated Objective-C method list";
    return std::nullopt;
  }
  const bool Small = (*Flags & 0x80000000U) != 0;
  const bool DirectSelectors = (*Flags & 0x40000000U) != 0;
  const uint32_t EntrySize = *Flags & 0xfffcU;
  if ((*Flags & 0x3fff0000U) || (DirectSelectors && !Small) ||
      EntrySize < (Small ? 12U : 24U) || EntrySize > 4096 ||
      *Count > Remaining ||
      !Data.bytes(Address, 8ULL + uint64_t(*Count) * EntrySize)) {
    Diagnostic = "Invalid or excessive Objective-C method list";
    return std::nullopt;
  }
  auto Relative = [&](va_t Slot, bool Nullable = false) -> std::optional<va_t> {
    const auto Value = Data.u32(Slot);
    if (!Value)
      return std::nullopt;
    if (Nullable && !*Value)
      return 0;
    const int64_t Offset = static_cast<int32_t>(*Value);
    if ((Offset < 0 && Slot < uint64_t(-Offset)) ||
        (Offset > 0 && Slot > InvalidVA - uint64_t(Offset)))
      return std::nullopt;
    return Offset < 0 ? Slot - uint64_t(-Offset) : Slot + uint64_t(Offset);
  };
  Remaining -= *Count;
  std::vector<MethodRecord> Records;
  Records.reserve(*Count);
  for (uint32_t I = 0; I < *Count; ++I) {
    MethodRecord Record;
    Record.Address = Address + 8 + uint64_t(I) * EntrySize;
    auto Selector =
        Small ? Relative(Record.Address) : Data.localPointer(Record.Address);
    if (Small && !DirectSelectors && Selector)
      Selector = Data.localPointer(*Selector);
    const auto Types = Small ? Relative(Record.Address + 4)
                             : Data.localPointer(Record.Address + 8);
    Record.Implementation = Small ? Relative(Record.Address + 8, true)
                                  : Data.localPointer(Record.Address + 16);
    if (Selector)
      Record.Selector = Data.string(*Selector).value_or("");
    if (Types)
      Record.TypeEncoding = Data.string(*Types).value_or("");
    Records.push_back(std::move(Record));
  }
  return Records;
}
} // namespace neverd::objc
