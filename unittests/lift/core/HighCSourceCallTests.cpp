#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/high/HighSourceFlow.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/Swift/SwiftRuntimeCalls.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <stdexcept>

using namespace neverd;

namespace neverd {
void coalesceBranchEntryStatements(HighFunc &Func);
void structureIfElse(HighFunc &, int, const MedFunc * = nullptr);
} // namespace neverd

namespace {
ExprPtr parameter(unsigned Id, TypeRef Type) {
  MedVar V;
  V.Kind = MedVar::Param;
  V.Id = Id;
  V.Size = Type->Size;
  V.TheArch = Arch::X64;
  return HighExpr::makeVar(V, Type);
}

HighFunc returning(llvm::StringRef Name, ExprPtr Value,
                   std::vector<TypeRef> Parameters = {}) {
  HighFunc Function;
  Function.Name = Name.str();
  Function.ReturnType = Value->Type;
  for (size_t I = 0; I < Parameters.size(); ++I)
    Function.Params.push_back({"arg" + std::to_string(I), Parameters[I]});
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = std::move(Value);
  Function.Body.push_back(std::move(Return));
  return Function;
}

ExprPtr call(SourceCallTypeHint Hint, TypeRef Carrier,
             std::vector<ExprPtr> Arguments = {}) {
  auto Result = HighExpr::makeCall(Hint.TargetName, Hint.TargetAddress,
                                   std::move(Arguments));
  Result->Type = std::move(Carrier);
  Result->SourceCallHint =
      std::make_shared<const SourceCallTypeHint>(std::move(Hint));
  return Result;
}

SourceCallTypeHint native(llvm::StringRef Name, TypeRef Return,
                          std::vector<TypeRef> Arguments) {
  SourceCallTypeHint Hint;
  Hint.TargetName = Name.str();
  Hint.Signature.ReturnType = std::move(Return);
  for (const auto &Type : Arguments)
    Hint.Signature.Parameters.push_back({"", Type});
  return Hint;
}

std::string emit(const std::vector<HighFunc> &Functions, bool Includes = true) {
  std::string Result;
  llvm::raw_string_ostream OS(Result);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.EmitIncludes = Includes;
  Options.EmitComments = false;
  EXPECT_TRUE(HighCEmitter().emit(Functions, OS, Options));
  return Result;
}

void compileAndRun(const std::string &Source) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  auto Program = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Program)) << "clang is required";
  const std::string Compiler = *Program;
#endif
  llvm::SmallString<128> SourcePath, BinaryPath, ErrorPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-source-call", "c",
                                                  SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-source-call", "exe",
                                                  BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-source-call", "err",
                                                  ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code EC;
  {
    llvm::raw_fd_ostream OS(SourcePath, EC);
    ASSERT_FALSE(EC);
    OS << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  llvm::SmallVector<llvm::StringRef, 12> Arguments{
      Compiler,
      "-std=c11",
      "-O1",
      "-fno-inline",
      "-fblocks",
      "-Werror=implicit-function-declaration",
      "-Werror=return-type",
      SourcePath,
      "-o",
      BinaryPath};
  std::string Error;
  const int Compiled = llvm::sys::ExecuteAndWait(
      Compiler, Arguments, std::nullopt, Redirects, 30, 0, &Error);
  auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
  ASSERT_EQ(Compiled, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "") << "\n"
                         << Source;
  const int Ran = llvm::sys::ExecuteAndWait(
      BinaryPath, {BinaryPath}, std::nullopt, Redirects, 30, 0, &Error);
  ASSERT_EQ(Ran, 0) << Error << "\n" << Source;
}

TEST(HighCSourceCalls, GuardedPhiCleanupKeepsTargetLabelsExecutable) {
  const auto Integer = NdType::makeInt(4);
  MedVar Variable;
  Variable.Kind = MedVar::Temp;
  Variable.Id = 10;
  Variable.Size = 4;
  auto Value = HighExpr::makeVar(Variable);
  auto Function = returning("guarded_value", Value, {Integer});
  auto Return = Function.Body.back();
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = 0x2000;
  HighStmt Enter;
  Enter.Kind = StmtKind::If;
  Enter.Cond = HighExpr::makeBinop(NdOp::INT_EQUAL, parameter(0, Integer),
                                   HighExpr::makeConst(0, 4));
  Enter.Body = {Jump};
  HighStmt Defined;
  Defined.Kind = StmtKind::Assign;
  Defined.Dst = Value;
  Defined.Val = HighExpr::makeConst(42, 4);
  HighStmt Copy = Defined;
  ++Variable.Id;
  Copy.Val = HighExpr::makeVar(Variable);
  Copy.IsPhiCopy = true;
  Copy.Addr = Jump.GotoTarget;
  HighStmt Define;
  Define.Kind = StmtKind::IfElse;
  Define.Cond = parameter(0, Integer);
  Define.Body = {Defined};
  Define.ElseBody = {Copy};
  HighStmt Use;
  Use.Kind = StmtKind::If;
  Use.Cond = parameter(0, Integer);
  Use.Body = {Return};
  Return.RetVal = HighExpr::makeConst(7, 4);
  Function.Body = {Enter, Define, Use, Return};
  EXPECT_FALSE(analyzeHighSourceFlow(Function, true).Items.empty());
  ASSERT_TRUE(eliminateHighDeadPhiCopies(Function));
  EXPECT_EQ(Function.Body[1].ElseBody[0].Addr, Jump.GotoTarget);
  const auto Report = analyzeHighSourceFlow(Function, true);
  ASSERT_TRUE(Report.Complete);
  EXPECT_TRUE(Report.Items.empty());
  compileAndRun(R"(
#if defined(__clang__)
#pragma clang diagnostic error "-Wc2x-extensions"
#endif
)" + emit({Function}) +
                R"(
int main(void) {
    for (int condition = -256; condition <= 256; ++condition)
        if (guarded_value(condition) != (condition ? 42 : 7)) return 1;
    return 0;
}
)");
}

