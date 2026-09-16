//===- HighCPointerAddressTests.cpp - Raw byte address projection --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/Common.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>
#include <string>
#include <vector>

namespace {
using namespace neverd;

ExprPtr parameter(unsigned Id, TypeRef ExprType = NdType::makeInt(8)) {
  MedVar Var;
  Var.Kind = MedVar::Param;
  Var.Id = static_cast<int>(Id);
  Var.Size = 8;
  Var.TheArch = Arch::X64;
  return HighExpr::makeVar(Var, ExprType);
}

void returnValue(HighFunc &Func, ExprPtr Value) {
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = std::move(Value);
  Func.Body.push_back(std::move(Return));
}

HighFunc pointerFunction(const char *Name, TypeRef ReturnType) {
  HighFunc Func;
  Func.Name = Name;
  Func.ReturnType = ReturnType;
  Func.Params = {{"arg0", NdType::makePtr(NdType::makeInt(4))},
                 {"arg1", NdType::makeInt(8, false)}};
  return Func;
}

ExprPtr byteOffset(ExprPtr Base = parameter(0)) {
  return HighExpr::makeBinop(NdOp::INT_ADD, Base,
                             HighExpr::makeBinop(NdOp::INT_MULT, parameter(1),
                                                 HighExpr::makeConst(4, 8)));
}

std::string emitFunctions(const std::vector<HighFunc> &Functions,
                          Arch TheArch = Arch::X64) {
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = TheArch;
  EXPECT_TRUE(HighCEmitter().emit(Functions, OS, Options));
  OS.flush();
  return Source;
}

TEST(HighCPointerAddresses, TypedAndMachineWidthParametersUseByteOffsets) {
  for (Arch TheArch : {Arch::X64, Arch::AArch64}) {
    for (bool TypedExpr : {false, true}) {
      SCOPED_TRACE(static_cast<int>(TheArch));
      SCOPED_TRACE(TypedExpr);
      auto Func = pointerFunction("indexed_load", NdType::makeInt(4));
      auto Base =
          parameter(0, TypedExpr ? Func.Params[0].Type : NdType::makeInt(8));
      returnValue(Func, HighExpr::makeLoad(byteOffset(Base), Func.ReturnType));
      const std::string Source = emitFunctions({Func}, TheArch);
      EXPECT_NE(Source.find("int32_t* arg0"), std::string::npos) << Source;
      EXPECT_NE(Source.find("(uintptr_t)arg0"), std::string::npos) << Source;
      EXPECT_NE(Source.find(" * "), std::string::npos) << Source;
      EXPECT_EQ(Source.find("(uintptr_t)(arg0 +"), std::string::npos) << Source;
    }
  }
}

TEST(HighCPointerAddresses, CastsPointerReturnsAfterMachineArithmetic) {
  auto Func = pointerFunction("advance", NdType::makePtr(NdType::makeInt(4)));
  returnValue(Func, byteOffset());
  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("return (int32_t*)(uintptr_t)("), std::string::npos)
      << Source;

  auto Identity = pointerFunction("identity", Func.ReturnType);
  returnValue(Identity, parameter(0));
  const std::string IdentitySource = emitFunctions({Identity});
  EXPECT_NE(IdentitySource.find("return arg0;"), std::string::npos)
      << IdentitySource;
  EXPECT_EQ(IdentitySource.find("(uintptr_t)arg0"), std::string::npos)
      << IdentitySource;
}

TEST(HighCPointerAddresses, PreservesParameterLvaluesAndAddressOf) {
  auto Func = pointerFunction("assign_then_load", NdType::makeInt(4));
  HighStmt Assign;
  Assign.Kind = StmtKind::Assign;
  Assign.Dst = parameter(0);
  Assign.Val = byteOffset();
  Func.Body.push_back(std::move(Assign));
  returnValue(Func, HighExpr::makeLoad(parameter(0), Func.ReturnType));
  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("arg0 = (int32_t*)(uintptr_t)("), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("(uintptr_t)arg0 ="), std::string::npos) << Source;

  auto Address =
      pointerFunction("address_of", NdType::makePtr(Func.Params[0].Type));
  auto Addr = std::make_shared<HighExpr>();
  Addr->Kind = ExprKind::Addr;
  Addr->Type = Address.ReturnType;
  Addr->Operands.push_back(parameter(0));
  returnValue(Address, Addr);
  const std::string AddressSource = emitFunctions({Address});
  EXPECT_NE(AddressSource.find("return &arg0;"), std::string::npos)
      << AddressSource;
  EXPECT_EQ(AddressSource.find("&(uintptr_t)"), std::string::npos)
      << AddressSource;
}

