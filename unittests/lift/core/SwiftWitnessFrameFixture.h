#ifndef NEVERD_TEST_SWIFTWITNESSFRAMEFIXTURE_H
#define NEVERD_TEST_SWIFTWITNESSFRAMEFIXTURE_H
#include "ImmutableNativeCallFixture.h"

namespace swift_witness_frame_test {
using namespace neverd;
inline void frameWitnessFixture(immutable_native_call_test::Fixture &F) {
  constexpr uint32_t Words[] = {
      0xa9bb53f3, 0xa9015bf5, 0xa9047bfd, 0xaa0003f3, 0xaa0103f4, 0xaa0203f5,
      0x92401476, 0x910006d6, 0xf85f82a8, 0xf90013e8, 0xaa0403e0, 0x94000035,
      0xf10006d6, 0x54ffffe1, 0xf94013e8, 0xf9400908, 0xaa1303e0, 0xaa1403e1,
      0xaa1503e2, 0xd63f0100, 0xa9447bfd, 0xa9415bf5, 0xa8c553f3, 0xd65f03c0};
  for (unsigned I = 0; I < std::size(Words); ++I)
    F.word(I, Words[I]);
  F.Image.Symbols[0].Size = sizeof(Words);
  F.Image.ImportPtrSlots[0x2000] = "_objc_release";
  F.Image.DyldBindSlots[0x2000] = {"_objc_release", 0,
                                   "/usr/lib/libobjc.A.dylib", false};
  F.EntrySignature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  F.EntrySignature.ReturnType = NdType::makePtr(NdType::makeVoid());
  for (unsigned I = 0; I < 5; ++I)
    F.EntrySignature.Parameters.push_back(
        {"arg" + std::to_string(I), I == 3
                                        ? NdType::makeInt(8, false)
                                        : NdType::makePtr(NdType::makeVoid())});
  std::string Error;
  if (!assignDarwinScalarSourceABI(F.EntrySignature, Arch::AArch64, Error))
    throw std::runtime_error(Error);
  F.run();
}
} // namespace swift_witness_frame_test

#endif
