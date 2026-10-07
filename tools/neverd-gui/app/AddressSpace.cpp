#include "AddressSpace.h"

#include <QJsonObject>
#include <algorithm>

namespace neverd::gui {

void AddressSpace::reset(const QJsonArray &regions) {
  regions_.clear();
  total_ = 0;
  for (const auto &value : regions) {
    const auto object = value.toObject();
    Region region;
    region.name = object.value("name").toString();
    region.start = addressValue(object.value("start")).value_or(0);
    region.end = addressValue(object.value("end")).value_or(region.start);
    region.linear = addressValue(object.value("linear")).value_or(total_);
    region.fileOffset = addressValue(object.value("file_offset"));
    region.exec = object.value("exec").toBool();
    if (region.end <= region.start)
      continue;
    total_ = std::max(total_, region.linear + (region.end - region.start));
    regions_.append(region);
  }
}

const AddressSpace::Region *AddressSpace::regionOf(Address address) const {
  auto it = std::upper_bound(
      regions_.cbegin(), regions_.cend(), address,
      [](Address value, const Region &r) { return value < r.start; });
  if (it == regions_.cbegin())
    return nullptr;
  --it;
  return address < it->end ? &*it : nullptr;
}

Address AddressSpace::linearOf(Address address) const {
  if (regions_.isEmpty())
    return 0;
  auto it = std::upper_bound(
      regions_.cbegin(), regions_.cend(), address,
      [](Address value, const Region &r) { return value < r.start; });
  if (it == regions_.cbegin())
    return 0;
  --it;
  if (address >= it->end)
    return it->linear + (it->end - it->start);
  return it->linear + (address - it->start);
}

Address AddressSpace::addressAt(Address linear) const {
  if (regions_.isEmpty())
    return 0;
  auto it = std::upper_bound(
      regions_.cbegin(), regions_.cend(), linear,
      [](Address value, const Region &r) { return value < r.linear; });
  if (it == regions_.cbegin())
    return regions_.front().start;
  --it;
  const Address offset =
      std::min<Address>(linear - it->linear, it->end - it->start - 1);
  return it->start + offset;
}

std::optional<Address> AddressSpace::fileOffsetOf(Address address) const {
  if (const auto *region = regionOf(address); region && region->fileOffset)
    return *region->fileOffset + (address - region->start);
  return std::nullopt;
}

} // namespace neverd::gui