TEST(HighCPointerAddresses, DoesNotRetypeIntegerParametersOrRenamedLocals) {
  auto Func = pointerFunction("integer_address", NdType::makeInt(8));
  Func.Params[0].Type = NdType::makeInt(8);
  returnValue(Func, byteOffset());
  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("int64_t integer_address(int64_t arg0, uint64_t arg1)"),
            std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("(uintptr_t)arg0"), std::string::npos) << Source;

  auto Local = pointerFunction("renamed_local", NdType::makeInt(8));
  auto Renamed = parameter(0);
  Renamed->Var.RenameTag = 3;
  HighStmt Assign;
  Assign.Kind = StmtKind::Assign;
  Assign.Dst = Renamed;
  Assign.Val = HighExpr::makeConst(12, 8);
  Local.Body.push_back(std::move(Assign));
  returnValue(Local, Renamed);
  const std::string LocalSource = emitFunctions({Local});
  EXPECT_NE(LocalSource.find("return v3;"), std::string::npos) << LocalSource;
  EXPECT_EQ(LocalSource.find("(uintptr_t)v3"), std::string::npos)
      << LocalSource;
}

TEST(HighCPointerAddresses, EmittedCExecutesByteLoadsStoresAndPointerResults) {
  const auto I32 = NdType::makeInt(4);
  const auto I32Ptr = NdType::makePtr(I32);
  std::vector<HighFunc> Functions;

  auto Load = pointerFunction("indexed_load", I32);
  returnValue(Load, HighExpr::makeLoad(byteOffset(), I32));
  Functions.push_back(std::move(Load));

  auto Advance = pointerFunction("advance", I32Ptr);
  returnValue(Advance, byteOffset());
  Functions.push_back(std::move(Advance));

  auto Identity = pointerFunction("identity", I32Ptr);
  returnValue(Identity, parameter(0));
  Functions.push_back(std::move(Identity));

  auto Difference = pointerFunction("difference", NdType::makeInt(8));
  Difference.Params[1].Type = I32Ptr;
  returnValue(Difference,
              HighExpr::makeBinop(NdOp::INT_SUB, parameter(0), parameter(1)));
  Functions.push_back(std::move(Difference));

  auto Mask = pointerFunction("address_mask", NdType::makeInt(8, false));
  returnValue(Mask, HighExpr::makeBinop(NdOp::INT_AND, parameter(0),
                                        HighExpr::makeConst(255, 8)));
  Functions.push_back(std::move(Mask));

  auto Store = pointerFunction("indexed_store", I32);
  HighStmt Write;
  Write.Kind = StmtKind::Store;
  Write.StoreAddr = byteOffset();
  Write.StoreVal = HighExpr::makeConst(91, 4);
  Write.MemoryOrdering = NdMemoryOrdering::Relaxed;
  Store.Body.push_back(std::move(Write));
  returnValue(Store, HighExpr::makeConst(0, 4));
  Functions.push_back(std::move(Store));

  auto Assign = pointerFunction("assign_then_load", I32);
  HighStmt Change;
  Change.Kind = StmtKind::Assign;
  Change.Dst = parameter(0);
  Change.Val = byteOffset();
  Assign.Body.push_back(std::move(Change));
  returnValue(Assign, HighExpr::makeLoad(parameter(0), I32));
  Functions.push_back(std::move(Assign));

  for (unsigned Form = 0; Form < 3; ++Form) {
    auto PointerStore = pointerFunction("pointer_store", I32Ptr);
    PointerStore.Name += std::to_string(Form);
    PointerStore.Params = {{"arg0", NdType::makePtr(I32Ptr)}, {"arg1", I32Ptr}};
    HighStmt WritePointer;
    auto Address = parameter(0, PointerStore.Params[0].Type);
    auto Value = parameter(1, I32Ptr);
    if (Form == 0) {
      WritePointer.Kind = StmtKind::Store;
      WritePointer.StoreAddr = Address;
      WritePointer.StoreVal = Value;
      WritePointer.MemoryOrdering = NdMemoryOrdering::Release;
    } else if (Form == 1) {
      WritePointer.Kind = StmtKind::Assign;
      WritePointer.Dst = HighExpr::makeLoad(Address, I32Ptr);
      WritePointer.Val = Value;
    } else {
      WritePointer.Kind = StmtKind::ExprStmt;
      WritePointer.Val = std::make_shared<HighExpr>();
      WritePointer.Val->Kind = ExprKind::Store;
      WritePointer.Val->Type = I32Ptr;
      WritePointer.Val->Operands = {Address, Value};
    }
    PointerStore.Body.push_back(std::move(WritePointer));
    returnValue(PointerStore, HighExpr::makeLoad(Address, I32Ptr));
    Functions.push_back(std::move(PointerStore));
  }

  std::string Source = emitFunctions(Functions);
  Source += R"(
int main(void) {
    int32_t values[16] = {3, 17, 29, 41, 53, 67, 79, 83};
    if (indexed_load(values, 1) != 17) return 1;
    if (advance(values, 2) != &values[2]) return 2;
    if (identity(values, 0) != values) return 3;
    if (difference(&values[3], &values[1]) != 8) return 4;
    if (address_mask(values, 0) != ((uintptr_t)values & 255)) return 5;
    indexed_store(values, 1);
    if (values[1] != 91 || values[4] != 53) return 6;
    if (assign_then_load(values, 2) != 29) return 7;
    int32_t *slot = 0;
    if (pointer_store0(&slot, &values[3]) != &values[3] || slot != &values[3]) return 8;
    if (pointer_store1(&slot, &values[7]) != &values[7] || slot != &values[7]) return 9;
    if (pointer_store2(&slot, &values[2]) != &values[2] || slot != &values[2]) return 10;
    if (pointer_store0(&slot, 0) || slot) return 11;
    if (pointer_store1(&slot, 0) || slot) return 12;
    if (pointer_store2(&slot, 0) || slot) return 13;
    if (advance(&values[2], UINT64_MAX) != &values[1]) return 14;
    if (advance(values, (UINT64_C(1) << 62) + 1) != &values[1]) return 15;
    if (difference(&values[1], &values[3]) != -8) return 16;
    return 0;
}
)";

