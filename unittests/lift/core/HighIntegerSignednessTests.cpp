//===- HighIntegerSignednessTests.cpp - Signedness of integer locals -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/high/MedToHigh.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

#include <string>

namespace {
using namespace neverd;

ExprPtr input() {
  MedVar V;
  V.Kind = MedVar::Param;
  V.Id = 0;
  V.Size = 8;
  V.TheArch = Arch::X64;
  return HighExpr::makeVar(V, NdType::makeInt(8, false));
}

/// A temporary as MedToHigh leaves it: a signed integer by default.
ExprPtr local(int Id, TypeRef Type = NdType::makeInt(8, true)) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = Id;
  V.Size = 8;
  V.TheArch = Arch::X64;
  return HighExpr::makeVar(V, std::move(Type));
}

ExprPtr constant(uint64_t N) { return HighExpr::makeConst(N, 8); }

ExprPtr op(NdOp Op, ExprPtr A, ExprPtr B, bool Bool = false) {
  auto E = HighExpr::makeBinop(Op, std::move(A), std::move(B));
  E->Type = Bool ? NdType::makeInt(1, false) : NdType::makeInt(8, true);
  return E;
}

HighStmt assign(ExprPtr Dst, ExprPtr Value) {
  HighStmt S;
  S.Kind = StmtKind::Assign;
  S.Dst = std::move(Dst);
  S.Val = std::move(Value);
  return S;
}

HighStmt result(ExprPtr Value) {
  HighStmt S;
  S.Kind = StmtKind::Return;
  S.RetVal = std::move(Value);
  return S;
}

HighStmt when(ExprPtr Cond, std::vector<HighStmt> Body) {
  HighStmt S;
  S.Kind = StmtKind::If;
  S.Cond = std::move(Cond);
  S.Body = std::move(Body);
  return S;
}

HighFunc function(const std::string &Name, std::vector<HighStmt> Body) {
  HighFunc F;
  F.Name = Name;
  F.Entry = 0x1000;
  F.ReturnType = NdType::makeInt(8, false);
  F.Params = {{"arg0", NdType::makeInt(8, false)}};
  F.Body = std::move(Body);
  return F;
}

std::string emit(const std::vector<HighFunc> &Functions) {
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  EXPECT_TRUE(HighCEmitter().emit(Functions, Out, Options));
  return Source;
}

std::string declarationOf(const std::string &Source, const std::string &Name) {
  const std::string Suffix = " " + Name + ";";
  size_t At = Source.find(Suffix);
  if (At == std::string::npos)
    return {};
  const size_t Start = Source.rfind('\n', At) + 1;
  return Source.substr(Start, At + Suffix.size() - Start);
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
      llvm::sys::fs::createTemporaryFile("neverd-signedness", "c", SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-signedness", "exe",
                                                  BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-signedness", "err",
                                                  ErrorPath));
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
        "-fsanitize=undefined",
        "-fsanitize-trap=undefined",
        "-Werror=uninitialized",
        "-Werror=return-type",
        SourcePath,
        "-o",
        BinaryPath};
    std::string Error;
    int Result = llvm::sys::ExecuteAndWait(Compiler, Arguments, std::nullopt,
                                           Redirects, 30, 0, &Error);
    auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Result, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "") << '\n'
                         << Source;
    Result = llvm::sys::ExecuteAndWait(BinaryPath, {BinaryPath}, std::nullopt,
                                       Redirects, 30, 0, &Error);
    ASSERT_EQ(Result, 0) << Error << '\n' << Source;
  }
}

TEST(HighIntegerSignedness, WrappingArithmeticLocalsBecomeUnsigned) {
  // t1 = x + 7; t2 = t1 * 3; return (t2 - t1) + t2, all wrapping at 64
  // bits.  Each local is read twice, so the writer keeps it a variable.
  HighFunc F = function(
      "wrap", {assign(local(1), op(NdOp::INT_ADD, input(), constant(7))),
               assign(local(2), op(NdOp::INT_MULT, local(1), constant(3))),
               assign(local(3), op(NdOp::INT_SUB, local(2), local(1))),
               result(op(NdOp::INT_ADD, local(3), local(2)))});
  chooseIntegerSignedness(F);
  const std::string Source = emit({F});
  EXPECT_EQ(declarationOf(Source, "t1"), "    uint64_t t1;") << Source;
  EXPECT_EQ(declarationOf(Source, "t2"), "    uint64_t t2;") << Source;
  EXPECT_NE(Source.find("t2 = t1 * 3;"), std::string::npos) << Source;
  compileAndRun(Source + R"(
int main(void) {
  const uint64_t xs[] = {0, 1, 7, 0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull,
                         0xFFFFFFFFFFFFFFF9ull, 0xFFFFFFFFFFFFFFFFull};
  for (unsigned i = 0; i < sizeof xs / sizeof xs[0]; ++i) {
    const uint64_t t1 = xs[i] + 7, t2 = t1 * 3;
    if (wrap(xs[i]) != (t2 - t1) + t2)
      return 1;
  }
  return 0;
}
)");
}

