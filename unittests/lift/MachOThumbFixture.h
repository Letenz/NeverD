//===- MachOThumbFixture.h - Thumb-only Mach-O test objects --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Endian.h"

#include <cstddef>
#include <filesystem>
#include <fstream>

namespace neverd::test {

// These assembly fixtures use only v6-M or v7-M instructions. Assemble that
// subset with the host tool, then explicitly set the subtype under test.
// Older Clang drivers cannot spell newer Mach-O M-profile subtypes; the
// tests exercise NeverD's header/mode handling, not Clang's subtype mapping.
inline const char *thumbFixtureAssemblerTriple(llvm::StringRef Fixture) {
  return Fixture.ends_with("_v6m.s") ? "armv6m-apple-darwin"
                                     : "armv7m-apple-darwin";
}

inline bool setThumbFixtureSubtype(const std::filesystem::path &Object,
                                   llvm::StringRef Triple) {
  const uint32_t Subtype =
      llvm::StringSwitch<uint32_t>(Triple)
          .Case("armv6m-apple-darwin", llvm::MachO::CPU_SUBTYPE_ARM_V6M)
          .Case("armv7m-apple-darwin", llvm::MachO::CPU_SUBTYPE_ARM_V7M)
          .Case("armv7em-apple-darwin", llvm::MachO::CPU_SUBTYPE_ARM_V7EM)
          .Case("armv8m.base-apple-darwin",
                llvm::MachO::CPU_SUBTYPE_ARM_V8M_BASE)
          .Case("armv8m.main-apple-darwin",
                llvm::MachO::CPU_SUBTYPE_ARM_V8M_MAIN)
          .Case("armv8.1m.main-apple-darwin",
                llvm::MachO::CPU_SUBTYPE_ARM_V8_1M_MAIN)
          .Default(0);
  if (!Subtype)
    return false;
  std::fstream File(Object, std::ios::in | std::ios::out | std::ios::binary);
  char Header[sizeof(llvm::MachO::mach_header)];
  if (!File.read(Header, sizeof(Header)) ||
      llvm::support::endian::read32le(Header) != llvm::MachO::MH_MAGIC ||
      llvm::support::endian::read32le(
          Header + offsetof(llvm::MachO::mach_header, cputype)) !=
          llvm::MachO::CPU_TYPE_ARM)
    return false;
  llvm::support::endian::write32le(
      Header + offsetof(llvm::MachO::mach_header, cpusubtype), Subtype);
  File.seekp(0);
  File.write(Header, sizeof(Header));
  File.flush();
  return bool(File);
}

} // namespace neverd::test