TEST(HighCSourceCalls, ConditionalDefaultPreservesResultsAndCallCounts) {
  const auto Integer = NdType::makeInt(4);
  MedVar Variable;
  Variable.Kind = MedVar::Temp;
  Variable.Id = 10;
  Variable.Size = 4;
  auto Value = HighExpr::makeVar(Variable);
  auto Function = returning("find_or_create", Value, {Integer, Integer});
  auto Return = Function.Body.back();
  Return.Addr = 0x1040;
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = Return.Addr;
  HighStmt Entry;
  Entry.Kind = StmtKind::If;
  Entry.Addr = 0x1000;
  Entry.Cond = HighExpr::makeBinop(NdOp::INT_EQUAL, parameter(0, Integer),
                                   HighExpr::makeConst(0, 4));
  Entry.Body = {Jump};
  Entry.Body[0].GotoTarget = 0x1030;
  HighStmt Lookup;
  Lookup.Kind = StmtKind::Assign;
  Lookup.Addr = 0x1008;
  Lookup.Dst = Value;
  Lookup.Val = call(native("lookup_once", Integer, {Integer}), Integer,
                    {parameter(1, Integer)});
  HighStmt Found;
  Found.Kind = StmtKind::If;
  Found.Addr = 0x1010;
  Found.Cond =
      HighExpr::makeBinop(NdOp::INT_NOTEQUAL, Value, HighExpr::makeConst(0, 4));
  Found.Body = {Jump};
  auto Create = Lookup;
  Create.Addr = 0x1018;
  Create.Val = call(native("create_once", Integer, {}), Integer);
  auto Default = Lookup;
  Default.Addr = 0x1030;
  Default.Val = HighExpr::makeConst(0, 4);
  Jump.Addr = 0x1020;
  Function.Body = {Entry, Lookup, Found, Create, Jump, Default, Return};
  structureIfElse(Function, 10);
  compileAndRun(emit({Function}) + R"(
static unsigned lookups, creations;
int32_t lookup_once(int32_t value) { ++lookups; return value; }
int32_t create_once(void) { ++creations; return 19; }
int main(void) {
    for (int key = -32; key <= 32; ++key) {
        for (int value = -32; value <= 32; ++value) {
            unsigned before_lookup = lookups, before_create = creations;
            if (find_or_create(key, value) != (key ? (value ? value : 19) : 0))
                return 1;
            if (lookups - before_lookup != (key != 0)) return 2;
            if (creations - before_create != (key != 0 && value == 0)) return 3;
        }
    }
    return 0;
}
)");
}

TEST(HighCSourceCalls, SharedNativeEntryExecutesCallAndPhiExactlyOnce) {
  const auto Integer = NdType::makeInt(4);
  MedVar Variable;
  Variable.Kind = MedVar::Temp;
  Variable.Id = 10;
  Variable.Size = 4;
  const auto Local = HighExpr::makeVar(Variable, Integer);
  auto Function = returning("entry_group", Local, {Integer});
  auto Return = Function.Body.back();
  Return.Addr = 0x1300;
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = 0x1220;
  HighStmt Branch;
  Branch.Kind = StmtKind::If;
  Branch.Addr = 0x1200;
  Branch.Cond = parameter(0, Integer);
  Branch.Body = {Jump};
  HighStmt Initial;
  Initial.Kind = StmtKind::Assign;
  Initial.Addr = 0x1204;
  Initial.Dst = Local;
  Initial.Val = HighExpr::makeConst(7, 4);
  HighStmt Skip = Jump;
  Skip.GotoTarget = Return.Addr;
  HighStmt Call;
  Call.Kind = StmtKind::Call;
  Call.Addr = 0x1220;
  Call.CallExpr = call(native("observe_entry", NdType::makeVoid(), {Integer}),
                       NdType::makeVoid(), {HighExpr::makeConst(5, 4)});
  HighStmt Phi = Initial;
  Phi.Addr = Call.Addr;
  Phi.Val = HighExpr::makeConst(42, 4);
  Phi.IsPhiCopy = true;
  Function.Body = {Branch, Initial, Skip, Call, Phi, Return};
  coalesceBranchEntryStatements(Function);
  compileAndRun(emit({Function}) + R"(
static int calls;
void observe_entry(int32_t value) { calls = calls * 10 + value; }
int main(void) {
    if (entry_group(0) != 7 || calls != 0) return 1;
    if (entry_group(1) != 42 || calls != 5) return 2;
    if (entry_group(0) != 7 || calls != 5) return 3;
    return 0;
}
)");
}

