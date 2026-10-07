#pragma once

namespace neverd::gui {

/// Worker protocol address classes (AddressClasses.def).
enum class AddressClass : int {
#define NEVERD_ADDRESS_CLASS(Name, Code) Name = Code,
#include "AddressClasses.def"
};

/// Worker protocol listing span roles (ListingRoles.def).
enum class ListingRole : int {
#define NEVERD_LISTING_ROLE(Name, Code) Name = Code,
#include "ListingRoles.def"
};

} // namespace neverd::gui
