//===- HighNarrowLocalsTests.cpp - Locals as wide as their reads ---------===//
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

TypeRef integer(uint16_t Bytes, bool Signed) {
  return NdType::makeInt(Bytes, Signed);
}

ExprPtr input() {
  MedVar V;
  V.Kind = MedVar::Param;
  V.Id = 0;
  V.Size = 8;
  V.TheArch = Arch::X64;
  return HighExpr::makeVar(V, integer(8, false));
}

/// A register-wide temporary as MedToHigh leaves it.  Locals that share
/// \p RenameTag are one C variable.
ExprPtr local(int Id, int SSAVer = 0, int RenameTag = -1) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = Id;
  V.SSAVer = SSAVer;
  V.RenameTag = RenameTag;
  V.Size = 8;
  V.TheArch = Arch::X64;
  return HighExpr::makeVar(V, integer(8, true));
}

ExprPtr constant(uint64_t N, uint16_t Bytes = 8) {
  return HighExpr::makeConst(N, Bytes);
}

ExprPtr op(NdOp Op, ExprPtr A, ExprPtr B, TypeRef Type) {
  auto E = HighExpr::makeBinop(Op, std::move(A), std::move(B));
  E->Type = std::move(Type);
  return E;
}

/// The low bytes of \p E, as MedToHigh leaves a truncation.
ExprPtr lowPart(ExprPtr E, TypeRef Type) {
  return op(NdOp::SUBBYTES, std::move(E), constant(0, 4), std::move(Type));
}

ExprPtr cast(ExprPtr E, TypeRef Type) {
  auto C = std::make_shared<HighExpr>();
  C->Kind = ExprKind::Cast;
  C->Type = C->CastTo = std::move(Type);
  C->Operands = {std::move(E)};
  return C;
}

ExprPtr extend(NdOp Op, ExprPtr E, TypeRef Type) {
  auto X = HighExpr::makeUnary(Op, std::move(E));
  X->Type = std::move(Type);
  return X;
}

