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
// the others.  Also xxd's: wrappers pass their arguments straight to a libc
// import, and end at a helper that never returns.
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdlib>
#include <set>
#include <string>
#include <utility>
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
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-sysv-call", Directory));
  const llvm::scope_exit Cleanup(
      [&] { llvm::sys::fs::remove_directories(Directory); });
  llvm::SmallString<128> SourcePath(Directory), BinaryPath(Directory),
      ErrorPath(Directory);
  llvm::sys::path::append(SourcePath, "program.c");
  llvm::sys::path::append(BinaryPath, "program.exe");
  llvm::sys::path::append(ErrorPath, "program.err");
  std::error_code EC;
  {
    llvm::raw_fd_ostream Out(SourcePath, EC);
    ASSERT_FALSE(EC);
    Out << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
#ifdef _WIN32
  // Retain both sanitizers. The static ASan runtime cannot intercept some
  // current UCRT math prologues; Clang's own dynamic runtime supports them.
  // Keep that DLL beside this test's executable, without changing PATH or
  // copying runtime files into another test's temporary directory.
  llvm::SmallString<128> RuntimePath(Directory);
  llvm::sys::path::append(RuntimePath, "runtime.txt");
  const std::optional<llvm::StringRef> RuntimeRedirects[] = {
      std::nullopt, RuntimePath.str(), ErrorPath.str()};
  ASSERT_EQ(
      llvm::sys::ExecuteAndWait(
          Compiler,
          {Compiler,
           "-print-file-name=lib/windows/clang_rt.asan_dynamic-x86_64.dll"},
          std::nullopt, RuntimeRedirects, 30),
      0);
  auto Runtime = llvm::MemoryBuffer::getFile(RuntimePath);
  ASSERT_TRUE(bool(Runtime));
  llvm::SmallString<128> RuntimeCopy(Directory);
  llvm::sys::path::append(RuntimeCopy, "clang_rt.asan_dynamic-x86_64.dll");
  ASSERT_FALSE(
      llvm::sys::fs::copy_file((*Runtime)->getBuffer().trim(), RuntimeCopy));
#endif
  for (const char *Optimization : {"-O0", "-O2"}) {
    llvm::SmallVector<llvm::StringRef, 12> Arguments{
        Compiler,
        "-std=c11",
        Optimization,
        "-fsanitize=undefined,address",
        "-Werror=uninitialized",
        "-Werror=return-type",
        SourcePath,
        "-o",
        BinaryPath};
#ifdef _WIN32
    Arguments.push_back("-shared-libasan");
#endif
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

/// An x86-64 ELF image with \p Code at Text, the given function symbols, and
/// the imports \p Imports bound to consecutive slots at 0x403000, each with
/// a PLT-style stub `jmp [slot]` at the address paired with it.
BinaryImage
makeImportImage(std::vector<uint8_t> Code,
                const std::vector<std::pair<va_t, const char *>> &Functions,
                const std::vector<std::pair<va_t, const char *>> &Imports) {
  constexpr va_t Got = 0x403000;
  for (size_t I = 0; I < Imports.size(); ++I) {
    const va_t Stub = Imports[I].first;
    std::vector<uint8_t> Jump = {0xFF, 0x25}; // jmp [rip + slot]
    for (uint8_t B : rel32(Stub + 6, Got + 8 * I))
      Jump.push_back(B);
    put(Code, Stub, Jump);
  }
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::ELF;
  Img.Base = 0x400000;
  Img.Entry = Text;
  Segment Seg;
  Seg.Name = ".text";
  Seg.VA = Text;
  Seg.Size = Code.size();
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data = std::move(Code);
  Img.Segments.push_back(std::move(Seg));
  if (!Imports.empty()) {
    Segment GotSeg;
    GotSeg.Name = ".got";
    GotSeg.VA = Got;
    GotSeg.Size = GotSeg.FileSz = 8 * Imports.size();
    GotSeg.Data.resize(GotSeg.Size);
    GotSeg.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Img.Segments.push_back(std::move(GotSeg));
  }
  for (size_t I = 0; I < Imports.size(); ++I) {
    Import Imp;
    Imp.Module = "libc.so.6";
    Imp.Name = Imports[I].second;
    Imp.IATAddr = Got + 8 * I;
    Img.Imports.push_back(std::move(Imp));
    EXPECT_TRUE(Img.recordImportStub(Imports[I].first, I));
  }
  for (const auto &[Entry, Name] : Functions) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    Img.Symbols.push_back(std::move(Function));
  }
  return Img;
}

/// HighC for the functions at \p Entries of \p Img.
std::string liftEntries(const BinaryImage &Img, std::set<va_t> Entries) {
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = std::move(Entries);
  auto Result = Pipeline().run(Img, Ctx, Opts);
  EXPECT_TRUE(Result.Success) << Result.Error;
  if (const auto *Root = std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR")) {
    llvm::SmallString<128> Directory(Root);
    llvm::sys::path::append(
        Directory,
        testing::UnitTest::GetInstance()->current_test_info()->name());
    EXPECT_FALSE(llvm::sys::fs::create_directories(Directory));
    llvm::SmallString<128> Path(Directory);
    llvm::sys::path::append(Path, "call-return-med.txt");
    std::error_code Error;
    llvm::raw_fd_ostream Dump(Path, Error);
    EXPECT_FALSE(Error);
    if (!Error)
      Pipeline::dumpMedIR(Result.MedFuncs, Dump);
  }
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  EXPECT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));
  return Source;
}

