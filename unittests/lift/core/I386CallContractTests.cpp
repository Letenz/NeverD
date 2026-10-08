//===- I386CallContractTests.cpp - i386 stack-passed call arguments -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// An i386 import thunk `jmp dword ptr [__imp__calloc]` reaches its import
// and passes on the arguments its caller left on the stack.
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/raw_ostream.h"

#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace neverd;

constexpr va_t Text = 0x401000;
constexpr va_t Iat = 0x403000;

/// The slot of import \p Index.
constexpr va_t slot(size_t Index) { return Iat + 4 * Index; }

/// A PE32 image with \p Code at Text, the function `_wrap` there, and the
/// msvcrt.dll imports \p Imports in consecutive slots at Iat.
BinaryImage makeImage(std::vector<uint8_t> Code,
                      const std::vector<const char *> &Imports) {
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x400000;
  Img.Entry = Text;
  Segment Seg;
  Seg.Name = ".text";
  Seg.VA = Text;
  Seg.Size = Code.size();
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data = std::move(Code);
  Img.Segments.push_back(std::move(Seg));
  Segment Idata;
  Idata.Name = ".idata";
  Idata.VA = Iat;
  Idata.Size = Idata.FileSz = 4 * Imports.size();
  Idata.Data.resize(Idata.Size);
  Idata.Flags = SegmentFlags::Readable;
  Img.Segments.push_back(std::move(Idata));
  for (size_t I = 0; I < Imports.size(); ++I) {
    Import Imp;
    Imp.Module = "msvcrt.dll";
    Imp.Name = Imports[I];
    Imp.IATAddr = slot(I);
    Img.Imports.push_back(std::move(Imp));
  }
  Symbol Function = Symbol::makeFunc(Text);
  Function.Name = "_wrap";
  Img.Symbols.push_back(std::move(Function));
  return Img;
}

/// The call of \p Callee in the HighC of `wrap` in \p Img: the rest of its
/// line from the callee's name.
std::string callIn(const BinaryImage &Img, const std::string &Callee) {
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = {Text};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  EXPECT_TRUE(Result.Success) << Result.Error;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  EXPECT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));
  const size_t Body = Source.find(" wrap(");
  const size_t At = Source.find(Callee + "(", Body);
  if (Body == std::string::npos || At == std::string::npos) {
    ADD_FAILURE() << Source;
    return {};
  }
  return Source.substr(At, Source.find('\n', At) - At);
}

TEST(I386CallContract, AThunkJumpingThroughTheSlotForwardsItsArguments) {
  // `jmp dword ptr [__imp__calloc]`: the thunk calls calloc, not the value
  // it would load from an address, with the two arguments its caller left
  // on the stack.
  std::vector<uint8_t> Code = {0xFF,
                               0x25, // jmp dword ptr [slot]
                               static_cast<uint8_t>(slot(0)),
                               static_cast<uint8_t>(slot(0) >> 8),
                               static_cast<uint8_t>(slot(0) >> 16),
                               static_cast<uint8_t>(slot(0) >> 24)};
  const std::string Call = callIn(makeImage(Code, {"calloc"}), "calloc");
  EXPECT_NE(Call.find("arg0"), std::string::npos) << Call;
  EXPECT_NE(Call.find("arg1"), std::string::npos) << Call;
  EXPECT_EQ(Call.find("unknown value"), std::string::npos) << Call;
}

} // namespace