TEST(HighIntegerSignedness, SignedComparisonsKeepALocalSigned) {
  // One wrapping definition against two signed comparisons: t1 stays
  // signed and compares without a cast.
  HighFunc F = function(
      "classify", {assign(local(1), op(NdOp::INT_SUB, input(), constant(1))),
                   when(op(NdOp::INT_SLESS, local(1), constant(0), true),
                        {result(constant(1))}),
                   when(op(NdOp::INT_SLESS, local(1), constant(10), true),
                        {result(constant(2))}),
                   result(constant(3))});
  chooseIntegerSignedness(F);
  const std::string Source = emit({F});
  EXPECT_EQ(declarationOf(Source, "t1"), "    int64_t t1;") << Source;
  EXPECT_NE(Source.find("t1 < 0"), std::string::npos) << Source;
  compileAndRun(Source + R"(
int main(void) {
  if (classify(0) != 1 || classify(5) != 2 || classify(100) != 3)
    return 1;
  if (classify(0x8000000000000000ull) != 3)
    return 2;
  return 0;
}
)");
}

TEST(HighIntegerSignedness, UnsignedMajorityStillComparesSigned) {
  // t1 = x + 1 feeds a multiplication and a logical shift, so it becomes
  // unsigned; its one signed comparison reinterprets it with a cast.
  HighFunc F = function(
      "mixed", {assign(local(1), op(NdOp::INT_ADD, input(), constant(1))),
                assign(local(2), op(NdOp::INT_MULT, local(1), constant(5))),
                when(op(NdOp::INT_SLESS, local(1), constant(0), true),
                     {result(local(2))}),
                result(op(NdOp::INT_RIGHT, local(1), constant(2)))});
  chooseIntegerSignedness(F);
  const std::string Source = emit({F});
  EXPECT_EQ(declarationOf(Source, "t1"), "    uint64_t t1;") << Source;
  EXPECT_NE(Source.find("(int64_t)t1 < 0"), std::string::npos) << Source;
  compileAndRun(Source + R"(
static uint64_t reference(uint64_t x) {
  uint64_t t1 = x + 1;
  if ((int64_t)t1 < 0)
    return t1 * 5;
  return t1 >> 2;
}
int main(void) {
  const uint64_t xs[] = {0, 10, 0x7FFFFFFFFFFFFFFEull, 0x7FFFFFFFFFFFFFFFull,
                         0xFFFFFFFFFFFFFFFAull, 0xFFFFFFFFFFFFFFFFull};
  for (unsigned i = 0; i < sizeof xs / sizeof xs[0]; ++i)
    if (mixed(xs[i]) != reference(xs[i]))
      return 1;
  return 0;
}
)");
}

TEST(HighIntegerSignedness, LoadsReadTheSignednessOfTheirOperation) {
  // The first word, lifted as signed, feeds wrapping addition; the second,
  // lifted as unsigned, a signed comparison.  Each read takes the
  // signedness its operation wants and needs no conversion.
  auto Word = [](uint64_t Offset, bool Signed) {
    return HighExpr::makeLoad(
        Offset ? op(NdOp::INT_ADD, input(), constant(Offset)) : input(),
        NdType::makeInt(8, Signed));
  };
  HighFunc F = function(
      "loads", {when(op(NdOp::INT_SLESS, Word(8, false), constant(0), true),
                     {result(constant(1))}),
                result(op(NdOp::INT_ADD, Word(0, true), constant(8)))});
  chooseIntegerSignedness(F);
  const std::string Source = emit({F});
  EXPECT_NE(Source.find("*(_SQWORD *)(arg0 + 8)) < 0"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("*(_QWORD *)arg0 + 8"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("(int64_t)"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("(uint64_t)*"), std::string::npos) << Source;
  compileAndRun(Source + R"(
int main(void) {
  uint64_t words[2] = {0xFFFFFFFFFFFFFFFCull, 5};
  if (loads((uint64_t)(uintptr_t)words) != 4)
    return 1;
  words[1] = 0x8000000000000000ull;
  if (loads((uint64_t)(uintptr_t)words) != 1)
    return 2;
  return 0;
}
)");
}

TEST(HighIntegerSignedness, NonIntegerLocalsKeepTheirTypes) {
  HighFunc F = function(
      "pointer", {assign(local(1, NdType::makePtr()), input()),
                  assign(local(2), op(NdOp::INT_ADD, local(1), constant(8))),
                  result(local(2))});
  chooseIntegerSignedness(F);
  bool PointerKept = false;
  for (const HighStmt &S : F.Body)
    if (S.Kind == StmtKind::Assign && S.Dst->Var.Id == 1)
      PointerKept = S.Dst->Type && S.Dst->Type->Kind == NdTypeKind::Ptr;
  EXPECT_TRUE(PointerKept);
  // A local with a pointer use keeps every use as it was.
  const ExprPtr &Use = F.Body[1].Val->Operands[0];
  ASSERT_TRUE(Use->Type);
  EXPECT_EQ(Use->Type->Kind, NdTypeKind::Int);
  EXPECT_TRUE(Use->Type->IsSigned);
}

} // namespace