TEST(SysVCallContract, PassThroughArgumentsReachAPrototypedImport) {
  // wrap(s, f) returns fputs(s, f) + 1 and fwd(s, f) tail-jumps to fputs,
  // both without writing RDI or RSI.  The prototype says fputs reads both,
  // so they are the functions' own parameters, and the stub passes them on.
  constexpr va_t Wrap = Text, Fwd = Text + 0x20, Stub = Text + 0x40;
  std::vector<uint8_t> Code(0x50, 0xCC);
  std::vector<uint8_t> WrapCode = {0x48, 0x83, 0xEC, 0x08, // sub rsp, 8
                                   0xE8};                  // call fputs
  for (uint8_t B : rel32(Wrap + 9, Stub))
    WrapCode.push_back(B);
  for (uint8_t B : {0x48, 0x83, 0xC4, 0x08, // add rsp, 8
                    0x83, 0xC0, 0x01,       // add eax, 1
                    0xC3})                  // ret
    WrapCode.push_back(B);
  put(Code, Wrap, WrapCode);
  std::vector<uint8_t> FwdCode = {0xE9}; // jmp fputs
  for (uint8_t B : rel32(Fwd + 5, Stub))
    FwdCode.push_back(B);
  put(Code, Fwd, FwdCode);
  const BinaryImage Img = makeImportImage(
      Code, {{Wrap, "wrap"}, {Fwd, "fwd"}, {Stub, "fputs_stub"}},
      {{Stub, "fputs"}});
  const std::string Source = liftEntries(Img, {Wrap, Fwd, Stub});
  for (const char *Name : {"wrap", "fwd", "fputs_stub"}) {
    SCOPED_TRACE(Name);
    const std::string Body = body(Source, Name);
    ASSERT_FALSE(Body.empty()) << Source;
    EXPECT_NE(Body.find("fputs(arg0, arg1)"), std::string::npos) << Body;
    EXPECT_EQ(Body.find("unknown value"), std::string::npos) << Body;
  }
}