/// `(uint64_t)((uint32_t)arg0 + Addend)`, as a 32-bit x64 instruction
/// leaves its result in a 64-bit register.
ExprPtr zeroExtendedSum(uint64_t Addend) {
  return extend(NdOp::INT_ZEXT,
                op(NdOp::INT_ADD, lowPart(input(), integer(4, false)),
                   constant(Addend, 4), integer(4, false)),
                integer(8, false));
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

HighStmt when(ExprPtr Cond, std::vector<HighStmt> Body,
              std::vector<HighStmt> ElseBody = {}) {
  HighStmt S;
  S.Kind = ElseBody.empty() ? StmtKind::If : StmtKind::IfElse;
  S.Cond = std::move(Cond);
  S.Body = std::move(Body);
  S.ElseBody = std::move(ElseBody);
  return S;
}

HighFunc function(const std::string &Name, std::vector<HighStmt> Body) {
  HighFunc F;
  F.Name = Name;
  F.Entry = 0x1000;
  F.ReturnType = integer(8, true);
  F.Params = {{"arg0", integer(8, false)}};
  F.Body = std::move(Body);
  return F;
}

/// Runs the passes in the order MedToHigh runs them, then prints the C.
std::string narrowAndEmit(HighFunc &F, bool ExpectNarrowed) {
  EXPECT_EQ(narrowLocals(F), ExpectNarrowed);
  chooseIntegerSignedness(F);
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  EXPECT_TRUE(HighCEmitter().emit({F}, Out, Options));
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
      llvm::sys::fs::createTemporaryFile("neverd-narrow", "c", SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-narrow", "exe", BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-narrow", "err", ErrorPath));
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

TEST(HighNarrowLocals, AZeroExtendedResultReadAsAnIntIsAnInt) {
  // t1 = (uint64_t)((uint32_t)arg0 + 1); every read takes (int32_t)t1.
  HighFunc F = function(
      "status", {assign(local(1), zeroExtendedSum(1)),
                 when(op(NdOp::INT_SLESS, cast(local(1), integer(4, true)),
                         constant(0, 4), integer(1, false)),
                      {result(constant(1))}),
                 result(extend(NdOp::INT_SEXT, cast(local(1), integer(4, true)),
                               integer(8, true)))});
  const std::string Source = narrowAndEmit(F, /*ExpectNarrowed=*/true);
  EXPECT_EQ(declarationOf(Source, "t1"), "    int32_t t1;") << Source;
  EXPECT_EQ(Source.find("(int32_t)t1"), std::string::npos) << Source;
  compileAndRun(Source + R"(
int main(void) {
  const uint64_t xs[] = {0, 1, 0x7FFFFFFEull, 0x7FFFFFFFull, 0xFFFFFFFFull,
                         0x123456789ull, 0xFFFFFFFF7FFFFFFFull};
  for (unsigned i = 0; i < sizeof xs / sizeof xs[0]; ++i) {
    const int32_t s = (int32_t)((uint32_t)xs[i] + 1);
    if (status(xs[i]) != (s < 0 ? 1 : (int64_t)s))
      return 1;
  }
  return 0;
}
)");
}

TEST(HighNarrowLocals, AConstantDefinitionKeepsItsLowBytes) {
  // One variable, two definitions: the zero-extended sum, or a constant
  // whose upper bytes no read sees.
  HighFunc F = function(
      "pick",
      {when(op(NdOp::INT_AND, input(), constant(1), integer(8, false)),
            {assign(local(1, 1, 3), zeroExtendedSum(5))},
            {assign(local(1, 2, 3), constant(0x1234567887654321))}),
       result(extend(NdOp::INT_SEXT, cast(local(1, 3, 3), integer(4, true)),
                     integer(8, true)))});
  const std::string Source = narrowAndEmit(F, /*ExpectNarrowed=*/true);
  EXPECT_NE(declarationOf(Source, "v3").find("int32_t v3;"), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("0x1234567887654321"), std::string::npos) << Source;
  compileAndRun(Source + R"(
int main(void) {
  const uint64_t xs[] = {0, 1, 2, 3, 0x7FFFFFFBull, 0xFFFFFFFFull,
                         0x123456789ull};
  for (unsigned i = 0; i < sizeof xs / sizeof xs[0]; ++i) {
    const uint32_t low = (xs[i] & 1) ? (uint32_t)xs[i] + 5 : 0x87654321u;
    if (pick(xs[i]) != (int64_t)(int32_t)low)
      return 1;
  }
  return 0;
}
)");
}

TEST(HighNarrowLocals, AWideReadKeepsTheRegisterWidth) {
  HighFunc F = function(
      "wide",
      {assign(local(1), zeroExtendedSum(1)),
       when(op(NdOp::INT_SLESS, cast(local(1), integer(4, true)),
               constant(0, 4), integer(1, false)),
            {result(constant(1))}),
       result(op(NdOp::INT_ADD, local(1), constant(1), integer(8, true)))});
  const std::string Source = narrowAndEmit(F, /*ExpectNarrowed=*/false);
  EXPECT_NE(declarationOf(Source, "t1").find("int64_t t1;"), std::string::npos)
      << Source;
}

TEST(HighNarrowLocals, AWideDefinitionKeepsTheRegisterWidth) {
  // The low bytes of a 64-bit sum are a 32-bit sum, but this pass changes
  // no arithmetic: only extensions and constants define a narrowed local.
  HighFunc F = function(
      "sum", {assign(local(1), op(NdOp::INT_ADD, input(), constant(1),
                                  integer(8, false))),
              when(op(NdOp::INT_SLESS, cast(local(1), integer(4, true)),
                      constant(0, 4), integer(1, false)),
                   {result(constant(1))}),
              result(extend(NdOp::INT_SEXT, cast(local(1), integer(4, true)),
                            integer(8, true)))});
  const std::string Source = narrowAndEmit(F, /*ExpectNarrowed=*/false);
  EXPECT_NE(declarationOf(Source, "t1").find("int64_t t1;"), std::string::npos)
      << Source;
}

} // namespace