TEST(HighCSourceCalls, CallbackDeclaratorsPreserveArgumentsAndReturnTypes) {
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  const auto Callback =
      NdType::makePtr(NdType::makeFunc(NdType::makeVoid(), {Pointer}));
  EXPECT_EQ(typeToC(Callback), "void (*)(void*)");
  EXPECT_EQ(declarationToC(Callback, "fn"), "void (*fn)(void*)");
  EXPECT_EQ(declarationToC(NdType::makePtr(Callback), "slot"),
            "void (**slot)(void*)");
  EXPECT_EQ(
      declarationToC(NdType::makePtr(NdType::makeFunc(Callback)), "factory"),
      "void (*(*factory)(void))(void*)");
  auto Cycle = NdType::makePtr();
  Cycle->Pointee = Cycle;
  EXPECT_THROW(typeToC(Cycle), std::invalid_argument);
  Cycle->Pointee.reset();
  EXPECT_THROW(typeToC(NdType::makePtr(NdType::makeFunc(nullptr))),
               std::invalid_argument);
  auto Identity =
      returning("callback_identity", parameter(0, Callback), {Callback});
  auto Forward =
      returning("callback_forward",
                call(native("callback_identity", Callback, {Callback}),
                     Callback, {parameter(0, Callback)}),
                {Callback});
  auto Apply =
      returning("callback_apply", parameter(1, Pointer), {Callback, Pointer});
  HighStmt Invoke;
  Invoke.Kind = StmtKind::Call;
  Invoke.CallExpr =
      call(native("consume_callback", NdType::makeVoid(), {Callback, Pointer}),
           NdType::makeVoid(), {parameter(0, Callback), parameter(1, Pointer)});
  Apply.Body.insert(Apply.Body.begin(), Invoke);
  const auto Source = emit({Forward, Identity, Apply});
  EXPECT_NE(Source.find("void (*callback_identity(void (*)(void*)))(void*);"),
            std::string::npos);
  EXPECT_EQ(Source.find("bad source call"), std::string::npos);
  auto WrongIdentity = Identity;
  WrongIdentity.Params[0].Type =
      NdType::makePtr(NdType::makeFunc(NdType::makeInt(8), {Pointer}));
  EXPECT_NE(emit({Forward, WrongIdentity})
                .find("bad source call: native parameter disagrees"),
            std::string::npos);
  compileAndRun(Source + R"(
void consume_callback(void (*fn)(void*), void *context) { fn(context); }
static void add(void *p) { ++*(int*)p; }
static void subtract(void *p) { --*(int*)p; }
int main(void) {
  int value = 7;
  for (int i = 0; i < 128; ++i) {
    void (*fn)(void*) = callback_forward(i % 3 ? add : subtract);
    if (fn != (i % 3 ? add : subtract)) return 1;
    int expected = value + (i % 3 ? 1 : -1);
    if (callback_apply(fn, &value) != &value || value != expected) return 2;
  }
  return 0;
}
)");
}

TEST(HighCSourceCalls, RuntimeCallbackPreservesPredicateAndContextOperands) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  Image.Arch = Arch::X64;
  Image.ImportPtrSlots[0x2180] = "_swift_once";
  const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
  ASSERT_TRUE(Hint);
  const auto Pointer = Hint->Signature.Parameters[0].Type;
  const auto Callback = Hint->Signature.Parameters[1].Type;
  auto Forward = returning("once_forward", parameter(2, Pointer),
                           {Pointer, Callback, Pointer});
  HighStmt Invoke;
  Invoke.Kind = StmtKind::Call;
  Invoke.CallExpr = call(
      *Hint, NdType::makeVoid(),
      {parameter(0, Pointer), parameter(1, Callback), parameter(2, Pointer)});
  Forward.Body.insert(Forward.Body.begin(), Invoke);
  const auto Source = emit({Forward});
  EXPECT_NE(
      Source.find("extern void swift_once(void*, void (*)(void*), void*);"),
      std::string::npos);
  EXPECT_EQ(Source.find("bad source call"), std::string::npos);
  // The portable model checks the call boundary. It does not claim to test
  // the runtime's concurrency algorithm or ownership of rebuilt predicates.
  compileAndRun(Source + R"(
static int calls, observed[2];
static uintptr_t predicates[2];
static void initialize(void *context) { ++*(int*)context; }
void swift_once(void *predicate, void (*fn)(void*), void *context) {
  ++calls;
  if (!*(uintptr_t*)predicate) {
    fn(context);
    *(uintptr_t*)predicate = UINTPTR_MAX;
  }
}
int main(void) {
  for (int i = 0; i < 128; ++i) {
    int index = i % 2;
    if (once_forward(&predicates[index], initialize, &observed[index]) !=
        &observed[index]) return 1;
  }
  return calls != 128 || observed[0] != 1 || observed[1] != 1 ||
         predicates[0] != UINTPTR_MAX || predicates[1] != UINTPTR_MAX;
}
)");
}

