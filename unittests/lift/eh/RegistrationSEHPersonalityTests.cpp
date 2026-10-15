//===- RegistrationSEHPersonalityTests.cpp - EH3 provider binding -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"

#include "llvm/Support/Endian.h"

using namespace neverd;

TEST(RegistrationSEHPersonality, RequiresTheRuntimeImportAndExecutableVeneer) {
  for (const char *Provider : {"msvcrt.dll", "ucrtbase.dll", "VCRUNTIME140.dll",
                               "vcruntime140d.dll"}) {
    SCOPED_TRACE(Provider);
    for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
      SCOPED_TRACE(Mutation);
      BinaryImage Image;
      Image.Arch = Mutation == 6 ? Arch::ARM : Arch::X86;
      Image.Bits = Bitness::Bits32;
      Image.Format = BinaryFormat::COFF;
      Image.Base = 0x400000;
      Segment Text;
      Text.VA = 0x401000;
      Text.Size = 6;
      Text.Flags = SegmentFlags::Readable;
      if (Mutation != 4)
        Text.Flags = Text.Flags | SegmentFlags::Executable;
      Text.Data = {0xff, uint8_t(Mutation == 3 ? 0x15 : 0x25), 0, 0, 0, 0};
      llvm::support::endian::write32le(Text.Data.data() + 2, 0x403000);
      if (Mutation == 5)
        Text.Data.pop_back();
      Image.Segments.push_back(std::move(Text));
      Segment Data;
      Data.VA = 0x403000;
      Data.Size = 4;
      Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Data.Data.resize(4);
      Image.Segments.push_back(std::move(Data));
      Image.Imports.push_back(
          {Mutation == 1 ? "custom.dll" : Provider,
           Mutation == 2 ? "_except_handler4" : "_except_handler3", 0,
           0x403000});
      EXPECT_EQ(coff_loader::isCheckedX86SEH3Personality(Image, 0x401000),
                Mutation == 0);
    }
  }
}
