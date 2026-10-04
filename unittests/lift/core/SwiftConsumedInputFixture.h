#ifndef NEVERD_TEST_SWIFTCONSUMEDINPUTFIXTURE_H
#define NEVERD_TEST_SWIFTCONSUMEDINPUTFIXTURE_H
#include "ImmutableNativeCallFixture.h"

#include "neverd/loader/Swift/SwiftConsumedInputEffects.h"
#include "neverd/pipeline/NativeSourceHints.h"
namespace swift_consumed_input_test {
using namespace neverd;
constexpr va_t Entry = 0x1000, Call = 0x1024, RuntimeSlot = 0x2000,
               MetadataSlot = 0x2008, WitnessSlot = 0x2010;
struct Fixture : immutable_native_call_test::Fixture {
  Fixture() {
    for (auto &S : Image.Segments)
      if (S.VA == RuntimeSlot) {
        S.Size = S.FileSz = 24;
        S.Data.resize(24);
      }
    for (auto &S : Image.Sections)
      if (S.VA == RuntimeSlot)
        S.Size = S.FileSz = 24;
    for (const auto &[Slot, Name] : std::map<va_t, std::string>{
             {RuntimeSlot, "_$ss11AnyHashableVyABxcSHRzlufC"},
             {MetadataSlot, "_$sSuN"},
             {WitnessSlot, "_$sSuSHsWP"}}) {
      Image.ImportPtrSlots[Slot] = Name;
      Image.DyldBindSlots[Slot] = {Name, 0, "/usr/lib/swift/libswiftCore.dylib",
                                   false};
    }
    const uint32_t Words[] = {0xd10083ff, 0xa9017bfd, 0x910043fd, 0xf90007e0,
                              0xb0000001, 0xf9400421, 0xb0000002, 0xf9400842,
                              0x910023e0, 0x94000037, 0xa9417bfd, 0x910083ff,
                              0xd65f03c0};
    Image.Symbols[0] = {"consumed_uint", Entry, sizeof(Words), true};
    for (unsigned I = 0; I < std::size(Words); ++I)
      word(I, Words[I]);
    runUInt();
  }
  void runUInt() {
    PipelineOptions O;
    O.EmitDumpOutput = false;
    O.OnlyFunctionEntries = {Entry};
    if (EntrySignature.ReturnType)
      O.SourceTypeHints.emplace(Entry, EntrySignature);
    Result = Pipeline().run(Image, Context, O);
  }
  const PipelineFunctionAudit *audit() const {
    for (const auto &A : Result.FunctionAudits)
      if (A.Entry == Entry)
        return &A;
    return nullptr;
  }
  std::optional<SourceFunctionTypeHint> infer(std::string &Error) {
    if (!low() || !med() || !high() || !audit())
      return std::nullopt;
    return inferNativeSourceTypeHint(Image, *med(), *high(), *audit(), Error,
                                     low());
  }
};
constexpr va_t IdentifierCall = Call + 4;
struct IdentifierFixture : Fixture {
  IdentifierFixture() {
    for (const auto &[Slot, Name] : std::map<va_t, std::string>{
             {MetadataSlot, "_$sSON"}, {WitnessSlot, "_$sSOSHsWP"}}) {
      Image.ImportPtrSlots[Slot] = Name;
      Image.DyldBindSlots[Slot].Name = Name;
    }
    // Keep the complete typed input. Independently form the opaque result
    // address between the witness load and the input-address computation.
    word(8, 0x9100a108);  // add x8, x8, #40
    word(9, 0x910023e0);  // add x0, sp, #8
    word(10, 0x94000036); // bl 0x1100
    word(11, 0xa9417bfd);
    word(12, 0x910083ff);
    word(13, 0xd65f03c0);
    Image.Symbols[0].Name = "consumed_identifier";
    Image.Symbols[0].Size = 56;
    runUInt();
  }
};
} // namespace swift_consumed_input_test
#endif
