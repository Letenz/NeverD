//===- UnpackNames.cpp - Stable names of the unpacking vocabulary ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "UnpackInternal.h"

#include "llvm/Support/ErrorHandling.h"

namespace neverd::unpack {
#define NEVERD_UNPACK_NAME_CASE(Type, Name, Text)                              \
  case Type::Name:                                                             \
    return Text;

const char *formatKindName(FormatKind Kind) {
  switch (Kind) {
#define NEVERD_UNPACK_FORMAT(Name, Text)                                       \
  NEVERD_UNPACK_NAME_CASE(FormatKind, Name, Text)
#include "neverd/unpack/Unpack.def"
#undef NEVERD_UNPACK_FORMAT
  }
  llvm_unreachable(text::UnknownName);
}
const char *packerKindName(PackerKind Kind) {
  switch (Kind) {
#define NEVERD_UNPACK_PACKER(Name, Text)                                       \
  NEVERD_UNPACK_NAME_CASE(PackerKind, Name, Text)
#include "neverd/unpack/Unpack.def"
#undef NEVERD_UNPACK_PACKER
  }
  llvm_unreachable(text::UnknownName);
}
const char *packerEvidenceName(PackerEvidence Evidence) {
  switch (Evidence) {
#define NEVERD_UNPACK_EVIDENCE(Name, Text)                                     \
  NEVERD_UNPACK_NAME_CASE(PackerEvidence, Name, Text)
#include "neverd/unpack/Unpack.def"
#undef NEVERD_UNPACK_EVIDENCE
  }
  llvm_unreachable(text::UnknownName);
}
const char *unpackOutcomeName(UnpackOutcome Outcome) {
  switch (Outcome) {
#define NEVERD_UNPACK_OUTCOME(Name, Text)                                      \
  NEVERD_UNPACK_NAME_CASE(UnpackOutcome, Name, Text)
#include "neverd/unpack/Unpack.def"
#undef NEVERD_UNPACK_OUTCOME
  }
  llvm_unreachable(text::UnknownName);
}
const char *entrySourceName(EntrySource Source) {
  switch (Source) {
#define NEVERD_UNPACK_ENTRY_SOURCE(Name, Text)                                 \
  NEVERD_UNPACK_NAME_CASE(EntrySource, Name, Text)
#include "neverd/unpack/Unpack.def"
#undef NEVERD_UNPACK_ENTRY_SOURCE
  }
  llvm_unreachable(text::UnknownName);
}
const char *importOriginName(ImportOrigin Origin) {
  switch (Origin) {
#define NEVERD_UNPACK_IMPORT_ORIGIN(Name, Text)                                \
  NEVERD_UNPACK_NAME_CASE(ImportOrigin, Name, Text)
#include "neverd/unpack/Unpack.def"
#undef NEVERD_UNPACK_IMPORT_ORIGIN
  }
  llvm_unreachable(text::UnknownName);
}
// The execution layer owns the instruction set vocabulary. Its table is read
// here so that identification and rebuilding need no CPU component.
const char *architectureName(emulation::GuestArchitecture Architecture) {
  switch (Architecture) {
#define NEVERD_GUEST_ARCHITECTURE(Name, Text)                                  \
  NEVERD_UNPACK_NAME_CASE(emulation::GuestArchitecture, Name, Text)
#include "neverd/emulation/ExecutionBackend.def"
#undef NEVERD_GUEST_ARCHITECTURE
  }
  llvm_unreachable(text::UnknownName);
}
#undef NEVERD_UNPACK_NAME_CASE
} // namespace neverd::unpack