#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  auto FoundCompiler = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(static_cast<bool>(FoundCompiler)) << "clang is required";
  const std::string Compiler = *FoundCompiler;
#endif
  llvm::SmallString<128> SourcePath, ExecutablePath, ErrorPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-pointer-values", "c",
                                                  SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-pointer-values",
                                                  "exe", ExecutablePath));
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-pointer-values",
                                                  "err", ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code EC;
  {
    llvm::raw_fd_ostream OS(SourcePath, EC);
    ASSERT_FALSE(EC) << EC.message();
    OS << Source;
  }
  llvm::SmallVector<llvm::StringRef, 12> Arguments{
      Compiler,
      "-std=c11",
      "-O1",
      "-Werror=int-conversion",
      "-Werror=incompatible-pointer-types",
      "-fsanitize=signed-integer-overflow",
      "-fsanitize-trap=signed-integer-overflow",
      SourcePath,
      "-o",
      ExecutablePath};
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  std::string Error;
  const int CompileStatus = llvm::sys::ExecuteAndWait(
      Compiler, Arguments, std::nullopt, Redirects, 30, 0, &Error);
  auto ErrorBuffer = llvm::MemoryBuffer::getFile(ErrorPath);
  ASSERT_EQ(CompileStatus, 0)
      << Error << (ErrorBuffer ? (*ErrorBuffer)->getBuffer().str() : "") << "\n"
      << Source;
  llvm::SmallVector<llvm::StringRef, 1> RunArguments{ExecutablePath};
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, RunArguments,
                                      std::nullopt, {}, 30, 0, &Error),
            0)
      << Error << "\n"
      << Source;
}

