//===- ExceptionForwarding.h - Unwinds a frame passes on --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Whether an unwind leaving a frame at one address runs any of its code.

#ifndef NEVERD_LOADER_EXCEPTIONFORWARDING_H
#define NEVERD_LOADER_EXCEPTIONFORWARDING_H

#include "neverd/loader/ExceptionFunction.h"

namespace neverd {

/// True when an unwind that leaves the frame \p Function describes at
/// \p Address runs none of the frame's code: the record only unwinds the
/// frame, or it is a complete address-form Itanium table (the LSDA its
/// handler data names), read by the C or C++ personality, whose call site
/// covering \p Address names no landing pad, so the exception goes on to the
/// caller.  An address the table leaves out does not count: the C++
/// personality terminates there.
inline bool exceptionForwardsUnwindAt(const ExceptionFunction &Function,
                                      va_t Address) {
  if (Function.describesUnwindOnly())
    return true;
  if (Function.ParseStatus != ExceptionParseStatus::Complete ||
      !Function.Itanium || !Function.Itanium->IsCallSiteAddressForm ||
      Function.SEH || Function.Cxx || Function.GSCookie ||
      Function.Registration || Function.Delphi || Function.DelphiScopes ||
      Function.Go || Function.Rust || Function.ObjC || Function.ARMEHABI ||
      (Function.Personality != ExceptionPersonality::GxxPersonalityV0 &&
       Function.Personality != ExceptionPersonality::GccPersonalityV0))
    return false;
  for (const ItaniumCallSite &Site : Function.Itanium->CallSites)
    if (Site.GuardedRange.contains(Address))
      return Site.LandingPadVA == 0;
  return false;
}

} // namespace neverd

#endif // NEVERD_LOADER_EXCEPTIONFORWARDING_H
