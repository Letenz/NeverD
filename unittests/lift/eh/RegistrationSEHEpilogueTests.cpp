//===- RegistrationSEHEpilogueTests.cpp - Original parent return proof ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/STLExtras.h"

using namespace neverd;

namespace {

BinaryImage epilogueImage(bool Scratch, unsigned Mutation) {
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x400000;
  Segment Text;
  Text.VA = 0x401000;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  const auto Emit = [&](std::initializer_list<uint8_t> Bytes) {
    const va_t Start = Text.VA + Text.Data.size();
    Text.Data.insert(Text.Data.end(), Bytes);
    return Start;
  };
  Emit({0x55, 0x8b, 0xec, 0x83, 0xec, 0x10});
  Emit({0x64, 0xa1, 0, 0, 0, 0, 0x89, 0x45, 0xf0});
  const va_t Install = Emit({0x64, 0x89, 0x25, 0, 0, 0, 0});
  Emit({0x83, 0xec, 0x0c, 0x53, 0x56, 0x57});
  if (Scratch)
    Emit({0x6a, 0});
  Emit({0x89, 0x65, 0xe8});
  if (Mutation == 1)
    Emit({0xc7, 0x45, 0x04, 0, 0, 0, 0}); // Caller return PC.
  if (Mutation == 2)
    Emit({0xc7, 0x45, 0x00, 0, 0, 0, 0}); // Saved EBP.
  if (Mutation == 3)
    Emit({0xc7, 0x45, 0xe0, 0, 0, 0, 0}); // Saved EBX.
  const va_t Active = Emit({0xc7, 0x45, 0xfc, 0, 0, 0, 0});
  const va_t Retired = Emit({0xc7, 0x45, 0xfc, 0xff, 0xff, 0xff, 0xff});
  Emit({0xe8, 0, 0, 0, 0});
  const va_t Finally = Emit({0xb8, 0x7b, 0, 0, 0});
  Emit({0x8b, 0x65, uint8_t(Mutation == 4 ? 0xe4 : 0xe8)});
  Emit({0x8b, 0x4d, 0xf0});
  Emit({0x64, 0x89, 0x0d, 0, 0, 0, 0});
  const va_t Epilogue = Text.VA + Text.Data.size();
  if (Mutation == 5)
    Emit({0xc3}); // A bare RET never establishes parent ownership.
  else {
    if (Scratch)
      Emit({0x59});
    Emit({0x5f, 0x5e, uint8_t(Mutation == 6 ? 0x59 : 0x5b)});
    Emit({0x8b, 0xe5, 0x5d, 0xc3});
  }
  Text.Size = Text.FileSz = Text.Data.size();
  Image.Segments.push_back(Text);
  Section Code;
  Code.VA = Text.VA;
  Code.Size = Code.FileSz = Text.Size;
  Code.Flags = Text.Flags;
  Image.Sections.push_back(Code);
  ExceptionFunction EH;
  EH.CodeRange = {Text.VA, Text.VA + Text.Size};
  EH.Encoding = ExceptionEncoding::X86ScopeTableEH3;
  EH.Personality = ExceptionPersonality::ExceptHandler3;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  auto &Chain = EH.Registration.emplace();
  Chain.SeededTryLevel = -1;
  Chain.TryLevelOffset = -4;
  Chain.RegistrationOffset = -16;
  Chain.ChainInstallVA = Install;
  Chain.Scopes = {{-1, 0, Finally, true}};
  Chain.TryLevelStores = {{Active, Active + 7, 0}, {Retired, Retired + 7, -1}};
  if (Mutation == 7)
    Chain.Scopes.push_back({-1, 0, Epilogue, true});
  Image.ExceptionMetadata.Functions.push_back(EH);
  Image.ExceptionMetadata.rebuildIndex();
  return Image;
}

} // namespace

TEST(RegistrationSEHEpilogue,
     FallthroughRestoresOriginalCallerOnEveryInvocation) {
  for (bool Scratch : {false, true})
    for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
      SCOPED_TRACE(Scratch);
      SCOPED_TRACE(Mutation);
      auto Image = epilogueImage(Scratch, Mutation);
      Decoder Decode;
      ASSERT_TRUE(Decode.init(Image));
      const auto Function = CFGBuilder().build(Image, Decode, 0x401000);
      ASSERT_TRUE(Function.RegistrationStates);
      const auto &State = *Function.RegistrationStates;
      EXPECT_EQ(State.Complete && State.CallbackStatesComplete &&
                    State.RegistrationLifetimeComplete &&
                    State.SEHInvocationExitsComplete,
                Mutation == 0);
      if (Mutation)
        continue;
      ASSERT_EQ(State.SEHInvocationExits.size(), 2u);
      EXPECT_TRUE(llvm::all_of(State.SEHInvocationExits, [](const auto &Exit) {
        return Exit.Exit ==
                   RegistrationSEHInvocationExit::Kind::ParentFallthrough &&
               Exit.TargetVA == Exit.EndAddress;
      }));
      EXPECT_EQ(State.SEHInvocationExits[0].CallVA, 0u);
      EXPECT_NE(State.SEHInvocationExits[1].CallVA, 0u);
    }
}
