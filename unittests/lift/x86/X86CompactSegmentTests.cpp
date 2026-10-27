//===- X86CompactSegmentTests.cpp - i386 compact-memory segments --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep CS/ES/SS offsets and masked accesses separate from process pointers.
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/render/X86/X86CompressExpand.h"
#include "../x86_64/X86FPStateAccuracyFixture.h"

#include "neverd/ir/low/X86/X86CompactShape.h"

#include "llvm/Object/ObjectFile.h"

#include <array>

namespace {
TEST(X86CompactSegment, EverySegmentRetainsItsOffsetAcrossDecodeRoutes) {
  unsigned Checks = 0;
  for (unsigned Kind = 0; Kind < 6; ++Kind)
    for (unsigned Width = 0; Width < 3; ++Width)
      for (unsigned Mode = 0; Mode < 3; ++Mode)
        for (uint8_t Prefix : {0, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65})
          for (bool Narrow : {false, true})
            for (unsigned Base = 0; Base < 3; ++Base)
              for (bool Strict : {false, true}) {
                const uint8_t Opcode = Kind < 2   ? (Mode ? 0x62 : 0x63)
                                       : Kind < 4 ? (Mode ? 0x89 : 0x8b)
                                                  : (Mode ? 0x88 : 0x8a);
                std::vector<uint8_t> Bytes;
                if (Prefix)
                  Bytes.push_back(Prefix);
                if (Narrow)
                  Bytes.push_back(0x67);
                Bytes.insert(
                    Bytes.end(),
                    {0x62, 0xf2, uint8_t(Kind & 1 ? 0xfd : 0x7d),
                     uint8_t(0x0a | (Width << 5) | (Mode == 2 ? 0x80 : 0)),
                     Opcode});
                if (Narrow) {
                  // BX+SI, BP+SI, BP+disp8(0).
                  Bytes.push_back(Base == 0 ? 0x08 : Base == 1 ? 0x0a : 0x4e);
                  if (Base == 2)
                    Bytes.push_back(0);
                } else {
                  // EAX, ESP, EBP+disp8(0).
                  Bytes.push_back(Base == 0 ? 0x08 : Base == 1 ? 0x0c : 0x4d);
                  if (Base)
                    Bytes.push_back(Base == 1 ? 0x24 : 0);
                }
                const unsigned Expected =
                    Prefix == 0x26 || Prefix == 0x2e || Prefix == 0x36 ? Prefix
                    : !Prefix && Base                                  ? 0x36
                                                                       : 0;
                Decoder D;
                ASSERT_TRUE(D.init(Arch::X86));
                D.setStrict(Strict);
                for (unsigned Route = 0; Route < 3; ++Route) {
                  SCOPED_TRACE(testing::PrintToString(Bytes));
                  DecodedInsn I{};
                  const auto Size =
                      Route == 0
                          ? D.decodeOne(Bytes.data(), Bytes.size(), 0x1000, I)
                      : Route == 1 ? D.decodeOneLight(Bytes.data(),
                                                      Bytes.size(), 0x1000, I)
                                   : D.decodeOneForLift(
                                         Bytes.data(), Bytes.size(), 0x1000, I);
                  ASSERT_EQ(Size, Bytes.size());
                  std::vector<LowOp> Ops;
                  ASSERT_NO_THROW(D.liftToLow(I, Ops));
                  const auto It =
                      std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
                        return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
                               isX86CompactMemory(
                                   Intrinsic(Op.Inputs[0].Offset));
                      });
                  ASSERT_NE(It, Ops.end());
                  const auto C = x86CompactControl(
                      Intrinsic(It->Inputs[0].Offset), It->Inputs[4].Offset);
                  ASSERT_TRUE(C);
                  EXPECT_EQ(C->SegmentPrefix, Expected);
                  EXPECT_FALSE(C->StackReference);
                  EXPECT_EQ(It->Inputs[4].Size, Expected ? 2 : 1);
                  EXPECT_TRUE(x86CompactShapeIsValid(
                      Intrinsic(It->Inputs[0].Offset),
                      x86CompactLowShape(*It, Arch::X86)));
                  ++Checks;
                }
              }
  EXPECT_EQ(Checks, 13608u);
  RecordProperty("decoder_lift_checks", Checks);
}

