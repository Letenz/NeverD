#ifndef NEVERD_LOADER_SOURCEUNWIND_H
#define NEVERD_LOADER_SOURCEUNWIND_H

#include "neverd/loader/ExceptionInfo.h"

#include <algorithm>
#include <set>

namespace neverd {
/// Structural unwind metadata that introduces no language dispatch.
inline bool isPlainSourceUnwind(const ExceptionFunction &Metadata) {
  // Darwin emits unwind ranges for ordinary leaf methods too. Only a fully
  // decoded structural frame with no language-dispatch state is benign here.
  if (Metadata.ParseStatus != ExceptionParseStatus::Complete ||
      Metadata.Personality != ExceptionPersonality::None ||
      Metadata.PersonalityVA || !Metadata.PersonalityName.empty() ||
      Metadata.HandlerDataVA || Metadata.hasLanguageTable() ||
      Metadata.GSCookie || Metadata.ARMEHABI || Metadata.Rust)
    return false;
  if (Metadata.ObjC) {
    const auto &ObjC = *Metadata.ObjC;
    // A runtime synchronization call can occur in an ordinary C body without
    // a landing pad or personality. Its call binding is checked separately;
    // the presence of a language annotation alone does not imply a handler.
    if (ObjC.Runtime != ObjCRuntimeKind::AppleNonFragile ||
        ObjC.UsesFragileSetjmp || ObjC.UsesMSVCTables ||
        !ObjC.LandingPads.empty())
      return false;
    for (const auto &Call : ObjC.RuntimeCalls)
      if (Call.Kind != ObjCRuntimeCallKind::SyncEnter &&
          Call.Kind != ObjCRuntimeCallKind::SyncExit &&
          Call.Kind != ObjCRuntimeCallKind::ARCCleanup)
        return false;
  }
  if (Metadata.Encoding == ExceptionEncoding::CompactUnwind) {
    if (!Metadata.Compact)
      return false;
  } else if (Metadata.Encoding == ExceptionEncoding::DwarfFDE) {
    if (!Metadata.Dwarf)
      return false;
  } else {
    return false;
  }
  if (Metadata.Compact &&
      (Metadata.Compact->PersonalityVA || Metadata.Compact->HasLSDA ||
       Metadata.Compact->LSDAVA ||
       Metadata.Compact->SemanticStatus !=
           CompactUnwindSemanticStatus::Complete))
    return false;
  return !Metadata.Dwarf || Metadata.Dwarf->LSDAVA == 0;
}

/// An unwind record can enclose several independently decoded Mach-O entries.
/// Its Objective-C runtime-call inventory belongs to the whole record, not to
/// every subentry. Only when the frame has no language dispatch or landing pads
/// may a complete source projection use the calls present in this entry's
/// decoded instructions. Keep the structural record and every other language
/// annotation intact; an incomplete function audit is rejected separately.
inline ExceptionFunction sourceUnwindForDecodedSubentry(
    const ExceptionFunction &Metadata, va_t Entry,
    const std::set<va_t> &DecodedInstructions) {
  if (Entry == Metadata.CodeRange.Begin ||
      !Metadata.CodeRange.contains(Entry) ||
      !DecodedInstructions.count(Entry) || !Metadata.ObjC ||
      Metadata.ObjC->RuntimeCalls.empty() ||
      !Metadata.ObjC->LandingPads.empty() ||
      Metadata.ObjC->UsesFragileSetjmp || Metadata.ObjC->UsesMSVCTables)
    return Metadata;
  ExceptionFunction Scoped = Metadata;
  Scoped.ObjC.reset();
  if (!isPlainSourceUnwind(Scoped))
    return Metadata;
  Scoped.ObjC = Metadata.ObjC;
  std::erase_if(Scoped.ObjC->RuntimeCalls, [&](const ObjCRuntimeCall &Call) {
    return !DecodedInstructions.count(Call.CallVA);
  });
  if (Scoped.ObjC->RuntimeCalls.empty())
    Scoped.ObjC.reset();
  return Scoped;
}
} // namespace neverd
#endif