TEST(SysVCallContract, AFloatArgumentReachesTheCalleeThatReadsIt) {
  // twice(x) = x + x; quadruple(x) doubles twice(x), passing x on in xmm0
  // without touching it. The alignment `push rax` reads all of rax: it is no
  // variadic call's count of vector arguments.
  constexpr va_t Twice = Text, Quadruple = Text + 0x10;
  std::vector<uint8_t> Code(0x20, 0xCC);
  put(Code, Twice,
      {0xF2, 0x0F, 0x58, 0xC0,                 // addsd xmm0, xmm0
       0xC3});                                 // ret
  std::vector<uint8_t> QuadrupleCode = {0x50,  // push rax
                                        0xE8}; // call twice
  for (uint8_t B : rel32(Quadruple + 6, Twice))
    QuadrupleCode.push_back(B);
  for (uint8_t B : {0xF2, 0x0F, 0x58, 0xC0, // addsd xmm0, xmm0
                    0x59,                   // pop rcx
                    0xC3})                  // ret
    QuadrupleCode.push_back(B);
  put(Code, Quadruple, QuadrupleCode);
  const BinaryImage Img =
      makeImportImage(Code, {{Twice, "twice"}, {Quadruple, "quadruple"}}, {});
  const std::string Source = liftEntries(Img, {Twice, Quadruple});
  EXPECT_NE(Source.find("double twice(double arg0)"), std::string::npos)
      << Source;
  const std::string Body = body(Source, "quadruple");
  ASSERT_FALSE(Body.empty()) << Source;
  EXPECT_NE(Body.find("double quadruple(double arg0)"), std::string::npos)
      << Body;
  EXPECT_NE(Body.find("twice(arg0)"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("unknown"), std::string::npos) << Body;
  compileAndRun(Source + R"(
int main(void) {
  return quadruple(1.5) == 6.0 && quadruple(-0.25) == -1.0 ? 0 : 1;
}
)");
}

TEST(SysVCallContract, AFloatArgumentReachesAPrototypedImport) {
  // twice_sin(x) = 2 * sin(x + x): math.h declares sin's double parameter,
  // so the sum in xmm0 is its argument and x the function's own parameter.
  constexpr va_t TwiceSin = Text, Stub = Text + 0x20;
  std::vector<uint8_t> Code(0x30, 0xCC);
  std::vector<uint8_t> TwiceSinCode = {0x48, 0x83, 0xEC, 0x08, // sub rsp, 8
                                       0xF2, 0x0F, 0x58,
                                       0xC0,  // addsd xmm0, xmm0
                                       0xE8}; // call sin
  for (uint8_t B : rel32(TwiceSin + 13, Stub))
    TwiceSinCode.push_back(B);
  for (uint8_t B : {0xF2, 0x0F, 0x58, 0xC0, // addsd xmm0, xmm0
                    0x48, 0x83, 0xC4, 0x08, // add rsp, 8
                    0xC3})                  // ret
    TwiceSinCode.push_back(B);
  put(Code, TwiceSin, TwiceSinCode);
  const BinaryImage Img = makeImportImage(
      Code, {{TwiceSin, "twice_sin"}, {Stub, "sin_stub"}}, {{Stub, "sin"}});
  const std::string Source = liftEntries(Img, {TwiceSin, Stub});
  const std::string Body = body(Source, "twice_sin");
  ASSERT_FALSE(Body.empty());
  EXPECT_NE(Body.find("double twice_sin(double arg0)"), std::string::npos)
      << Body;
  EXPECT_NE(Body.find("sin("), std::string::npos) << Body;
  EXPECT_EQ(Body.find("unknown"), std::string::npos) << Body;
  // A declared double import must receive the sum and its result must feed
  // the second operation. Execute a known independent implementation so
  // explicit FP state temporaries cannot hide a stale ABI value.
  compileAndRun("#include <math.h>\n#define sin neverd_test_sine\n"
                "double sin(double);\n" +
                Source + R"(
double sin(double value) {
  _mm_setcsr(0x3f81);
  return value * value;
}
int main(void) {
  uint32_t saved = _mm_getcsr();
  _mm_setcsr(0x1f80);
  double first = twice_sin(1.5);
  uint32_t first_state = _mm_getcsr();
  _mm_setcsr(0x1f80);
  double second = twice_sin(-0.25);
  uint32_t second_state = _mm_getcsr();
  _mm_setcsr(saved);
  return first == 18.0 && second == 0.5 &&
         first_state == 0x3f81 && second_state == 0x3f81 ? 0 : 1;
}
)");
}

TEST(SysVCallContract, AScalarMathImportWithAnIntegerArgumentKeepsItsReturn) {
  constexpr va_t Wrap = Text, Stub = Text + 0x20;
  std::vector<uint8_t> Code(0x30, 0xCC);
  std::vector<uint8_t> WrapCode = {0x48, 0x83, 0xEC, 0x08,       // sub rsp, 8
                                   0xBF, 0x02, 0x00, 0x00, 0x00, // mov edi, 2
                                   0xE8};                        // call ldexp
  for (uint8_t B : rel32(Wrap + WrapCode.size() + 4, Stub))
    WrapCode.push_back(B);
  for (uint8_t B : {0xF2, 0x0F, 0x58, 0xC0, // addsd xmm0, xmm0
                    0x48, 0x83, 0xC4, 0x08, 0xC3})
    WrapCode.push_back(B);
  put(Code, Wrap, WrapCode);
  const BinaryImage Img =
      makeImportImage(Code, {{Wrap, "twice_ldexp"}}, {{Stub, "ldexp"}});
  const std::string Source = liftEntries(Img, {Wrap});
  compileAndRun("#include <math.h>\n#define ldexp neverd_test_ldexp\n"
                "double ldexp(double, int);\n" +
                Source + R"(
double ldexp(double value, int exponent) {
  return exponent == 2 ? value * 4.0 : -100.0;
}
int main(void) {
  return twice_ldexp(1.5) == 12.0 && twice_ldexp(-0.25) == -2.0 ? 0 : 1;
}
)");
}

TEST(SysVCallContract, ALocalMathNameDoesNotOverrideMixedReturnEvidence) {
  for (const auto &[Name, Imported] : {std::pair{"sin", false},
                                       {"custom", true},
                                       {"cpow", true},
                                       {"sinl", true}}) {
    SCOPED_TRACE(Name);
    constexpr va_t Stub = Text + 0x20;
    BinaryImage Img = makeImportImage(
        std::vector<uint8_t>(0x30, 0xCC), {{Stub, Name}},
        Imported ? std::vector<std::pair<va_t, const char *>>{{Stub, Name}}
                 : std::vector<std::pair<va_t, const char *>>{});
    LowFunc Low;
    Low.Entry = Text;
    Low.Name = "mixed_caller";
    LowBlock Block;
    Block.Id = 0;
    Block.StartAddr = Text;
    Block.EndAddr = Text + 16;
    LowOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Output = NdVar::reg(x86reg::RAX, 8);
    Call.addInput(NdVar::cst(Stub, 8));
    Call.Addr = Text;
    Block.Ops.push_back(Call);
    for (const auto &[Address, Register] :
         {std::pair<uint64_t, uint64_t>{0x404000, x86reg::RAX},
          {0x404008, x86reg::XMM0}}) {
      LowOp Store;
      Store.Opcode = NdOp::STORE;
      Store.addInput(NdVar::cst(Address, 8));
      Store.addInput(NdVar::reg(Register, 8));
      Store.Addr = Text + 4;
      Block.Ops.push_back(Store);
    }
    LowOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.addInput(NdVar::reg(x86reg::RAX, 8));
    Return.Addr = Text + 12;
    Block.Ops.push_back(Return);
    Low.Blocks.push_back(Block);
    LowToMedConverter Converter;
    Converter.setBinaryImage(&Img);
    const auto Med = Converter.convert(Low, Arch::X64);
    unsigned Calls = 0;
    for (const auto &B : Med.Blocks)
      for (const auto &Op : B.Ops)
        if (Op.Opcode == NdOp::CALL) {
          ++Calls;
          EXPECT_EQ(Op.Output.Kind, MedVar::Temp);
          EXPECT_EQ(Op.Output.Size, 16);
        }
    EXPECT_EQ(Calls, 1U);
  }
}

TEST(SysVCallContract, ACallerReturnsTheFloatItsImportReturns) {
  // sin_twice(x) = sin(x + x): the result sin leaves in xmm0 is the one
  // sin_twice returns, though no instruction of its own writes it.
  constexpr va_t SinTwice = Text, Stub = Text + 0x20;
  std::vector<uint8_t> Code(0x30, 0xCC);
  std::vector<uint8_t> SinTwiceCode = {0x48, 0x83, 0xEC, 0x08, // sub rsp, 8
                                       0xF2, 0x0F, 0x58,
                                       0xC0,  // addsd xmm0, xmm0
                                       0xE8}; // call sin
  for (uint8_t B : rel32(SinTwice + 13, Stub))
    SinTwiceCode.push_back(B);
  for (uint8_t B : {0x48, 0x83, 0xC4, 0x08, // add rsp, 8
                    0xC3})                  // ret
    SinTwiceCode.push_back(B);
  put(Code, SinTwice, SinTwiceCode);
  const BinaryImage Img = makeImportImage(
      Code, {{SinTwice, "sin_twice"}, {Stub, "sin_stub"}}, {{Stub, "sin"}});
  const std::string Body =
      body(liftEntries(Img, {SinTwice, Stub}), "sin_twice");
  ASSERT_FALSE(Body.empty());
  EXPECT_NE(Body.find("double sin_twice(double arg0)"), std::string::npos)
      << Body;
  EXPECT_NE(Body.find("return sin(arg0 + arg0);"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("unknown"), std::string::npos) << Body;
}

TEST(SysVCallContract, AForwarderReturnsTheFloatItsCalleeReturns) {
  // forward(x) = twice(x) passes x on and returns twice's result in xmm0,
  // writing neither itself.
  constexpr va_t Twice = Text, Forward = Text + 0x10;
  std::vector<uint8_t> Code(0x20, 0xCC);
  put(Code, Twice,
      {0xF2, 0x0F, 0x58, 0xC0,               // addsd xmm0, xmm0
       0xC3});                               // ret
  std::vector<uint8_t> ForwardCode = {0x50,  // push rax
                                      0xE8}; // call twice
  for (uint8_t B : rel32(Forward + 6, Twice))
    ForwardCode.push_back(B);
  for (uint8_t B : {0x59,  // pop rcx
                    0xC3}) // ret
    ForwardCode.push_back(B);
  put(Code, Forward, ForwardCode);
  const BinaryImage Img =
      makeImportImage(Code, {{Twice, "twice"}, {Forward, "forward"}}, {});
  const std::string Source = liftEntries(Img, {Twice, Forward});
  const std::string Body = body(Source, "forward");
  ASSERT_FALSE(Body.empty()) << Source;
  EXPECT_NE(Body.find("double forward(double arg0)"), std::string::npos)
      << Body;
  EXPECT_NE(Body.find("twice(arg0)"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("unknown"), std::string::npos) << Body;
  compileAndRun(Source + R"(
int main(void) {
  return forward(1.5) == 3.0 && forward(-0.25) == -0.5 ? 0 : 1;
}
)");
}

TEST(SysVCallContract, AVariadicImportsFixedArgumentsReachItFromAJoin) {
  // wrap(flag) picks the format in either arm of an `if` and calls
  // __fprintf_chk(0, 2, fmt) after the join: the format reaches the call as
  // the value of EDX on both paths, not as a value the call site never set.
  constexpr va_t Wrap = Text, Stub = Text + 0x40;
  std::vector<uint8_t> Code(0x50, 0xCC);
  std::vector<uint8_t> WrapCode = {
      0x48, 0x83, 0xEC, 0x08,       // sub rsp, 8
      0x85, 0xFF,                   // test edi, edi
      0x74, 0x07,                   // je L1
      0xBA, 0x11, 0x11, 0x00, 0x00, // mov edx, 0x1111
      0xEB, 0x05,                   // jmp L2
      0xBA, 0x22, 0x22, 0x00, 0x00, // L1: mov edx, 0x2222
      0xBE, 0x02, 0x00, 0x00, 0x00, // L2: mov esi, 2
      0x31, 0xFF,                   // xor edi, edi
      0x31, 0xC0,                   // xor eax, eax
      0xE8};                        // call __fprintf_chk
  for (uint8_t B : rel32(Wrap + WrapCode.size() + 4, Stub))
    WrapCode.push_back(B);
  for (uint8_t B : {0x48, 0x83, 0xC4, 0x08, // add rsp, 8
                    0xC3})                  // ret
    WrapCode.push_back(B);
  put(Code, Wrap, WrapCode);
  const BinaryImage Img =
      makeImportImage(Code, {{Wrap, "wrap"}}, {{Stub, "__fprintf_chk"}});
  const std::string Source = liftEntries(Img, {Wrap});
  const std::string Body = body(Source, "wrap");
  ASSERT_FALSE(Body.empty()) << Source;
  EXPECT_NE(Body.find("__fprintf_chk("), std::string::npos) << Body;
  EXPECT_EQ(Body.find("unknown value"), std::string::npos) << Body;
}

TEST(SysVCallContract, AJumpThroughALoadedSlotPassesTheArgumentsSetBefore) {
  // GCC's register_tm_clones: RDI and RSI are set and tested, RAX is
  // loaded from the GOT slot of _ITM_registerTMCloneTable and tested, and
  // `jmp rax` alone in its block passes the arguments set two blocks up.
  constexpr va_t Wrap = Text, Slot = 0x403000;
  std::vector<uint8_t> Code(0x60, 0xCC);
  std::vector<uint8_t> WrapCode = {0x48, 0x8D, 0x3D}; // lea rdi, [rip+data]
  for (uint8_t B : rel32(Wrap + 7, Text + 0x50))
    WrapCode.push_back(B);
  for (uint8_t B : {0xBE, 0x08, 0x00, 0x00, 0x00, // mov esi, 8
                    0x85, 0xF6,                   // test esi, esi
                    0x74, 0x10,                   // je done
                    0x48, 0x8B, 0x05})            // mov rax, [rip+slot]
    WrapCode.push_back(B);
  for (uint8_t B : rel32(Wrap + 23, Slot))
    WrapCode.push_back(B);
  for (uint8_t B : {0x48, 0x85, 0xC0, // test rax, rax
                    0x74, 0x02,       // je done
                    0xFF, 0xE0,       // jmp rax
                    0xC3})            // done: ret
    WrapCode.push_back(B);
  put(Code, Wrap, WrapCode);
  BinaryImage Img = makeImportImage(Code, {{Wrap, "wrap"}}, {});
  Segment Got;
  Got.Name = ".got";
  Got.VA = Slot;
  Got.Size = Got.FileSz = 8;
  Got.Data.resize(8);
  Got.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Img.Segments.push_back(std::move(Got));
  ASSERT_TRUE(Img.recordImportStorageSlot(Slot, "_ITM_registerTMCloneTable", 0,
                                          ImportStorageEvidence::LoaderBind));
  const std::string Source = liftEntries(Img, {Wrap});
  const std::string Body = body(Source, "wrap");
  ASSERT_FALSE(Body.empty()) << Source;
  EXPECT_NE(Body.find("_ITM_registerTMCloneTable("), std::string::npos) << Body;
  EXPECT_EQ(Body.find("unknown value"), std::string::npos) << Body;
}

TEST(SysVCallContract, ACallThroughALoaderBoundSlotPassesThePrototype) {
  // Built with -fno-plt, wrap(s, f) calls fputs through the GOT slot that a
  // GLOB_DAT relocation binds, which no import directory entry lists.
  constexpr va_t Wrap = Text, Slot = 0x403000;
  std::vector<uint8_t> Code(0x20, 0xCC);
  std::vector<uint8_t> WrapCode = {0x48, 0x83, 0xEC, 0x08, // sub rsp, 8
                                   0xFF, 0x15};            // call [rip+slot]
  for (uint8_t B : rel32(Wrap + 10, Slot))
    WrapCode.push_back(B);
  for (uint8_t B : {0x48, 0x83, 0xC4, 0x08, // add rsp, 8
                    0xC3})                  // ret
    WrapCode.push_back(B);
  put(Code, Wrap, WrapCode);
  BinaryImage Img = makeImportImage(Code, {{Wrap, "wrap"}}, {});
  Segment Got;
  Got.Name = ".got";
  Got.VA = Slot;
  Got.Size = Got.FileSz = 8;
  Got.Data.resize(8);
  Got.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Img.Segments.push_back(std::move(Got));
  ASSERT_TRUE(Img.recordImportStorageSlot(Slot, "fputs", 0,
                                          ImportStorageEvidence::LoaderBind));
  const std::string Source = liftEntries(Img, {Wrap});
  const std::string Body = body(Source, "wrap");
  ASSERT_FALSE(Body.empty()) << Source;
  EXPECT_NE(Body.find("fputs(arg0, arg1)"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("unknown value"), std::string::npos) << Body;
}

TEST(SysVCallContract, AnArgumentOnlyTheCallSiteScanReadsKeepsItsValue) {
  // parse(o): s = o->text + 1 in RDI, tested by a load through it; the call
  // to an import with no prototype in the next block passes it on.  Only
  // the call site's register scan reads that RDI, after the uses were
  // counted, so its value must still print.
  constexpr va_t Parse = Text, Stub = Text + 0x30;
  std::vector<uint8_t> Code(0x40, 0xCC);
  std::vector<uint8_t> ParseCode = {0x48, 0x8B, 0x47, 0x10, // mov rax, [rdi+16]
                                    0x48, 0x8D, 0x78, 0x01, // lea rdi, [rax+1]
                                    0x80, 0x3F, 0x2D, // cmp byte [rdi], '-'
                                    0x74, 0x08,       // je minus
                                    0x31, 0xF6,       // xor esi, esi
                                    0xE8};            // call parse_num
  for (uint8_t B : rel32(Parse + 20, Stub))
    ParseCode.push_back(B);
  for (uint8_t B : {0xC3,                         // ret
                    0xB8, 0x01, 0x00, 0x00, 0x00, // minus: mov eax, 1
                    0xC3})                        // ret
    ParseCode.push_back(B);
  put(Code, Parse, ParseCode);
  const BinaryImage Img =
      makeImportImage(Code, {{Parse, "parse"}}, {{Stub, "parse_num"}});
  const std::string Source = liftEntries(Img, {Parse});
  const std::string Body = body(Source, "parse");
  ASSERT_FALSE(Body.empty()) << Source;
  EXPECT_NE(Body.find("parse_num("), std::string::npos) << Body;
  EXPECT_EQ(Body.find("unknown register"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("unknown value"), std::string::npos) << Body;
}

TEST(SysVCallContract, ARegisterTheCalleeLeavesAloneIsNoReturnedField) {
  // add(a, b): rdx = b; eax = inc(a); return eax + edx.  inc never writes
  // RDX, so GCC keeps b there across the call; EDX after it is b, not a
  // second eightbyte that inc returns.
  constexpr va_t Add = Text, Inc = Text + 0x10;
  std::vector<uint8_t> Code(0x20, 0xCC);
  std::vector<uint8_t> AddCode = {0x48, 0x89, 0xF2, // mov rdx, rsi
                                  0xE8};            // call inc
  for (uint8_t B : rel32(Add + 8, Inc))
    AddCode.push_back(B);
  for (uint8_t B : {0x01, 0xD0, // add eax, edx
                    0xC3})      // ret
    AddCode.push_back(B);
  put(Code, Add, AddCode);
  put(Code, Inc,
      {0x8D, 0x47, 0x01, // lea eax, [rdi+1]
       0xC3});           // ret
  const BinaryImage Img =
      makeImportImage(Code, {{Add, "add"}, {Inc, "inc"}}, {});
  const std::string Source = liftEntries(Img, {Add, Inc});
  const std::string Body = body(Source, "add");
  ASSERT_FALSE(Body.empty()) << Source;
  EXPECT_EQ(Body.find(">> 32"), std::string::npos) << Body;
  compileAndRun(Source + R"(
int main(void) {
  return (uint32_t)add(5, 7) == 13 && (uint32_t)add(-3, 40) == 38 ? 0 : 1;
}
)");
}

TEST(SysVCallContract, ACallThatEndsItsFunctionDoesNotReturn) {
  // pfatal(name) calls error(2, 0, NULL, name): error returns only for a
  // zero status, so GCC places nothing after the call, and pfatal's sized
  // symbol ends with it.  What follows the 2-byte no-op is next's code.
  constexpr va_t PFatal = Text, Next = Text + 0x13, ErrorStub = Text + 0x30;
  std::vector<uint8_t> Code(0x40, 0xCC);
  std::vector<uint8_t> PFatalCode = {0x48, 0x89, 0xF9, // mov rcx, rdi
                                     0xBF, 0x02, 0x00, 0x00, 0x00, // mov edi, 2
                                     0x31, 0xF6, // xor esi, esi
                                     0x31, 0xD2, // xor edx, edx
                                     0xE8};      // call error
  for (uint8_t B : rel32(PFatal + 17, ErrorStub))
    PFatalCode.push_back(B);
  PFatalCode.push_back(0x66); // xchg ax, ax
  PFatalCode.push_back(0x90);
  put(Code, PFatal, PFatalCode);
  put(Code, Next,
      {0x48, 0x8D, 0x47, 0x01, // lea rax, [rdi+1]
       0xC3});                 // ret
  BinaryImage Img = makeImportImage(Code, {{PFatal, "pfatal"}, {Next, "next"}},
                                    {{ErrorStub, "error"}});
  Img.Symbols.front().Size = 17;
  ASSERT_EQ(PFatal + static_cast<va_t>(PFatalCode.size()), Next);
  const std::string Source = liftEntries(Img, {PFatal});
  const std::string Body = body(Source, "pfatal");
  ASSERT_FALSE(Body.empty()) << Source;
  EXPECT_NE(Body.find("error("), std::string::npos) << Body;
  EXPECT_EQ(Body.find("next("), std::string::npos) << Body;
  EXPECT_EQ(Body.find("unknown value"), std::string::npos) << Body;
}

TEST(SysVCallContract, AVariadicPrologueSpillReadsNoParameter) {
  // A variadic panic(fmt, ...) spills RSI..R9 to its register save area
  // around `test al, al`: GCC with a frame pointer off RBP (sed, patch),
  // GCC and Clang without one off RSP, and Clang -O0 where the branch over
  // the vector spills rejoins, beside a home slot for RDI.  wrapper(s) calls
  // panic("%s", s) and caller(x) calls wrapper(x).  Decompiling caller alone
  // summarizes the other two as its callees, the way the GUI does: the spills
  // of registers wrapper never sets are no parameters of wrapper, so caller
  // passes it exactly one argument.
  const std::vector<std::vector<uint8_t>> Prologues = {
      {0xF3, 0x0F, 0x1E, 0xFA,                    // endbr64
       0x55,                                      // push rbp
       0x48, 0x89, 0xE5,                          // mov rbp, rsp
       0x53,                                      // push rbx
       0x48, 0x89, 0xFB,                          // mov rbx, rdi
       0x48, 0x81, 0xEC, 0xD8, 0x00, 0x00, 0x00,  // sub rsp, 0xd8
       0x48, 0x89, 0xB5, 0x48, 0xFF, 0xFF, 0xFF,  // mov [rbp-0xb8], rsi
       0x48, 0x89, 0x95, 0x50, 0xFF, 0xFF, 0xFF,  // mov [rbp-0xb0], rdx
       0x48, 0x89, 0x8D, 0x58, 0xFF, 0xFF, 0xFF,  // mov [rbp-0xa8], rcx
       0x4C, 0x89, 0x85, 0x60, 0xFF, 0xFF, 0xFF,  // mov [rbp-0xa0], r8
       0x4C, 0x89, 0x8D, 0x68, 0xFF, 0xFF, 0xFF,  // mov [rbp-0x98], r9
       0x84, 0xC0,                                // test al, al
       0x74, 0x07,                                // je spilled
       0x0F, 0x29, 0x85, 0x70, 0xFF, 0xFF, 0xFF}, // movaps [rbp-0x90], xmm0
      {0xF3, 0x0F, 0x1E, 0xFA,                    // endbr64
       0x53,                                      // push rbx
       0x48, 0x89, 0xFB,                          // mov rbx, rdi
       0x48, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00,  // sub rsp, 0xd0
       0x48, 0x89, 0x74, 0x24, 0x28,              // mov [rsp+0x28], rsi
       0x48, 0x89, 0x54, 0x24, 0x30,              // mov [rsp+0x30], rdx
       0x48, 0x89, 0x4C, 0x24, 0x38,              // mov [rsp+0x38], rcx
       0x4C, 0x89, 0x44, 0x24, 0x40,              // mov [rsp+0x40], r8
       0x4C, 0x89, 0x4C, 0x24, 0x48,              // mov [rsp+0x48], r9
       0x84, 0xC0,                                // test al, al
       0x74, 0x05,                                // je spilled
       0x0F, 0x29, 0x44, 0x24, 0x50},             // movaps [rsp+0x50], xmm0
      {0x55,                                      // push rbp
       0x48, 0x89, 0xE5,                          // mov rbp, rsp
       0x48, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00,  // sub rsp, 0xd0
       0x84, 0xC0,                                // test al, al
       0x74, 0x07,                                // je spilled
       0x0F, 0x29, 0x85, 0x60, 0xFF, 0xFF, 0xFF,  // movaps [rbp-0xa0], xmm0
       0x4C, 0x89, 0x8D, 0x58, 0xFF, 0xFF, 0xFF,  // spilled: mov [rbp-0xa8], r9
       0x4C, 0x89, 0x85, 0x50, 0xFF, 0xFF, 0xFF,  // mov [rbp-0xb0], r8
       0x48, 0x89, 0x8D, 0x48, 0xFF, 0xFF, 0xFF,  // mov [rbp-0xb8], rcx
       0x48, 0x89, 0x95, 0x40, 0xFF, 0xFF, 0xFF,  // mov [rbp-0xc0], rdx
       0x48, 0x89, 0xB5, 0x38, 0xFF, 0xFF, 0xFF,  // mov [rbp-0xc8], rsi
       0x48, 0x89, 0x7D, 0xF8,                    // mov [rbp-0x8], rdi
       0x48, 0x8B, 0x5D, 0xF8}};                  // mov rbx, [rbp-0x8]
  constexpr va_t Panic = Text, Wrapper = Text + 0x60, Caller = Text + 0x80,
                 ExitStub = Text + 0xA0;
  for (const std::vector<uint8_t> &Prologue : Prologues) {
    SCOPED_TRACE(Prologue.size());
    std::vector<uint8_t> Code(0xB0, 0xCC);
    std::vector<uint8_t> PanicCode = Prologue;
    for (uint8_t B : {0x48, 0x89, 0xDF, // mov rdi, rbx
                      0xE8})            // call exit
      PanicCode.push_back(B);
    for (uint8_t B : rel32(Panic + PanicCode.size() + 4, ExitStub))
      PanicCode.push_back(B);
    ASSERT_LE(PanicCode.size(), Wrapper - Panic);
    put(Code, Panic, PanicCode);
    std::vector<uint8_t> WrapperCode = {0x48, 0x89, 0xFE, // mov rsi, rdi
                                        0x31, 0xC0,       // xor eax, eax
                                        0xBF, 0x00, 0x20,
                                        0x40, 0x00, // mov edi, fmt
                                        0xE8};      // call panic
    for (uint8_t B : rel32(Wrapper + 15, Panic))
      WrapperCode.push_back(B);
    put(Code, Wrapper, WrapperCode);
    std::vector<uint8_t> CallerCode = {0x48, 0x83, 0xEC, 0x08, // sub rsp, 8
                                       0xE8};                  // call wrapper
    for (uint8_t B : rel32(Caller + 9, Wrapper))
      CallerCode.push_back(B);
    for (uint8_t B : {0x48, 0x83, 0xC4, 0x08, // add rsp, 8
                      0xC3})                  // ret
      CallerCode.push_back(B);
    put(Code, Caller, CallerCode);
    const BinaryImage Img = makeImportImage(
        Code, {{Panic, "panic"}, {Wrapper, "wrapper"}, {Caller, "caller"}},
        {{ExitStub, "exit"}});
    llvm::LLVMContext Ctx;
    PipelineOptions Opts;
    Opts.EmitDumpOutput = false;
    Opts.OnlyFunctionEntries = {Caller};
    const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
    ASSERT_TRUE(Result.Success) << Result.Error;
    for (const va_t Entry : {Panic, Wrapper}) {
      const auto Reads = Result.CallEntryReadGPRs.find(Entry);
      ASSERT_NE(Reads, Result.CallEntryReadGPRs.end()) << std::hex << Entry;
      for (const uint64_t Spilled :
           {x86reg::RSI, x86reg::RDX, x86reg::RCX, x86reg::R8, x86reg::R9})
        EXPECT_EQ(Reads->second[Spilled / 8], 0)
            << std::hex << Entry << " reads " << Spilled;
      EXPECT_EQ(Reads->second[x86reg::RDI / 8], 8) << std::hex << Entry;
    }
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = Img.Arch;
    Options.Format = Img.Format;
    Options.Image = &Img;
    ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));
    const std::string Body = body(Source, "caller");
    ASSERT_FALSE(Body.empty()) << Source;
    EXPECT_NE(Body.find("wrapper(arg0)"), std::string::npos) << Body;
    EXPECT_EQ(Body.find("unknown"), std::string::npos) << Body;
  }
}

TEST(SysVCallContract, ACopyOfEaxAfterSeteIsNoVectorCount) {
  // is_slash(c) is `c == '/'`: GCC sets AL with sete and copies the whole of
  // EAX, so the summary reads RAX's upper bytes as well.  Only a read of
  // exactly AL is a variadic prologue's vector count, so the call still
  // passes is_slash's one parameter, not the RSI the caller left set.
  constexpr va_t IsSlash = Text, Caller = Text + 0x20;
  std::vector<uint8_t> Code(0x40, 0xCC);
  put(Code, IsSlash,
      {0x83, 0xFF, 0x2F, // cmp edi, '/'
       0x0F, 0x94, 0xC0, // sete al
       0x89, 0xC2,       // mov edx, eax
       0x89, 0xD0,       // mov eax, edx
       0xC3});           // ret
  std::vector<uint8_t> CallerCode = {
      0x48, 0x83, 0xEC, 0x08,       // sub rsp, 8
      0xBE, 0x03, 0x00, 0x00, 0x00, // mov esi, 3
      0xBF, 0x5C, 0x00, 0x00, 0x00, // mov edi, '\\'
      0xE8};                        // call is_slash
  for (uint8_t B : rel32(Caller + CallerCode.size() + 4, IsSlash))
    CallerCode.push_back(B);
  for (uint8_t B : {0x48, 0x83, 0xC4, 0x08, // add rsp, 8
                    0xC3})                  // ret
    CallerCode.push_back(B);
  put(Code, Caller, CallerCode);
  const BinaryImage Img =
      makeImportImage(Code, {{IsSlash, "is_slash"}, {Caller, "caller"}}, {});
  const std::string Source = liftEntries(Img, {Caller});
  const std::string Body = body(Source, "caller");
  ASSERT_FALSE(Body.empty()) << Source;
  const size_t Call = Body.find("is_slash(");
  ASSERT_NE(Call, std::string::npos) << Body;
  const std::string Line = Body.substr(Call, Body.find('\n', Call) - Call);
  EXPECT_EQ(Line.find(','), std::string::npos) << Line;
}

TEST(SysVCallContract, GccSplitVaStartMarksTheFunctionVariadic) {
  // fatal(fmt, ...) as GCC builds it: va_start stores gp_offset and fp_offset
  // as two 32-bit constants where Clang stores one 64-bit word.  Either marks
  // the variadic prologue, so its vector register save area is no parameter.
  constexpr va_t Fatal = Text, VprintfStub = Text + 0x100,
                 ExitStub = Text + 0x110;
  std::vector<uint8_t> Code(0x120, 0xCC);
  std::vector<uint8_t> FatalCode = {
      0x55,                                     // push rbp
      0x48, 0x89, 0xE5,                         // mov rbp, rsp
      0x48, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00, // sub rsp, 0xd0
      0x48, 0x89, 0xB5, 0x58, 0xFF, 0xFF, 0xFF, // mov [rbp-0xa8], rsi
      0x48, 0x89, 0x95, 0x60, 0xFF, 0xFF, 0xFF, // mov [rbp-0xa0], rdx
      0x48, 0x89, 0x8D, 0x68, 0xFF, 0xFF, 0xFF, // mov [rbp-0x98], rcx
      0x4C, 0x89, 0x85, 0x70, 0xFF, 0xFF, 0xFF, // mov [rbp-0x90], r8
      0x4C, 0x89, 0x8D, 0x78, 0xFF, 0xFF, 0xFF, // mov [rbp-0x88], r9
      0x84, 0xC0,                               // test al, al
      0x74, 0x04,                               // je spilled
      0x0F, 0x29, 0x45, 0x80,                   // movaps [rbp-0x80], xmm0
      0x48, 0x8D, 0x45, 0x10,                   // spilled: lea rax, [rbp+0x10]
      0x48, 0x89, 0x85, 0x38, 0xFF, 0xFF, 0xFF, // mov [rbp-0xc8], rax
      0x48, 0x8D, 0x85, 0x50, 0xFF, 0xFF, 0xFF, // lea rax, [rbp-0xb0]
      0x48, 0x89, 0x85, 0x40, 0xFF, 0xFF, 0xFF, // mov [rbp-0xc0], rax
      0xC7, 0x85, 0x30, 0xFF, 0xFF, 0xFF,       // mov dword [rbp-0xd0],
      0x08, 0x00, 0x00, 0x00,                   //   8 (gp_offset)
      0xC7, 0x85, 0x34, 0xFF, 0xFF, 0xFF,       // mov dword [rbp-0xcc],
      0x30, 0x00, 0x00, 0x00,                   //   0x30 (fp_offset)
      0x48, 0x8D, 0xB5, 0x30, 0xFF, 0xFF, 0xFF, // lea rsi, [rbp-0xd0]
      0xE8};                                    // call vprintf
  for (uint8_t B : rel32(Fatal + FatalCode.size() + 4, VprintfStub))
    FatalCode.push_back(B);
  for (uint8_t B : {0xBF, 0x02, 0x00, 0x00, 0x00, // mov edi, 2
                    0xE8})                        // call exit
    FatalCode.push_back(B);
  for (uint8_t B : rel32(Fatal + FatalCode.size() + 4, ExitStub))
    FatalCode.push_back(B);
  ASSERT_LE(FatalCode.size(), VprintfStub - Fatal);
  put(Code, Fatal, FatalCode);
  const BinaryImage Img = makeImportImage(
      Code, {{Fatal, "fatal"}}, {{VprintfStub, "vprintf"}, {ExitStub, "exit"}});
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = {Fatal};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  const auto F = std::find_if(
      Result.MedFuncs.begin(), Result.MedFuncs.end(),
      [&](const MedFunc &Function) { return Function.Entry == Fatal; });
  ASSERT_NE(F, Result.MedFuncs.end());
  EXPECT_TRUE(F->IsVariadic);
  for (const MedVar &Param : F->Params)
    EXPECT_LE(Param.Size, 8u) << "vector save-area slot as a parameter";
}

TEST(SysVCallContract, AnAlignmentPushIsNoStackArgument) {
  // die() calls the variadic panic("out of memory") and never returns, so
  // it aligns the stack for the call with a push of RAX (Clang) or a push,
  // pop and `sub rsp, 8` (GCC) instead of a frame.  RAX's incoming value is
  // undefined: the slot it lands in at the call passes nothing.
  constexpr va_t Panic = Text, Die = Text + 0x60, ExitStub = Text + 0xA0;
  const std::vector<std::vector<uint8_t>> Alignments = {
      {0x50},                    // push rax
      {0x50, 0x58,               // push rax; pop rax
       0x48, 0x83, 0xEC, 0x08}}; // sub rsp, 8
  for (const std::vector<uint8_t> &Alignment : Alignments) {
    SCOPED_TRACE(Alignment.size());
    std::vector<uint8_t> Code(0xB0, 0xCC);
    std::vector<uint8_t> PanicCode = {
        0x48, 0x81, 0xEC, 0xD8, 0x00, 0x00, 0x00, // sub rsp, 0xd8
        0x48, 0x89, 0x74, 0x24, 0x28,             // mov [rsp+0x28], rsi
        0x48, 0x89, 0x54, 0x24, 0x30,             // mov [rsp+0x30], rdx
        0x48, 0x89, 0x4C, 0x24, 0x38,             // mov [rsp+0x38], rcx
        0x4C, 0x89, 0x44, 0x24, 0x40,             // mov [rsp+0x40], r8
        0x4C, 0x89, 0x4C, 0x24, 0x48,             // mov [rsp+0x48], r9
        0x84, 0xC0,                               // test al, al
        0x74, 0x05,                               // je spilled
        0x0F, 0x29, 0x44, 0x24, 0x50,             // movaps [rsp+0x50], xmm0
        0xE8};                                    // spilled: call exit
    for (uint8_t B : rel32(Panic + PanicCode.size() + 4, ExitStub))
      PanicCode.push_back(B);
    put(Code, Panic, PanicCode);
    std::vector<uint8_t> DieCode = {0xBF, 0x00, 0x20,
                                    0x40, 0x00,  // mov edi, fmt
                                    0x31, 0xC0}; // xor eax, eax
    DieCode.insert(DieCode.begin(), Alignment.begin(), Alignment.end());
    DieCode.push_back(0xE8); // call panic
    for (uint8_t B : rel32(Die + DieCode.size() + 4, Panic))
      DieCode.push_back(B);
    put(Code, Die, DieCode);
    const BinaryImage Img = makeImportImage(
        Code, {{Panic, "panic"}, {Die, "die"}}, {{ExitStub, "exit"}});
    const std::string Source = liftEntries(Img, {Die});
    const std::string Body = body(Source, "die");
    ASSERT_FALSE(Body.empty()) << Source;
    const size_t Call = Body.find("panic(");
    ASSERT_NE(Call, std::string::npos) << Body;
    const std::string Line = Body.substr(Call, Body.find('\n', Call) - Call);
    EXPECT_EQ(Line.find(','), std::string::npos) << Line;
    EXPECT_EQ(Body.find("unknown"), std::string::npos) << Body;
  }
}

TEST(SysVCallContract, ErrorCheckingWrapperEndsAtItsExitHelper) {
  // xxd's shape: put(s, f) returns fputs(s, f) unless it fails, then calls
  // die(3), which ends in exit.  GCC aligns the next function with no-ops,
  // and the bytes after them are another function's, not put's.
  constexpr va_t Die = Text, Put = Text + 0x20, Next = Text + 0x40,
                 FputsStub = Text + 0x60, ExitStub = Text + 0x70;
  std::vector<uint8_t> Code(0x80, 0xCC);
  std::vector<uint8_t> DieCode = {0x48, 0x83, 0xEC, 0x08, // sub rsp, 8
                                  0xE8};                  // call exit
  for (uint8_t B : rel32(Die + 9, ExitStub))
    DieCode.push_back(B);
  put(Code, Die, DieCode);
  std::vector<uint8_t> PutCode = {0x55,             // push rbp
                                  0x48, 0x89, 0xE5, // mov rbp, rsp
                                  0xE8};            // call fputs
  for (uint8_t B : rel32(Put + 9, FputsStub))
    PutCode.push_back(B);
  for (uint8_t B : {0x83, 0xF8, 0xFF,             // cmp eax, -1
                    0x74, 0x02,                   // je fail
                    0x5D,                         // pop rbp
                    0xC3,                         // ret
                    0xBF, 0x03, 0x00, 0x00, 0x00, // fail: mov edi, 3
                    0xE8})                        // call die
    PutCode.push_back(B);
  for (uint8_t B : rel32(Put + 26, Die))
    PutCode.push_back(B);
  // nop word ptr [rax+rax*1+0x0] up to Next.
  for (uint8_t B : {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00})
    PutCode.push_back(B);
  put(Code, Put, PutCode);
  // next(a, b, c): a function that reads three argument registers.
  put(Code, Next,
      {0x48, 0x8D, 0x04, 0x37, // lea rax, [rdi+rsi]
       0x48, 0x01, 0xD0,       // add rax, rdx
       0xC3});                 // ret
  const BinaryImage Img =
      makeImportImage(Code, {{Die, "die"}, {Put, "put"}, {Next, "next"}},
                      {{FputsStub, "fputs"}, {ExitStub, "exit"}});
  ASSERT_EQ(Put + static_cast<va_t>(PutCode.size()), Next);
  const std::string Source = liftEntries(Img, {Put});
  const std::string Body = body(Source, "put");
  ASSERT_FALSE(Body.empty()) << Source;
  EXPECT_NE(Body.find("fputs(arg0, arg1)"), std::string::npos) << Body;
  EXPECT_NE(Body.find("die(3)"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("next("), std::string::npos) << Body;
  EXPECT_EQ(Body.find("unknown value"), std::string::npos) << Body;
}

} // namespace