TEST(HighCSourceCalls, NoncontiguousOrNestedEntriesRemainAmbiguous) {
  for (bool Nested : {false, true}) {
    auto Function = returning("ambiguous_entry", HighExpr::makeConst(1, 4));
    HighStmt Jump;
    Jump.Kind = StmtKind::Goto;
    Jump.GotoTarget = 0x1220;
    HighStmt First;
    First.Kind = StmtKind::Block;
    First.Addr = 0x1220;
    HighStmt Second = First;
    Second.IsPhiCopy = true;
    HighStmt Separator;
    Separator.Kind = StmtKind::Block;
    Separator.Addr = 0x1230;
    if (Nested) {
      Separator.Body.push_back(Second);
      Function.Body = {Jump, First, Separator, Function.Body.back()};
    } else {
      Function.Body = {Jump, First, Separator, Second, Function.Body.back()};
    }
    coalesceBranchEntryStatements(Function);
    unsigned Entries = 0;
    walkStmts(Function.Body, [&](const HighStmt &Statement) {
      Entries += Statement.Addr == 0x1220;
    });
    EXPECT_EQ(Entries, 2U);
  }
}

TEST(HighCSourceCalls, ConditionalEntryEvaluatesBeforeItsTakenEdgeCopies) {
  for (bool ExplicitElse : {false, true}) {
    const auto Integer = NdType::makeInt(4);
    MedVar Variable;
    Variable.Kind = MedVar::Temp;
    Variable.Id = 10;
    Variable.Size = 4;
    auto Local = HighExpr::makeVar(Variable, Integer);
    auto Function = returning("branch_phi_entry", Local, {Integer});
    auto Return = Function.Body.back();
    Return.Addr = 0x1300;
    HighStmt Jump;
    Jump.Kind = StmtKind::Goto;
    Jump.GotoTarget = 0x1200;
    HighStmt Exit = Jump;
    Exit.GotoTarget = Return.Addr;
    HighStmt Copy;
    Copy.Kind = StmtKind::Assign;
    Copy.IsPhiCopy = true;
    Copy.Addr = Jump.GotoTarget;
    Copy.Dst = Local;
    Copy.Val = HighExpr::makeConst(11, 4);
    HighStmt Branch;
    Branch.Kind = ExplicitElse ? StmtKind::IfElse : StmtKind::If;
    Branch.Addr = Jump.GotoTarget;
    Branch.Cond = parameter(0, Integer);
    Branch.Body = {Copy, Exit};
    Copy.Val = HighExpr::makeConst(23, 4);
    Function.Body = {Jump};
    if (ExplicitElse) {
      Branch.ElseBody = {Copy, Exit};
      Function.Body.push_back(Branch);
    } else {
      Function.Body.push_back(Branch);
      Function.Body.push_back(Copy);
    }
    Function.Body.push_back(Return);
    coalesceBranchEntryStatements(Function);
    compileAndRun(emit({Function}) + R"(
int main(void) {
    if (branch_phi_entry(0) != 23) return 1;
    if (branch_phi_entry(1) != 11) return 2;
    if (branch_phi_entry(-1) != 11) return 3;
    return 0;
}
)");
  }
}