LowOp compact(bool Expand, bool Zero, unsigned Prefix) {
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  if (Expand)
    Op.Output = NdVar::tmp(900, 16);
  Op.addInput(NdVar::cst(unsigned(Expand ? Intrinsic::EVEXExpandLoad
                                         : Intrinsic::EVEXCompressStore),
                         2));
  Op.addInput(NdVar::tmp(901, 8));
  Op.addInput(NdVar::cst(0, 8));
  Op.addInput(NdVar::tmp(902, 16));
  Op.addInput(NdVar::scalar(
      4 | (Zero ? X86CompactZero : 0) | (Prefix << X86CompactSegmentShift), 2));
  return Op;
}

TEST(X86CompactSegment, ExtendedControlsCannotAcquireAnotherModeOrSegment) {
  for (unsigned Prefix : {0x26u, 0x2eu, 0x36u}) {
    auto Op = compact(true, false, Prefix);
    const auto Id = Intrinsic::EVEXExpandLoad;
    EXPECT_TRUE(x86CompactShapeIsValid(Id, x86CompactLowShape(Op, Arch::X86)));
    EXPECT_FALSE(x86CompactShapeIsValid(Id, x86CompactLowShape(Op, Arch::X64)));
    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      auto Bad = Op;
      switch (Mutation) {
      case 0:
        Bad.Inputs[4].Size = 1;
        break;
      case 1:
        Bad.Inputs[4].Offset |= X86CompactStackReference;
        break;
      case 2:
        Bad.Inputs[4].Offset = 4;
        break;
      case 3:
        Bad.Inputs[4].Offset = 0x6404;
        break;
      case 4:
        Bad.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
        break;
      case 5:
        Bad.Inputs[4].Provenance = ConstantAddressProvenance::Address;
        break;
      }
      EXPECT_FALSE(
          x86CompactShapeIsValid(Id, x86CompactLowShape(Bad, Arch::X86)));
    }
  }
}

TEST(X86CompactSegment, ConcreteAccessNeedsDescriptorsOnlyForActiveLanes) {
  for (unsigned Prefix : {0x26u, 0x2eu, 0x36u})
    for (unsigned Mode = 0; Mode < 3; ++Mode)
      for (unsigned Mask : {0u, 1u}) {
        auto Op = compact(Mode != 0, Mode == 2, Prefix);
        Op.Inputs[2] = NdVar::cst(Mask, 8);
        BinaryImage Image;
        Image.Arch = Arch::X86;
        Segment Data;
        Data.VA = 0x4000;
        Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
        Data.Data.resize(64, 0x31);
        Data.Size = Data.FileSz = 64;
        Image.Segments.push_back(std::move(Data));
        NdOpEmulator E(Image);
        E.setStrictMode(true);
        E.setLoadCollect(true);
        E.setRegisterBytes(900, std::vector<uint8_t>(16, 0xa5));
        if (Mode == 1 || Mask)
          E.setRegisterBytes(902, std::vector<uint8_t>(16, 0x17));
        if (Mask)
          E.setRegister(901, 0x4000);
        EXPECT_EQ(E.step(Op), Mask == 0);
        EXPECT_TRUE(E.getLoadRecords().empty());
        const auto Result = E.getRegisterBytes(900);
        ASSERT_TRUE(Result);
        EXPECT_EQ(*Result, std::vector<uint8_t>(16, Mask || !Mode ? 0xa5
                                                    : Mode == 1   ? 0x17
                                                                  : 0));
      }
}

