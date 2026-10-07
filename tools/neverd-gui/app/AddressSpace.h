#pragma once

#include "Address.h"

#include <QJsonArray>
#include <QString>
#include <QVector>
#include <optional>

namespace neverd::gui {

/// Mapped regions of the open image in address order, with the linear
/// position used by address-proportional scroll bars and the overview.
class AddressSpace {
public:
  struct Region {
    QString name;
    Address start = 0, end = 0, linear = 0;
    std::optional<Address> fileOffset;
    bool exec = false;
  };

  void reset(const QJsonArray &regions);
  void clear() {
    regions_.clear();
    total_ = 0;
  }
  bool empty() const { return regions_.isEmpty(); }
  const QVector<Region> &regions() const { return regions_; }
  Address total() const { return total_; }
  Address first() const {
    return regions_.isEmpty() ? 0 : regions_.front().start;
  }
  Address last() const {
    return regions_.isEmpty() ? 0 : regions_.back().end - 1;
  }

  /// Linear position of \p address, or of the next mapped address.
  Address linearOf(Address address) const;
  /// Address at a linear position.
  Address addressAt(Address linear) const;
  const Region *regionOf(Address address) const;
  std::optional<Address> fileOffsetOf(Address address) const;

private:
  QVector<Region> regions_;
  Address total_ = 0;
};

} // namespace neverd::gui