TEST(HighCSourceCalls, ConditionalEntryDoesNotHideOtherNestedInstructions) {
  for (bool LaterCopy : {false, true}) {
    auto Function = returning("ambiguous_branch", HighExpr::makeConst(1, 4));
    HighStmt Jump;
    Jump.Kind = StmtKind::Goto;
    Jump.GotoTarget = 0x1200;
    HighStmt Branch;
    Branch.Kind = StmtKind::If;
    Branch.Addr = Jump.GotoTarget;
    Branch.Cond = HighExpr::makeConst(1, 1);
    HighStmt Inner;
    Inner.Kind = StmtKind::Assign;
    Inner.Addr = Branch.Addr;
    Inner.IsPhiCopy = LaterCopy;
    MedVar Local;
    Local.Kind = MedVar::Temp;
    Local.Id = 10;
    Local.Size = 4;
    Inner.Dst = HighExpr::makeVar(Local, NdType::makeInt(4));
    Inner.Val = HighExpr::makeConst(23, 4);
    if (LaterCopy) {
      HighStmt Separator;
      Separator.Kind = StmtKind::Block;
      Separator.Addr = 0x1210;
      Branch.Body.push_back(Separator);
    }
    Branch.Body.push_back(Inner);
    Function.Body = {Jump, Branch, Function.Body.back()};
    coalesceBranchEntryStatements(Function);
    unsigned Entries = 0;
    walkStmts(Function.Body, [&](const HighStmt &Statement) {
      Entries += Statement.Addr == Branch.Addr;
    });
    EXPECT_EQ(Entries, 2U);
  }
}

TEST(HighCSourceCalls,
     NativeForwardPrototypesPreserveFloatingArgumentAndResultBits) {
  std::vector<HighFunc> Functions;
  for (unsigned Width : {4U, 8U}) {
    auto Bits = NdType::makeInt(Width, false);
    auto Float = NdType::makeFloat(Width);
    const std::string Name = "round_trip_" + std::to_string(Width);
    const std::string Helper = "_helper_" + std::to_string(Width);
    Functions.push_back(returning(
        Name, call(native(Helper, Float, {Float}), Bits, {parameter(0, Bits)}),
        {Bits}));
    Functions.push_back(returning(Helper, parameter(0, Float), {Float}));
  }
  const auto Source = emit(Functions);
  EXPECT_EQ(Source.find("extern int helper"), std::string::npos);
  EXPECT_NE(Source.find("float helper_4(float);"), std::string::npos);
  EXPECT_NE(Source.find("double helper_8(double);"), std::string::npos);
  EXPECT_EQ(Source.find("bad source call"), std::string::npos) << Source;
  compileAndRun(Source + R"(
int main(void) {
  const uint32_t a[] = {0,0x80000000U,1,0x3fa00000U,0x7fc00042U,0x7f800000U};
  const uint64_t b[] = {0,0x8000000000000000ULL,1,0x3ff4000000000000ULL,
                        0x7ff8000000000042ULL,0x7ff0000000000000ULL};
  for (unsigned i=0; i<sizeof(a)/sizeof(a[0]); ++i)
    if (round_trip_4(a[i]) != a[i]) return 1;
  for (unsigned i=0; i<sizeof(b)/sizeof(b[0]); ++i)
    if (round_trip_8(b[i]) != b[i]) return 2;
  return 0;
})");
}

TEST(HighCSourceCalls,
     MessageCallCompilesAndExecutesMixedPointerFloatIntegerSignature) {
  auto U64 = NdType::makeInt(8, false);
  auto I32 = NdType::makeInt(4);
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto Double = NdType::makeFloat(8);
  auto Hint = native("objc_msgSend", Double, {Pointer, Pointer, Double, I32});
  Hint.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Hint.Selector = "compute:count:";
  auto Expression = call(Hint, U64,
                         {parameter(0, U64), parameter(1, U64),
                          parameter(2, U64), parameter(3, I32)});
  const auto Source =
      emit({returning("send", Expression, {U64, U64, U64, I32})}, false);
  EXPECT_EQ(Source.find("extern int objc_msgSend"), std::string::npos);
  EXPECT_NE(Source.find("(*)(id, SEL, double, int32_t)"), std::string::npos);
  // A portable dispatch double tests the generated host ABI and exact operands.
  // The separate macOS executable corpus exercises the actual Objective-C
  // runtime.
  compileAndRun(R"(
#include <stdint.h>
typedef void *id;
typedef void *SEL;
static double implementation(id receiver, SEL selector, double value, int32_t count) {
  return (receiver == (id)(uintptr_t)0x1234 && selector == (SEL)(uintptr_t)0x5678)
          ? value + count : -999;
}
static double (*objc_msgSend)(id, SEL, double, int32_t) = implementation;
)" + Source + R"(
int main(void) {
  for (int32_t count=-3; count<4; ++count) {
    double input=1.25, expected=input+count;
    uint64_t bits=__builtin_bit_cast(uint64_t,input);
    if (send(0x1234,0x5678,bits,count) != __builtin_bit_cast(uint64_t,expected)) return 1;
  }
  return 0;
})");
}