class X86CompactSegmentSource : public X86FPStateFixture {
protected:
  void checkObject(llvm::StringRef Data, unsigned Prefix, bool Expand) {
    auto Obj = llvm::object::ObjectFile::createObjectFile(
        llvm::MemoryBufferRef(Data, "segment.o"));
    ASSERT_TRUE(bool(Obj)) << llvm::toString(Obj.takeError());
    Decoder D;
    ASSERT_TRUE(D.init(Arch::X86));
    unsigned Count = 0;
    for (const auto &Section : (*Obj)->sections()) {
      if (!Section.isText())
        continue;
      auto Text = Section.getContents();
      ASSERT_TRUE(bool(Text)) << llvm::toString(Text.takeError());
      for (size_t Offset = 0; Offset < Text->size();) {
        DecodedInsn I{};
        const auto Size = D.decodeOneForLift(
            reinterpret_cast<const uint8_t *>(Text->data() + Offset),
            Text->size() - Offset, Entry + Offset, I);
        ASSERT_GT(Size, 0);
        if (I.Id == (Expand ? X86_INS_VPEXPANDD : X86_INS_VPCOMPRESSD)) {
          ++Count;
          EXPECT_EQ(I.Raw->detail->x86.prefix[1], Prefix);
          bool Memory = false;
          for (unsigned N = 0; N < I.Raw->detail->x86.op_count; ++N)
            Memory |= I.Raw->detail->x86.operands[N].type == X86_OP_MEM;
          EXPECT_TRUE(Memory);
        }
        Offset += Size;
      }
    }
    EXPECT_EQ(Count, 1u);
  }
};

