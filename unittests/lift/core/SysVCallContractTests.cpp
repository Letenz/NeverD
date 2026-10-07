//===- SysVCallContractTests.cpp - System V call arguments and returns ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The shapes of QtXml's QDomNode::save and QDomNode::isDocument (x86-64
// System V): a method passes its incoming `this` on in RDI without writing it,
// a virtual call takes the object a dominating block loaded into RDI, and a
// void method returns without writing RAX on one path while it tail-calls on
// the others.
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <vector>

namespace {
using namespace neverd;

constexpr va_t Text = 0x401000;
constexpr va_t IsDoc = Text;
constexpr va_t Save = Text + 0x40;
constexpr va_t Helper = Text + 0x80;
constexpr va_t Helper2 = Text + 0xA0;

void put(std::vector<uint8_t> &Code, va_t At, std::vector<uint8_t> Bytes) {
  std::copy(Bytes.begin(), Bytes.end(), Code.begin() + (At - Text));
}

uint8_t rel8(va_t From, va_t To) {
  return static_cast<uint8_t>(static_cast<int8_t>(To - From));
}

std::vector<uint8_t> rel32(va_t Next, va_t To) {
  const uint32_t Delta = static_cast<uint32_t>(To - Next);
  return {static_cast<uint8_t>(Delta), static_cast<uint8_t>(Delta >> 8),
          static_cast<uint8_t>(Delta >> 16), static_cast<uint8_t>(Delta >> 24)};
}

BinaryImage makeImage() {
  std::vector<uint8_t> Code(0xC0, 0xCC);
  // is_doc(node): rdi = node->impl; return impl && impl->vtbl[11](impl) == 9.
  put(Code, IsDoc, {0x48, 0x8B,
                    0x3F, // mov rdi, [rdi]
                    0x48, 0x85,
                    0xFF,                              // test rdi, rdi
                    0x74, rel8(IsDoc + 8, IsDoc + 29), // je none
                    0x48, 0x83,
                    0xEC, 0x08, // sub rsp, 8
                    0x48, 0x8B,
                    0x07, // mov rax, [rdi]
                    0xFF, 0x50,
                    0x58, // call [rax+0x58]
                    0x83, 0xF8,
                    0x09, // cmp eax, 9
                    0x0F, 0x94,
                    0xC0, // sete al
                    0x48, 0x83,
                    0xC4, 0x08, // add rsp, 8
                    0xC3,       // ret
                    0x31, 0xC0, // none: xor eax, eax
                    0xC3});     // ret
  // save(node): if (!node->impl) return; tail-call helper or helper2 with
  // node->impl after is_doc(node), which receives node untouched in RDI.
  std::vector<uint8_t> SaveCode = {0x48, 0x83,
                                   0x3F, 0x00, // cmp qword [rdi], 0
                                   0x74, rel8(Save + 6, Save + 37), // je done
                                   0x53,                            // push rbx
                                   0x48, 0x89,
                                   0xFB,  // mov rbx, rdi
                                   0xE8}; // call is_doc
  for (uint8_t B : rel32(Save + 15, IsDoc))
    SaveCode.push_back(B);
  for (uint8_t B : std::vector<uint8_t>{0x84, 0xC0, // test al, al
                                        0x74, rel8(Save + 19, Save + 28), 0x48,
                                        0x8B, 0x3B, // mov rdi, [rbx]
                                        0x5B,       // pop rbx
                                        0xE9})      // jmp helper
    SaveCode.push_back(B);
  for (uint8_t B : rel32(Save + 28, Helper))
    SaveCode.push_back(B);
  for (uint8_t B :
       std::vector<uint8_t>{0x48, 0x8B, 0x3B, // other: mov rdi,[rbx]
                            0x5B,             // pop rbx
                            0xE9})            // jmp helper2
    SaveCode.push_back(B);
  for (uint8_t B : rel32(Save + 37, Helper2))
    SaveCode.push_back(B);
  SaveCode.push_back(0xC3); // done: ret, RAX never written
  put(Code, Save, SaveCode);
  // helper(impl): impl->field8 = 1; helper2 stores 2.
  put(Code, Helper, {0xC7, 0x47, 0x08, 0x01, 0x00, 0x00, 0x00, 0xC3});
  put(Code, Helper2, {0xC7, 0x47, 0x08, 0x02, 0x00, 0x00, 0x00, 0xC3});

  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::ELF;
  Img.Base = 0x400000;
  Img.Entry = Save;
  Segment Seg;
  Seg.Name = ".text";
  Seg.VA = Text;
  Seg.Size = Code.size();
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data = std::move(Code);
  Img.Segments.push_back(std::move(Seg));
  const std::pair<va_t, uint64_t> Funcs[] = {
      {IsDoc, 32}, {Save, 38}, {Helper, 8}, {Helper2, 8}};
  const char *Names[] = {"is_doc", "save", "helper", "helper2"};
  for (unsigned I = 0; I < 4; ++I) {
    Symbol Function = Symbol::makeFunc(Funcs[I].first, Funcs[I].second);
    Function.Name = Names[I];
    Img.Symbols.push_back(std::move(Function));
    Img.KnownCodeRanges.push_back(
        {Funcs[I].first, Funcs[I].first + Funcs[I].second});
  }
  return Img;
}

std::string lift() {
  BinaryImage Img = makeImage();
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  for (va_t Entry : {IsDoc, Save, Helper, Helper2})
    Opts.OnlyFunctionEntries.insert(Entry);
  auto Result = Pipeline().run(Img, Ctx, Opts);
  EXPECT_TRUE(Result.Success) << Result.Error;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  EXPECT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));
  return Source;
}