TEST(HighCSourceCalls, RuntimeImportsCompileAndPreserveObjectAndVoidEffects) {
  auto U64 = NdType::makeInt(8, false);
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto Retain = native("objc_retain", Pointer, {Pointer});
  Retain.CallKind = SourceCallTypeHint::Kind::ObjCRuntimeCall;
  Retain.TargetAddress = 0xdeadbeef;
  auto Release = native("objc_release", NdType::makeVoid(), {Pointer});
  Release.CallKind = SourceCallTypeHint::Kind::ObjCRuntimeCall;
  Release.TargetAddress = 0xdeadbef7;
  auto Function = returning("retain_and_release",
                            call(Retain, U64, {parameter(0, U64)}), {U64});
  HighStmt Statement;
  Statement.Kind = StmtKind::Call;
  Statement.CallExpr = call(Release, NdType::makeVoid(), {parameter(0, U64)});
  Function.Body.insert(Function.Body.begin(), Statement);
  const auto Source = emit({Function});
  EXPECT_EQ(Source.find("bad source call"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("DEADBEEF"), std::string::npos);
  EXPECT_NE(Source.find("void* objc_retain(void*);"), std::string::npos);
  EXPECT_NE(Source.find("void objc_release(void*);"), std::string::npos);
  compileAndRun(Source + R"(
static void *last_released;
static int retained, released;
void *objc_retain(void *object) { ++retained; return object; }
void objc_release(void *object) { ++released; last_released = object; }
int main(void) {
  const uintptr_t values[] = {0, 0x12345678, UINT64_C(0xfedcba9876543210)};
  for (unsigned i = 0; i < 3; ++i)
    if (retain_and_release(values[i]) != values[i] ||
        (uintptr_t)last_released != values[i]) return 1;
  return retained != 3 || released != 3;
})");
}

TEST(HighCSourceCalls, RuntimeWeakCallsUsePublicObjectStorageTypes) {
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto Storage = NdType::makePtr(Pointer);
  auto Store = native("objc_storeWeak", Pointer, {Storage, Pointer});
  Store.CallKind = SourceCallTypeHint::Kind::ObjCRuntimeCall;
  auto Load = native("objc_loadWeak", Pointer, {Storage});
  Load.CallKind = SourceCallTypeHint::Kind::ObjCRuntimeCall;
  const std::vector<HighFunc> Functions{
      returning(
          "weak_store",
          call(Store, Pointer, {parameter(0, Storage), parameter(1, Pointer)}),
          {Storage, Pointer}),
      returning("weak_load", call(Load, Pointer, {parameter(0, Storage)}),
                {Storage})};
  const auto Source = emit(Functions);
  EXPECT_NE(Source.find("#include <objc/runtime.h>"), std::string::npos);
  EXPECT_EQ(Source.find("extern void* objc_storeWeak"), std::string::npos);
  EXPECT_EQ(Source.find("extern void* objc_loadWeak"), std::string::npos);
  compileAndRun(R"(
#include <stdint.h>
typedef struct objc_object *id;
id objc_storeWeak(id *location, id object) { *location = object; return object; }
id objc_loadWeak(id *location) { return *location; }
)" + emit(Functions, false) +
                R"(
int main(void) {
  id slot = 0;
  id value = (id)(uintptr_t)1234;
  if (weak_store((void **)&slot, value) != value || weak_load((void **)&slot) != value) return 1;
  return weak_store((void **)&slot, 0) != 0 || weak_load((void **)&slot) != 0;
}
)");
}

TEST(HighCSourceCalls, RuntimeReferencesUseEscapedNamesAndNoOriginalAddresses) {
  auto U64 = NdType::makeInt(8, false);
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  std::vector<HighFunc> Functions;
  const SourceCallTypeHint::Kind Kinds[] = {
      SourceCallTypeHint::Kind::RuntimeSelector,
      SourceCallTypeHint::Kind::RuntimeClass,
      SourceCallTypeHint::Kind::RuntimeMetaclass,
      SourceCallTypeHint::Kind::RuntimeIvarOffset};
  for (size_t I = 0; I < 4; ++I) {
    auto Hint =
        native("quote\"slash\\line\n\?\?/9", I == 3 ? U64 : Pointer, {});
    Hint.CallKind = Kinds[I];
    Hint.OwnerClass = "Owner";
    Hint.TargetAddress = 0xdeadbeef;
    Functions.push_back(
        returning("reference_" + std::to_string(I), call(Hint, U64)));
  }
  const auto Source = emit(Functions, false);
  EXPECT_EQ(Source.find("DEADBEEF"), std::string::npos);
  EXPECT_EQ(Source.find("extern int"), std::string::npos);
  EXPECT_NE(Source.find("\\012\\?\\?/9"), std::string::npos);
  compileAndRun(R"(
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef void *Class;
static const char wanted[]="quote\"slash\\line\n\?\?/9";
static void *sel_registerName(const char *s) { return (void *)(uintptr_t)(strcmp(s,wanted)==0 ? 11 : 0); }
static void *objc_getClass(const char *s) { return (void *)(uintptr_t)(strcmp(s,"Owner")==0 ? 31 : strcmp(s,wanted)==0 ? 12 : 0); }
static void *objc_getMetaClass(const char *s) { return (void *)(uintptr_t)(strcmp(s,wanted)==0 ? 13 : 0); }
static void *class_getInstanceVariable(Class c,const char *s) { return (void *)(uintptr_t)(c==(void *)(uintptr_t)31 && strcmp(s,wanted)==0 ? 32 : 0); }
static ptrdiff_t ivar_getOffset(void *v) { return v==(void *)(uintptr_t)32 ? 24 : -1; }
)" + Source + R"(
int main(void) { return reference_0()!=11 || reference_1()!=12 || reference_2()!=13 || reference_3()!=24; }
)");
}

