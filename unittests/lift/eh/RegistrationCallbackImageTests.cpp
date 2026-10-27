//===- RegistrationCallbackImageTests.cpp - PE32 callback data bounds ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks live zero-fill ownership without granting immutable value facts.

#include "../../../lib/ir/low/X86/RegistrationABIPrivate.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/loader/BinaryImage.h"

using namespace neverd;

TEST(RegistrationCallbackImage, RequiresUniqueMappedDataAndRuntimePermissions) {
  for (unsigned Mutation = 0; Mutation != 15; ++Mutation)
    for (bool Write : {false, true}) {
      SCOPED_TRACE(Mutation);
      SCOPED_TRACE(Write);
      BinaryImage Image;
      Image.Arch = Arch::X86;
      Image.Bits = Bitness::Bits32;
      Image.Format = BinaryFormat::COFF;
      Segment Data;
      Data.VA = 0x403000;
      Data.Size = 64;
      Data.Data.resize(64);
      Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Image.Segments.push_back(Data);
      Section Storage;
      Storage.VA = Data.VA;
      Storage.Size = Data.Size;
      Storage.Flags = Data.Flags;
      Image.Sections.push_back(Storage);
      va_t Address = Data.VA + 8;
      size_t Work = 0;
      if (Mutation == 1)
        Address = Data.VA + 62;
      if (Mutation == 2)
        Image.Sections.clear();
      if (Mutation == 3)
        Image.Segments.clear();
      if (Mutation == 4)
        Image.Segments.front().Data.resize(10);
      if (Mutation == 5)
        Image.Segments.front().Flags = SegmentFlags::Readable;
      if (Mutation == 6)
        Image.Sections.front().Flags = SegmentFlags::Readable;
      if (Mutation == 7)
        Image.Segments.front().ReadOnlyAfterRelocations = true;
      if (Mutation == 8) {
        Storage.VA += 10;
        Image.Sections.push_back(Storage);
      }
      if (Mutation == 9) {
        Data.VA += 10;
        Image.Segments.push_back(Data);
      }
      if (Mutation == 10)
        Image.Segments.front().Flags = Data.Flags | SegmentFlags::Executable;
      if (Mutation == 11)
        Image.Sections.front().Flags = Data.Flags | SegmentFlags::Executable;
      if (Mutation == 12) {
        Import Binding;
        Binding.IATAddr = Address + 2;
        Image.Imports.push_back(Binding);
      }
      if (Mutation == 13)
        Image.IsRelocatable = true;
      if (Mutation == 14)
        Work = limits::kMaxRegistrationEHStateWork;
      const bool Accepted =
          Mutation == 0 || (!Write && Mutation >= 5 && Mutation <= 7);
      EXPECT_EQ(registration_abi::hasSEHCallbackImageMemory(Image, Address, 4,
                                                            Write, Work),
                Accepted);
      EXPECT_LE(Work, limits::kMaxRegistrationEHStateWork);
    }
}
