//===- X86SystemStateFixtures.h - Independent system encodings -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Hand-authored x86/x64 system instruction encodings for pipeline tests.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_X86_64_X86SYSTEMSTATEFIXTURES_H
#define NEVERD_UNITTESTS_LIFT_X86_64_X86SYSTEMSTATEFIXTURES_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <cstdint>
#include <vector>

namespace neverd::test {

struct SystemEncoding {
  std::vector<uint8_t> Bytes;
  Intrinsic Id;
  const char *Mnemonic;
};

inline std::vector<SystemEncoding> systemEncodings(Arch A) {
  std::vector<SystemEncoding> E = {
      {{0xf3, 0x0f, 0xc7, 0xf8}, Intrinsic::X86ReadPid, "rdpid"},
      {{0x0f, 0x06}, Intrinsic::X86ClearTaskSwitched, "clts"},
      {{0xf3, 0x0f, 0xae, 0xf0}, Intrinsic::X86UserMonitor, "umonitor"},
      {{0x67, 0xf3, 0x0f, 0xae, 0xf0}, Intrinsic::X86UserMonitor, "umonitor"},
      {{0x64, 0xf3, 0x0f, 0xae, 0xf0}, Intrinsic::X86UserMonitor, "umonitor"},
      {{0xf2, 0x0f, 0xae, 0xf1}, Intrinsic::X86UserWait, "umwait"},
      {{0x66, 0x0f, 0xae, 0xf1}, Intrinsic::X86TimedPause, "tpause"},
      {{0x0f, 0x02, 0xc1}, Intrinsic::X86LoadAccessRights, "lar"},
      {{0x66, 0x0f, 0x02, 0xc1}, Intrinsic::X86LoadAccessRights, "lar"},
      {{0x0f, 0x02, 0x04, 0x24}, Intrinsic::X86LoadAccessRightsMemory, "lar"},
      {{0x65, 0x0f, 0x02, 0x04, 0x24},
       Intrinsic::X86LoadAccessRightsMemory,
       "lar"},
  };
  if (A == Arch::X86) {
    E.push_back({{0x63, 0xc8}, Intrinsic::X86AdjustRpl, "arpl"});
    E.push_back({{0x63, 0x0c, 0x24}, Intrinsic::X86AdjustRplMemory, "arpl"});
    E.push_back(
        {{0x64, 0x63, 0x0c, 0x24}, Intrinsic::X86AdjustRplMemory, "arpl"});
  } else {
    for (const bool Wide : {false, true})
      for (const unsigned Op : {0u, 1u, 2u, 3u}) {
        std::vector<uint8_t> Bytes{0xf3};
        if (Wide)
          Bytes.push_back(0x48);
        Bytes.insert(Bytes.end(),
                     {0x0f, 0xae, static_cast<uint8_t>(0xc0 + 8 * Op)});
        const Intrinsic Ids[] = {
            Intrinsic::X86ReadFsBase, Intrinsic::X86ReadGsBase,
            Intrinsic::X86WriteFsBase, Intrinsic::X86WriteGsBase};
        const char *Names[] = {"rdfsbase", "rdgsbase", "wrfsbase", "wrgsbase"};
        E.push_back({Bytes, Ids[Op], Names[Op]});
      }
    E.push_back(
        {{0x48, 0x0f, 0x02, 0xc1}, Intrinsic::X86LoadAccessRights, "lar"});
    E.push_back({{0x48, 0x0f, 0x02, 0x04, 0x24},
                 Intrinsic::X86LoadAccessRightsMemory,
                 "lar"});
  }
  return E;
}

} // namespace neverd::test

#endif // NEVERD_UNITTESTS_LIFT_X86_64_X86SYSTEMSTATEFIXTURES_H
