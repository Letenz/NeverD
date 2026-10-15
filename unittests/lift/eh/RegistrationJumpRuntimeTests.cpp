//===- RegistrationJumpRuntimeTests.cpp - Nonlocal runtime identity -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Refuse lookalike, changed and unauthenticated jump-runtime bindings.

#include "../../../lib/ir/low/X86/RegistrationJumpRuntime.h"
#include "gtest/gtest.h"

#include "neverd/loader/COFF/COFFLoader.h"

using namespace neverd;
using namespace neverd::registration_state;

#ifdef NEVERD_BINARY_CORPUS_ROOT
TEST(RegistrationJumpRuntime, RequiresCurrentImportAndImmutableVeneer) {
  struct Routine {
    va_t Target;
    va_t Slot;
    JumpRuntimeBinding::Kind Kind;
  };
  const Routine Routines[] = {
      {0x407c7e, 0x408058, JumpRuntimeBinding::Kind::Capture},
      {0x407c66, 0x408044, JumpRuntimeBinding::Kind::Resume},
      {0x407c78, 0x40805c, JumpRuntimeBinding::Kind::EH3Unwind}};
  for (const auto &Routine : Routines)
    for (bool Indirect : {false, true})
      for (unsigned Mutation = 0; Mutation != 14; ++Mutation) {
        SCOPED_TRACE(Routine.Target);
        SCOPED_TRACE(Indirect);
        SCOPED_TRACE(Mutation);
        auto Image = COFFLoader().load(std::string(NEVERD_BINARY_CORPUS_ROOT) +
                                       "/corpus/windows-eh/msvc/vs2019/x86/"
                                       "native/no-gs/o0/windows-seh-tests/"
                                       "xcpt4-msvc-x86-native-no-gs-o0.exe");
        ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
        auto Imported = llvm::find_if(Image->Imports, [&](const Import &I) {
          return I.IATAddr == Routine.Slot;
        });
        ASSERT_NE(Imported, Image->Imports.end());
        if (Mutation == 1)
          Imported->Module = "application.dll";
        if (Mutation == 2)
          Imported->Name = "same_abi_opaque";
        if (Mutation == 3)
          Imported->IsDelayImport = true;
        if (Mutation == 4) {
          Image->Imports.push_back(*Imported);
          Image->Imports.back().Module = "application.dll";
        }
        if (Mutation == 5)
          Image->ImportStorageSlots.clear();
        if (Mutation == 6)
          Image->ConflictingImportStorageSlots.insert(Routine.Slot);
        if (Mutation == 7)
          Image->Bits = Bitness::Bits64;
        if (Mutation == 8)
          Image->Arch = Arch::ARM;
        if (Mutation == 9)
          Image->Format = BinaryFormat::ELF;
        if (Mutation == 10 || Mutation == 11) {
          auto *Bytes = const_cast<uint8_t *>(Image->readVA(Routine.Target, 6));
          ASSERT_TRUE(Bytes);
          if (Mutation == 10)
            Bytes[1] = 0x15; // CALL is not the JMP veneer.
          else
            ++Bytes[2]; // A neighbouring IAT byte is not this binding.
        }
        if (Mutation == 12) {
          for (auto &Segment : Image->Segments)
            if (Segment.isExecutable())
              Segment.Flags = Segment.Flags | SegmentFlags::Writable;
          for (auto &Section : Image->Sections)
            if (Section.isExecutable())
              Section.Flags = Section.Flags | SegmentFlags::Writable;
        }
        if (Mutation == 13)
          Imported->Module = "VCRUNTIME140D.dll";
        const auto Binding = checkedJumpRuntime(
            *Image, Indirect ? Routine.Slot : Routine.Target, Indirect);
        const bool Valid = Mutation == 0 || Mutation == 13 ||
                           (Indirect && Mutation >= 10 && Mutation <= 12);
        EXPECT_EQ(Binding.has_value(), Valid);
        if (Binding) {
          EXPECT_EQ(Binding->Routine, Routine.Kind);
          EXPECT_EQ(Binding->ImportSlot, Routine.Slot);
          EXPECT_EQ(Binding->Indirect, Indirect);
        }
      }
}
#endif