TEST(HighCSourceCalls, RejectsWrongArgumentCountWidthAndIncompleteBlocks) {
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto I32 = NdType::makeInt(4);
  auto Hint = native("objc_msgSend", I32, {Pointer, Pointer, I32});
  Hint.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Hint.Selector = "step:";
  auto Missing = call(Hint, I32, {HighExpr::makeConst(0, 8)});
  EXPECT_NE(emit({returning("missing", Missing)}, false)
                .find("bad source call: argument count"),
            std::string::npos);
  auto WrongWidth = call(Hint, I32,
                         {HighExpr::makeConst(0, 8), HighExpr::makeConst(0, 8),
                          HighExpr::makeConst(1, 8)});
  EXPECT_NE(emit({returning("width", WrongWidth)}, false)
                .find("bad source call: argument carrier"),
            std::string::npos);
  Hint.CallKind = SourceCallTypeHint::Kind::BlockInvoke;
  EXPECT_NE(emit({returning("block", call(Hint, I32,
                                          {HighExpr::makeConst(0, 8),
                                           HighExpr::makeConst(0, 8),
                                           HighExpr::makeConst(1, 4)}))},
                 false)
                .find("bad source call: block invocation"),
            std::string::npos);
}

TEST(HighCSourceCalls,
     Super2UsesTheCurrentClassRecordAndAnExplicitRuntimeDeclaration) {
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto I32 = NdType::makeInt(4);
  auto Hint = native("objc_msgSendSuper2", I32, {Pointer, Pointer, I32});
  Hint.CallKind = SourceCallTypeHint::Kind::ObjCSuper2;
  Hint.Selector = "step:";
  auto Expr = call(Hint, I32,
                   {HighExpr::makeConst(0, 8), HighExpr::makeConst(0, 8),
                    HighExpr::makeConst(1, 4)});
  auto Source = emit({returning("super_send", Expr)});
  EXPECT_NE(Source.find("#include <objc/message.h>"), std::string::npos);
  EXPECT_NE(Source.find("extern void objc_msgSendSuper2(void);"),
            std::string::npos);
  EXPECT_NE(Source.find("(*)(struct objc_super *, SEL, int32_t)"),
            std::string::npos);
  EXPECT_EQ(Source.find("extern int objc_msgSend"), std::string::npos);
}

TEST(HighCSourceCalls, NativeTargetAddressSurvivesProjectionRenaming) {
  auto Type = NdType::makeInt(4);
  auto Hint = native("original_symbol", Type, {Type});
  Hint.TargetAddress = 0x1400;
  auto Caller =
      returning("invoke", call(Hint, Type, {parameter(0, Type)}), {Type});
  Caller.Entry = 0x1300;
  auto Helper = returning("neverd_objc_imp_1400", parameter(0, Type), {Type});
  Helper.Entry = 0x1400;
  const auto Source = emit({Caller, Helper});
  EXPECT_EQ(Source.find("original_symbol"), std::string::npos);
  EXPECT_NE(Source.find("int32_t neverd_objc_imp_1400(int32_t);"),
            std::string::npos);
  compileAndRun(Source + "int main(void) { return invoke(-37) != -37; }\n");
}

TEST(HighCSourceCalls,
     TypedBlockDispatchExecutesCapturedIntegerAndMixedFloatValues) {
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto I32 = NdType::makeInt(4);
  auto U64 = NdType::makeInt(8, false);
  auto F64 = NdType::makeFloat(8);
  auto Integer = native("", I32, {Pointer, I32});
  Integer.CallKind = SourceCallTypeHint::Kind::BlockInvoke;
  Integer.Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  auto Floating = native("", F64, {Pointer, F64, I32});
  Floating.CallKind = SourceCallTypeHint::Kind::BlockInvoke;
  Floating.Signature.Origin = SourceFunctionTypeHint::OriginKind::BlockRuntime;
  auto A =
      returning("integer_block",
                call(Integer, I32, {parameter(0, Pointer), parameter(1, I32)}),
                {Pointer, I32});
  auto B = returning(
      "floating_block",
      call(Floating, U64,
           {parameter(0, Pointer), parameter(1, U64), parameter(2, I32)}),
      {Pointer, U64, I32});
  auto Source = emit({A, B});
  EXPECT_EQ(Source.find("bad source call"), std::string::npos);
  compileAndRun(Source + R"(
void *_NSConcreteStackBlock[32], *_NSConcreteGlobalBlock[32];
int main(void) {
  int captured = 17;
  int (^integer)(int) = ^(int value) { return (int)((unsigned)value * 3u + (unsigned)captured); };
  double (^floating)(double,int) = ^(double a,int b) { return a * 2.0 + b; };
  unsigned cases[] = {0,1,0x80,0xff,0x80000001u,0xffffffffu};
  for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
    int x=(int)cases[i];
    if (integer_block((void *)integer,x)!=integer(x)) return 1;
    double a=(double)(int)i - 2.25;
    uint64_t bits=__builtin_bit_cast(uint64_t,a);
    uint64_t actual=floating_block((void *)floating,bits,x);
    if (__builtin_bit_cast(double,actual)!=floating(a,x)) return 2;
  }
  return 0;
}
)");
}

