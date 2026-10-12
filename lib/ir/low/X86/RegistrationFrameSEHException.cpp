//===- RegistrationFrameSEHException.cpp - SEH runtime field coordinates
//---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationFrame.h"

namespace neverd::registration_state {

void FrameState::enterSEHFilter(va_t Entry, int32_t ExceptionPointersOffset) {
  enterCallback(Entry);
  SEHFilterEntry = Entry;
  store(ExceptionPointersOffset, 4,
        FrameValue::sehException(
            {SEHExceptionAddress::Kind::Pointers, Entry, 0}));
}

std::optional<FrameValue>
FrameState::sehExceptionLoad(const SEHExceptionAddress &Address,
                             uint16_t Width) const {
  if (CallbackEntry != Address.Filter || SEHFilterEntry != Address.Filter ||
      Width != 4)
    return std::nullopt;
  if (Address.Storage == SEHExceptionAddress::Kind::Pointers &&
      Address.Offset == 0)
    return FrameValue::sehException(
        {SEHExceptionAddress::Kind::Record, Address.Filter, 0});
  if (Address.Storage == SEHExceptionAddress::Kind::Record &&
      (Address.Offset == 0 || Address.Offset == 4))
    // No concrete exception origin or RaiseException argument has been bound.
    // The DWORD is unknown and may carry pointer bits. A later memory sink
    // needs its own address proof; field width alone cannot exclude an alias.
    return FrameValue{{}, {}, false, true};
  return std::nullopt;
}

} // namespace neverd::registration_state