TEST(HighCPointerAddresses, BytePointersKeepCPointerArithmetic) {
  auto Func = pointerFunction("byte_load", NdType::makeInt(1, false));
  Func.Params[0].Type = NdType::makePtr(NdType::makeInt(1, false));
  returnValue(Func, HighExpr::makeLoad(
                        HighExpr::makeBinop(NdOp::INT_ADD,
                                            parameter(0, Func.Params[0].Type),
                                            parameter(1)),
                        Func.ReturnType));
  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("uint8_t* arg0"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("(uintptr_t)arg0"), std::string::npos) << Source;
  EXPECT_NE(Source.find("*(uint8_t *)(arg0 +"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, FrameSlotsRenderAsNamedLocalsAndAddressOf) {
  const auto I32 = NdType::makeInt(4);
  HighFunc Func;
  Func.Name = "frame_slot";
  Func.FrameSize = 16;
  Func.ReturnType = NdType::makePtr(I32);
  MedVar SP;
  SP.Kind = MedVar::Reg;
  SP.Size = 8;
  SP.TheArch = Arch::X64;
  SP.RegOff = getTargetRegInfo(Arch::X64).StackPointer;
  auto SlotAddr = HighExpr::makeBinop(
      NdOp::INT_SUB, HighExpr::makeVar(SP, NdType::makeInt(8, false)),
      HighExpr::makeConst(8, 8));
  HighStmt Init;
  Init.Kind = StmtKind::Store;
  Init.StoreAddr = SlotAddr;
  Init.StoreVal = HighExpr::makeConst(7, 4);
  Func.Body.push_back(std::move(Init));
  auto Addr = std::make_shared<HighExpr>();
  Addr->Kind = ExprKind::Addr;
  Addr->Type = Func.ReturnType;
  Addr->Operands.push_back(HighExpr::makeLoad(SlotAddr, I32));
  returnValue(Func, Addr);
  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("int32_t var_m8;"), std::string::npos) << Source;
  EXPECT_NE(Source.find("var_m8 = 7;"), std::string::npos) << Source;
  EXPECT_NE(Source.find("return &var_m8;"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("stack_storage"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("neverd_mem_"), std::string::npos) << Source;
}

BinaryImage makeImageObjectFixture(va_t Addr, std::vector<uint8_t> Bytes,
                                   bool Writable) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Segment Seg;
  Seg.Name = Writable ? ".data" : ".rdata";
  Seg.VA = Addr;
  Seg.Size = Bytes.size();
  Seg.Flags = SegmentFlags::Readable;
  if (Writable)
    Seg.Flags = Seg.Flags | SegmentFlags::Writable;
  Seg.Data = std::move(Bytes);
  Img.Segments.push_back(std::move(Seg));
  return Img;
}

TEST(HighCPointerAddresses, FoldsReadonlyImageIntegerLoad) {
  BinaryImage Img =
      makeImageObjectFixture(0x140003260, {0x01, 0x10, 0x42, 0xE0}, false);
  HighFunc Func;
  Func.Name = "load_code";
  Func.ReturnType = NdType::makeInt(4, false);
  returnValue(Func, HighExpr::makeLoad(HighExpr::makeConst(0x140003260, 8),
                                       Func.ReturnType));
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::COFF;
  Options.Image = &Img;
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Options));
  OS.flush();
  EXPECT_NE(Source.find("0xE0421001"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("0x140003260"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, NamesWritableImageDataStore) {
  BinaryImage Img =
      makeImageObjectFixture(0x1400050E0, {0, 0, 0, 0}, true);
  HighFunc Func;
  Func.Name = "store_sink";
  Func.ReturnType = NdType::makeVoid();
  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.StoreAddr = HighExpr::makeConst(0x1400050E0, 8);
  Store.StoreVal = HighExpr::makeConst(41, 4);
  Store.StoreVal->Type = NdType::makeInt(4, true);
  Func.Body.push_back(std::move(Store));
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::COFF;
  Options.Image = &Img;
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Options));
  OS.flush();
  EXPECT_NE(Source.find("int32_t g_1400050E0;"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("g_1400050E0 = 41;"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("*(int32_t *)(0x1400050E0)"), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("dword_"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("data_1400050E0"), std::string::npos) << Source;
}

TEST(LLVMCPointerAddresses, NamesWritableNdDataGlobal) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-data", Context);
  llvm::Type *I32 = llvm::Type::getInt32Ty(Context);
  auto *GV = new llvm::GlobalVariable(
      Module, I32, /*isConstant=*/false, llvm::GlobalValue::ExternalLinkage,
      nullptr, makeNdDataSymbol(0x1400050E0));
  llvm::FunctionType *FnTy =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  llvm::Function *Function = llvm::Function::Create(
      FnTy, llvm::GlobalValue::ExternalLinkage, "store_sink", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  Builder.CreateStore(llvm::ConstantInt::get(I32, 41), GV);
  Builder.CreateRetVoid();

  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  ASSERT_TRUE(LLVMCEmitter().emit(Module, OS, Options));
  OS.flush();
  EXPECT_NE(Source.find("g_1400050E0"), std::string::npos) << Source;
  EXPECT_NE(Source.find("extern uint32_t g_1400050E0;"), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("data_1400050E0"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("dword_"), std::string::npos) << Source;
}

TEST(LLVMCPointerAddresses, DeclaresAssignedTempsAndUnusedCallResults) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-locals", Context);
  llvm::Type *I32 = llvm::Type::getInt32Ty(Context);
  llvm::Type *I64 = llvm::Type::getInt64Ty(Context);
  llvm::FunctionType *FnTy = llvm::FunctionType::get(I32, {I32}, false);
  llvm::Function *Function = llvm::Function::Create(
      FnTy, llvm::GlobalValue::ExternalLinkage, "probe_like", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  llvm::Value *Code = Builder.CreateAdd(
      Function->getArg(0), llvm::ConstantInt::get(I32, 0xE0421001), "t22");
  llvm::FunctionType *RaiseTy = llvm::FunctionType::get(
      I64, {I32, I32, I32, I64}, false);
  llvm::Function *Raise = llvm::Function::Create(
      RaiseTy, llvm::GlobalValue::ExternalLinkage, "RaiseException", Module);
  Builder.CreateCall(Raise,
                     {Code, llvm::ConstantInt::get(I32, 0),
                      llvm::ConstantInt::get(I32, 0),
                      llvm::ConstantInt::get(I64, 0)},
                     "v36");
  Builder.CreateMul(Function->getArg(0), llvm::ConstantInt::get(I32, 3),
                    "dead_flag");
  Builder.CreateRet(Code);

  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  ASSERT_TRUE(LLVMCEmitter().emit(Module, OS, Options));
  OS.flush();
  EXPECT_NE(Source.find("uint32_t t22"), std::string::npos) << Source;
  EXPECT_NE(Source.find("uint64_t v36"), std::string::npos) << Source;
  EXPECT_NE(Source.find("uint32_t dead_flag"), std::string::npos) << Source;
  EXPECT_NE(Source.find("RaiseException"), std::string::npos) << Source;
  const size_t DeadAssign = Source.find("dead_flag");
  ASSERT_NE(DeadAssign, std::string::npos) << Source;
  EXPECT_NE(Source.find(" = ", DeadAssign), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, UsesDebugDataObjectName) {
  BinaryImage Img =
      makeImageObjectFixture(0x1400050E0, {0, 0, 0, 0}, true);
  class NamedDataDbg : public NullDebugContext {
  public:
    std::vector<DataObjectSym> allDataObjects() const override {
      return {{"ProbeSink", 0x1400050E0, 4, false}};
    }
    bool hasInfo() const override { return true; }
  } Dbg;
  HighFunc Func;
  Func.Name = "store_sink";
  Func.ReturnType = NdType::makeVoid();
  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.StoreAddr = HighExpr::makeConst(0x1400050E0, 8);
  Store.StoreVal = HighExpr::makeConst(41, 4);
  Store.StoreVal->Type = NdType::makeInt(4, true);
  Func.Body.push_back(std::move(Store));
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::COFF;
  Options.Image = &Img;
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Options, &Dbg));
  OS.flush();
  EXPECT_NE(Source.find("int32_t ProbeSink;"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("ProbeSink = 41;"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, DeclaresAssignedTempsAndUnusedCallResults) {
  HighFunc Func;
  Func.Name = "probe_like";
  Func.ReturnType = NdType::makeInt(4);
  Func.Params = {{"arg0", NdType::makeInt(4)}};

  MedVar Temp;
  Temp.Kind = MedVar::Temp;
  Temp.Id = 22;
  Temp.SSAVer = 1;
  Temp.Size = 4;
  Temp.TheArch = Arch::X64;

  MedVar CallDest;
  CallDest.Kind = MedVar::Reg;
  CallDest.Id = 36;
  CallDest.SSAVer = 0;
  CallDest.Size = 8;
  CallDest.TheArch = Arch::X64;

  MedVar PhiDest;
  PhiDest.Kind = MedVar::Reg;
  PhiDest.Id = 3;
  PhiDest.SSAVer = 0;
  PhiDest.Size = 8;
  PhiDest.TheArch = Arch::X64;

  HighStmt LoadCode;
  LoadCode.Kind = StmtKind::Assign;
  LoadCode.Dst = HighExpr::makeVar(Temp, NdType::makeInt(4, false));
  LoadCode.Val = HighExpr::makeConst(0xE0421001, 4);

  HighStmt Call;
  Call.Kind = StmtKind::Assign;
  Call.Dst = HighExpr::makeVar(CallDest, NdType::makeInt(8));
  Call.Val = HighExpr::makeCall(
      "RaiseException", 0,
      {HighExpr::makeVar(Temp, NdType::makeInt(4, false)),
       HighExpr::makeConst(0, 4), HighExpr::makeConst(0, 4),
       HighExpr::makeConst(0, 8)});
  Call.Val->Type = NdType::makeInt(8);

  HighStmt Branch;
  Branch.Kind = StmtKind::IfElse;
  Branch.Cond = HighExpr::makeBinop(NdOp::INT_EQUAL,
                                    parameter(0, NdType::makeInt(4)),
                                    HighExpr::makeConst(7, 4));
  Branch.Body.push_back(std::move(LoadCode));
  Branch.Body.push_back(std::move(Call));
  Func.Body.push_back(std::move(Branch));

  HighStmt PhiAssign;
  PhiAssign.Kind = StmtKind::Assign;
  PhiAssign.Dst = HighExpr::makeVar(PhiDest, NdType::makeInt(8));
  PhiAssign.Dst->Kind = ExprKind::Phi;
  PhiAssign.Val = HighExpr::makeConst(41, 8);
  Func.Body.push_back(std::move(PhiAssign));

  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = HighExpr::makeVar(PhiDest, NdType::makeInt(4));
  Ret.RetVal->Kind = ExprKind::Phi;
  Func.Body.push_back(std::move(Ret));

  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("t22_1;"), std::string::npos) << Source;
  EXPECT_NE(Source.find("v36_0;"), std::string::npos) << Source;
  EXPECT_NE(Source.find("v3_0;"), std::string::npos) << Source;
  EXPECT_NE(Source.find("t22_1 = "), std::string::npos) << Source;
  EXPECT_NE(Source.find("v36_0 = RaiseException"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, DeclaresFrameSlotAfterParamHomeOverwrite) {
  HighFunc Func;
  Func.Name = "home_then_write";
  Func.ReturnType = NdType::makeInt(4);
  Func.Params = {{"arg0", NdType::makeInt(4)}};
  Func.FrameSize = 16;
  MedVar SP;
  SP.Kind = MedVar::Reg;
  SP.Size = 8;
  SP.TheArch = Arch::X64;
  SP.RegOff = getTargetRegInfo(Arch::X64).StackPointer;
  auto SlotAddr = HighExpr::makeBinop(
      NdOp::INT_SUB, HighExpr::makeVar(SP, NdType::makeInt(8, false)),
      HighExpr::makeConst(8, 8));
  HighStmt Spill;
  Spill.Kind = StmtKind::Store;
  Spill.StoreAddr = SlotAddr;
  Spill.StoreVal = parameter(0, NdType::makeInt(4));
  Func.Body.push_back(std::move(Spill));
  HighStmt Overwrite;
  Overwrite.Kind = StmtKind::Store;
  Overwrite.StoreAddr = SlotAddr;
  Overwrite.StoreVal = HighExpr::makeConst(41, 4);
  Overwrite.StoreVal->Type = NdType::makeInt(4, true);
  Func.Body.push_back(std::move(Overwrite));
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = HighExpr::makeLoad(SlotAddr, NdType::makeInt(4));
  Func.Body.push_back(std::move(Ret));
  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("var_m8;"), std::string::npos) << Source;
  EXPECT_NE(Source.find("var_m8 = 41;"), std::string::npos) << Source;
}

} // namespace