/// The text of function \p Name's definition.
std::string body(const std::string &Source, const std::string &Name) {
  const size_t At = Source.find(" " + Name + "(");
  if (At == std::string::npos)
    return {};
  const size_t Open = Source.find('{', At);
  const size_t Close = Source.find("\n}", Open);
  if (Open == std::string::npos || Close == std::string::npos)
    return {};
  return Source.substr(Source.rfind('\n', At) + 1,
                       Close + 2 - (Source.rfind('\n', At) + 1));
}

void compileAndRun(const std::string &Source) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  const auto Program = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Program)) << "clang is required for emitted C execution";
  const std::string Compiler = *Program;
#endif
  llvm::SmallString<128> SourcePath, BinaryPath, ErrorPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-sysv-call", "c", SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-sysv-call", "exe",
                                                  BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-sysv-call", "err", ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code EC;
  {
    llvm::raw_fd_ostream Out(SourcePath, EC);
    ASSERT_FALSE(EC);
    Out << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  for (const char *Optimization : {"-O0", "-O2"}) {
    const llvm::SmallVector<llvm::StringRef, 12> Arguments{
        Compiler,
        "-std=c11",
        Optimization,
        "-fsanitize=undefined,address",
        "-Werror=uninitialized",
        "-Werror=return-type",
        SourcePath,
        "-o",
        BinaryPath};
    std::string Error;
    int Result = llvm::sys::ExecuteAndWait(Compiler, Arguments, std::nullopt,
                                           Redirects, 60, 0, &Error);
    auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Result, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "") << '\n'
                         << Source;
    Result = llvm::sys::ExecuteAndWait(BinaryPath, {BinaryPath}, std::nullopt,
                                       Redirects, 30, 0, &Error);
    Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Result, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "") << '\n'
                         << Source;
  }
}