TEST(HighCSourceCalls, BlockReceiverIsEvaluatedExactlyOnce) {
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto I32 = NdType::makeInt(4);
  auto Hint = native("", I32, {Pointer, I32});
  Hint.CallKind = SourceCallTypeHint::Kind::BlockInvoke;
  Hint.Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  auto Next = call(native("next_block", Pointer, {}), Pointer);
  auto Source = emit({returning(
      "invoke_once", call(Hint, I32, {Next, parameter(0, I32)}), {I32})});
  compileAndRun(Source + R"(
void *_NSConcreteGlobalBlock[32];
static unsigned calls;
void *next_block(void) {
  ++calls;
  return (void *)^(int value) { return value + 7; };
}
int main(void) { return invoke_once(5)!=12 || calls!=1; }
)");
}

TEST(HighCSourceCalls,
     BlockSourceAddressesUseActualDefinitionsAndBoundHelpers) {
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto U64 = NdType::makeInt(8, false);
  auto I32 = NdType::makeInt(4);
  std::vector<HighFunc> Functions;
  auto Address = native("original_untrusted_name", Pointer, {});
  Address.CallKind = SourceCallTypeHint::Kind::NativeAddress;
  Address.TargetAddress = 0xabc;
  Functions.push_back(returning("get_invoke", call(Address, U64)));
  auto Definition =
      returning("neverd_block_invoke_abc", parameter(0, I32), {I32});
  Definition.Entry = 0xabc;
  Functions.push_back(Definition);
  Address.CallKind = SourceCallTypeHint::Kind::RuntimeBlockIsa;
  Address.TargetName = "__NSConcreteStackBlock";
  Functions.push_back(returning("get_isa", call(Address, U64)));
  Address.CallKind = SourceCallTypeHint::Kind::RuntimeBlockDescriptor;
  Address.TargetAddress = 0xdead;
  Functions.push_back(returning("get_descriptor", call(Address, U64)));
  Address.CallKind = SourceCallTypeHint::Kind::RuntimeBlockLiteral;
  Address.TargetAddress = 0xbeef;
  Functions.push_back(returning("get_literal", call(Address, U64)));
  auto Source = emit(Functions);
  EXPECT_EQ(Source.find("original_untrusted_name"), std::string::npos);
  EXPECT_NE(Source.find("int32_t neverd_block_invoke_abc(int32_t);"),
            std::string::npos);
  compileAndRun(Source + R"(
void *_NSConcreteStackBlock[32];
static int descriptor, literal;
uintptr_t neverd_block_descriptor_dead_address(void) { return (uintptr_t)&descriptor; }
uintptr_t neverd_block_literal_beef_address(void) { return (uintptr_t)&literal; }
int main(void) {
  if (((int32_t (*)(int32_t))get_invoke())(-71)!=-71) return 1;
  if (get_isa()!=(uintptr_t)_NSConcreteStackBlock) return 2;
  return get_descriptor()!=(uintptr_t)&descriptor || get_literal()!=(uintptr_t)&literal;
}
)");
}

TEST(HighCSourceCalls,
     UnknownOrOperandBearingSourceAddressesRemainExplicitFailures) {
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto Hint = native("_NSConcreteStackBlock_extra", Pointer, {});
  Hint.CallKind = SourceCallTypeHint::Kind::RuntimeBlockIsa;
  EXPECT_NE(emit({returning("bad_isa", call(Hint, Pointer))})
                .find("bad source call: unknown concrete"),
            std::string::npos);
  Hint.CallKind = SourceCallTypeHint::Kind::NativeAddress;
  Hint.TargetAddress = 0x1000;
  EXPECT_NE(emit({returning("missing", call(Hint, Pointer))})
                .find("bad source call: native address"),
            std::string::npos);
  Hint.CallKind = SourceCallTypeHint::Kind::RuntimeBlockDescriptor;
  EXPECT_NE(emit({returning("args",
                            call(Hint, Pointer, {HighExpr::makeConst(0, 8)}))})
                .find("bad source call: invalid source address"),
            std::string::npos);
}
} // namespace