TEST_F(X86CompactSegmentSource, NativeAndBothCRoutesKeepSegmentsAndRawOffsets) {
  unsigned Objects = 0;
  for (auto Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
    for (uint8_t Prefix : {0x26, 0x2e, 0x36})
      for (unsigned Mode = 0; Mode < 3; ++Mode)
        for (bool NoOpt : {false, true}) {
          SCOPED_TRACE(testing::Message()
                       << unsigned(Format) << '/' << unsigned(Prefix) << '/'
                       << Mode << '/' << NoOpt);
          // Merge loads use a scalar stack parameter. Absolute stores/zeroing
          // loads deliberately alias a DS object which the last MOV returns.
          std::vector<uint8_t> Bytes = {
              0xba, 5, 0, 0, 0, 0xc5, 0xf8, 0x92, 0xd2, 0xc5, 0xf1, 0xef, 0xc9};
          if (Mode == 1)
            Bytes.insert(Bytes.end(), {0x8b, 0x44, 0x24, 4});
          Bytes.insert(Bytes.end(), {Prefix, 0x62, 0xf2, 0x7d,
                                     uint8_t(Mode == 2 ? 0x8a : 0x0a),
                                     uint8_t(Mode ? 0x89 : 0x8b),
                                     uint8_t(Mode == 1 ? 0x08 : 0x0d)});
          if (Mode != 1)
            Bytes.insert(Bytes.end(), {0, 0x40, 0, 0});
          Bytes.insert(Bytes.end(), {0xa1, 0, 0x40, 0, 0, 0xc3});
          BinaryImage Image;
          Image.Arch = Arch::X86;
          Image.Bits = Bitness::Bits32;
          Image.Format = Format;
          Image.Entry = Image.Base = Entry;
          Segment Text;
          Text.VA = Entry;
          Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
          Text.Data = Bytes;
          Text.Size = Text.FileSz = Bytes.size();
          Image.Segments.push_back(std::move(Text));
          Segment Data;
          Data.VA = 0x4000;
          Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
          Data.Data.resize(64, 0x31);
          Data.Size = Data.FileSz = 64;
          Image.Segments.push_back(std::move(Data));
          auto Symbol = Symbol::makeFunc(Entry, Bytes.size());
          Symbol.Name = "compact_segment";
          Image.Symbols.push_back(std::move(Symbol));
          for (unsigned Route = 0; Route < 3; ++Route) {
            llvm::LLVMContext Context;
            PipelineOptions Options;
            Options.LiftMode = Route != 1;
            Options.SourceProjection = Route == 2;
            Options.NoOpt = NoOpt;
            Options.EmitDumpOutput = false;
            Options.OnlyFunctionEntries = {Entry};
            auto Result = Pipeline().run(Image, Context, Options);
            ASSERT_TRUE(Result.Success) << Result.Error;
            if (Route == 0) {
              ASSERT_NE(Result.LlvmModule, nullptr);
              unsigned Calls = 0;
              for (const auto &F : *Result.LlvmModule)
                for (const auto &Block : F)
                  for (const auto &I : Block)
                    if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&I))
                      if (const auto P = x86CompactCallPlan(Arch::X86, *Call)) {
                        ++Calls;
                        EXPECT_EQ(P->Prefix, Prefix);
                        if (Mode != 1) {
                          const auto *Pointer =
                              llvm::dyn_cast<llvm::ConstantExpr>(
                                  Call->getArgOperand(0));
                          ASSERT_NE(Pointer, nullptr);
                          ASSERT_EQ(Pointer->getOpcode(),
                                    llvm::Instruction::IntToPtr);
                          const auto *Offset =
                              llvm::dyn_cast<llvm::ConstantInt>(
                                  Pointer->getOperand(0));
                          ASSERT_NE(Offset, nullptr);
                          EXPECT_EQ(Offset->getZExtValue(), 0x4000u);
                        }
                      }
              EXPECT_EQ(Calls, 1u);
              auto Object =
                  Codegen().compile(*Result.LlvmModule, Arch::X86, Format);
              ASSERT_TRUE(Object.Success);
              const llvm::StringRef Raw(
                  reinterpret_cast<const char *>(Object.ObjectData.data()),
                  Object.ObjectData.size());
              checkObject(Raw, Prefix, Mode != 0);
              retain("native.o", Raw);
              std::string IR;
              llvm::raw_string_ostream OS(IR);
              Result.LlvmModule->print(OS, nullptr);
              retain("native.ll", IR);
              ++Objects;
              continue;
            }
            CEmitterOptions Emit;
            Emit.TheArch = Arch::X86;
            Emit.Format = Format;
            Emit.Image = &Image;
            Emit.PreserveLLVMFunctionTypes = true;
            std::string Source;
            llvm::raw_string_ostream OS(Source);
            ASSERT_TRUE(Route == 1
                            ? HighCEmitter().emit(Result.HighFuncs, OS, Emit)
                            : LLVMCEmitter().emit(*Result.LlvmModule, OS, Emit,
                                                  nullptr, &Image));
            const std::string Include = "#include <string.h>";
            if (auto At = Source.find(Include); At != std::string::npos)
              Source.replace(At, Include.size(),
                             "extern void *memcpy(void *, const void *, "
                             "__SIZE_TYPE__);\nextern void *memset(void *, "
                             "int, __SIZE_TYPE__);");
            const auto Stem = Route == 1 ? "high" : "llvm-c";
            const auto Input = file(std::string(Stem) + ".c");
            write(Input, Source);
            retain(std::string(Stem) + ".c", Source);
            const auto Triple = Format == BinaryFormat::ELF ? "i686-linux-gnu"
                                : Format == BinaryFormat::COFF
                                    ? "i686-pc-windows-msvc"
                                    : "i686-apple-darwin";
            for (const auto *Opt : {"-O0", "-O2"}) {
              const auto Output = file(std::string(Stem) + Opt + ".o");
              auto Built = command(Compiler, {std::string("--target=") + Triple,
                                              "-std=gnu23", "-ffreestanding",
                                              Opt, "-c", Input, "-o", Output});
              ASSERT_EQ(Built.Status, 0) << Built.Error << Source;
              checkObject(contents(Output), Prefix, Mode != 0);
              retainFile(std::string(Stem) + Opt + ".o", Output);
              ++Objects;
            }
          }
          ++ProbeNumber;
        }
  EXPECT_EQ(Objects, 270u);
  RecordProperty("native_objects", Objects);
}
} // namespace