TEST(SysVCallContract, PassThroughThisAndUndefinedReturnPathsStayFaithful) {
  const std::string Source = lift();
  const std::string SaveBody = body(Source, "save");
  const std::string IsDocBody = body(Source, "is_doc");
  ASSERT_FALSE(SaveBody.empty()) << Source;
  ASSERT_FALSE(IsDocBody.empty()) << Source;
  // A path that leaves RAX as the caller left it makes save void, and its
  // tail calls still run.
  EXPECT_EQ(SaveBody.rfind("void save(", 0), 0u) << SaveBody;
  EXPECT_EQ(SaveBody.find("unknown return value"), std::string::npos)
      << SaveBody;
  EXPECT_NE(SaveBody.find("helper("), std::string::npos) << SaveBody;
  EXPECT_NE(SaveBody.find("helper2("), std::string::npos) << SaveBody;
  // is_doc reads the RDI save never wrote: the call passes save's own node.
  EXPECT_EQ(SaveBody.find("is_doc()"), std::string::npos) << SaveBody;
  EXPECT_NE(SaveBody.find("is_doc("), std::string::npos) << SaveBody;
  // The virtual call takes the object the dominating block loaded into RDI.
  EXPECT_EQ(IsDocBody.find("+ 88)))()"), std::string::npos) << IsDocBody;
  compileAndRun(Source + R"(
static uint64_t document_type(void *self) { return self ? 9 : 0; }
static uint64_t element_type(void *self) { return self ? 1 : 0; }
struct impl {
  void **vtbl;
  int32_t field8;
};
int main(void) {
  void *document_vtbl[12] = {0}, *element_vtbl[12] = {0};
  document_vtbl[11] = (void *)document_type;
  element_vtbl[11] = (void *)element_type;
  struct impl document = {document_vtbl, 0}, element = {element_vtbl, 0};
  struct impl *documentNode = &document, *elementNode = &element, *null = 0;
  save(&documentNode);
  save(&elementNode);
  save(&null);
  return document.field8 == 1 && element.field8 == 2 ? 0 : 1;
}
)");
}

TEST(SysVCallContract, ALowByteWriteIsTheReturnValueOnEveryPath) {
  // is_small(c): al = 0, set to 1 only for 1 <= c <= 4.  A comparison chain
  // reaches one shared ret, where AL merges, and one fall-through ret that
  // returns the cleared AL directly.  RAX above AL is the caller's.
  constexpr va_t IsZero = 0x401000;
  std::vector<uint8_t> Code = {0x30, 0xC0,             // xor al, al
                               0x40, 0x84, 0xFF,       // test dil, dil
                               0x74, 0x25,             // je done
                               0x40, 0x80, 0xFF, 0x04, // cmp dil, 4
                               0x76, 0x1D,             // jbe set
                               0x40, 0x80, 0xFF, 0x10, // cmp dil, 0x10
                               0x74, 0x19,             // je done
                               0x40, 0x80, 0xFF, 0x18, // cmp dil, 0x18
                               0x74, 0x13,             // je done
                               0x40, 0x80, 0xFF, 0x4F, // cmp dil, 0x4f
                               0x76, 0x0D,             // jbe done
                               0x40, 0x80, 0xFF, 0x51, // cmp dil, 0x51
                               0x76, 0x07,             // jbe done
                               0x40, 0x80, 0xC7, 0x08, // add dil, 8
                               0xC3,                   // ret
                               0xB0, 0x01,             // set: mov al, 1
                               0xC3};                  // done: ret
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::ELF;
  Img.Base = 0x400000;
  Img.Entry = IsZero;
  Segment Seg;
  Seg.Name = ".text";
  Seg.VA = IsZero;
  Seg.Size = Code.size();
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data = Code;
  Img.Segments.push_back(std::move(Seg));
  Symbol Function = Symbol::makeFunc(IsZero, Code.size());
  Function.Name = "is_zero";
  Img.Symbols.push_back(std::move(Function));
  Img.KnownCodeRanges.push_back({IsZero, IsZero + Code.size()});
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries.insert(IsZero);
  auto Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));
  EXPECT_EQ(Source.find("unknown return value"), std::string::npos) << Source;
  compileAndRun(Source + R"(
int main(void) {
  for (int c = 0; c < 256; ++c)
    if ((uint8_t)is_zero(c) != (c >= 1 && c <= 4))
      return 1;
  return 0;
}
)");
}

} // namespace
