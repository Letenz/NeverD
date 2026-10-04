#include "../../../lib/ir/high/pass/HighDCEDetail.h"
#include "../../../lib/sdk/capi/ObjCSwiftOnceSources.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/loader/ObjC/ObjCEncoding.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

using namespace neverd;
using namespace neverd::sdk;

namespace {
void executeOnceSource(const std::string &Source) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  const auto Found = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(Found);
  const std::string Compiler = *Found;
#endif
  llvm::SmallString<128> SourcePath, BinaryPath, ErrorPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-once", "c", SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-once", "exe", BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-once", "err", ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code FileError;
  {
    llvm::raw_fd_ostream Out(SourcePath, FileError);
    ASSERT_FALSE(FileError);
    Out << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  for (llvm::StringRef Optimization : {"-O0", "-O2"}) {
    llvm::SmallVector<llvm::StringRef> Args{
        Compiler,  "-x",       "c",  "-std=gnu11", Optimization,
        "-Werror", SourcePath, "-o", BinaryPath};
    std::string Error;
    const auto Status = llvm::sys::ExecuteAndWait(Compiler, Args, std::nullopt,
                                                  Redirects, 30, 0, &Error);
    const auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Status, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "")
                         << Source;
    EXPECT_EQ(llvm::sys::ExecuteAndWait(BinaryPath, {BinaryPath}, std::nullopt,
                                        Redirects, 30, 0, &Error),
              0)
        << Error << Source;
  }
}

struct OnceFixture {
  BinaryImage Image;
  PipelineResult Pipeline;
  ExprPtr Once;
  explicit OnceFixture(Arch Architecture) {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Architecture;
    Image.Bits = Bitness::Bits64;
    for (unsigned I = 0; I < 2; ++I) {
      Segment S;
      S.VA = 0x1000 + 0x1000 * I;
      S.Size = S.FileSz = 0x100;
      S.Flags = SegmentFlags::Readable |
                (I ? SegmentFlags::Writable : SegmentFlags::Executable);
      S.Data.resize(S.Size);
      Image.Segments.push_back(S);
      Section Sec;
      Sec.VA = S.VA;
      Sec.Size = S.Size;
      Sec.Flags = S.Flags;
      Sec.Type = I ? 0 : llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
      Image.Sections.push_back(Sec);
    }
    Image.Symbols.push_back({"_predicate", 0x2000, 8, false});
    Image.Symbols.push_back({"_object", 0x2010, 8, false});
    Image.ImportPtrSlots[0x2080] = "_swift_once";
    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    auto Param = [&](unsigned Id) {
      MedVar V;
      V.Kind = MedVar::Param;
      V.Id = Id;
      V.Size = 8;
      return HighExpr::makeVar(V, Pointer);
    };
    HighFunc Getter;
    Getter.Entry = 0x1000;
    Getter.Name = "unrelated_name";
    Getter.ReturnType = Pointer;
    SourceFunctionTypeHint Signature;
    Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Signature.ReturnType = Pointer;
    for (unsigned I = 0; I < 3; ++I) {
      Signature.Parameters.push_back({"arg" + std::to_string(I), Pointer});
      Getter.Params.push_back({"arg" + std::to_string(I), Pointer});
    }
    std::string Error;
    EXPECT_TRUE(assignDarwinScalarSourceABI(Signature, Architecture, Error));
    Getter.SourceTypeHint = Signature;
    const auto Runtime = swiftRuntimeSourceCallHint(Image, 0x2080);
    EXPECT_TRUE(Runtime);
    Once = HighExpr::makeCall("swift_once", 0x2080,
                              {Param(0), Param(2), Param(0)});
    Once->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Runtime);
    Once->Type = NdType::makeVoid();
    HighStmt Predicate, Invoke, Return;
    Predicate.Kind = StmtKind::ExprStmt;
    Predicate.Val = HighExpr::makeLoad(Param(0), NdType::makeInt(8));
    Invoke.Kind = StmtKind::Call;
    Invoke.CallExpr = Once;
    Return.Kind = StmtKind::Return;
    Return.RetVal = HighExpr::makeLoad(Param(1), NdType::makeInt(8));
    Getter.Body = {Predicate, Invoke, Return};
    HighFunc Callback;
    Callback.Entry = 0x1080;
    Callback.Name = "initial_value";
    Callback.SourceTypeHint =
        swift_once_source_detail::callbackHint(Architecture);
    Callback.Params = {{"once_context", Pointer}};
    Callback.ReturnType = NdType::makeVoid();
    Return.RetVal.reset();
    Callback.Body = {Return};
    HighFunc Caller;
    Caller.Entry = 0x10c0;
    Caller.Name = "get_value";
    Caller.SourceTypeHint = Signature;
    Caller.SourceTypeHint->Parameters.clear();
    Caller.ReturnType = Pointer;
    Return.RetVal = HighExpr::makeCall(
        Getter.Name, Getter.Entry,
        {HighExpr::makeConst(0x2000, 8), HighExpr::makeConst(0x2010, 8),
         HighExpr::makeConst(Callback.Entry, 8)});
    auto Hint = std::make_shared<SourceCallTypeHint>();
    Hint->CallKind = SourceCallTypeHint::Kind::Native;
    Hint->TargetAddress = Getter.Entry;
    Hint->Signature = Signature;
    Return.RetVal->SourceCallHint = Hint;
    Return.RetVal->Type = Pointer;
    Caller.Body = {Return};
    Pipeline.SourceImage = &Image;
    Pipeline.HighFuncs = {Getter, Callback, Caller};
  }
  std::map<va_t, const HighFunc *> functions() const {
    std::map<va_t, const HighFunc *> Result;
    for (const auto &F : Pipeline.HighFuncs)
      Result.emplace(F.Entry, &F);
    return Result;
  }
};

struct StringOnceFixture : OnceFixture {
  ExprPtr Bridge;
  explicit StringOnceFixture(Arch Architecture) : OnceFixture(Architecture) {
    constexpr va_t BridgeSlot = 0x2088;
    const std::string BridgeName =
        "_$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF";
    Image.Symbols.push_back({"_string", 0x2020, 16, false});
    Image.ImportPtrSlots[BridgeSlot] = BridgeName;
    Image.DyldBindSlots[BridgeSlot] = {
        BridgeName, 0,
        "/System/Library/Frameworks/Foundation.framework/Foundation", false};

    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    const auto Integer = NdType::makeInt(8, false);
    auto Param = [&](unsigned Id, const TypeRef &Type) {
      MedVar V;
      V.Kind = MedVar::Param;
      V.Id = Id;
      V.Size = 8;
      return HighExpr::makeVar(V, Type);
    };
    auto &Getter = Pipeline.HighFuncs[0];
    SourceFunctionTypeHint Signature;
    Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Signature.ReturnType = Pointer;
    Signature.Parameters = {{"predicate", Pointer},
                            {"string_word", Integer},
                            {"string_storage", Integer},
                            {"initializer", Pointer}};
    std::string Error;
    EXPECT_TRUE(assignDarwinScalarSourceABI(Signature, Architecture, Error));
    Getter.SourceTypeHint = Signature;
    Getter.Params = {{"predicate", Pointer},
                     {"string_word", Integer},
                     {"string_storage", Integer},
                     {"initializer", Pointer}};
    Once->Operands[1] = Param(3, Pointer);
    Bridge =
        HighExpr::makeCall(BridgeName, BridgeSlot,
                           {HighExpr::makeLoad(Param(1, Integer), Integer),
                            HighExpr::makeLoad(Param(2, Integer), Integer)});
    Bridge->Type = Pointer;
    const auto BridgeHint = swiftStringSourceCallHint(Image, BridgeSlot);
    EXPECT_TRUE(BridgeHint);
    Bridge->SourceCallHint = std::make_shared<SourceCallTypeHint>(*BridgeHint);
    Getter.Body[2].RetVal = Bridge;

    auto &Caller = Pipeline.HighFuncs[2];
    Caller.Body[0].RetVal = HighExpr::makeCall(
        Getter.Name, Getter.Entry,
        {HighExpr::makeConst(0x2000, 8), HighExpr::makeConst(0x2020, 8),
         HighExpr::makeConst(0x2028, 8), HighExpr::makeConst(0x1080, 8)});
    auto Hint = std::make_shared<SourceCallTypeHint>();
    Hint->CallKind = SourceCallTypeHint::Kind::Native;
    Hint->TargetAddress = Getter.Entry;
    Hint->Signature = Signature;
    Caller.Body[0].RetVal->SourceCallHint = std::move(Hint);
    Caller.Body[0].RetVal->Type = Pointer;
  }
};

struct AddressorFixture {
  static constexpr va_t AccessorAddress = 0x1000;
  static constexpr va_t InitializerAddress = 0x1080;
  static constexpr va_t CallerAddress = 0x10c0;
  static constexpr va_t PredicateAddress = 0x2000;
  static constexpr va_t StorageAddress = 0x2010;
  static constexpr va_t RuntimeSlot = 0x2080;
  static constexpr const char *AccessorName = "_$s4Test5valueSo8NSObjectCvau";
  static constexpr const char *InitializerName = "_$s4Test5value_WZ";
  static constexpr const char *PredicateName = "_$s4Test5value_Wz";
  static constexpr const char *StorageName = "_$s4Test5valueSo8NSObjectCvpZ";

  BinaryImage Image;
  PipelineResult Pipeline;
  ExprPtr Once;

  explicit AddressorFixture(Arch Architecture, bool SharedReturn = false) {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Architecture;
    Image.Bits = Bitness::Bits64;
    for (unsigned I = 0; I < 2; ++I) {
      Segment S;
      S.VA = 0x1000 + 0x1000 * I;
      S.Size = S.FileSz = 0x100;
      S.Flags = SegmentFlags::Readable |
                (I ? SegmentFlags::Writable : SegmentFlags::Executable);
      S.Data.resize(S.Size);
      Image.Segments.push_back(S);
      Section Sec;
      Sec.VA = S.VA;
      Sec.Size = S.Size;
      Sec.Flags = S.Flags;
      Sec.Type = I ? 0 : llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
      Image.Sections.push_back(Sec);
    }
    Image.Symbols = {{AccessorName, AccessorAddress, 0, true},
                     {InitializerName, InitializerAddress, 0, true},
                     {PredicateName, PredicateAddress, 8, false},
                     {StorageName, StorageAddress, 8, false}};
    Image.ImportPtrSlots[RuntimeSlot] = "_swift_once";

    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    const auto Integer = NdType::makeInt(8);
    auto Param = [&](unsigned Id) {
      MedVar V;
      V.Kind = MedVar::Param;
      V.Id = Id;
      V.Size = 8;
      return HighExpr::makeVar(V, Pointer);
    };
    SourceFunctionTypeHint NativeSignature;
    NativeSignature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    NativeSignature.ReturnType = Pointer;
    for (unsigned I = 0; I < 3; ++I)
      NativeSignature.Parameters.push_back(
          {"arg" + std::to_string(I), Pointer});
    std::string Error;
    EXPECT_TRUE(
        assignDarwinScalarSourceABI(NativeSignature, Architecture, Error));

    HighFunc Accessor;
    Accessor.Entry = AccessorAddress;
    Accessor.Name = AccessorName;
    Accessor.ReturnType = Pointer;
    for (unsigned I = 0; I < 3; ++I)
      Accessor.Params.push_back({"arg" + std::to_string(I), Pointer});
    MedVar LoadedVar;
    LoadedVar.Kind = MedVar::Temp;
    LoadedVar.Id = 1;
    LoadedVar.Size = 8;
    auto Loaded = HighExpr::makeVar(LoadedVar, Integer);
    HighStmt Load;
    Load.Kind = StmtKind::Assign;
    Load.Dst = Loaded;
    Load.Val = HighExpr::makeLoad(
        HighExpr::makeConst(PredicateAddress, 8,
                            ConstantAddressProvenance::DataAddress),
        Integer);
    const auto Runtime = swiftRuntimeSourceCallHint(Image, RuntimeSlot);
    EXPECT_TRUE(Runtime);
    Once = HighExpr::makeCall(
        "swift_once", RuntimeSlot,
        {HighExpr::makeConst(PredicateAddress, 8,
                             ConstantAddressProvenance::DataAddress),
         HighExpr::makeConst(InitializerAddress, 8,
                             ConstantAddressProvenance::CodeAddress),
         Param(2)});
    Once->Type = NdType::makeVoid();
    Once->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Runtime);
    HighStmt Invoke;
    Invoke.Kind = StmtKind::Call;
    Invoke.CallExpr = Once;
    HighStmt FirstReturn;
    FirstReturn.Kind = StmtKind::Return;
    FirstReturn.RetVal = HighExpr::makeConst(
        StorageAddress, 8, ConstantAddressProvenance::DataAddress);
    HighStmt Initialize;
    Initialize.Kind = StmtKind::If;
    Initialize.Cond = HighExpr::makeBinop(
        NdOp::INT_NOTEQUAL,
        HighExpr::makeBinop(NdOp::INT_ADD,
                            HighExpr::makeVar(LoadedVar, Integer),
                            HighExpr::makeConst(1, 8)),
        HighExpr::makeConst(0, 8));
    Initialize.Body = {Invoke, FirstReturn};
    HighStmt SecondReturn = FirstReturn;
    HighStmt ContinuationLabel;
    ContinuationLabel.Kind = StmtKind::Block;
    ContinuationLabel.Addr = AccessorAddress + 0x3c;
    Accessor.Body = {Load, Initialize, ContinuationLabel, SecondReturn};
    if (SharedReturn) {
      auto &Guard = Accessor.Body[1];
      Guard.Cond->Op = NdOp::INT_EQUAL;
      Guard.ElseBody.push_back(Guard.Body.front());
      Guard.Body.clear();
    }

    HighFunc Initializer;
    Initializer.Entry = InitializerAddress;
    Initializer.Name = InitializerName;
    Initializer.ReturnType = NdType::makeVoid();
    Initializer.SourceTypeHint =
        swift_once_source_detail::callbackHint(Architecture);
    Initializer.Params = {{"once_context", Pointer}};
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Initializer.Body = {Return};

    HighFunc Caller;
    Caller.Entry = CallerAddress;
    Caller.Name = "read_value";
    Caller.ReturnType = Pointer;
    Caller.SourceTypeHint = NativeSignature;
    Caller.SourceTypeHint->Parameters.clear();
    Return.RetVal = HighExpr::makeCall(AccessorName, AccessorAddress,
                                       std::vector<ExprPtr>{});
    Return.RetVal->Type = Pointer;
    auto AddressorCall = std::make_shared<SourceCallTypeHint>();
    AddressorCall->CallKind = SourceCallTypeHint::Kind::Native;
    AddressorCall->TargetAddress = AccessorAddress;
    AddressorCall->Signature =
        swift_once_source_detail::addressorHint(Architecture);
    Return.RetVal->SourceCallHint = std::move(AddressorCall);
    Caller.Body = {Return};
    Pipeline.SourceImage = &Image;
    Pipeline.HighFuncs = {Accessor, Initializer, Caller};
  }

  std::map<va_t, const HighFunc *> functions() const {
    std::map<va_t, const HighFunc *> Result;
    for (const auto &F : Pipeline.HighFuncs)
      Result.emplace(F.Entry, &F);
    return Result;
  }
};

struct ObjCThunkFixture : AddressorFixture {
  static constexpr va_t ThunkAddress = 0x10e0;
  static constexpr va_t RetainSlot = 0x2088;
  static constexpr const char *ThunkName = "_$s4Test5valueSo8NSObjectCvgZTo";

  explicit ObjCThunkFixture(Arch Architecture, bool SeparateRetain = false,
                            bool SharedReturn = false)
      : AddressorFixture(Architecture) {
    Image.Symbols.push_back({ThunkName, ThunkAddress, 0, true});
    const auto RetainName =
        SeparateRetain ? "_swift_retain" : "_objc_retainAutoreleaseReturnValue";
    Image.ImportPtrSlots[RetainSlot] = RetainName;
    Image.DyldBindSlots[RetainSlot] = {RetainName, 0,
                                       SeparateRetain
                                           ? "/usr/lib/swift/libswiftCore.dylib"
                                           : "/usr/lib/libobjc.A.dylib",
                                       false};
    ObjCMethod Method;
    Method.Status = "supported";
    Method.Implementation = ThunkAddress;
    Method.Selector = "value";
    Method.TypeEncoding = "@16@0:8";
    Method.TypeHint =
        parseObjCMethodEncoding(Method.Selector, Method.TypeEncoding);
    std::string Error;
    EXPECT_TRUE(Method.TypeHint);
    if (!Method.TypeHint)
      return;
    EXPECT_TRUE(
        assignDarwinObjCSourceABI(*Method.TypeHint, Architecture, Error))
        << Error;
    Image.ObjCMethods.push_back(Method);

    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    const auto Integer = NdType::makeInt(8);
    auto Param = [&](unsigned Id) {
      MedVar V;
      V.Kind = MedVar::Param;
      V.Id = Id;
      V.Size = 8;
      return HighExpr::makeVar(V, Integer);
    };
    auto Local = [&](unsigned Id, const TypeRef &Type) {
      MedVar V;
      V.Kind = MedVar::Temp;
      V.Id = Id;
      V.Size = Type->Size;
      return HighExpr::makeVar(V, Type);
    };
    HighFunc Thunk;
    Thunk.Entry = ThunkAddress;
    Thunk.Name = ThunkName;
    Thunk.ReturnType = Integer;
    Thunk.Params = {{"arg0", Integer}, {"arg1", Integer}, {"arg2", Integer}};
    auto PredicateValue = Local(1, Integer);
    HighStmt LoadPredicate;
    LoadPredicate.Kind = StmtKind::Assign;
    LoadPredicate.Dst = PredicateValue;
    LoadPredicate.Val = HighExpr::makeLoad(
        HighExpr::makeConst(PredicateAddress, 8,
                            ConstantAddressProvenance::DataAddress),
        Integer);
    const auto Runtime = swiftRuntimeSourceCallHint(Image, RuntimeSlot);
    EXPECT_TRUE(Runtime);
    if (!Runtime)
      return;
    auto OnceCall = HighExpr::makeCall(
        "swift_once", RuntimeSlot,
        {HighExpr::makeConst(PredicateAddress, 8,
                             ConstantAddressProvenance::DataAddress),
         HighExpr::makeConst(InitializerAddress, 8,
                             ConstantAddressProvenance::CodeAddress),
         Param(2)});
    OnceCall->Type = NdType::makeVoid();
    OnceCall->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Runtime);
    HighStmt Invoke;
    Invoke.Kind = StmtKind::Call;
    Invoke.CallExpr = OnceCall;
    HighStmt Jump;
    Jump.Kind = StmtKind::Goto;
    Jump.GotoTarget = ThunkAddress + 0x1c;
    HighStmt Initialize;
    Initialize.Kind = StmtKind::If;
    Initialize.Cond =
        HighExpr::makeBinop(NdOp::INT_NOTEQUAL,
                            HighExpr::makeBinop(NdOp::INT_ADD, PredicateValue,
                                                HighExpr::makeConst(1, 8)),
                            HighExpr::makeConst(0, 8));
    Initialize.Body = {Invoke, Jump};
    HighStmt Label;
    Label.Kind = StmtKind::Block;
    Label.Addr = Jump.GotoTarget;
    auto StorageValue = Local(2, Integer);
    HighStmt LoadStorage;
    LoadStorage.Kind = StmtKind::Assign;
    LoadStorage.Dst = StorageValue;
    LoadStorage.Val = HighExpr::makeLoad(
        HighExpr::makeConst(StorageAddress, 8,
                            ConstantAddressProvenance::DataAddress),
        Integer);
    const auto Retain = SeparateRetain
                            ? swiftRuntimeSourceCallHint(Image, RetainSlot)
                            : objcRuntimeSourceCallHint(Image, RetainSlot);
    EXPECT_TRUE(Retain);
    if (!Retain)
      return;
    auto ResultValue = Local(3, Pointer);
    HighStmt RetainResult;
    RetainResult.Kind = StmtKind::Assign;
    RetainResult.Dst = ResultValue;
    RetainResult.Val =
        HighExpr::makeCall(Retain->TargetName, RetainSlot, {StorageValue});
    RetainResult.Val->Type = Pointer;
    RetainResult.Val->SourceCallHint =
        std::make_shared<SourceCallTypeHint>(*Retain);
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = ResultValue;
    Thunk.Body = {LoadPredicate, Initialize,   Label,
                  LoadStorage,   RetainResult, Return};
    if (SeparateRetain) {
      HighStmt EntryLabel;
      EntryLabel.Kind = StmtKind::Block;
      EntryLabel.Addr = ThunkAddress + 0x40;
      Thunk.Body[1].Body.insert(Thunk.Body[1].Body.begin(), EntryLabel);
      const va_t AutoreleaseSlot = 0x20a0;
      Image.ImportPtrSlots[AutoreleaseSlot] = "_objc_autoreleaseReturnValue";
      Image.DyldBindSlots[AutoreleaseSlot] = {
          "_objc_autoreleaseReturnValue", 0, "/usr/lib/libobjc.A.dylib", false};
      const auto Autorelease =
          objcRuntimeSourceCallHint(Image, AutoreleaseSlot);
      EXPECT_TRUE(Autorelease);
      if (!Autorelease)
        return;
      HighStmt ReleaseResult;
      ReleaseResult.Kind = StmtKind::Assign;
      ReleaseResult.Dst = Local(4, Pointer);
      ReleaseResult.Val = HighExpr::makeCall(Autorelease->TargetName,
                                             AutoreleaseSlot, {ResultValue});
      ReleaseResult.Val->Type = Pointer;
      ReleaseResult.Val->SourceCallHint =
          std::make_shared<SourceCallTypeHint>(*Autorelease);
      Thunk.Body.back().RetVal = ReleaseResult.Dst;
      Thunk.Body.insert(Thunk.Body.end() - 1, ReleaseResult);
    }
    if (SharedReturn) {
      auto &Guard = Thunk.Body[1];
      Guard.Cond->Op = NdOp::INT_EQUAL;
      Guard.ElseBody.push_back(Guard.Body[SeparateRetain ? 1 : 0]);
      Guard.Body.clear();
    }
    Pipeline.HighFuncs[0] = std::move(Thunk);
  }
};

struct ObjCConstructorFixture : ObjCThunkFixture {
  explicit ObjCConstructorFixture(Arch Architecture)
      : ObjCThunkFixture(Architecture) {
    auto &Constructor = Pipeline.HighFuncs[0];
    Constructor.Name = "_$s4Test6ObjectCACycfcTo";
    Image.Symbols.back().Name = Constructor.Name;
    Image.ObjCMethods[0].Selector = "init";
    MedVar Self;
    Self.Kind = MedVar::Param;
    Self.Id = 0;
    Self.Size = 8;
    // A constructor uses its declared receiver and publishes the initialized
    // object. These effects must survive removal of the unused context.
    HighStmt Publish;
    Publish.Kind = StmtKind::Store;
    Publish.StoreAddr = HighExpr::makeBinop(
        NdOp::INT_ADD, HighExpr::makeVar(Self, NdType::makeInt(8)),
        HighExpr::makeConst(8, 8));
    Publish.StoreVal = Constructor.Body[3].Dst;
    Constructor.Body.insert(Constructor.Body.begin() + 4, Publish);
  }
};

struct ObjCBridgedGetterFixture : ObjCThunkFixture {
  static constexpr va_t BridgeSlot = 0x20b0;
  explicit ObjCBridgedGetterFixture(Arch Architecture, bool Array = false)
      : ObjCThunkFixture(Architecture) {
    Image.ObjCMethods[0].IsClassMethod = true;
    const std::string BridgeName =
        Array ? "$sSa10FoundationE19_bridgeToObjectiveCSo7NSArrayCyF"
              : "$sSD10FoundationE19_bridgeToObjectiveCSo12NSDictionaryCyF";
    Image.ImportPtrSlots[BridgeSlot] = "_" + BridgeName;
    Image.DyldBindSlots[BridgeSlot] = {
        "_" + BridgeName, 0,
        "/System/Library/Frameworks/Foundation.framework/Foundation", false};
    const auto BridgeHint = swiftRuntimeSourceCallHint(Image, BridgeSlot);
    EXPECT_TRUE(BridgeHint);
    if (!BridgeHint)
      return;
    auto &Thunk = Pipeline.HighFuncs[0];
    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    MedVar Value;
    Value.Kind = MedVar::Temp;
    Value.Id = 10;
    Value.Size = 8;
    HighStmt Bridge;
    Bridge.Kind = StmtKind::Assign;
    Bridge.Dst = HighExpr::makeVar(Value, Pointer);
    std::vector<ExprPtr> Arguments{Thunk.Body[3].Dst};
    Arguments.push_back(HighExpr::makeConst(0x40, 8));
    if (!Array) {
      Arguments.push_back(HighExpr::makeConst(0x50, 8));
      Arguments.push_back(HighExpr::makeConst(0x60, 8));
    }
    Bridge.Val = HighExpr::makeCall(BridgeName, BridgeSlot, Arguments);
    Bridge.Val->Type = Pointer;
    Bridge.Val->SourceCallHint =
        std::make_shared<SourceCallTypeHint>(*BridgeHint);
    Thunk.Body[4].Val->Operands[0] = Bridge.Dst;
    Thunk.Body.insert(Thunk.Body.begin() + 4, std::move(Bridge));
  }
};

struct NestedCallbackFixture : OnceFixture {
  static constexpr va_t NestedPredicate = 0x2020;
  static constexpr va_t LeafAddress = 0x10a0;
  ExprPtr NestedOnce;
  NestedCallbackFixture() : OnceFixture(Arch::AArch64) {
    Image.Symbols.push_back({"_$s4Test5outer_WZ", 0x1080, 0, true});
    Image.Symbols.push_back({"_$s4Test4leaf_Wz", NestedPredicate, 8, false});
    Image.Symbols.push_back({"_$s4Test4leaf_WZ", LeafAddress, 0, true});
    auto &Outer = Pipeline.HighFuncs[1];
    Outer.Name = "_$s4Test5outer_WZ";
    Outer.SourceTypeHint.reset();
    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    Outer.Params = {{"arg0", Pointer}, {"arg1", Pointer}, {"arg2", Pointer}};
    MedVar Context;
    Context.Kind = MedVar::Param;
    Context.Id = 2;
    Context.Size = 8;
    NestedOnce = HighExpr::makeCall(
        "swift_once", 0x2080,
        {HighExpr::makeConst(NestedPredicate, 8,
                             ConstantAddressProvenance::DataAddress),
         HighExpr::makeConst(LeafAddress, 8,
                             ConstantAddressProvenance::CodeAddress),
         HighExpr::makeVar(Context, Pointer)});
    NestedOnce->Type = NdType::makeVoid();
    NestedOnce->SourceCallHint = std::make_shared<SourceCallTypeHint>(
        *swiftRuntimeSourceCallHint(Image, 0x2080));
    HighStmt Invoke, Publish;
    Invoke.Kind = StmtKind::Call;
    Invoke.CallExpr = NestedOnce;
    Publish.Kind = StmtKind::Store;
    Publish.StoreAddr =
        HighExpr::makeConst(0x2010, 8, ConstantAddressProvenance::DataAddress);
    Publish.StoreVal = HighExpr::makeConst(7, 8);
    Outer.Body.insert(Outer.Body.begin(), {Invoke, Publish});
    HighFunc Leaf;
    Leaf.Entry = LeafAddress;
    Leaf.Name = "_$s4Test4leaf_WZ";
    Leaf.SourceTypeHint = swift_once_source_detail::callbackHint(Image.Arch);
    Leaf.Params = {{"once_context", Pointer}};
    Leaf.ReturnType = NdType::makeVoid();
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Leaf.Body = {Return};
    Pipeline.HighFuncs.push_back(std::move(Leaf));
  }
};

struct ConditionalOnceContextFixture : NestedCallbackFixture {
  ExprPtr Context;
  ConditionalOnceContextFixture() {
    Image.Symbols.push_back({"_context_choice", 0x2018, 8, false});
    MedVar Variable;
    Variable.Kind = MedVar::Temp;
    Variable.TheArch = Image.Arch;
    Variable.Id = 50003;
    Variable.Size = 8;
    Context = HighExpr::makeVar(Variable, NdType::makeInt(8, false));
    HighStmt Copy;
    Copy.Kind = StmtKind::Assign;
    Copy.IsPhiCopy = true;
    Copy.Addr = 0x1088;
    Copy.Dst = Context;
    Copy.Val = NestedOnce->Operands[2];
    HighStmt Branch;
    Branch.Kind = StmtKind::IfElse;
    Branch.Addr = 0x1084;
    Branch.Cond = HighExpr::makeLoad(
        HighExpr::makeConst(0x2018, 8, ConstantAddressProvenance::DataAddress),
        NdType::makeInt(8, false));
    Branch.Body = {Copy};
    Copy.Addr = 0x108c;
    Copy.Val = HighExpr::makeUndef(8);
    Branch.ElseBody = {Copy};
    NestedOnce->Operands[2] = Context;
    Pipeline.HighFuncs[1].Body.insert(Pipeline.HighFuncs[1].Body.begin(),
                                      std::move(Branch));
  }
};

ExprPtr ignoredCallbackFrameReturn() {
  MedVar Stack;
  Stack.Kind = MedVar::Reg;
  Stack.TheArch = Arch::AArch64;
  Stack.RegOff = getTargetRegInfo(Stack.TheArch).StackPointer;
  Stack.Id = Stack.RegOff;
  Stack.Size = 8;
  auto Value =
      HighExpr::makeBinop(NdOp::INT_ADD, HighExpr::makeConst(uint64_t(-248), 8),
                          HighExpr::makeVar(Stack, NdType::makeInt(8)));
  Value->Type = NdType::makeInt(8);
  return Value;
}

struct TypedOnceCallerFixture : NestedCallbackFixture {
  ExprPtr RootOnce;
  TypedOnceCallerFixture() {
    auto &Outer = Pipeline.HighFuncs[1];
    Outer.ReturnType = NdType::makeInt(8);
    Outer.Body.back().RetVal = ignoredCallbackFrameReturn();
    Pipeline.HighFuncs.erase(
        std::remove_if(Pipeline.HighFuncs.begin(), Pipeline.HighFuncs.end(),
                       [](const HighFunc &F) {
                         return F.Entry == 0x1000 || F.Entry == 0x10c0;
                       }),
        Pipeline.HighFuncs.end());
    Image.Symbols.push_back({"_$s4Test5outer_Wz", 0x2040, 8, false});
    HighFunc Caller;
    Caller.Entry = 0x10c0;
    Caller.Name = "typed_once_caller";
    Caller.SourceTypeHint = swift_once_source_detail::callbackHint(Image.Arch);
    Caller.SourceTypeHint->Origin =
        SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Caller.Params = {{"input", NdType::makePtr(NdType::makeVoid())}};
    Caller.ReturnType = NdType::makeVoid();
    MedVar Input;
    Input.Kind = MedVar::Param;
    Input.Id = 0;
    Input.Size = 8;
    RootOnce = HighExpr::makeCall(
        "swift_once", 0x2080,
        {HighExpr::makeConst(0x2040, 8, ConstantAddressProvenance::DataAddress),
         HighExpr::makeConst(0x1080, 8, ConstantAddressProvenance::CodeAddress),
         HighExpr::makeVar(Input, Caller.Params.front().Type)});
    RootOnce->Type = NdType::makeVoid();
    RootOnce->SourceCallHint = std::make_shared<SourceCallTypeHint>(
        *swiftRuntimeSourceCallHint(Image, 0x2080));
    HighStmt Invoke, Return;
    Invoke.Kind = StmtKind::Call;
    Invoke.CallExpr = RootOnce;
    Return.Kind = StmtKind::Return;
    Caller.Body = {Invoke, Return};
    Pipeline.HighFuncs.push_back(std::move(Caller));
  }
};

struct NestedCallbackGraphFixture : NestedCallbackFixture {
  static constexpr va_t MiddleAddress = 0x1090;
  static constexpr va_t SecondLeafAddress = 0x10b0;
  static constexpr va_t MiddlePredicate = 0x2028;
  static constexpr va_t SecondPredicate = 0x2038;
  ExprPtr MiddleOnce, SecondOnce;

  NestedCallbackGraphFixture() {
    Image.Symbols.push_back({"_$s4Test6middle_Wz", MiddlePredicate, 8, false});
    Image.Symbols.push_back({"_$s4Test6middle_WZ", MiddleAddress, 0, true});
    Image.Symbols.push_back({"_$s4Test6second_Wz", SecondPredicate, 8, false});
    Image.Symbols.push_back({"_$s4Test6second_WZ", SecondLeafAddress, 0, true});
    auto Middle = Pipeline.HighFuncs[1];
    Middle.Entry = MiddleAddress;
    Middle.Name = "_$s4Test6middle_WZ";
    MiddleOnce = std::make_shared<HighExpr>(*NestedOnce);
    Middle.Body[0].CallExpr = MiddleOnce;
    Middle.Body[1].StoreVal = HighExpr::makeConst(9, 8);
    NestedOnce->Operands[0] = HighExpr::makeConst(
        MiddlePredicate, 8, ConstantAddressProvenance::DataAddress);
    NestedOnce->Operands[1] = HighExpr::makeConst(
        MiddleAddress, 8, ConstantAddressProvenance::CodeAddress);
    SecondOnce = std::make_shared<HighExpr>(*NestedOnce);
    auto UnknownContext = HighExpr::makeUndef(8);
    UnknownContext->Type = NdType::makePtr(NdType::makeVoid());
    SecondOnce->Operands = {
        HighExpr::makeConst(SecondPredicate, 8,
                            ConstantAddressProvenance::DataAddress),
        HighExpr::makeConst(SecondLeafAddress, 8,
                            ConstantAddressProvenance::CodeAddress),
        UnknownContext};
    HighStmt Invoke;
    Invoke.Kind = StmtKind::Call;
    Invoke.CallExpr = SecondOnce;
    Pipeline.HighFuncs[1].Body.insert(Pipeline.HighFuncs[1].Body.begin() + 1,
                                      Invoke);
    auto Second = Pipeline.HighFuncs.back();
    Second.Entry = SecondLeafAddress;
    Second.Name = "_$s4Test6second_WZ";
    Pipeline.HighFuncs.push_back(std::move(Middle));
    Pipeline.HighFuncs.push_back(std::move(Second));

    // A shared Objective-C thunk must prove all descendants independently.
    // Its two leading machine registers have no source-level use.
    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    const auto Param = [&](unsigned Id) {
      MedVar V;
      V.Kind = MedVar::Param;
      V.Id = Id;
      V.Size = 8;
      return HighExpr::makeVar(V, Pointer);
    };
    auto &Getter = Pipeline.HighFuncs[0];
    Getter.Params.insert(Getter.Params.begin(),
                         {{"objc_self", Pointer}, {"objc_cmd", Pointer}});
    Getter.Body[0].Val->Operands[0] = Param(2);
    Once->Operands = {Param(2), Param(4), Param(2)};
    Getter.Body[2].RetVal->Operands[0] = Param(3);
    SourceFunctionTypeHint Signature;
    Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Signature.ReturnType = Pointer;
    for (unsigned I = 0; I < 5; ++I)
      Signature.Parameters.push_back({"arg" + std::to_string(I), Pointer});
    std::string Error;
    EXPECT_TRUE(assignDarwinScalarSourceABI(Signature, Image.Arch, Error))
        << Error;
    Getter.SourceTypeHint = Signature;
    auto &Call = Pipeline.HighFuncs[2].Body[0].RetVal;
    Call->Operands.insert(Call->Operands.begin(), {HighExpr::makeConst(0, 8),
                                                   HighExpr::makeConst(0, 8)});
    auto CallHint = std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
    CallHint->Signature = Signature;
    Call->SourceCallHint = std::move(CallHint);
  }
};

TEST(SwiftOnceSources, GenericNativeOnceCallsEraseIgnoredContexts) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    OnceFixture F(Architecture);
    F.Pipeline.HighFuncs.resize(2);
    F.Once->Operands[0] =
        HighExpr::makeConst(0x2000, 8, ConstantAddressProvenance::DataAddress);
    F.Once->Operands[1] =
        HighExpr::makeConst(0x1080, 8, ConstantAddressProvenance::CodeAddress);

    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    ASSERT_EQ(Plan.CallbackHints.count(0x1080), 1U);
    const auto Functions = F.functions();
    const auto Bound = bindSwiftOnceSourceReferences(F.Pipeline.HighFuncs[0],
                                                     F.Image, Plan, Functions);
    EXPECT_EQ(Bound.Dependencies, std::set<va_t>{0x1080});
    EXPECT_EQ(Bound.LocalStorageExtents,
              (std::map<va_t, uint64_t>{{0x2000, 8}}));
    const auto &Call = Bound.Function.Body[1].CallExpr;
    ASSERT_TRUE(Call);
    ASSERT_EQ(Call->Operands.size(), 3U);
    ASSERT_TRUE(Call->Operands[0]->SourceCallHint);
    EXPECT_EQ(Call->Operands[0]->SourceCallHint->CallKind,
              SourceCallTypeHint::Kind::RuntimeLocalStorageAddress);
    EXPECT_TRUE(
        swiftOnceCallbackBound(*Call->Operands[1], F.Image, Plan, Functions));
    EXPECT_EQ(Call->Operands[2]->Kind, ExprKind::Const);
    EXPECT_EQ(Call->Operands[2]->ConstVal, 0U);
    EXPECT_EQ(F.Once->Operands[2]->Kind, ExprKind::Var);
  }
}

TEST(SwiftOnceSources, SharedObjCGetterErasesOnlyProvenUnobservedContext) {
  OnceFixture F(Arch::AArch64);
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  const auto Param = [&](unsigned Id) {
    MedVar V;
    V.Kind = MedVar::Param;
    V.Id = Id;
    V.Size = 8;
    return HighExpr::makeVar(V, Pointer);
  };
  auto &Getter = F.Pipeline.HighFuncs[0];
  Getter.Name = "_$s4Test5valueSo8NSObjectCSgvgZToTm";
  Getter.Params.resize(5, {"argument", Pointer});
  Getter.SourceTypeHint->Parameters.resize(5, {"argument", Pointer});
  std::string Error;
  ASSERT_TRUE(
      assignDarwinScalarSourceABI(*Getter.SourceTypeHint, F.Image.Arch, Error));
  auto Unknown = HighExpr::makeUndef(8);
  Unknown->Type = Pointer;
  F.Once->Operands = {Param(2), Param(4), Unknown};
  Getter.Body.front().Val->Operands[0] = Param(2);
  Getter.Body.back().RetVal->Operands[0] = Param(3);
  HighStmt UpdateStorage;
  UpdateStorage.Kind = StmtKind::Store;
  UpdateStorage.StoreAddr = Param(3);
  UpdateStorage.StoreVal = HighExpr::makeConst(1, 8);
  Getter.Body.insert(Getter.Body.end() - 1, UpdateStorage);

  auto &Callback = F.Pipeline.HighFuncs[1];
  Callback.Name = "_$s4Test5value_WZ";
  F.Image.Symbols = {{"_$s4Test5value_Wz", 0x2000, 8, false},
                     {"_$s4Test5valueSo8NSObjectCSgvpZ", 0x2010, 8, false},
                     {Callback.Name, Callback.Entry, 0, true}};
  auto &Caller = F.Pipeline.HighFuncs[2];
  auto Call = Caller.Body.front().RetVal;
  Call->Operands = {HighExpr::makeConst(0, 8), HighExpr::makeConst(0, 8),
                    HighExpr::makeConst(0x2000, 8),
                    HighExpr::makeConst(0x2010, 8),
                    HighExpr::makeConst(Callback.Entry, 8)};
  auto Hint = std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
  Hint->Signature = *Getter.SourceTypeHint;
  Call->SourceCallHint = std::move(Hint);
  LowFunc Direct;
  Direct.Entry = Caller.Entry;
  LowBlock Block;
  LowOp NativeCall;
  NativeCall.Opcode = NdOp::CALL;
  NativeCall.addInput(NdVar::cst(Getter.Entry, 8));
  Block.Ops.push_back(std::move(NativeCall));
  Direct.Blocks.push_back(std::move(Block));
  F.Pipeline.LowFuncs.push_back(std::move(Direct));

  EXPECT_FALSE(swift_once_source_detail::getterContract(Getter, F.Image));
  ASSERT_TRUE(
      swift_once_source_detail::sharedObjCOnceGetterContract(Getter, F.Image));
  const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  ASSERT_EQ(Plan.Getters.count(Getter.Entry), 1U);
  EXPECT_TRUE(Plan.Getters.at(Getter.Entry).UndefinedContext);
  ASSERT_EQ(Plan.UnobservedContextGetters.count(Getter.Entry), 1U);
  const auto Functions = F.functions();
  auto Bound = bindSwiftOnceSourceReferences(Getter, F.Image, Plan, Functions);
  ASSERT_EQ(Bound.Function.Body[1].CallExpr->Operands[2]->Kind,
            ExprKind::Const);
  EXPECT_EQ(Bound.Function.Body[1].CallExpr->Operands[2]->ConstVal, 0U);
  EXPECT_EQ(Bound.Function.Body[2].Kind, StmtKind::Store);
  EXPECT_EQ(F.Once->Operands[2]->Kind, ExprKind::Undef);

  auto NativeHint = Getter.SourceTypeHint;
  Getter.SourceTypeHint.reset();
  const auto UntypedPlan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  EXPECT_EQ(UntypedPlan.GetterHints.count(Getter.Entry), 1U);
  EXPECT_FALSE(UntypedPlan.UnobservedContextGetters.count(Getter.Entry));
  Getter.SourceTypeHint = std::move(NativeHint);

  F.Pipeline.LowFuncs.front().Blocks.front().Ops.push_back(
      F.Pipeline.LowFuncs.front().Blocks.front().Ops.front());
  const auto UnmatchedPlan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  EXPECT_FALSE(UnmatchedPlan.UnobservedContextGetters.count(Getter.Entry));
  F.Pipeline.LowFuncs.front().Blocks.front().Ops.pop_back();

  HighStmt Observe;
  Observe.Kind = StmtKind::ExprStmt;
  Observe.Val = Param(0);
  Callback.Body.insert(Callback.Body.begin(), Observe);
  const auto ReadingPlan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  EXPECT_FALSE(ReadingPlan.UnobservedContextGetters.count(Getter.Entry));
  Bound = bindSwiftOnceSourceReferences(Getter, F.Image, ReadingPlan,
                                        F.functions());
  EXPECT_EQ(Bound.Function.Body[1].CallExpr->Operands[2]->Kind,
            ExprKind::Undef);
}

TEST(SwiftOnceSources, ErasedContextLeavesNoDeadUnknownDefinition) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    OnceFixture F(Architecture);
    F.Pipeline.HighFuncs.resize(2);
    F.Once->Operands[0] =
        HighExpr::makeConst(0x2000, 8, ConstantAddressProvenance::DataAddress);
    F.Once->Operands[1] =
        HighExpr::makeConst(0x1080, 8, ConstantAddressProvenance::CodeAddress);
    MedVar Context;
    Context.Kind = MedVar::Temp;
    Context.Id = 77;
    Context.Size = 8;
    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    F.Once->Operands[2] = HighExpr::makeVar(Context, Pointer);
    HighStmt Unknown;
    Unknown.Kind = StmtKind::Assign;
    Unknown.Dst = HighExpr::makeVar(Context, Pointer);
    Unknown.Val = HighExpr::makeUndef(8);
    F.Pipeline.HighFuncs[0].Body.insert(
        F.Pipeline.HighFuncs[0].Body.begin() + 1, Unknown);

    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    ASSERT_EQ(Plan.CallbackHints.count(0x1080), 1U);
    const auto Functions = F.functions();
    auto Bound = bindSwiftOnceSourceReferences(F.Pipeline.HighFuncs[0], F.Image,
                                               Plan, Functions);
    ASSERT_EQ(Bound.Dependencies, std::set<va_t>{0x1080});
    eliminateUnusedValues(Bound.Function.Body);
    EXPECT_EQ(Bound.Function.Body.size(), 3U);
    EXPECT_EQ(Bound.Function.Body[1].Kind, StmtKind::Call);
    EXPECT_EQ(Bound.Function.Body[1].CallExpr->Operands[2]->Kind,
              ExprKind::Const);

    auto Observed = F.Pipeline.HighFuncs[0];
    Observed.Body.back().RetVal = HighExpr::makeVar(Context, Pointer);
    auto ObservedBinding =
        bindSwiftOnceSourceReferences(Observed, F.Image, Plan, Functions);
    ASSERT_EQ(ObservedBinding.Dependencies, std::set<va_t>{0x1080});
    eliminateUnusedValues(ObservedBinding.Function.Body);
    EXPECT_EQ(ObservedBinding.Function.Body.size(), 4U);
    EXPECT_EQ(ObservedBinding.Function.Body[1].Val->Kind, ExprKind::Undef);

    auto UnsafePlan = Plan;
    UnsafePlan.CallbackHints.clear();
    auto Unbound = bindSwiftOnceSourceReferences(
        F.Pipeline.HighFuncs[0], F.Image, UnsafePlan, Functions);
    ASSERT_TRUE(Unbound.Dependencies.empty());
    eliminateUnusedValues(Unbound.Function.Body);
    EXPECT_EQ(Unbound.Function.Body.size(), 4U);
    EXPECT_EQ(Unbound.Function.Body[1].Val->Kind, ExprKind::Undef);
  }
}

TEST(SwiftOnceSources, ClosedIgnoredContextMayFillOnlyUnknownCallerInput) {
  OnceFixture F(Arch::AArch64);
  F.Pipeline.HighFuncs.resize(2);
  F.Once->Operands[0] =
      HighExpr::makeConst(0x2000, 8, ConstantAddressProvenance::DataAddress);
  F.Once->Operands[1] =
      HighExpr::makeConst(0x1080, 8, ConstantAddressProvenance::CodeAddress);
  auto &Callee = F.Pipeline.HighFuncs[0];
  Callee.Body.erase(Callee.Body.begin());
  const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  const auto Functions = F.functions();
  const auto BoundCallee =
      bindSwiftOnceSourceReferences(Callee, F.Image, Plan, Functions);
  ASSERT_EQ(BoundCallee.ErasedSwiftOnceContextParameters,
            (std::set<size_t>{0}));
  EXPECT_TRUE(swift_once_source_detail::projectedOnceContextUnused(
      BoundCallee.Function, 0));
  EXPECT_FALSE(swift_once_source_detail::projectedOnceContextUnused(
      BoundCallee.Function, 1));

  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto Unknown = HighExpr::makeUndef(8);
  Unknown->Type = Pointer;
  auto Other = HighExpr::makeConst(0, 8);
  Other->Type = Pointer;
  auto Call =
      HighExpr::makeCall(Callee.Name, Callee.Entry, {Unknown, Other, Other});
  Call->Type = Callee.ReturnType;
  auto Hint = std::make_shared<SourceCallTypeHint>();
  Hint->CallKind = SourceCallTypeHint::Kind::Native;
  Hint->TargetAddress = Callee.Entry;
  Hint->TargetName = Callee.Name;
  Hint->Signature = *Callee.SourceTypeHint;
  Call->SourceCallHint = std::move(Hint);
  HighFunc Caller;
  Caller.Entry = 0x10c0;
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = Call;
  Caller.Body = {Return};
  const std::map<va_t, std::set<size_t>> Proof{{Callee.Entry, {0}}};
  const auto BoundCaller =
      bindSwiftOnceSourceReferences(Caller, F.Image, Plan, Functions, &Proof);
  ASSERT_TRUE(BoundCaller.Function.Body.front().RetVal);
  ASSERT_EQ(BoundCaller.Function.Body.front().RetVal->Operands.size(), 3U);
  EXPECT_EQ(BoundCaller.Function.Body.front().RetVal->Operands[0]->Kind,
            ExprKind::Const);
  EXPECT_EQ(BoundCaller.Function.Body.front().RetVal->Operands[0]->ConstVal,
            0U);
  EXPECT_EQ(Call->Operands[0]->Kind, ExprKind::Undef);
  EXPECT_EQ(bindSwiftOnceSourceReferences(Caller, F.Image, Plan, Functions)
                .Function.Body.front()
                .RetVal->Operands[0]
                ->Kind,
            ExprKind::Undef);

  Caller.Body.front().RetVal->Operands[0] = HighExpr::makeCall("effect", 0, {});
  EXPECT_EQ(
      bindSwiftOnceSourceReferences(Caller, F.Image, Plan, Functions, &Proof)
          .Function.Body.front()
          .RetVal->Operands[0]
          ->Kind,
      ExprKind::Call);
}

TEST(SwiftOnceSources, GenericNativeOnceCallsRequireIndependentLeafEvidence) {
  for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
    OnceFixture F(Arch::AArch64);
    F.Pipeline.HighFuncs.resize(2);
    F.Once->Operands[0] =
        HighExpr::makeConst(0x2000, 8, ConstantAddressProvenance::DataAddress);
    F.Once->Operands[1] =
        HighExpr::makeConst(0x1080, 8, ConstantAddressProvenance::CodeAddress);
    if (Mutation == 0) {
      MedVar Context;
      Context.Kind = MedVar::Param;
      Context.Id = 0;
      Context.Size = 8;
      HighStmt Use;
      Use.Kind = StmtKind::ExprStmt;
      Use.Val = HighExpr::makeVar(Context, NdType::makePtr(NdType::makeVoid()));
      F.Pipeline.HighFuncs[1].Body.insert(F.Pipeline.HighFuncs[1].Body.begin(),
                                          std::move(Use));
    } else if (Mutation == 1) {
      F.Once->Operands[0] = HighExpr::makeConst(
          0x1008, 8, ConstantAddressProvenance::CodeAddress);
    } else if (Mutation == 2) {
      LowFunc Direct;
      LowBlock Block;
      LowOp Call;
      Call.Opcode = NdOp::CALL;
      Call.addInput(NdVar::cst(0x1080, 8));
      Block.Ops.push_back(std::move(Call));
      Direct.Blocks.push_back(std::move(Block));
      F.Pipeline.LowFuncs.push_back(std::move(Direct));
    } else {
      auto Changed =
          std::make_shared<SourceCallTypeHint>(*F.Once->SourceCallHint);
      Changed->TargetName = "other_once";
      F.Once->SourceCallHint = std::move(Changed);
    }
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    EXPECT_EQ(Plan.CallbackHints.count(0x1080), 0U) << Mutation;
  }
}

TEST(SwiftOnceSources,
     ProjectsSingleNestedCallbackWithoutDroppingOtherEffects) {
  NestedCallbackFixture F;
  const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  ASSERT_EQ(Plan.NestedCallbacks.size(), 1U);
  ASSERT_EQ(Plan.CallbackHints.size(), 2U);
  const auto &Original = F.Pipeline.HighFuncs[1];
  const auto Projected =
      projectSwiftOnceNestedCallback(Original, F.Image, Plan, F.functions());
  ASSERT_TRUE(Projected);
  ASSERT_TRUE(Projected->Function.SourceTypeHint);
  EXPECT_EQ(Projected->Function.Params.size(), 1U);
  ASSERT_EQ(Projected->Function.Body.size(), Original.Body.size());
  EXPECT_EQ(Projected->Function.Body[1].Kind, StmtKind::Store);
  EXPECT_EQ(Projected->Function.Body[1].StoreVal->ConstVal, 7U);
  EXPECT_EQ(Projected->Dependencies, std::set<va_t>{F.LeafAddress});
  EXPECT_EQ(Projected->LocalStorageExtents,
            (std::map<va_t, uint64_t>{{F.NestedPredicate, 8}}));
  const auto &Call = Projected->Function.Body[0].CallExpr;
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->Operands[2]->Kind, ExprKind::Const);
  EXPECT_EQ(Call->Operands[2]->ConstVal, 0U);
  EXPECT_TRUE(
      swiftOnceCallbackBound(*Call->Operands[1], F.Image, Plan, F.functions()));
  EXPECT_EQ(F.NestedOnce->Operands[2]->Kind, ExprKind::Var);
  EXPECT_FALSE(Original.SourceTypeHint);
  auto Functions = F.functions();
  Functions[Original.Entry] = &Projected->Function;
  const auto Caller = bindSwiftOnceSourceReferences(F.Pipeline.HighFuncs[2],
                                                    F.Image, Plan, Functions);
  EXPECT_EQ(Caller.Dependencies, std::set<va_t>{Original.Entry});
}

TEST(SwiftOnceSources, SharedObjCGetterRootsIndependentNestedCallback) {
  const auto Prepare = [](NestedCallbackFixture &F) {
    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    const auto Param = [&](unsigned Id) {
      MedVar V;
      V.Kind = MedVar::Param;
      V.Id = Id;
      V.Size = 8;
      return HighExpr::makeVar(V, Pointer);
    };
    auto &Getter = F.Pipeline.HighFuncs[0];
    Getter.Params.insert(Getter.Params.begin(),
                         {{"objc_self", Pointer}, {"objc_cmd", Pointer}});
    Getter.Body[0].Val->Operands[0] = Param(2);
    F.Once->Operands = {Param(2), Param(4), Param(2)};
    Getter.Body[2].RetVal->Operands[0] = Param(3);
    SourceFunctionTypeHint Signature;
    Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Signature.ReturnType = Pointer;
    for (unsigned I = 0; I < 5; ++I)
      Signature.Parameters.push_back({"arg" + std::to_string(I), Pointer});
    std::string Error;
    EXPECT_TRUE(assignDarwinScalarSourceABI(Signature, F.Image.Arch, Error))
        << Error;
    Getter.SourceTypeHint = Signature;
    auto &Call = F.Pipeline.HighFuncs[2].Body[0].RetVal;
    Call->Operands.insert(Call->Operands.begin(), {Param(0), Param(1)});
    auto CallHint = std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
    CallHint->Signature = Signature;
    Call->SourceCallHint = std::move(CallHint);
  };

  NestedCallbackFixture F;
  Prepare(F);
  const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  ASSERT_EQ(Plan.Getters.size(), 1U);
  ASSERT_EQ(Plan.CallbackHints.count(F.Pipeline.HighFuncs[1].Entry), 1U);
  ASSERT_EQ(Plan.NestedCallbacks.size(), 1U);
  const auto Projected = projectSwiftOnceNestedCallback(
      F.Pipeline.HighFuncs[1], F.Image, Plan, F.functions());
  ASSERT_TRUE(Projected);
  auto Functions = F.functions();
  Functions[F.Pipeline.HighFuncs[1].Entry] = &Projected->Function;
  const auto Bound = bindSwiftOnceSourceReferences(F.Pipeline.HighFuncs[2],
                                                   F.Image, Plan, Functions);
  EXPECT_EQ(Bound.Dependencies, std::set<va_t>{F.Pipeline.HighFuncs[1].Entry});

  NestedCallbackFixture ReadsContext;
  Prepare(ReadsContext);
  HighStmt Use;
  Use.Kind = StmtKind::ExprStmt;
  MedVar Context;
  Context.Kind = MedVar::Param;
  Context.Id = 0;
  Context.Size = 8;
  Use.Val = HighExpr::makeVar(Context, NdType::makePtr(NdType::makeVoid()));
  ReadsContext.Pipeline.HighFuncs.back().Body.insert(
      ReadsContext.Pipeline.HighFuncs.back().Body.begin(), Use);
  const auto Rejected =
      discoverSwiftOnceSources(ReadsContext.Image, ReadsContext.Pipeline);
  EXPECT_FALSE(
      Rejected.CallbackHints.count(ReadsContext.Pipeline.HighFuncs[1].Entry));
  EXPECT_TRUE(Rejected.NestedCallbacks.empty());
}

TEST(SwiftOnceSources, NestedCallbackPreservesRetainInIgnoredReturn) {
  NestedCallbackFixture F;
  constexpr va_t RetainSlot = 0x2088;
  F.Image.ImportPtrSlots[RetainSlot] = "_objc_retain";
  F.Image.DynInfo.NeededLibs.push_back("/usr/lib/libobjc.A.dylib");
  F.Image.DyldBindSlots[RetainSlot] = {"_objc_retain", 0,
                                       "/usr/lib/libobjc.A.dylib", false};
  const auto Hint = objcRuntimeSourceCallHint(F.Image, RetainSlot);
  ASSERT_TRUE(Hint);
  auto &Outer = F.Pipeline.HighFuncs[1];
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto Retain = HighExpr::makeCall(
      "objc_retain", RetainSlot,
      {HighExpr::makeConst(0x2010, 8, ConstantAddressProvenance::DataAddress)});
  Retain->Type = Pointer;
  Retain->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
  Outer.ReturnType = Pointer;
  Outer.Body.back().RetVal = Retain;

  const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  ASSERT_EQ(Plan.NestedCallbacks.count(Outer.Entry), 1U);
  const auto Projected =
      projectSwiftOnceNestedCallback(Outer, F.Image, Plan, F.functions());
  ASSERT_TRUE(Projected);
  ASSERT_GE(Projected->Function.Body.size(), 2U);
  const auto &Effect =
      Projected->Function.Body[Projected->Function.Body.size() - 2];
  EXPECT_EQ(Effect.Kind, StmtKind::Call);
  EXPECT_EQ(Effect.CallExpr->SourceCallHint->TargetName, "objc_retain");
  const auto &Return = Projected->Function.Body.back();
  EXPECT_EQ(Return.Kind, StmtKind::Return);
  EXPECT_FALSE(Return.RetVal);
  EXPECT_EQ(Projected->Function.ReturnType->Kind, NdTypeKind::Void);

  auto Forged = std::make_shared<SourceCallTypeHint>(
      *Outer.Body.back().RetVal->SourceCallHint);
  Forged->TargetName = "objc_release";
  Outer.Body.back().RetVal->SourceCallHint = std::move(Forged);
  EXPECT_FALSE(
      swift_once_source_detail::nestedCallbackContract(Outer, F.Image));
}

TEST(SwiftOnceSources, NestedCallbackDiscardsOnlyPureIgnoredReturnArithmetic) {
  NestedCallbackFixture F;
  auto &Outer = F.Pipeline.HighFuncs[1];
  Outer.FrameSize = 512;
  Outer.ReturnType = NdType::makeInt(8);
  Outer.Body.back().RetVal = ignoredCallbackFrameReturn();
  const auto OriginalReturn = Outer.Body.back().RetVal;
  const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  ASSERT_EQ(Plan.NestedCallbacks.count(Outer.Entry), 1U);
  const auto Projected =
      projectSwiftOnceNestedCallback(Outer, F.Image, Plan, F.functions());
  ASSERT_TRUE(Projected);
  EXPECT_EQ(Projected->Function.ReturnType->Kind, NdTypeKind::Void);
  EXPECT_FALSE(Projected->Function.Body.back().RetVal);
  ASSERT_EQ(Projected->Function.Body.size(), Outer.Body.size());
  EXPECT_EQ(Projected->Function.Body[0].Kind, StmtKind::Call);
  EXPECT_EQ(Projected->Function.Body[1].Kind, StmtKind::Store);
  EXPECT_EQ(Outer.Body.back().RetVal, OriginalReturn);
  EXPECT_FALSE(Outer.SourceTypeHint);

  // The local return proof cannot authenticate a changed descendant.
  F.Pipeline.HighFuncs.back().SourceTypeHint.reset();
  EXPECT_FALSE(
      projectSwiftOnceNestedCallback(Outer, F.Image, Plan, F.functions()));
}

TEST(SwiftOnceSources, TypedOnceCallerRootsAnIndependentNestedCallbackChain) {
  TypedOnceCallerFixture F;
  const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  EXPECT_TRUE(Plan.Getters.empty());
  EXPECT_TRUE(Plan.ObjCThunks.empty());
  EXPECT_TRUE(Plan.Addressors.empty());
  ASSERT_EQ(Plan.NestedCallbacks.count(0x1080), 1U);
  ASSERT_EQ(Plan.CallbackHints.size(), 2U);
  PipelineOptions Options;
  EXPECT_EQ(applySwiftOnceSourceHints(Plan, Options), 1U);
  EXPECT_FALSE(Options.SourceTypeHints.count(0x1080));
  const auto Projected =
      projectSwiftOnceNestedCallbacks(F.Image, Plan, F.functions());
  ASSERT_EQ(Projected.size(), 1U);
  auto Functions = F.functions();
  Functions[0x1080] = &Projected.at(0x1080).Function;
  const auto Caller = bindSwiftOnceSourceReferences(F.Pipeline.HighFuncs.back(),
                                                    F.Image, Plan, Functions);
  EXPECT_EQ(Caller.Dependencies, std::set<va_t>{0x1080});
  ASSERT_EQ(Caller.Function.Body[0].CallExpr->Operands[2]->Kind,
            ExprKind::Const);
  EXPECT_EQ(Caller.Function.Body[0].CallExpr->Operands[2]->ConstVal, 0U);
  EXPECT_EQ(F.RootOnce->Operands[2]->Kind, ExprKind::Var);
}

TEST(SwiftOnceSources, TypedOnceRootStillRequiresIndependentCurrentEvidence) {
  for (unsigned Variant = 0; Variant != 6; ++Variant) {
    SCOPED_TRACE(Variant);
    TypedOnceCallerFixture F;
    if (Variant == 0)
      F.Pipeline.HighFuncs.back().SourceTypeHint.reset();
    else if (Variant == 1)
      F.RootOnce->SourceCallHint.reset();
    else if (Variant == 2)
      F.Pipeline.HighFuncs[1].Body.back().RetVal = HighExpr::makeUndef(8);
    else if (Variant == 3)
      F.Image.Symbols.push_back({"_$s4Test5outer_WZ", 0x1080, 0, true});
    else {
      LowFunc Direct;
      LowBlock Block;
      LowOp Call;
      Call.Opcode = NdOp::CALL;
      Call.addInput(NdVar::cst(Variant == 4 ? 0x1080 : F.LeafAddress, 8));
      Block.Ops.push_back(Call);
      Direct.Blocks.push_back(std::move(Block));
      F.Pipeline.LowFuncs.push_back(std::move(Direct));
    }
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    EXPECT_TRUE(Plan.CallbackHints.empty());
    EXPECT_TRUE(Plan.NestedCallbacks.empty());
  }
}

TEST(SwiftOnceSources, TypedOnceCallerKeepsContextEvaluationEffects) {
  for (const bool Load : {false, true}) {
    TypedOnceCallerFixture F;
    auto Input = F.RootOnce->Operands[2];
    auto Effect = Load ? HighExpr::makeLoad(Input, Input->Type)
                       : HighExpr::makeCall("effect", 0x10f0, {Input});
    Effect->Type = Input->Type;
    F.RootOnce->Operands[2] = Effect;
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    const auto Projected =
        projectSwiftOnceNestedCallbacks(F.Image, Plan, F.functions());
    ASSERT_EQ(Projected.size(), 1U);
    auto Functions = F.functions();
    Functions[0x1080] = &Projected.at(0x1080).Function;
    const auto Caller = bindSwiftOnceSourceReferences(
        F.Pipeline.HighFuncs.back(), F.Image, Plan, Functions);
    EXPECT_EQ(Caller.Dependencies, std::set<va_t>{0x1080});
    const auto &Context = Caller.Function.Body[0].CallExpr->Operands[2];
    EXPECT_EQ(Context->Kind, Effect->Kind);
    EXPECT_EQ(Context->Operands.front()->Var.Kind, MedVar::Param);
    EXPECT_TRUE(Caller.ErasedSwiftOnceContextParameters.empty());
    EXPECT_FALSE(swift_once_source_detail::projectedOnceContextUnused(
        Caller.Function, 0));
  }
}

TEST(SwiftOnceSources, IgnoredOnceContextStillObservesIndirectCallerUses) {
  for (const auto Kind : {ExprKind::Var, ExprKind::Phi}) {
    TypedOnceCallerFixture F;
    auto Caller = F.Pipeline.HighFuncs.back();
    Caller.Body.erase(Caller.Body.begin());
    EXPECT_TRUE(
        swift_once_source_detail::projectedOnceContextUnused(Caller, 0));
    auto Target = std::make_shared<HighExpr>(*F.RootOnce->Operands[2]);
    Target->Kind = Kind;
    HighStmt Invoke;
    Invoke.Kind = StmtKind::Call;
    Invoke.CallExpr = HighExpr::makeCall({}, 0, {});
    Invoke.CallExpr->Type = NdType::makeVoid();
    Invoke.CallExpr->IsIndirectCall = true;
    Invoke.CallExpr->IndirectTarget = Target;
    Caller.Body.insert(Caller.Body.begin(), Invoke);
    EXPECT_FALSE(
        swift_once_source_detail::projectedOnceContextUnused(Caller, 0));
  }
}

TEST(SwiftOnceSources, EmittedTypedOnceCallerPreservesContextRetainAtO0AndO2) {
  TypedOnceCallerFixture F;
  constexpr va_t RetainSlot = 0x2088;
  F.Image.ImportPtrSlots[RetainSlot] = "_objc_retain";
  F.Image.DynInfo.NeededLibs.push_back("/usr/lib/libobjc.A.dylib");
  F.Image.DyldBindSlots[RetainSlot] = {"_objc_retain", 0,
                                       "/usr/lib/libobjc.A.dylib", false};
  const auto Hint = objcRuntimeSourceCallHint(F.Image, RetainSlot);
  ASSERT_TRUE(Hint);
  auto Retain =
      HighExpr::makeCall("objc_retain", RetainSlot, {F.RootOnce->Operands[2]});
  Retain->Type = NdType::makePtr(NdType::makeVoid());
  Retain->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
  F.RootOnce->Operands[2] = Retain;
  const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  const auto Projected =
      projectSwiftOnceNestedCallbacks(F.Image, Plan, F.functions());
  ASSERT_EQ(Projected.size(), 1U);
  auto Functions = F.functions();
  Functions[0x1080] = &Projected.at(0x1080).Function;
  std::vector<HighFunc> Bodies;
  for (const va_t Entry : {va_t{0x10c0}, va_t{0x1080}, F.LeafAddress}) {
    const auto Once = bindSwiftOnceSourceReferences(*Functions.at(Entry),
                                                    F.Image, Plan, Functions);
    auto Bound = bindObjCSourceReferences(Once.Function, F.Image);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    Bodies.push_back(std::move(Bound.Function));
  }
  CEmitterOptions Options;
  Options.TheArch = F.Image.Arch;
  Options.Format = F.Image.Format;
  Options.Image = &F.Image;
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  HighCEmitter Emitter;
  Emitter.prepareImageFunctionNames(F.Image);
  ASSERT_TRUE(Emitter.emit(Bodies, Out, Options));
  Source += R"C(
#include <stdlib.h>
static int64_t outer_predicate, leaf_predicate;
static uint64_t object;
static unsigned calls, retains;
static void *last_context;
uintptr_t neverd_local_storage_2010_address(void) { return (uintptr_t)&object; }
uintptr_t neverd_local_storage_2020_address(void) { return (uintptr_t)&leaf_predicate; }
uintptr_t neverd_local_storage_2040_address(void) { return (uintptr_t)&outer_predicate; }
void *objc_retain(void *context) {
  ++retains;
  last_context = context;
  return context;
}
void swift_once(void *p, void (*callback)(void *), void *context) {
  ++calls;
  if (p == &outer_predicate) {
    if (context != last_context || !context) abort();
  } else if (p != &leaf_predicate || context) abort();
  if (*(int64_t *)p != -1) {
    *(int64_t *)p = -1;
    callback(context);
  }
}
int main(void) {
  typed_once_caller((void *)(uintptr_t)0xBAD);
  if (object != 7 || calls != 2 || retains != 1) return 1;
  typed_once_caller((void *)(uintptr_t)0xF00);
  if (object != 7 || calls != 3 || retains != 2) return 2;
  return 0;
}
)C";
  executeOnceSource(Source);
}

TEST(SwiftOnceSources, IgnoredCallbackViewsRejectHiddenIndirectTargets) {
  for (unsigned Variant = 0; Variant != 6; ++Variant) {
    SCOPED_TRACE(Variant);
    NestedCallbackFixture F;
    auto &Outer = F.Pipeline.HighFuncs[1];
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    auto Context = F.NestedOnce->Operands[2];
    if (Variant % 3 == 1) {
      Context = HighExpr::makeUndef(8);
      Context->Type = NdType::makePtr(NdType::makeVoid());
    } else if (Variant % 3 == 2) {
      Context->Var.Kind = MedVar::Temp;
    }
    if (Variant >= 3) {
      auto View = std::make_shared<HighExpr>();
      View->Kind = ExprKind::Cast;
      View->Type = View->CastTo = Context->Type;
      View->Operands = {Context};
      Context = std::move(View);
    }
    Context->IndirectTarget = HighExpr::makeCall("effect", 0x10f0, {});
    F.NestedOnce->Operands[2] = Context;
    EXPECT_FALSE(swift_once_source_detail::nestedOnceContext(Context));
    EXPECT_FALSE(
        swift_once_source_detail::nestedCallbackContract(Outer, F.Image));
    EXPECT_FALSE(
        projectSwiftOnceNestedCallback(Outer, F.Image, Plan, F.functions()));
  }
}

TEST(SwiftOnceSources, RetainedCallbackReturnRejectsMalformedScalarViews) {
  for (unsigned Variant = 0; Variant != 4; ++Variant) {
    SCOPED_TRACE(Variant);
    NestedCallbackFixture F;
    constexpr va_t RetainSlot = 0x2088;
    F.Image.ImportPtrSlots[RetainSlot] = "_objc_retain";
    F.Image.DynInfo.NeededLibs.push_back("/usr/lib/libobjc.A.dylib");
    F.Image.DyldBindSlots[RetainSlot] = {"_objc_retain", 0,
                                         "/usr/lib/libobjc.A.dylib", false};
    const auto Hint = objcRuntimeSourceCallHint(F.Image, RetainSlot);
    ASSERT_TRUE(Hint);
    auto Retain = HighExpr::makeCall("objc_retain", RetainSlot,
                                     {HighExpr::makeConst(0, 8)});
    Retain->Type = NdType::makePtr(NdType::makeVoid());
    Retain->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    ASSERT_TRUE(
        swift_once_source_detail::retainedCallbackReturn(Retain, F.Image));
    ExprPtr Value = Retain;
    if (Variant != 0) {
      Value = std::make_shared<HighExpr>();
      Value->Kind = ExprKind::Cast;
      Value->Type = Value->CastTo = NdType::makeInt(8);
      Value->Operands = {Retain};
    }
    if (Variant < 2)
      Value->IndirectTarget = HighExpr::makeCall("effect", 0x10f0, {});
    else if (Variant == 2)
      Value->MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
    else
      Value->CastTo = NdType::makeInt(4);
    auto &Outer = F.Pipeline.HighFuncs[1];
    Outer.ReturnType = Value->Type;
    Outer.Body.back().RetVal = Value;
    EXPECT_FALSE(
        swift_once_source_detail::retainedCallbackReturn(Value, F.Image));
    EXPECT_FALSE(
        swift_once_source_detail::nestedCallbackContract(Outer, F.Image));
  }
}

TEST(SwiftOnceSources, IgnoredReturnArithmeticRejectsEffectsAndMalformedViews) {
  for (unsigned Variant = 0; Variant != 17; ++Variant) {
    SCOPED_TRACE(Variant);
    NestedCallbackFixture F;
    auto &Outer = F.Pipeline.HighFuncs[1];
    Outer.ReturnType = NdType::makeInt(8);
    auto Value = ignoredCallbackFrameReturn();
    auto &Operand = Value->Operands[1];
    switch (Variant) {
    case 0:
      Operand = HighExpr::makeLoad(Operand, NdType::makeInt(8));
      break;
    case 1:
      Operand = HighExpr::makeCall("effect", 0x10f0, {});
      Operand->Type = NdType::makeInt(8);
      break;
    case 2:
      Operand = HighExpr::makeUndef(8);
      break;
    case 3:
      Operand->Var.Kind = MedVar::Param;
      Operand->Var.Id = 2;
      break;
    case 4:
      Value->Op = NdOp::INT_DIV;
      break;
    case 5:
      Value->MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case 6:
      Operand->MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 7:
      Value->IndirectTarget = HighExpr::makeCall("effect", 0x10f0, {});
      break;
    case 8:
      Operand->IndirectTarget = HighExpr::makeCall("effect", 0x10f0, {});
      break;
    case 9:
      Operand->Type = NdType::makeInt(4);
      break;
    case 10:
      Operand->Type = NdType::makeFloat(8);
      break;
    case 11:
      Value->Operands.push_back(HighExpr::makeConst(1, 8));
      break;
    case 12:
      Operand->Operands.push_back(HighExpr::makeConst(1, 8));
      break;
    case 13:
      Operand.reset();
      break;
    case 14:
      Value->IntrinsicOutputs.push_back({});
      break;
    case 15:
      Operand->Var.Size = 4;
      break;
    case 16:
      Value->Operands[0]->Type = NdType::makePtr(NdType::makeVoid());
      break;
    }
    EXPECT_FALSE(swift_once_source_detail::discardableCallbackReturn(Value));
    Outer.Body.back().RetVal = Value;
    EXPECT_FALSE(
        swift_once_source_detail::nestedCallbackContract(Outer, F.Image));
  }
  auto Cycle = ignoredCallbackFrameReturn();
  Cycle->Operands[1] = Cycle;
  EXPECT_FALSE(swift_once_source_detail::discardableCallbackReturn(Cycle));
  Cycle->Operands.clear();
}

TEST(SwiftOnceSources, IgnoredReturnArithmeticChecksEveryNestedReturn) {
  NestedCallbackFixture F;
  auto &Outer = F.Pipeline.HighFuncs[1];
  Outer.ReturnType = NdType::makeInt(8);
  auto Difference = HighExpr::makeBinop(
      NdOp::INT_SUB, ignoredCallbackFrameReturn(), HighExpr::makeConst(16, 8));
  Difference->Type = NdType::makeInt(8);
  HighStmt Early;
  Early.Kind = StmtKind::Return;
  Early.RetVal = Difference;
  Early.Addr = 0x1088;
  HighStmt Branch;
  Branch.Kind = StmtKind::If;
  Branch.Cond = HighExpr::makeLoad(
      HighExpr::makeConst(0x2010, 8, ConstantAddressProvenance::DataAddress),
      NdType::makeInt(8));
  Branch.Body = {Early};
  Outer.Body.insert(Outer.Body.end() - 1, Branch);
  Outer.Body.back().RetVal = ignoredCallbackFrameReturn();
  const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  ASSERT_EQ(Plan.NestedCallbacks.count(Outer.Entry), 1U);
  const auto Projected =
      projectSwiftOnceNestedCallback(Outer, F.Image, Plan, F.functions());
  ASSERT_TRUE(Projected);
  unsigned Returns = 0;
  walkStmts(Projected->Function.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Return) {
      ++Returns;
      EXPECT_FALSE(S.RetVal);
    }
  });
  EXPECT_EQ(Returns, 2U);
  EXPECT_EQ(Outer.Body[2].Body[0].RetVal, Difference);
  // Effects nested in any return must invalidate an already discovered plan.
  Outer.Body[2].Body[0].RetVal =
      HighExpr::makeLoad(Difference, NdType::makeInt(8));
  EXPECT_FALSE(
      projectSwiftOnceNestedCallback(Outer, F.Image, Plan, F.functions()));
}

TEST(SwiftOnceSources, ProjectsNestedGraphInDependencyOrderAndKeepsEffects) {
  for (const bool Reverse : {false, true}) {
    SCOPED_TRACE(Reverse);
    NestedCallbackGraphFixture F;
    if (Reverse)
      std::reverse(F.Pipeline.HighFuncs.begin(), F.Pipeline.HighFuncs.end());
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    ASSERT_EQ(Plan.NestedCallbacks.size(), 2U);
    ASSERT_EQ(Plan.CallbackHints.size(), 4U);
    PipelineOptions Options;
    EXPECT_EQ(applySwiftOnceSourceHints(Plan, Options), 2U);
    EXPECT_FALSE(Options.SourceTypeHints.count(0x1080));
    EXPECT_FALSE(Options.SourceTypeHints.count(F.MiddleAddress));
    EXPECT_TRUE(Options.SourceTypeHints.count(F.LeafAddress));
    EXPECT_TRUE(Options.SourceTypeHints.count(F.SecondLeafAddress));

    auto Functions = F.functions();
    const auto Projected =
        projectSwiftOnceNestedCallbacks(F.Image, Plan, Functions);
    ASSERT_EQ(Projected.size(), 2U);
    for (const auto &[Entry, Projection] : Projected) {
      EXPECT_EQ(Projection.Function.Params.size(), 1U);
      EXPECT_EQ(Projection.Function.ReturnType->Kind, NdTypeKind::Void);
      EXPECT_TRUE(
          swift_once_source_detail::ignoresContext(Projection.Function));
      Functions[Entry] = &Projection.Function;
    }
    const auto &Outer = Projected.at(0x1080);
    EXPECT_EQ(Outer.Dependencies,
              (std::set<va_t>{F.MiddleAddress, F.SecondLeafAddress}));
    EXPECT_EQ(Outer.LocalStorageExtents,
              (std::map<va_t, uint64_t>{{F.MiddlePredicate, 8},
                                        {F.SecondPredicate, 8}}));
    ASSERT_EQ(Outer.Function.Body.size(), 4U);
    EXPECT_EQ(Outer.Function.Body[2].Kind, StmtKind::Store);
    EXPECT_EQ(Outer.Function.Body[2].StoreVal->ConstVal, 7U);
    for (unsigned I = 0; I < 2; ++I) {
      const auto &Call = Outer.Function.Body[I].CallExpr;
      ASSERT_TRUE(Call);
      EXPECT_EQ(Call->Operands[2]->Kind, ExprKind::Const);
      EXPECT_EQ(Call->Operands[2]->ConstVal, 0U);
      EXPECT_TRUE(
          swiftOnceCallbackBound(*Call->Operands[1], F.Image, Plan, Functions));
    }
    const auto &Middle = Projected.at(F.MiddleAddress);
    EXPECT_EQ(Middle.Dependencies, std::set<va_t>{F.LeafAddress});
    ASSERT_EQ(Middle.Function.Body.size(), 3U);
    EXPECT_EQ(Middle.Function.Body[1].StoreVal->ConstVal, 9U);
    EXPECT_EQ(F.NestedOnce->Operands[2]->Kind, ExprKind::Var);
    EXPECT_EQ(F.SecondOnce->Operands[2]->Kind, ExprKind::Undef);
    EXPECT_FALSE(F.functions().at(0x1080)->SourceTypeHint);
    EXPECT_FALSE(F.functions().at(F.MiddleAddress)->SourceTypeHint);
  }
}

TEST(SwiftOnceSources, NestedContextsMayUseUnobservedScalarRegisterViews) {
  NestedCallbackGraphFixture F;
  MedVar Scratch;
  Scratch.Kind = MedVar::Reg;
  Scratch.Id = 50002;
  Scratch.Size = 8;
  F.SecondOnce->Operands[2] = HighExpr::makeVar(Scratch, NdType::makeInt(8));
  const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  ASSERT_EQ(Plan.NestedCallbacks.size(), 2U);
  const auto Projected =
      projectSwiftOnceNestedCallbacks(F.Image, Plan, F.functions());
  ASSERT_EQ(Projected.size(), 2U);
  const auto Flow = analyzeHighSourceFlow(Projected.at(0x1080).Function, true);
  EXPECT_TRUE(Flow.Complete);
  EXPECT_TRUE(Flow.Items.empty());
  EXPECT_EQ(F.SecondOnce->Operands[2]->Kind, ExprKind::Var);
}

TEST(SwiftOnceSources, NestedContextPhiBecomesDeadOnlyAfterDescendantProof) {
  ConditionalOnceContextFixture F;
  const auto &Original = F.Pipeline.HighFuncs[1];
  const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  ASSERT_EQ(Plan.NestedCallbacks.count(Original.Entry), 1U);
  const auto Projected =
      projectSwiftOnceNestedCallback(Original, F.Image, Plan, F.functions());
  ASSERT_TRUE(Projected);
  const auto Flow = analyzeHighSourceFlow(Projected->Function, false);
  EXPECT_TRUE(Flow.Complete);
  EXPECT_TRUE(Flow.Items.empty());
  EXPECT_TRUE(swift_once_source_detail::ignoresContext(Projected->Function));
  EXPECT_EQ(Projected->Function.Body[0].Body[0].Addr, 0x1088U);
  EXPECT_EQ(Projected->Function.Body[0].ElseBody[0].Addr, 0x108cU);
  EXPECT_EQ(Projected->Function.Body[1].CallExpr->Operands[2]->Kind,
            ExprKind::Const);
  EXPECT_EQ(Original.Body[0].ElseBody[0].Val->Kind, ExprKind::Undef);
  EXPECT_EQ(F.NestedOnce->Operands[2], F.Context);
  EXPECT_FALSE(Original.SourceTypeHint);

  HighStmt Observe;
  Observe.Kind = StmtKind::Store;
  Observe.StoreAddr = HighExpr::makeConst(0x2010, 8);
  MedVar Input;
  Input.Kind = MedVar::Param;
  Input.Id = 0;
  Input.Size = 8;
  Observe.StoreVal = HighExpr::makeVar(Input, NdType::makeInt(8));
  F.Pipeline.HighFuncs.back().Body.insert(
      F.Pipeline.HighFuncs.back().Body.begin(), Observe);
  EXPECT_FALSE(
      projectSwiftOnceNestedCallback(Original, F.Image, Plan, F.functions()));
  EXPECT_TRUE(
      discoverSwiftOnceSources(F.Image, F.Pipeline).NestedCallbacks.empty());
}

TEST(SwiftOnceSources, NestedContextPhiKeepsEveryOtherObservation) {
  for (unsigned Variant = 0; Variant != 10; ++Variant) {
    SCOPED_TRACE(Variant);
    ConditionalOnceContextFixture F;
    auto &Outer = F.Pipeline.HighFuncs[1];
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    ASSERT_EQ(Plan.NestedCallbacks.count(Outer.Entry), 1U);
    auto &Unknown = Outer.Body[0].ElseBody[0];
    HighStmt Observe;
    Observe.Kind = StmtKind::Store;
    Observe.StoreAddr = HighExpr::makeConst(0x2010, 8);
    Observe.StoreVal = F.Context;
    switch (Variant) {
    case 0:
      Outer.Body.insert(Outer.Body.end() - 1, Observe);
      break;
    case 1:
      Outer.ReturnType = F.Context->Type;
      Outer.Body.back().RetVal = F.Context;
      break;
    case 2: {
      HighStmt Branch;
      Branch.Kind = StmtKind::If;
      Branch.Cond = F.Context;
      Observe.StoreVal = HighExpr::makeConst(8, 8);
      Branch.Body = {Observe};
      Outer.Body.insert(Outer.Body.end() - 1, Branch);
      break;
    }
    case 3:
      Observe = HighStmt{};
      Observe.Kind = StmtKind::Call;
      Observe.CallExpr = HighExpr::makeCall({}, 0, {});
      Observe.CallExpr->Type = NdType::makeVoid();
      Observe.CallExpr->IsIndirectCall = true;
      Observe.CallExpr->IndirectTarget = F.Context;
      Outer.Body.insert(Outer.Body.end() - 1, Observe);
      break;
    case 4:
      Unknown.IsPhiCopy = false;
      break;
    case 5:
      Unknown.Val =
          HighExpr::makeLoad(Outer.Body[0].Body[0].Val, NdType::makeInt(8));
      break;
    case 6:
      Unknown.Val->MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
      break;
    case 7:
      Unknown.Val->IndirectTarget = Outer.Body[0].Body[0].Val;
      break;
    case 8:
      F.Image.Arch = Arch::X64;
      break;
    case 9:
      F.NestedOnce->SourceCallHint.reset();
      break;
    }
    EXPECT_FALSE(
        projectSwiftOnceNestedCallback(Outer, F.Image, Plan, F.functions()));
    EXPECT_TRUE(
        discoverSwiftOnceSources(F.Image, F.Pipeline).NestedCallbacks.empty());
  }
}

TEST(SwiftOnceSources, CallbackContextProofIncludesIndirectTargets) {
  for (const auto Kind : {ExprKind::Var, ExprKind::Phi}) {
    NestedCallbackFixture F;
    auto &Leaf = F.Pipeline.HighFuncs.back();
    MedVar Input;
    Input.Kind = MedVar::Param;
    Input.Id = 0;
    Input.Size = 8;
    auto Target = HighExpr::makeVar(Input, NdType::makePtr(NdType::makeVoid()));
    Target->Kind = Kind;
    HighStmt Observe;
    Observe.Kind = StmtKind::Call;
    Observe.CallExpr = HighExpr::makeCall({}, 0, {});
    Observe.CallExpr->Type = NdType::makeVoid();
    Observe.CallExpr->IsIndirectCall = true;
    Observe.CallExpr->IndirectTarget = Target;
    Leaf.Body.insert(Leaf.Body.begin(), Observe);
    EXPECT_FALSE(swift_once_source_detail::ignoresContext(Leaf));
    EXPECT_TRUE(
        discoverSwiftOnceSources(F.Image, F.Pipeline).NestedCallbacks.empty());
  }
}

TEST(SwiftOnceSources, EmittedContextPhiPreservesOnceAndStoreEffects) {
  ConditionalOnceContextFixture F;
  auto &Outer = F.Pipeline.HighFuncs[1];
  Outer.FrameSize = 512;
  Outer.ReturnType = NdType::makeInt(8);
  Outer.Body.back().RetVal = ignoredCallbackFrameReturn();
  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.StoreAddr =
      HighExpr::makeConst(0x2010, 8, ConstantAddressProvenance::DataAddress);
  Store.StoreVal = HighExpr::makeConst(85, 8);
  F.Pipeline.HighFuncs.back().Body.insert(
      F.Pipeline.HighFuncs.back().Body.begin(), Store);
  const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  auto Functions = F.functions();
  const auto Projected =
      projectSwiftOnceNestedCallbacks(F.Image, Plan, Functions);
  ASSERT_EQ(Projected.size(), 1U);
  Functions[0x1080] = &Projected.at(0x1080).Function;
  std::vector<HighFunc> Bodies;
  for (const va_t Entry : {va_t{0x1080}, F.LeafAddress}) {
    auto Bound = bindObjCSourceReferences(*Functions.at(Entry), F.Image);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    if (Entry == 0x1080)
      Bound.Function.Name = "test_context_phi";
    Bodies.push_back(std::move(Bound.Function));
  }
  CEmitterOptions Options;
  Options.TheArch = F.Image.Arch;
  Options.Format = F.Image.Format;
  Options.Image = &F.Image;
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  HighCEmitter Emitter;
  Emitter.prepareImageFunctionNames(F.Image);
  ASSERT_TRUE(Emitter.emit(Bodies, Out, Options));
  Source += R"C(
#include <stdlib.h>
static int64_t predicate;
static uint64_t object, choice;
static unsigned calls, initializations;
uintptr_t neverd_local_storage_2010_address(void) { return (uintptr_t)&object; }
uintptr_t neverd_local_storage_2018_address(void) { return (uintptr_t)&choice; }
uintptr_t neverd_local_storage_2020_address(void) { return (uintptr_t)&predicate; }
void swift_once(void *p, void (*callback)(void *), void *context) {
  if (p != &predicate || context) abort();
  ++calls;
  if (predicate != -1) {
    predicate = -1;
    ++initializations;
    callback(context);
    if (object != 85) abort();
  }
}
int main(void) {
  const uint64_t choices[] = {0, 1, 2, UINT64_MAX};
  for (unsigned i = 0; i < 4; ++i) {
    choice = choices[i]; predicate = 0; object = calls = initializations = 0;
    test_context_phi((void *)(uintptr_t)0xBAD);
    if (object != 7 || calls != 1 || initializations != 1) return 1;
    test_context_phi((void *)(uintptr_t)0xF00);
    if (object != 7 || calls != 2 || initializations != 1) return 2;
  }
  return 0;
}
)C";
  executeOnceSource(Source);
}

TEST(SwiftOnceSources, EmittedNestedGraphExecutesAllEffectsAtO0AndO2) {
  NestedCallbackGraphFixture F;
  F.Image.Symbols.push_back({"_first", 0x2040, 8, false});
  F.Image.Symbols.push_back({"_second", 0x2048, 8, false});
  for (const auto &[Index, Address, Value] :
       {std::tuple{3U, va_t{0x2040}, uint64_t{1}},
        std::tuple{5U, va_t{0x2048}, uint64_t{2}}}) {
    HighStmt Store;
    Store.Kind = StmtKind::Store;
    Store.StoreAddr =
        HighExpr::makeConst(Address, 8, ConstantAddressProvenance::DataAddress);
    Store.StoreVal = HighExpr::makeConst(Value, 8);
    F.Pipeline.HighFuncs[Index].Body.insert(
        F.Pipeline.HighFuncs[Index].Body.begin(), std::move(Store));
  }
  const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  auto Functions = F.functions();
  const auto Projected =
      projectSwiftOnceNestedCallbacks(F.Image, Plan, Functions);
  ASSERT_EQ(Projected.size(), 2U);
  for (const auto &[Entry, Projection] : Projected)
    Functions[Entry] = &Projection.Function;
  std::vector<HighFunc> Bodies;
  for (const va_t Entry :
       {va_t{0x1080}, F.MiddleAddress, F.LeafAddress, F.SecondLeafAddress}) {
    auto Bound = bindObjCSourceReferences(*Functions.at(Entry), F.Image);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    if (Entry == 0x1080)
      Bound.Function.Name = "test_nested_outer";
    Bodies.push_back(std::move(Bound.Function));
  }
  CEmitterOptions Options;
  Options.TheArch = F.Image.Arch;
  Options.Format = F.Image.Format;
  Options.Image = &F.Image;
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  HighCEmitter Emitter;
  Emitter.prepareImageFunctionNames(F.Image);
  ASSERT_TRUE(Emitter.emit(Bodies, Out, Options));
  Source += R"C(
#include <stdlib.h>
static int64_t middle_predicate, leaf_predicate, second_predicate;
static uint64_t object, first, second;
static uintptr_t calls[8];
static unsigned count;
uintptr_t neverd_local_storage_2010_address(void) { return (uintptr_t)&object; }
uintptr_t neverd_local_storage_2020_address(void) { return (uintptr_t)&leaf_predicate; }
uintptr_t neverd_local_storage_2028_address(void) { return (uintptr_t)&middle_predicate; }
uintptr_t neverd_local_storage_2038_address(void) { return (uintptr_t)&second_predicate; }
uintptr_t neverd_local_storage_2040_address(void) { return (uintptr_t)&first; }
uintptr_t neverd_local_storage_2048_address(void) { return (uintptr_t)&second; }
void swift_once(void *predicate, void (*callback)(void *), void *context) {
  if (context || count == 8) abort();
  if (predicate == &second_predicate && count == 2 &&
      (object != 9 || first != 1)) abort();
  calls[count++] = (uintptr_t)predicate;
  if (*(int64_t *)predicate != -1) {
    *(int64_t *)predicate = -1;
    callback(context);
  }
}
int main(void) {
  test_nested_outer((void *)(uintptr_t)0xBAD);
  if (object != 7 || first != 1 || second != 2 || count != 3) return 1;
  test_nested_outer((void *)(uintptr_t)0xBAD);
  if (object != 7 || first != 1 || second != 2 || count != 5) return 2;
  const uintptr_t expected[] = {(uintptr_t)&middle_predicate,
      (uintptr_t)&leaf_predicate, (uintptr_t)&second_predicate,
      (uintptr_t)&middle_predicate, (uintptr_t)&second_predicate};
  for (unsigned i = 0; i < 5; ++i) if (calls[i] != expected[i]) return 3;
  return 0;
}
)C";
  executeOnceSource(Source);
}

TEST(SwiftOnceSources, CallbackAnalysisDoesNotAuthorizeAnUnprovedABI) {
  NestedCallbackGraphFixture F;
  F.Pipeline.HighFuncs[1].Body[2].StoreVal = HighExpr::makeUndef(8);
  const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  EXPECT_EQ(Plan.CallbackAnalysisRoots,
            (std::set<va_t>{0x1080, F.MiddleAddress, F.LeafAddress,
                            F.SecondLeafAddress}));
  EXPECT_TRUE(Plan.CallbackHints.empty());
  EXPECT_TRUE(Plan.NestedCallbacks.empty());
  PipelineOptions Options;
  EXPECT_EQ(applySwiftOnceSourceHints(Plan, Options), 0U);
  EXPECT_TRUE(Options.SourceTypeHints.empty());
  EXPECT_TRUE(
      projectSwiftOnceNestedCallbacks(F.Image, Plan, F.functions()).empty());
}

TEST(SwiftOnceSources, EveryNestedBranchRequiresIndependentEvidence) {
  for (unsigned Mutation = 0; Mutation < 11; ++Mutation) {
    SCOPED_TRACE(Mutation);
    NestedCallbackGraphFixture F;
    auto &Middle = F.Pipeline.HighFuncs[4];
    if (Mutation == 0 || Mutation == 1) {
      HighStmt Use;
      Use.Kind = StmtKind::ExprStmt;
      MedVar Context;
      Context.Kind = MedVar::Param;
      Context.Id = Mutation == 0 ? 2 : 0;
      Context.Size = 8;
      Use.Val = HighExpr::makeVar(Context, NdType::makePtr(NdType::makeVoid()));
      auto &Body =
          Mutation == 0 ? Middle.Body : F.Pipeline.HighFuncs.back().Body;
      Body.insert(Body.begin(), std::move(Use));
    } else if (Mutation == 2)
      F.Image.Symbols.back().Name = "_$s4Test5other_WZ";
    else if (Mutation == 3)
      F.Pipeline.HighFuncs.pop_back();
    else if (Mutation == 4 || Mutation == 5) {
      LowFunc Direct;
      LowBlock Block;
      LowOp Call;
      Call.Opcode = NdOp::CALL;
      Call.addInput(
          NdVar::cst(Mutation == 4 ? F.MiddleAddress : F.SecondLeafAddress, 8));
      Block.Ops.push_back(Call);
      Direct.Blocks.push_back(std::move(Block));
      F.Pipeline.LowFuncs.push_back(std::move(Direct));
    } else if (Mutation == 6) {
      F.Image.Symbols.push_back({"_$s4Test5outer_Wz", 0x2050, 8, false});
      F.MiddleOnce->Operands[0] = HighExpr::makeConst(
          0x2050, 8, ConstantAddressProvenance::DataAddress);
      F.MiddleOnce->Operands[1] = HighExpr::makeConst(
          0x1080, 8, ConstantAddressProvenance::CodeAddress);
    } else if (Mutation == 7) {
      auto Forged =
          std::make_shared<SourceCallTypeHint>(*F.SecondOnce->SourceCallHint);
      Forged->TargetName = "other_once";
      F.SecondOnce->SourceCallHint = std::move(Forged);
    } else if (Mutation == 8)
      F.Pipeline.HighFuncs[1].Body[2].StoreVal = F.SecondOnce->Operands[2];
    else if (Mutation == 9)
      F.SecondOnce->Operands[2] = HighExpr::makeConst(17, 8);
    else
      F.SecondOnce->Operands[2] = HighExpr::makeLoad(
          HighExpr::makeConst(0x2010, 8), NdType::makeInt(8));
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    EXPECT_FALSE(Plan.CallbackHints.count(0x1080));
    EXPECT_TRUE(Plan.NestedCallbacks.empty());
  }
}

TEST(SwiftOnceSources, NestedGraphRevalidatesChildrenBeforePublishingParents) {
  for (unsigned Mutation = 0; Mutation < 3; ++Mutation) {
    NestedCallbackGraphFixture F;
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    ASSERT_EQ(Plan.NestedCallbacks.size(), 2U);
    if (Mutation == 0)
      F.Pipeline.HighFuncs.back().SourceTypeHint.reset();
    else if (Mutation == 1)
      F.MiddleOnce->Operands[2] = HighExpr::makeConst(17, 8);
    else {
      auto Forged =
          std::make_shared<SourceCallTypeHint>(*F.MiddleOnce->SourceCallHint);
      Forged->TargetName = "other_once";
      F.MiddleOnce->SourceCallHint = std::move(Forged);
    }
    const auto Projected =
        projectSwiftOnceNestedCallbacks(F.Image, Plan, F.functions());
    EXPECT_FALSE(Projected.count(0x1080)) << Mutation;
  }
}

TEST(SwiftOnceSources, NestedGraphsHaveABoundedDepth) {
  for (const unsigned Extra : {14U, 15U}) {
    SCOPED_TRACE(Extra);
    NestedCallbackGraphFixture F;
    F.Image.Segments[0].Size = F.Image.Segments[0].FileSz = 0x400;
    F.Image.Segments[0].Data.resize(0x400);
    F.Image.Sections[0].Size = 0x400;
    F.Image.Segments[1].Size = F.Image.Segments[1].FileSz = 0x300;
    F.Image.Segments[1].Data.resize(0x300);
    F.Image.Sections[1].Size = 0x300;
    const auto Template = F.Pipeline.HighFuncs[4];
    va_t Next = F.LeafAddress, Predicate = F.NestedPredicate;
    for (unsigned I = 0; I < Extra; ++I) {
      const va_t Entry = 0x1100 + I * 16;
      const va_t NewPredicate = 0x2100 + I * 8;
      const std::string Name = "_$s4Test6nested" + std::to_string(I);
      F.Image.Symbols.push_back({Name + "_WZ", Entry, 0, true});
      F.Image.Symbols.push_back({Name + "_Wz", NewPredicate, 8, false});
      auto Callback = Template;
      Callback.Entry = Entry;
      Callback.Name = Name + "_WZ";
      auto Once = std::make_shared<HighExpr>(*F.MiddleOnce);
      Once->Operands[0] = HighExpr::makeConst(
          Predicate, 8, ConstantAddressProvenance::DataAddress);
      Once->Operands[1] =
          HighExpr::makeConst(Next, 8, ConstantAddressProvenance::CodeAddress);
      Callback.Body[0].CallExpr = std::move(Once);
      F.Pipeline.HighFuncs.push_back(std::move(Callback));
      Next = Entry;
      Predicate = NewPredicate;
    }
    F.MiddleOnce->Operands[0] = HighExpr::makeConst(
        Predicate, 8, ConstantAddressProvenance::DataAddress);
    F.MiddleOnce->Operands[1] =
        HighExpr::makeConst(Next, 8, ConstantAddressProvenance::CodeAddress);
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    if (Extra == 14) {
      ASSERT_EQ(Plan.NestedCallbacks.size(), 16U);
      const auto Projected =
          projectSwiftOnceNestedCallbacks(F.Image, Plan, F.functions());
      EXPECT_EQ(Projected.size(), 16U);
      EXPECT_TRUE(Projected.count(0x1080));
    } else {
      EXPECT_FALSE(Plan.CallbackHints.count(0x1080));
      EXPECT_TRUE(Plan.NestedCallbacks.empty());
    }
  }
}

TEST(SwiftOnceSources, NestedCallbacksRequireExactIndependentLeafEvidence) {
  for (unsigned Case = 0; Case != 9; ++Case) {
    SCOPED_TRACE(Case);
    NestedCallbackFixture F;
    auto &Outer = F.Pipeline.HighFuncs[1];
    auto &Leaf = F.Pipeline.HighFuncs.back();
    if (Case == 0)
      F.NestedOnce->Operands[2]->Var.Id = 0;
    else if (Case == 1) {
      HighStmt Use;
      Use.Kind = StmtKind::ExprStmt;
      Use.Val = F.NestedOnce->Operands[2];
      Outer.Body.insert(Outer.Body.begin(), Use);
    } else if (Case == 2)
      Outer.Body.insert(Outer.Body.begin(), 32, Outer.Body.front());
    else if (Case == 3)
      F.Image.Symbols.back().Name = "_$s4Test5other_WZ";
    else if (Case == 4)
      F.Image.Symbols[F.Image.Symbols.size() - 3].Name = "ordinary_callback";
    else if (Case == 5) {
      HighStmt Use;
      Use.Kind = StmtKind::ExprStmt;
      Use.Val = F.NestedOnce->Operands[2];
      Leaf.Body.insert(Leaf.Body.begin(), Use);
    } else if (Case == 6 || Case == 7) {
      LowFunc Direct;
      LowBlock Block;
      LowOp Call;
      Call.Opcode = NdOp::CALL;
      Call.addInput(NdVar::cst(Case == 6 ? Outer.Entry : Leaf.Entry, 8));
      Block.Ops.push_back(Call);
      Direct.Blocks.push_back(std::move(Block));
      F.Pipeline.LowFuncs.push_back(std::move(Direct));
    } else {
      auto Changed =
          std::make_shared<SourceCallTypeHint>(*F.NestedOnce->SourceCallHint);
      Changed->TargetName = "other_once";
      F.NestedOnce->SourceCallHint = std::move(Changed);
    }
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    EXPECT_TRUE(Plan.NestedCallbacks.empty());
  }
  NestedCallbackFixture F;
  const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  F.Pipeline.HighFuncs[1].ReturnType = NdType::makeInt(8);
  F.Pipeline.HighFuncs[1].Body.back().RetVal = HighExpr::makeConst(7, 8);
  const auto Discarded = projectSwiftOnceNestedCallback(
      F.Pipeline.HighFuncs[1], F.Image, Plan, F.functions());
  ASSERT_TRUE(Discarded);
  EXPECT_FALSE(Discarded->Function.Body.back().RetVal);
  EXPECT_EQ(Discarded->Function.ReturnType->Kind, NdTypeKind::Void);
  F.Pipeline.HighFuncs[1].Body.back().RetVal =
      HighExpr::makeLoad(HighExpr::makeConst(0x2010, 8), NdType::makeInt(8));
  EXPECT_FALSE(projectSwiftOnceNestedCallback(F.Pipeline.HighFuncs[1], F.Image,
                                              Plan, F.functions()));
  MedVar StackReturn;
  StackReturn.Kind = MedVar::Stack;
  StackReturn.Id = 1;
  StackReturn.Size = 8;
  StackReturn.StackOff = -8;
  F.Pipeline.HighFuncs[1].Body.back().RetVal =
      HighExpr::makeVar(StackReturn, NdType::makeInt(8));
  EXPECT_FALSE(projectSwiftOnceNestedCallback(F.Pipeline.HighFuncs[1], F.Image,
                                              Plan, F.functions()));
  F.Pipeline.HighFuncs[1].Body.back().RetVal.reset();
  F.Pipeline.HighFuncs[1].ReturnType = NdType::makeVoid();
  F.Pipeline.HighFuncs.back().SourceTypeHint.reset();
  EXPECT_FALSE(projectSwiftOnceNestedCallback(F.Pipeline.HighFuncs[1], F.Image,
                                              Plan, F.functions()));
}

TEST(SwiftOnceSources, BindsStorageAndCallbackAsOneDependencyGroup) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    OnceFixture F(Architecture);
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    ASSERT_EQ(Plan.Getters.size(), 1U);
    ASSERT_EQ(Plan.CallbackHints.size(), 1U);
    PipelineOptions Options;
    EXPECT_EQ(applySwiftOnceSourceHints(Plan, Options), 1U);
    EXPECT_EQ(applySwiftOnceSourceHints(Plan, Options), 0U);
    const auto Bound = bindSwiftOnceSourceReferences(
        F.Pipeline.HighFuncs.back(), F.Image, Plan, F.functions());
    EXPECT_EQ(Bound.Dependencies, std::set<va_t>{0x1080});
    EXPECT_EQ(Bound.LocalStorageExtents,
              (std::map<va_t, uint64_t>{{0x2000, 8}, {0x2010, 8}}));
    const auto Call = Bound.Function.Body[0].RetVal;
    EXPECT_TRUE(swiftOnceCallbackBound(*Call->Operands[2], F.Image, Plan,
                                       F.functions()));
    EXPECT_TRUE(
        bindObjCSourceReferences(Bound.Function, F.Image).Limitation.empty());
    EXPECT_EQ(F.Pipeline.HighFuncs.back().Body[0].RetVal->Operands[0]->Kind,
              ExprKind::Const);
  }
}

TEST(SwiftOnceSources, BindsCanonicalZeroArgumentAddressor) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    AddressorFixture F(Architecture);
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    EXPECT_TRUE(Plan.Getters.empty());
    ASSERT_EQ(Plan.Addressors.size(), 1U);
    ASSERT_EQ(Plan.AddressorHints.size(), 1U);
    ASSERT_EQ(Plan.CallbackHints.size(), 1U);
    PipelineOptions Options;
    EXPECT_EQ(applySwiftOnceSourceHints(Plan, Options), 2U);
    EXPECT_EQ(
        Options.SourceCalleeTypeHints.count(AddressorFixture::AccessorAddress),
        1U);
    EXPECT_EQ(
        Options.SourceTypeHints.count(AddressorFixture::InitializerAddress),
        1U);
    const auto Bound = bindSwiftOnceSourceReferences(
        F.Pipeline.HighFuncs.back(), F.Image, Plan, F.functions());
    EXPECT_EQ(Bound.Dependencies,
              std::set<va_t>{AddressorFixture::InitializerAddress});
    EXPECT_EQ(
        Bound.LocalStorageExtents,
        (std::map<va_t, uint64_t>{{AddressorFixture::PredicateAddress, 8},
                                  {AddressorFixture::StorageAddress, 8}}));
    EXPECT_EQ(Bound.SwiftOnceAccessors,
              std::set<va_t>{AddressorFixture::AccessorAddress});
    const auto Call = Bound.Function.Body[0].RetVal;
    ASSERT_TRUE(Call->SourceCallHint);
    EXPECT_EQ(Call->SourceCallHint->CallKind,
              SourceCallTypeHint::Kind::RuntimeSwiftOnceAccessor);
    EXPECT_TRUE(swiftOnceAddressorBound(*Call, F.Image, Plan, F.functions()));
    auto RawCaller = F.Pipeline.HighFuncs.back();
    RawCaller.Body[0].RetVal =
        std::make_shared<HighExpr>(*RawCaller.Body[0].RetVal);
    RawCaller.Body[0].RetVal->SourceCallHint.reset();
    const auto RawBound =
        bindSwiftOnceSourceReferences(RawCaller, F.Image, Plan, F.functions());
    ASSERT_TRUE(RawBound.Function.Body[0].RetVal->SourceCallHint);
    EXPECT_EQ(RawBound.Function.Body[0].RetVal->SourceCallHint->CallKind,
              SourceCallTypeHint::Kind::RuntimeSwiftOnceAccessor);
    auto WrongCarrier = *Call;
    WrongCarrier.Type = NdType::makeInt(8);
    EXPECT_FALSE(
        swiftOnceAddressorBound(WrongCarrier, F.Image, Plan, F.functions()));
    auto ForgedMetadata = *Call;
    auto ForgedHint =
        std::make_shared<SourceCallTypeHint>(*ForgedMetadata.SourceCallHint);
    ForgedHint->SwiftTypeMetadata =
        SourceCallTypeHint::SwiftTypeMetadataAddress{};
    ForgedMetadata.SourceCallHint = std::move(ForgedHint);
    EXPECT_FALSE(
        swiftOnceAddressorBound(ForgedMetadata, F.Image, Plan, F.functions()));

    const auto Initializer = bindSwiftOnceSourceReferences(
        F.Pipeline.HighFuncs[1], F.Image, Plan, F.functions());
    EXPECT_EQ(Initializer.Function.Name,
              swiftOnceInitializerName(AddressorFixture::InitializerAddress));
    std::set<std::string> Shared;
    const auto Helpers = renderSwiftOnceAddressorHelpers(
        F.Image, Bound.SwiftOnceAccessors, Plan, F.functions(), Shared);
    EXPECT_NE(Helpers.find("neverd_swift_once_accessor_1000"),
              std::string::npos);
    EXPECT_NE(Helpers.find("neverd_local_storage_2000_address"),
              std::string::npos);
    EXPECT_NE(Helpers.find("neverd_local_storage_2010_address"),
              std::string::npos);
    EXPECT_NE(Helpers.find("neverd_swift_once_initializer_1080"),
              std::string::npos);
    EXPECT_EQ(Shared, std::set<std::string>{"neverd_swift_once_accessor_1000"});
  }
}

TEST(SwiftOnceSources, BindsSharedReturnAddressor) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    AddressorFixture F(Architecture, true);
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    ASSERT_EQ(Plan.Addressors.size(), 1U);
    EXPECT_EQ(Plan.AddressorHints.size(), 1U);
    EXPECT_EQ(Plan.CallbackHints.size(), 1U);
    const auto Contracts =
        swiftOnceNativeCalleeContracts(F.Image, F.Pipeline, Plan);
    ASSERT_EQ(Contracts.ZeroArgumentPointerCallees.size(), 1U);
    const auto Bound = bindSwiftOnceSourceReferences(
        F.Pipeline.HighFuncs.back(), F.Image, Plan, F.functions());
    EXPECT_EQ(Bound.Dependencies,
              std::set<va_t>{AddressorFixture::InitializerAddress});
    EXPECT_EQ(Bound.SwiftOnceAccessors,
              std::set<va_t>{AddressorFixture::AccessorAddress});
    EXPECT_EQ(
        Bound.LocalStorageExtents,
        (std::map<va_t, uint64_t>{{AddressorFixture::PredicateAddress, 8},
                                  {AddressorFixture::StorageAddress, 8}}));
    const auto Call = Bound.Function.Body[0].RetVal;
    ASSERT_TRUE(Call->SourceCallHint);
    EXPECT_TRUE(swiftOnceAddressorBound(*Call, F.Image, Plan, F.functions()));
    std::set<std::string> Shared;
    const auto Helpers = renderSwiftOnceAddressorHelpers(
        F.Image, Bound.SwiftOnceAccessors, Plan, F.functions(), Shared);
    EXPECT_NE(Helpers.find("neverd_swift_once_initializer_1080"),
              std::string::npos);
  }
}

TEST(SwiftOnceSources, BindsSharedReturnAddressorAfterIfElseStructuring) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    AddressorFixture F(Architecture, true);
    F.Pipeline.HighFuncs[0].Body[1].Kind = StmtKind::IfElse;

    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    ASSERT_EQ(Plan.Addressors.size(), 1U);
    EXPECT_EQ(Plan.Addressors.begin()->second.Predicate,
              AddressorFixture::PredicateAddress);
    EXPECT_EQ(Plan.Addressors.begin()->second.Storage,
              AddressorFixture::StorageAddress);
    EXPECT_EQ(Plan.Addressors.begin()->second.Initializer,
              AddressorFixture::InitializerAddress);
  }
}

TEST(SwiftOnceSources, EarlyReturnAddressorRevalidatesBothStoragePaths) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool InlineLoad : {false, true})
      for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
        SCOPED_TRACE(static_cast<int>(Architecture));
        SCOPED_TRACE(InlineLoad);
        SCOPED_TRACE(Mutation);
        AddressorFixture F(Architecture);
        auto &Accessor = F.Pipeline.HighFuncs[0];
        auto &Body = Accessor.Body;
        auto &Guard = Body[1];
        auto Invoke = Guard.Body.front();
        Guard.Cond->Op = NdOp::INT_EQUAL;
        Guard.Body.erase(Guard.Body.begin());
        // Keep the inert continuation label; selection is independent of it.
        Body.insert(Body.end() - 1, Invoke);
        if (InlineLoad) {
          Body[1].Cond->Operands[0]->Operands[0] = Body[0].Val;
          Body.erase(Body.begin());
        }
        const size_t GuardIndex = InlineLoad ? 0 : 1;
        auto &CurrentGuard = Body[GuardIndex];
        if (Mutation == 1)
          CurrentGuard.Cond->Op = NdOp::INT_NOTEQUAL;
        if (Mutation == 2)
          CurrentGuard.Body[0].RetVal = HighExpr::makeConst(0x2018, 8);
        if (Mutation == 3)
          F.Once->Operands[0] = HighExpr::makeConst(0x2020, 8);
        if (Mutation == 4)
          F.Once->Operands[1] = HighExpr::makeConst(0x1090, 8);
        if (Mutation == 5)
          F.Once->Operands[2] = HighExpr::makeConst(0, 8);
        if (Mutation == 6)
          CurrentGuard.Body.insert(CurrentGuard.Body.begin(), Invoke);
        if (Mutation == 7)
          Body.insert(Body.end() - 1, Invoke);
        if (Mutation == 8)
          CurrentGuard.ElseBody.push_back(Invoke);
        const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
        if (Mutation) {
          EXPECT_TRUE(Plan.Addressors.empty());
          continue;
        }
        ASSERT_EQ(Plan.Addressors.size(), 1U);
        ASSERT_EQ(Plan.CallbackHints.size(), 1U);
        const auto Bound = bindSwiftOnceSourceReferences(
            F.Pipeline.HighFuncs.back(), F.Image, Plan, F.functions());
        EXPECT_EQ(Bound.Dependencies,
                  std::set<va_t>{AddressorFixture::InitializerAddress});
        ASSERT_EQ(Bound.SwiftOnceAccessors.size(), 1U);
        const auto Call = Bound.Function.Body[0].RetVal;
        ASSERT_TRUE(Call);
        ASSERT_TRUE(
            swiftOnceAddressorBound(*Call, F.Image, Plan, F.functions()));
        // Publication cannot reuse a prior plan after either return changes.
        Body[GuardIndex].Body[0].RetVal = HighExpr::makeConst(0x2018, 8);
        EXPECT_FALSE(
            swiftOnceAddressorBound(*Call, F.Image, Plan, F.functions()));
      }
}

TEST(SwiftOnceSources, SharedReturnAddressorRequiresExactControlFlow) {
  for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    AddressorFixture F(Arch::AArch64, true);
    auto &Accessor = F.Pipeline.HighFuncs[0];
    auto &Guard = Accessor.Body[1];
    if (Mutation == 0)
      Guard.Cond->Op = NdOp::INT_NOTEQUAL;
    if (Mutation == 1)
      Guard.Body.push_back(Guard.ElseBody.front());
    if (Mutation == 2)
      Guard.ElseBody.push_back(Guard.ElseBody.front());
    if (Mutation == 3)
      Accessor.Body.back().RetVal = HighExpr::makeConst(0x2018, 8);
    if (Mutation == 4)
      F.Once->Operands[1] = HighExpr::makeConst(0x1090, 8);
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    EXPECT_TRUE(Plan.Addressors.empty());
    const auto Bound = bindSwiftOnceSourceReferences(
        F.Pipeline.HighFuncs.back(), F.Image, Plan, F.functions());
    EXPECT_TRUE(Bound.SwiftOnceAccessors.empty());
  }
}

TEST(SwiftOnceSources, NativeCalleeContractsRevalidateCurrentBodiesAndABI) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
      SCOPED_TRACE(unsigned(Architecture));
      SCOPED_TRACE(Mutation);
      AddressorFixture F(Architecture);
      auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
      ASSERT_EQ(Plan.Addressors.size(), 1U);
      PipelineOptions Persisted;
      applySwiftOnceSourceHints(Plan, Persisted);
      auto &Initializer = F.Pipeline.HighFuncs[1];
      BinaryImage OtherImage;
      if (Mutation == 1)
        F.Pipeline.SourceImage = &OtherImage;
      if (Mutation == 2)
        Plan.Addressors.begin()->second.Storage += 8;
      if (Mutation == 3)
        F.Once->Operands[2] = HighExpr::makeConst(0, 8);
      if (Mutation == 4) {
        HighStmt Use;
        Use.Kind = StmtKind::ExprStmt;
        MedVar Context;
        Context.Kind = MedVar::Param;
        Context.Id = 0;
        Context.Size = 8;
        Use.Val = HighExpr::makeVar(Context, NdType::makeInt(8));
        Initializer.Body.insert(Initializer.Body.begin(), Use);
      }
      if (Mutation == 5)
        Initializer.SourceTypeHint.reset();
      if (Mutation == 6) {
        // Mutating both the plan and the current declaration cannot forge the
        // canonical callback ABI that authorizes dropping the x2 context.
        Initializer.SourceTypeHint->Parameters.clear();
        Plan.CallbackHints[AddressorFixture::InitializerAddress] =
            *Initializer.SourceTypeHint;
      }
      if (Mutation == 7)
        Plan.Addressors.begin()->second.Initializer += 4;
      if (Mutation == 8)
        F.Pipeline.HighFuncs[0].Name += "forged";
      if (Mutation == 9) {
        HighStmt UnknownEdge;
        UnknownEdge.Kind = StmtKind::Goto;
        UnknownEdge.GotoTarget = 0x1234;
        Initializer.Body = {UnknownEdge};
      }
      if (Mutation == 10)
        F.Pipeline.HighFuncs.erase(F.Pipeline.HighFuncs.begin() + 1);
      if (Mutation == 11)
        F.Pipeline.HighFuncs.push_back(F.Pipeline.HighFuncs.front());
      const auto Contracts =
          swiftOnceNativeCalleeContracts(F.Image, F.Pipeline, Plan);
      if (Mutation)
        EXPECT_TRUE(Contracts.ZeroArgumentPointerCallees.empty());
      else {
        EXPECT_EQ(Contracts.SourceImage, &F.Image);
        ASSERT_EQ(Contracts.ZeroArgumentPointerCallees.size(), 1U);
        EXPECT_TRUE(equalSourceABIs(
            Contracts.ZeroArgumentPointerCallees.at(
                AddressorFixture::AccessorAddress),
            swift_once_source_detail::addressorHint(Architecture)));
      }
      if (Mutation == 6) {
        std::set<std::string> Shared;
        EXPECT_THROW(renderSwiftOnceAddressorHelpers(
                         F.Image, {AddressorFixture::AccessorAddress}, Plan,
                         F.functions(), Shared),
                     std::runtime_error);
        EXPECT_TRUE(Shared.empty());
      }
      // Persisted hints never become evidence for this round's contracts.
      EXPECT_EQ(Persisted.SourceCalleeTypeHints.size(), 1U);
      EXPECT_FALSE(
          Persisted.SourceTypeHints.count(AddressorFixture::AccessorAddress));
    }
}

TEST(SwiftOnceSources, NativeCalleeContractDoesNotCloseInitializerBody) {
  AddressorFixture F(Arch::AArch64);
  auto &Initializer = F.Pipeline.HighFuncs[1];
  HighStmt Unknown;
  Unknown.Kind = StmtKind::Call;
  Unknown.CallExpr = HighExpr::makeCall("unknown", 0x10f0, {});
  Unknown.CallExpr->Type = NdType::makeVoid();
  Initializer.Body.insert(Initializer.Body.begin(), Unknown);
  const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  const auto Contracts =
      swiftOnceNativeCalleeContracts(F.Image, F.Pipeline, Plan);
  ASSERT_EQ(Contracts.ZeroArgumentPointerCallees.size(), 1U);
  const auto Bound = bindSwiftOnceSourceReferences(
      F.Pipeline.HighFuncs.back(), F.Image, Plan, F.functions());
  EXPECT_EQ(Bound.Dependencies,
            std::set<va_t>{AddressorFixture::InitializerAddress});
  PipelineFunctionAudit Audit;
  Audit.Entry = Initializer.Entry;
  Audit.Disposition = PipelineFunctionDisposition::Accepted;
  Audit.HasLowIR = Audit.HasMedIR = Audit.MedIRVerified = true;
  Audit.DecodedInstructions = Audit.LiftedInstructions = 2;
  const auto Diagnostics =
      sourceBodyDiagnostics(Initializer, *Initializer.SourceTypeHint, &Audit);
  EXPECT_NE(Diagnostics.firstUnboundCall(), nullptr);
  EXPECT_FALSE(Diagnostics.limitation().empty());
}

TEST(SwiftOnceSources, ProjectsCanonicalObjCLazyStaticGetterThunk) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    ObjCThunkFixture F(Architecture);
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    ASSERT_EQ(Plan.ObjCThunks.size(), 1U);
    ASSERT_EQ(Plan.CallbackHints.size(), 1U);
    EXPECT_TRUE(Plan.ObjCThunks.count(ObjCThunkFixture::ThunkAddress));
    PipelineOptions Options;
    EXPECT_EQ(applySwiftOnceSourceHints(Plan, Options), 1U);
    EXPECT_TRUE(
        Options.SourceTypeHints.count(AddressorFixture::InitializerAddress));

    auto Bound = bindSwiftOnceSourceReferences(F.Pipeline.HighFuncs[0], F.Image,
                                               Plan, F.functions());
    EXPECT_EQ(Bound.Dependencies,
              std::set<va_t>{AddressorFixture::InitializerAddress});
    EXPECT_EQ(
        Bound.LocalStorageExtents,
        (std::map<va_t, uint64_t>{{AddressorFixture::PredicateAddress, 8},
                                  {AddressorFixture::StorageAddress, 8}}));
    EXPECT_EQ(Bound.SwiftOnceObjCThunks,
              std::set<va_t>{ObjCThunkFixture::ThunkAddress});
    ASSERT_TRUE(finalizeSwiftOnceObjCThunkProjection(Bound.Function, Plan));
    ASSERT_TRUE(Bound.Function.SourceTypeHint);
    EXPECT_EQ(Bound.Function.Params.size(), 2U);
    const auto &Once = *Bound.Function.Body[1].Body[0].CallExpr;
    ASSERT_EQ(Once.Operands.size(), 3U);
    EXPECT_EQ(Once.Operands[2]->Kind, ExprKind::Const);
    EXPECT_EQ(Once.Operands[2]->ConstVal, 0U);
    EXPECT_EQ(Once.Operands[2]->Type->Kind, NdTypeKind::Ptr);
    const auto Functions = F.functions();
    const auto Projection =
        bindObjCSourceReferences(Bound.Function, F.Image, nullptr, &Functions);
    EXPECT_TRUE(Projection.Limitation.empty()) << Projection.Limitation;
  }
}

TEST(SwiftOnceSources, ProjectsInlinedPredicateObjCLazyStaticGetterThunk) {
  ObjCThunkFixture F(Arch::AArch64);
  auto &Thunk = F.Pipeline.HighFuncs[0];
  ASSERT_EQ(Thunk.Body.size(), 6U);
  auto PredicateLoad = Thunk.Body[0].Val;
  Thunk.Body[1].Cond->Operands[0]->Operands[0] = PredicateLoad;
  Thunk.Body[1].Body.pop_back();
  Thunk.Body.erase(Thunk.Body.begin());
  const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  ASSERT_EQ(Plan.ObjCThunks.size(), 1U);
  ASSERT_EQ(Plan.CallbackHints.size(), 1U);
  auto Bound =
      bindSwiftOnceSourceReferences(Thunk, F.Image, Plan, F.functions());
  ASSERT_EQ(Bound.SwiftOnceObjCThunks,
            std::set<va_t>{ObjCThunkFixture::ThunkAddress});
  ASSERT_TRUE(finalizeSwiftOnceObjCThunkProjection(Bound.Function, Plan));
  EXPECT_EQ(Bound.Function.Params.size(), 2U);
  Thunk.Body[0].Cond->Operands[0]->Operands[0] = HighExpr::makeConst(0, 8);
  EXPECT_TRUE(discoverSwiftOnceSources(F.Image, F.Pipeline).ObjCThunks.empty());
}

TEST(SwiftOnceSources, ProjectsSharedReturnObjCLazyStaticGetterThunk) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const bool SeparateRetain : {false, true}) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(SeparateRetain);
      ObjCThunkFixture F(Architecture, SeparateRetain, true);
      const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
      ASSERT_EQ(Plan.ObjCThunks.size(), 1U);
      auto Bound = bindSwiftOnceSourceReferences(F.Pipeline.HighFuncs[0],
                                                 F.Image, Plan, F.functions());
      ASSERT_EQ(Bound.SwiftOnceObjCThunks,
                std::set<va_t>{ObjCThunkFixture::ThunkAddress});
      EXPECT_EQ(Bound.Dependencies,
                std::set<va_t>{AddressorFixture::InitializerAddress});
      ASSERT_TRUE(finalizeSwiftOnceObjCThunkProjection(Bound.Function, Plan));
      ASSERT_EQ(Bound.Function.Params.size(), 2U);
      const auto &Once = *Bound.Function.Body[1].ElseBody[0].CallExpr;
      ASSERT_EQ(Once.Operands.size(), 3U);
      ASSERT_EQ(Once.Operands[2]->Kind, ExprKind::Const);
      EXPECT_EQ(Once.Operands[2]->ConstVal, 0U);
      EXPECT_EQ(Once.Operands[2]->Type->Kind, NdTypeKind::Ptr);
      const auto Functions = F.functions();
      const auto Projection = bindObjCSourceReferences(Bound.Function, F.Image,
                                                       nullptr, &Functions);
      EXPECT_TRUE(Projection.Limitation.empty()) << Projection.Limitation;
    }
  }
}

TEST(SwiftOnceSources, ProjectsIfElseSharedReturnObjCLazyStaticGetterThunk) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const bool SeparateRetain : {false, true}) {
      ObjCThunkFixture F(Architecture, SeparateRetain, true);
      F.Pipeline.HighFuncs[0].Body[1].Kind = StmtKind::IfElse;

      const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
      ASSERT_EQ(Plan.ObjCThunks.size(), 1U);
      auto Bound = bindSwiftOnceSourceReferences(F.Pipeline.HighFuncs[0],
                                                 F.Image, Plan, F.functions());
      EXPECT_EQ(Bound.SwiftOnceObjCThunks,
                std::set<va_t>{ObjCThunkFixture::ThunkAddress});
      EXPECT_TRUE(finalizeSwiftOnceObjCThunkProjection(Bound.Function, Plan));
      EXPECT_EQ(Bound.Function.Params.size(), 2U);
    }
  }
}

TEST(SwiftOnceSources, SharedReturnObjCGetterRequiresExactControlFlow) {
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ObjCThunkFixture F(Arch::AArch64, false, true);
    auto &Thunk = F.Pipeline.HighFuncs[0];
    auto &Guard = Thunk.Body[1];
    if (Mutation == 0)
      Guard.Cond->Op = NdOp::INT_NOTEQUAL;
    if (Mutation == 1)
      Guard.Body.push_back(Guard.ElseBody[0]);
    if (Mutation == 2)
      Guard.ElseBody.push_back(Guard.ElseBody[0]);
    if (Mutation == 3)
      Thunk.Body[2].CallExpr = Guard.ElseBody[0].CallExpr;
    if (Mutation == 4)
      Thunk.Body[2].GotoTarget = Thunk.Body[2].Addr;
    if (Mutation == 5)
      Thunk.Body[2].Addr = 0x2000;
    if (Mutation == 6)
      Guard.ElseBody[0].CallExpr->Operands[2] = HighExpr::makeConst(0, 8);
    if (Mutation == 7)
      Thunk.Body[5].RetVal = Thunk.Body[3].Dst;
    EXPECT_TRUE(
        discoverSwiftOnceSources(F.Image, F.Pipeline).ObjCThunks.empty());
  }
}

TEST(SwiftOnceSources, RejectsObjCLazyStaticGetterEvidenceDrift) {
  for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
    ObjCThunkFixture F(Arch::AArch64);
    auto &Thunk = F.Pipeline.HighFuncs[0];
    if (Mutation == 0)
      Thunk.Name = "forged";
    if (Mutation == 1)
      F.Image.Symbols.back().Name = "forged";
    if (Mutation == 2)
      Thunk.Body[1].Body[0].CallExpr->Operands[2] = HighExpr::makeConst(0, 8);
    if (Mutation == 3) {
      HighStmt Use;
      Use.Kind = StmtKind::ExprStmt;
      MedVar Context;
      Context.Kind = MedVar::Param;
      Context.Id = 2;
      Context.Size = 8;
      Use.Val = HighExpr::makeVar(Context, NdType::makeInt(8));
      Thunk.Body.insert(Thunk.Body.begin() + 3, std::move(Use));
    }
    EXPECT_TRUE(
        discoverSwiftOnceSources(F.Image, F.Pipeline).ObjCThunks.empty())
        << Mutation;
  }
}

TEST(SwiftOnceSources, NativeSwiftGetterPreservesRetainAndAutoreleaseCalls) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation != 12; ++Mutation) {
      SCOPED_TRACE(Mutation);
      ObjCThunkFixture F(Architecture, true);
      auto &Thunk = F.Pipeline.HighFuncs[0];
      if (Mutation == 1)
        F.Image.DyldBindSlots[ObjCThunkFixture::RetainSlot].Module =
            "/tmp/libswiftCore.dylib";
      if (Mutation == 2)
        F.Image.DyldBindSlots[0x20a0].Module = "/tmp/libobjc.A.dylib";
      if (Mutation == 3)
        Thunk.Body[5].Val->Operands[0] = Thunk.Body[3].Dst;
      if (Mutation == 4)
        Thunk.Body[6].RetVal = Thunk.Body[4].Dst;
      if (Mutation == 5)
        Thunk.Body[5].Val->IsIndirectCall = true;
      if (Mutation == 6)
        F.Image.DyldBindSlots[ObjCThunkFixture::RetainSlot].WeakImport = true;
      if (Mutation == 7)
        Thunk.Body[4].Val->MemoryOrdering = NdMemoryOrdering::Acquire;
      if (Mutation == 8)
        Thunk.Body[5].Val->MemoryOrdering = NdMemoryOrdering::Acquire;
      if (Mutation == 9)
        Thunk.Body[1].Body[0].Val = HighExpr::makeConst(1, 8);
      if (Mutation == 10)
        Thunk.Body[1].Body[0].Body.push_back(Thunk.Body[4]);
      if (Mutation == 11)
        Thunk.Body[1].Body[0].MemoryOrdering = NdMemoryOrdering::Acquire;
      const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
      if (Mutation) {
        EXPECT_TRUE(Plan.ObjCThunks.empty());
        continue;
      }
      ASSERT_EQ(Plan.ObjCThunks.size(), 1U);
      auto Bound =
          bindSwiftOnceSourceReferences(Thunk, F.Image, Plan, F.functions());
      ASSERT_TRUE(finalizeSwiftOnceObjCThunkProjection(Bound.Function, Plan));
      EXPECT_EQ(Bound.Function.Params.size(), 2U);
      ASSERT_EQ(Bound.Function.Body.size(), 7U);
      EXPECT_EQ(Bound.Function.Body[4].Val->SourceCallHint->TargetName,
                "swift_retain");
      EXPECT_EQ(Bound.Function.Body[5].Val->SourceCallHint->TargetName,
                "objc_autoreleaseReturnValue");
      EXPECT_EQ(Bound.Function.Body[1].Body[1].CallExpr->Operands[2]->ConstVal,
                0U);
      const auto Functions = F.functions();
      const auto Projection = bindObjCSourceReferences(Bound.Function, F.Image,
                                                       nullptr, &Functions);
      EXPECT_TRUE(Projection.Limitation.empty()) << Projection.Limitation;
    }
  }
}

TEST(SwiftOnceSources, ProjectsBridgedLazyClassGetterAndPreservesBridge) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const bool Array : {false, true}) {
      ObjCBridgedGetterFixture F(Architecture, Array);
      const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
      ASSERT_EQ(Plan.ObjCThunks.size(), 1U);
      ASSERT_EQ(Plan.CallbackHints.size(), 1U);
      auto Bound = bindSwiftOnceSourceReferences(F.Pipeline.HighFuncs[0],
                                                 F.Image, Plan, F.functions());
      ASSERT_EQ(Bound.SwiftOnceObjCThunks,
                std::set<va_t>{ObjCThunkFixture::ThunkAddress});
      EXPECT_EQ(Bound.Dependencies,
                std::set<va_t>{AddressorFixture::InitializerAddress});
      ASSERT_TRUE(finalizeSwiftOnceObjCThunkProjection(Bound.Function, Plan));
      ASSERT_EQ(Bound.Function.Params.size(), 2U);
      ASSERT_EQ(Bound.Function.Body.size(), 7U);
      const auto &Once = *Bound.Function.Body[1].Body[0].CallExpr;
      ASSERT_EQ(Once.Operands[2]->Kind, ExprKind::Const);
      EXPECT_EQ(Once.Operands[2]->ConstVal, 0U);
      const auto &Bridge = Bound.Function.Body[4];
      ASSERT_TRUE(Bridge.Val && Bridge.Val->SourceCallHint);
      EXPECT_EQ(Bridge.Val->SourceCallHint->TargetAddress,
                ObjCBridgedGetterFixture::BridgeSlot);
      EXPECT_EQ(Bound.Function.Body[5].Val->Operands[0]->Var, Bridge.Dst->Var);
      const auto Functions = F.functions();
      const auto Projection = bindObjCSourceReferences(Bound.Function, F.Image,
                                                       nullptr, &Functions);
      EXPECT_TRUE(Projection.Limitation.empty()) << Projection.Limitation;
    }
  }
}

TEST(SwiftOnceSources, RejectsBridgedLazyClassGetterEvidenceDrift) {
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ObjCBridgedGetterFixture F(Arch::AArch64);
    auto &Thunk = F.Pipeline.HighFuncs[0];
    if (Mutation == 0)
      F.Image.ObjCMethods[0].IsClassMethod = false;
    if (Mutation == 1)
      F.Image.ObjCMethods[0].Selector = "value:";
    if (Mutation == 2) {
      auto Hint = std::make_shared<SourceCallTypeHint>(
          *Thunk.Body[4].Val->SourceCallHint);
      Hint->TargetName = "forged_bridge";
      Thunk.Body[4].Val->SourceCallHint = std::move(Hint);
    }
    if (Mutation == 3)
      F.Image.DyldBindSlots[ObjCBridgedGetterFixture::BridgeSlot].Module =
          "/tmp/Foundation";
    if (Mutation == 4) {
      HighStmt Use;
      Use.Kind = StmtKind::ExprStmt;
      MedVar Context;
      Context.Kind = MedVar::Param;
      Context.Id = 2;
      Context.Size = 8;
      Use.Val = HighExpr::makeVar(Context, NdType::makeInt(8));
      Thunk.Body.insert(Thunk.Body.end() - 1, std::move(Use));
    }
    if (Mutation == 5) {
      HighStmt Use;
      Use.Kind = StmtKind::ExprStmt;
      MedVar Context;
      Context.Kind = MedVar::Param;
      Context.Id = 0;
      Context.Size = 8;
      Use.Val = HighExpr::makeVar(Context, NdType::makeInt(8));
      F.Pipeline.HighFuncs[1].Body.insert(F.Pipeline.HighFuncs[1].Body.begin(),
                                          std::move(Use));
    }
    if (Mutation == 6)
      F.Image.Symbols.push_back(F.Image.Symbols.back());
    if (Mutation == 7)
      Thunk.Body.push_back(Thunk.Body[1].Body[0]);
    if (Mutation == 8)
      Thunk.Body[4].Val = HighExpr::makeConst(0, 8);
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    if (Mutation != 5)
      EXPECT_TRUE(Plan.ObjCThunks.empty());
    auto Bound =
        bindSwiftOnceSourceReferences(Thunk, F.Image, Plan, F.functions());
    EXPECT_TRUE(Bound.SwiftOnceObjCThunks.empty());
    EXPECT_FALSE(finalizeSwiftOnceObjCThunkProjection(Bound.Function, Plan));
  }
}

TEST(SwiftOnceSources, ProjectsConstructorOnceContextAndPreservesEffects) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    ObjCConstructorFixture F(Architecture);
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    ASSERT_EQ(Plan.ObjCThunks.size(), 1U);
    ASSERT_EQ(Plan.CallbackHints.size(), 1U);
    auto Bound = bindSwiftOnceSourceReferences(F.Pipeline.HighFuncs[0], F.Image,
                                               Plan, F.functions());
    ASSERT_EQ(Bound.SwiftOnceObjCThunks.size(), 1U);
    EXPECT_EQ(Bound.Dependencies,
              std::set<va_t>{AddressorFixture::InitializerAddress});
    EXPECT_EQ(
        Bound.LocalStorageExtents,
        (std::map<va_t, uint64_t>{{AddressorFixture::PredicateAddress, 8}}));
    ASSERT_TRUE(finalizeSwiftOnceObjCThunkProjection(Bound.Function, Plan));
    ASSERT_EQ(Bound.Function.Params.size(), 2U);
    ASSERT_EQ(Bound.Function.Body.size(), 7U);
    const auto &Once = *Bound.Function.Body[1].Body[0].CallExpr;
    ASSERT_EQ(Once.Operands[2]->Kind, ExprKind::Const);
    EXPECT_EQ(Once.Operands[2]->ConstVal, 0U);
    const auto &Publish = Bound.Function.Body[4];
    EXPECT_EQ(Publish.Kind, StmtKind::Store);
    EXPECT_EQ(Publish.StoreAddr->Operands[0]->Var.Id, 0);
    EXPECT_EQ(Publish.StoreAddr->Operands[1]->ConstVal, 8U);
    EXPECT_EQ(Publish.StoreVal->Var,
              F.Pipeline.HighFuncs[0].Body[4].StoreVal->Var);
    EXPECT_EQ(Bound.Function.Body[5].Val->SourceCallHint->TargetName,
              "objc_retainAutoreleaseReturnValue");
    EXPECT_EQ(Bound.Function.Body[6].RetVal->Var,
              F.Pipeline.HighFuncs[0].Body[6].RetVal->Var);
    // Projection works on copies; it cannot erase context in the machine body.
    EXPECT_EQ(
        F.Pipeline.HighFuncs[0].Body[1].Body[0].CallExpr->Operands[2]->Var.Id,
        2);
  }
}

TEST(SwiftOnceSources, RejectsConstructorOnceContextEvidenceDrift) {
  for (unsigned Mutation = 0; Mutation < 13; ++Mutation) {
    ObjCConstructorFixture F(Arch::AArch64);
    auto &Constructor = F.Pipeline.HighFuncs[0];
    auto &Once = Constructor.Body[1].Body[0].CallExpr;
    if (Mutation == 0)
      F.Image.ObjCMethods[0].Selector = "value";
    if (Mutation == 1)
      Constructor.Name = "unrelated";
    if (Mutation == 2)
      Constructor.Body[4].StoreVal = Once->Operands[2];
    if (Mutation == 3)
      Constructor.Body[1].Cond = Once->Operands[2];
    if (Mutation == 4)
      Once->Operands[2] = HighExpr::makeConst(0, 8);
    if (Mutation == 5)
      Constructor.ReturnType = NdType::makeFloat(8);
    if (Mutation == 6)
      for (auto &Symbol : F.Image.Symbols)
        if (Symbol.Addr == AddressorFixture::InitializerAddress)
          Symbol.Name = "wrong_initializer";
    if (Mutation == 7) {
      auto Hint = std::make_shared<SourceCallTypeHint>(*Once->SourceCallHint);
      Hint->WeakImport = true;
      Once->SourceCallHint = std::move(Hint);
    }
    if (Mutation == 8)
      F.Image.Symbols.push_back(F.Image.Symbols.back());
    if (Mutation == 9)
      Constructor.Body.push_back(Constructor.Body[1].Body[0]);
    if (Mutation == 10) {
      HighStmt Read;
      Read.Kind = StmtKind::ExprStmt;
      MedVar Context;
      Context.Kind = MedVar::Param;
      Context.Id = 0;
      Context.Size = 8;
      Read.Val =
          HighExpr::makeVar(Context, NdType::makePtr(NdType::makeVoid()));
      F.Pipeline.HighFuncs[1].Body.insert(F.Pipeline.HighFuncs[1].Body.begin(),
                                          Read);
    }
    if (Mutation == 11) {
      LowFunc DirectCaller;
      DirectCaller.Blocks.emplace_back();
      LowOp Call;
      Call.Opcode = NdOp::CALL;
      Call.addInput(NdVar::cst(AddressorFixture::InitializerAddress, 8));
      DirectCaller.Blocks[0].Ops.push_back(Call);
      F.Pipeline.LowFuncs.push_back(DirectCaller);
    }
    if (Mutation == 12)
      F.Image.ObjCMethods[0].IsClassMethod = true;
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    auto Bound = bindSwiftOnceSourceReferences(Constructor, F.Image, Plan,
                                               F.functions());
    EXPECT_TRUE(Bound.SwiftOnceObjCThunks.empty()) << Mutation;
    EXPECT_FALSE(finalizeSwiftOnceObjCThunkProjection(Bound.Function, Plan))
        << Mutation;
  }
}

TEST(SwiftOnceSources, RebuildsCanonicalObjCClassMetadataAccessorCall) {
  AddressorFixture F(Arch::AArch64);
  constexpr va_t Accessor = 0x1040;
  constexpr va_t CallerAddress = 0x10f0;
  constexpr va_t Cache = 0x2040;
  constexpr va_t ClassReference = 0x2030;
  constexpr va_t ObjCSelf = 0x2088;
  constexpr va_t MetadataRuntime = 0x2090;
  const std::string AccessorName = "_$sSo7UIColorCMa";
  F.Image.Symbols.push_back({AccessorName, Accessor, 0, true});
  F.Image.Symbols.push_back({"_$sSo7UIColorCML", Cache, 8, false});
  ObjCSourceReference Reference;
  Reference.Address = ClassReference;
  Reference.Size = 8;
  Reference.Name = "UIColor";
  Reference.TheKind = ObjCSourceReference::Kind::Class;
  F.Image.ObjCSourceReferences[ClassReference] = Reference;
  F.Image.ImportPtrSlots[ObjCSelf] = "_objc_opt_self";
  F.Image.DyldBindSlots[ObjCSelf] = {"_objc_opt_self", 0,
                                     "/usr/lib/libobjc.A.dylib", false};
  F.Image.ImportPtrSlots[MetadataRuntime] = "_swift_getObjCClassMetadata";
  F.Image.DyldBindSlots[MetadataRuntime] = {"_swift_getObjCClassMetadata", 0,
                                            "/usr/lib/swift/libswiftCore.dylib",
                                            false};

  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  const auto Integer = NdType::makeInt(8);
  auto Variable = [&](unsigned Id, const TypeRef &Type) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.Id = Id;
    V.Size = 8;
    return HighExpr::makeVar(V, Type);
  };
  auto CacheValue = Variable(1, Integer);
  auto ClassValue = Variable(2, Integer);
  auto SelfValue = Variable(3, Pointer);
  auto MetadataValue = Variable(4, Pointer);

  HighFunc MetadataAccessor;
  MetadataAccessor.Entry = Accessor;
  MetadataAccessor.Name = AccessorName;
  MetadataAccessor.ReturnType = Pointer;
  SourceFunctionTypeHint AccessorSignature;
  AccessorSignature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  AccessorSignature.ReturnType = Pointer;
  AccessorSignature.Parameters = {{"request", Integer}};
  std::string Error;
  ASSERT_TRUE(
      assignDarwinScalarSourceABI(AccessorSignature, Arch::AArch64, Error));
  MetadataAccessor.SourceTypeHint = AccessorSignature;
  MetadataAccessor.Params = {{"request", Integer}};
  HighStmt LoadCache;
  LoadCache.Kind = StmtKind::Assign;
  LoadCache.Dst = CacheValue;
  LoadCache.Val = HighExpr::makeLoad(
      HighExpr::makeConst(Cache, 8, ConstantAddressProvenance::DataAddress),
      Integer);
  HighStmt FastPath;
  FastPath.Kind = StmtKind::If;
  FastPath.Cond = HighExpr::makeBinop(NdOp::INT_NOTEQUAL, CacheValue,
                                      HighExpr::makeConst(0, 8));
  HighStmt FastReturn;
  FastReturn.Kind = StmtKind::Return;
  FastReturn.RetVal = CacheValue;
  FastPath.Body = {FastReturn};
  HighStmt LoadClass;
  LoadClass.Kind = StmtKind::Assign;
  LoadClass.Dst = ClassValue;
  LoadClass.Val = HighExpr::makeLoad(
      HighExpr::makeConst(ClassReference, 8,
                          ConstantAddressProvenance::DataAddress),
      Integer);
  HighStmt GetSelf;
  GetSelf.Kind = StmtKind::Assign;
  GetSelf.Dst = SelfValue;
  GetSelf.Val = HighExpr::makeCall("objc_opt_self", ObjCSelf, {ClassValue});
  const auto ObjCSelfHint = objcRuntimeSourceCallHint(F.Image, ObjCSelf);
  ASSERT_TRUE(ObjCSelfHint);
  GetSelf.Val->Type = Pointer;
  GetSelf.Val->SourceCallHint =
      std::make_shared<SourceCallTypeHint>(*ObjCSelfHint);
  GetSelf.Val->CallAddr = 0;
  HighStmt GetMetadata;
  GetMetadata.Kind = StmtKind::Assign;
  GetMetadata.Dst = MetadataValue;
  GetMetadata.Val = HighExpr::makeCall("swift_getObjCClassMetadata",
                                       MetadataRuntime, {SelfValue});
  const auto MetadataHint =
      swiftRuntimeSourceCallHint(F.Image, MetadataRuntime);
  ASSERT_TRUE(MetadataHint);
  GetMetadata.Val->Type = Pointer;
  GetMetadata.Val->SourceCallHint =
      std::make_shared<SourceCallTypeHint>(*MetadataHint);
  GetMetadata.Val->CallAddr = 0;
  HighStmt Publish;
  Publish.Kind = StmtKind::Store;
  Publish.StoreAddr =
      HighExpr::makeConst(Cache, 8, ConstantAddressProvenance::DataAddress);
  Publish.StoreVal = MetadataValue;
  Publish.MemoryOrdering = NdMemoryOrdering::Release;
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = MetadataValue;
  MetadataAccessor.Body = {LoadCache,   FastPath, LoadClass, GetSelf,
                           GetMetadata, Publish,  Return};

  HighFunc Caller;
  Caller.Entry = CallerAddress;
  Caller.Name = "metadata_user";
  Caller.ReturnType = Pointer;
  Caller.SourceTypeHint = swift_once_source_detail::callbackHint(Arch::AArch64);
  Caller.SourceTypeHint->Parameters.clear();
  Return.RetVal =
      HighExpr::makeCall(AccessorName, Accessor, {HighExpr::makeConst(0, 8)});
  Return.RetVal->Type = Pointer;
  auto NativeCall = std::make_shared<SourceCallTypeHint>();
  NativeCall->CallKind = SourceCallTypeHint::Kind::Native;
  NativeCall->TargetAddress = Accessor;
  NativeCall->TargetName = AccessorName;
  NativeCall->Signature = AccessorSignature;
  Return.RetVal->SourceCallHint = std::move(NativeCall);
  Return.RetVal->CallAddr = 0;
  Caller.Body = {Return};
  F.Pipeline.HighFuncs.push_back(MetadataAccessor);
  F.Pipeline.HighFuncs.push_back(Caller);

  const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  ASSERT_EQ(Plan.ObjCClassMetadataAccessors.size(), 1U);
  const auto Bound = bindSwiftOnceSourceReferences(
      F.Pipeline.HighFuncs.back(), F.Image, Plan, F.functions());
  EXPECT_TRUE(Bound.Dependencies.empty());
  const auto Call = Bound.Function.Body[0].RetVal;
  ASSERT_TRUE(Call->SourceCallHint);
  EXPECT_EQ(Call->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::SwiftRuntimeCall);
  EXPECT_EQ(Call->SourceCallHint->TargetName, "swift_getObjCClassMetadata");
  ASSERT_EQ(Call->Operands.size(), 1U);
  ASSERT_TRUE(Call->Operands[0]->SourceCallHint);
  EXPECT_EQ(Call->Operands[0]->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeClass);
  EXPECT_EQ(Call->Operands[0]->SourceCallHint->TargetName, "UIColor");
  EXPECT_TRUE(
      bindObjCSourceReferences(Bound.Function, F.Image).Limitation.empty());

  SourceFunctionTypeHint ZeroParameterSignature;
  ZeroParameterSignature.Origin =
      SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  ZeroParameterSignature.ReturnType = Pointer;
  Error.clear();
  ASSERT_TRUE(assignDarwinScalarSourceABI(ZeroParameterSignature, Arch::AArch64,
                                          Error));
  MetadataAccessor.Params.clear();
  MetadataAccessor.SourceTypeHint = ZeroParameterSignature;
  Caller.Body[0].RetVal->Operands.clear();
  auto ZeroParameterCall = std::make_shared<SourceCallTypeHint>(
      *Caller.Body[0].RetVal->SourceCallHint);
  ZeroParameterCall->Signature = ZeroParameterSignature;
  Caller.Body[0].RetVal->SourceCallHint = std::move(ZeroParameterCall);
  F.Pipeline.HighFuncs[F.Pipeline.HighFuncs.size() - 2] = MetadataAccessor;
  F.Pipeline.HighFuncs.back() = Caller;
  const auto ZeroParameterPlan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  ASSERT_EQ(ZeroParameterPlan.ObjCClassMetadataAccessors.size(), 1U);
  const auto ZeroParameterBound = bindSwiftOnceSourceReferences(
      F.Pipeline.HighFuncs.back(), F.Image, ZeroParameterPlan, F.functions());
  ASSERT_TRUE(ZeroParameterBound.Function.Body[0].RetVal->SourceCallHint);
  EXPECT_EQ(
      ZeroParameterBound.Function.Body[0].RetVal->SourceCallHint->CallKind,
      SourceCallTypeHint::Kind::SwiftRuntimeCall);

  auto Drifted = MetadataAccessor;
  Drifted.Body[5].MemoryOrdering = NdMemoryOrdering::None;
  F.Pipeline.HighFuncs[F.Pipeline.HighFuncs.size() - 2] = std::move(Drifted);
  EXPECT_TRUE(discoverSwiftOnceSources(F.Image, F.Pipeline)
                  .ObjCClassMetadataAccessors.empty());
}

TEST(SwiftOnceSources, RejectsAddressorEvidenceDrift) {
  for (unsigned Case = 0; Case < 15; ++Case) {
    SCOPED_TRACE(Case);
    AddressorFixture F(Arch::AArch64);
    auto &Accessor = F.Pipeline.HighFuncs[0];
    auto &Initializer = F.Pipeline.HighFuncs[1];
    auto &Caller = F.Pipeline.HighFuncs[2];
    if (Case == 0)
      Accessor.Name += "x";
    if (Case == 1)
      F.Image.Symbols[2].Name += "x";
    if (Case == 2)
      F.Image.Symbols[3].Name += "x";
    if (Case == 3)
      F.Once->Operands[0] = HighExpr::makeConst(0x2018, 8);
    if (Case == 4)
      F.Once->Operands[2] = F.Once->Operands[1];
    if (Case == 5)
      Accessor.Body[1].Body[1].RetVal = HighExpr::makeConst(0x2018, 8);
    if (Case == 6)
      Accessor.Body[1].Cond->Op = NdOp::INT_EQUAL;
    if (Case == 7) {
      HighStmt Use;
      Use.Kind = StmtKind::ExprStmt;
      MedVar Context;
      Context.Kind = MedVar::Param;
      Context.Id = 0;
      Context.Size = 8;
      Use.Val = HighExpr::makeVar(Context, NdType::makePtr(NdType::makeVoid()));
      Initializer.Body.insert(Initializer.Body.begin(), Use);
    }
    if (Case == 8)
      Caller.Body[0].RetVal->Operands.push_back(HighExpr::makeConst(1, 8));
    if (Case == 9) {
      LowFunc DirectCaller;
      DirectCaller.Blocks.emplace_back();
      LowOp Call;
      Call.Opcode = NdOp::CALL;
      Call.addInput(NdVar::cst(AddressorFixture::InitializerAddress, 8));
      DirectCaller.Blocks[0].Ops.push_back(Call);
      F.Pipeline.LowFuncs.push_back(DirectCaller);
    }
    if (Case == 10) {
      HighStmt Effect;
      Effect.Kind = StmtKind::ExprStmt;
      Effect.Val = HighExpr::makeConst(1, 8);
      Accessor.Body[2].Body.push_back(Effect);
    }
    if (Case == 11)
      Accessor.Params[1].Type = NdType::makeInt(4);
    if (Case == 12)
      Accessor.ReturnType = NdType::makeVoid();
    if (Case == 13 || Case == 14) {
      auto Forged = std::make_shared<SourceCallTypeHint>(
          *Caller.Body[0].RetVal->SourceCallHint);
      if (Case == 13)
        Forged->TargetAddress += 8;
      else
        Forged->Signature.ReturnType = NdType::makeInt(8);
      Caller.Body[0].RetVal->SourceCallHint = std::move(Forged);
    }
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    const auto Bound =
        bindSwiftOnceSourceReferences(Caller, F.Image, Plan, F.functions());
    EXPECT_TRUE(Bound.Dependencies.empty());
    EXPECT_TRUE(Bound.LocalStorageExtents.empty());
    EXPECT_TRUE(Bound.SwiftOnceAccessors.empty());
  }
}

TEST(SwiftOnceSources, RejectsUnprovedParameterUsesAndCallbackContext) {
  for (unsigned Case = 0; Case < 12; ++Case) {
    OnceFixture F(Arch::AArch64);
    auto &Getter = F.Pipeline.HighFuncs[0];
    auto &Callback = F.Pipeline.HighFuncs[1];
    if (Case == 0)
      F.Once->Operands[2] = F.Once->Operands[1];
    if (Case == 1)
      Getter.Body[2].RetVal = F.Once->Operands[0];
    if (Case == 2)
      Getter.Body[2].RetVal->Type = NdType::makeInt(4);
    if (Case == 3)
      Getter.Body[0].Val->MemoryOrdering = NdMemoryOrdering::Acquire;
    if (Case == 4)
      F.Once->Operands[1]->Var.SSAVer = 1;
    if (Case == 5) {
      HighStmt Use;
      Use.Kind = StmtKind::ExprStmt;
      Use.Val = F.Once->Operands[0];
      Callback.Body.insert(Callback.Body.begin(), Use);
    }
    if (Case == 6)
      F.Image.Segments[1].Data[0] = 1;
    if (Case == 7)
      F.Image.Symbols.clear();
    if (Case == 8)
      F.Once->IntrinsicOutputs.push_back({});
    if (Case == 9)
      Callback.SourceTypeHint->ReturnType = NdType::makeInt(8);
    if (Case == 10)
      F.Pipeline.HighFuncs.back().Body[0].RetVal->SourceCallHint =
          std::make_shared<SourceCallTypeHint>();
    if (Case == 11)
      F.Pipeline.SourceImage = nullptr;
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    auto Bound = bindSwiftOnceSourceReferences(F.Pipeline.HighFuncs.back(),
                                               F.Image, Plan, F.functions());
    EXPECT_TRUE(Bound.Dependencies.empty()) << Case;
    EXPECT_TRUE(Bound.LocalStorageExtents.empty()) << Case;
  }
}
TEST(SwiftOnceSources, RejectsForgedCallbackEffectsAndKeepsExistingABI) {
  OnceFixture F(Arch::AArch64);
  const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  const auto Bound = bindSwiftOnceSourceReferences(
      F.Pipeline.HighFuncs.back(), F.Image, Plan, F.functions());
  ASSERT_EQ(Bound.Dependencies.size(), 1U);
  const auto Callback = Bound.Function.Body[0].RetVal->Operands[2];
  for (unsigned Case = 0; Case < 3; ++Case) {
    auto Forged = *Callback;
    if (Case == 0)
      Forged.IntrinsicOutputs.push_back({});
    if (Case == 1)
      Forged.CallAddr = 0x1080;
    if (Case == 2)
      Forged.CallTarget = "unexpected";
    EXPECT_FALSE(swiftOnceCallbackBound(Forged, F.Image, Plan, F.functions()));
  }
  PipelineOptions Options;
  auto Existing = *F.Pipeline.HighFuncs[1].SourceTypeHint;
  Existing.ReturnType = NdType::makeInt(8);
  Options.SourceTypeHints.emplace(0x1080, Existing);
  EXPECT_EQ(applySwiftOnceSourceHints(Plan, Options), 0U);
  EXPECT_EQ(Options.SourceTypeHints.at(0x1080).ReturnType->Kind,
            NdTypeKind::Int);
}

TEST(SwiftOnceSources, OrdinaryDirectCallerPreventsCallbackABIOverride) {
  OnceFixture F(Arch::AArch64);
  LowFunc DirectCaller;
  DirectCaller.Entry = 0x1040;
  DirectCaller.Blocks.emplace_back();
  LowOp Call;
  Call.Opcode = NdOp::CALL;
  Call.addInput(NdVar::cst(0x1080, 8));
  DirectCaller.Blocks[0].Ops.push_back(Call);
  F.Pipeline.LowFuncs.push_back(DirectCaller);
  const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  EXPECT_EQ(Plan.Getters.size(), 1U);
  EXPECT_TRUE(Plan.CallbackHints.empty());
  const auto Bound = bindSwiftOnceSourceReferences(
      F.Pipeline.HighFuncs.back(), F.Image, Plan, F.functions());
  EXPECT_TRUE(Bound.Dependencies.empty());
  EXPECT_TRUE(Bound.LocalStorageExtents.empty());
}

TEST(SwiftOnceSources, SharedObjectGetterIgnoresTwoLeadingObjCRegisters) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    OnceFixture F(Architecture);
    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    auto Param = [&](unsigned Id) {
      MedVar V;
      V.Kind = MedVar::Param;
      V.Id = Id;
      V.Size = 8;
      return HighExpr::makeVar(V, Pointer);
    };
    auto &Getter = F.Pipeline.HighFuncs[0];
    Getter.Params = {{"objc_self", Pointer},
                     {"objc_cmd", Pointer},
                     {"predicate", Pointer},
                     {"storage", Pointer},
                     {"initializer", Pointer}};
    SourceFunctionTypeHint Signature;
    Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Signature.ReturnType = Pointer;
    for (const auto &P : Getter.Params)
      Signature.Parameters.push_back({P.Name, P.Type});
    std::string Error;
    ASSERT_TRUE(assignDarwinScalarSourceABI(Signature, Architecture, Error))
        << Error;
    Getter.SourceTypeHint = Signature;
    Getter.Body[0].Val->Operands[0] = Param(2);
    F.Once->Operands = {Param(2), Param(4), Param(2)};
    Getter.Body[2].RetVal->Operands[0] = Param(3);
    auto &Call = F.Pipeline.HighFuncs.back().Body[0].RetVal;
    Call->Operands.insert(Call->Operands.begin(), {HighExpr::makeConst(0, 8),
                                                   HighExpr::makeConst(0, 8)});
    auto Hint = std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
    Hint->Signature = Signature;
    Call->SourceCallHint = std::move(Hint);

    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    ASSERT_EQ(Plan.Getters.size(), 1U);
    ASSERT_EQ(Plan.CallbackHints.size(), 1U);
    EXPECT_EQ(Plan.Getters.begin()->second.parameterCount(), 5U);
    const auto Bound = bindSwiftOnceSourceReferences(
        F.Pipeline.HighFuncs.back(), F.Image, Plan, F.functions());
    EXPECT_EQ(Bound.Dependencies, std::set<va_t>{0x1080});
    EXPECT_EQ(Bound.LocalStorageExtents,
              (std::map<va_t, uint64_t>{{0x2000, 8}, {0x2010, 8}}));
    EXPECT_TRUE(
        bindObjCSourceReferences(Bound.Function, F.Image).Limitation.empty());

    auto &Callback = F.Pipeline.HighFuncs[1];
    Callback.Body[0].RetVal = Param(0);
    const auto UnsafeCallback = discoverSwiftOnceSources(F.Image, F.Pipeline);
    EXPECT_EQ(UnsafeCallback.Getters.size(), 1U);
    EXPECT_TRUE(UnsafeCallback.CallbackHints.empty());
    EXPECT_TRUE(bindSwiftOnceSourceReferences(F.Pipeline.HighFuncs.back(),
                                              F.Image, UnsafeCallback,
                                              F.functions())
                    .Dependencies.empty());
    Callback.Body[0].RetVal.reset();

    HighStmt Escape;
    Escape.Kind = StmtKind::ExprStmt;
    Escape.Val = Param(0);
    Getter.Body.insert(Getter.Body.begin(), Escape);
    EXPECT_TRUE(discoverSwiftOnceSources(F.Image, F.Pipeline).Getters.empty());
  }
}

TEST(SwiftOnceSources, UntypedSharedObjectGetterProvesFiveParameterABI) {
  OnceFixture F(Arch::AArch64);
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto Param = [&](unsigned Id) {
    MedVar V;
    V.Kind = MedVar::Param;
    V.Id = Id;
    V.Size = 8;
    return HighExpr::makeVar(V, Pointer);
  };
  auto &Getter = F.Pipeline.HighFuncs[0];
  Getter.Params = {{"objc_self", Pointer},
                   {"objc_cmd", Pointer},
                   {"predicate", Pointer},
                   {"storage", Pointer},
                   {"initializer", Pointer}};
  Getter.SourceTypeHint.reset();
  Getter.Body[0].Val->Operands[0] = Param(2);
  F.Once->Operands = {Param(2), Param(4), Param(2)};
  Getter.Body[2].RetVal->Operands[0] = Param(3);

  const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  ASSERT_EQ(Plan.Getters.size(), 1U);
  ASSERT_EQ(Plan.GetterHints.size(), 1U);
  EXPECT_EQ(Plan.Getters.begin()->second.parameterCount(), 5U);
  PipelineOptions Options;
  EXPECT_GE(applySwiftOnceSourceHints(Plan, Options), 1U);
  Getter.SourceTypeHint = Options.SourceTypeHints.at(Getter.Entry);
  auto &Call = F.Pipeline.HighFuncs.back().Body[0].RetVal;
  Call->Operands.insert(Call->Operands.begin(),
                        {HighExpr::makeConst(0, 8), HighExpr::makeConst(0, 8)});
  auto Hint = std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
  Hint->Signature = *Getter.SourceTypeHint;
  Call->SourceCallHint = std::move(Hint);
  const auto TypedPlan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  ASSERT_EQ(TypedPlan.Getters.size(), 1U);
  ASSERT_EQ(TypedPlan.CallbackHints.size(), 1U);
  EXPECT_TRUE(TypedPlan.GetterHints.empty());
  const auto Bound = bindSwiftOnceSourceReferences(
      F.Pipeline.HighFuncs.back(), F.Image, TypedPlan, F.functions());
  EXPECT_EQ(Bound.Dependencies, std::set<va_t>{0x1080});
  EXPECT_TRUE(
      bindObjCSourceReferences(Bound.Function, F.Image).Limitation.empty());

  Getter.SourceTypeHint.reset();
  HighStmt Escape;
  Escape.Kind = StmtKind::ExprStmt;
  Escape.Val = Param(0);
  Getter.Body.insert(Getter.Body.begin(), Escape);
  EXPECT_TRUE(discoverSwiftOnceSources(F.Image, F.Pipeline).Getters.empty());
}

TEST(SwiftOnceSources, SharedObjectGetterBindsInteriorMergedStorage) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    OnceFixture F(Architecture);
    F.Image.Symbols = {{"_predicate", 0x2000, 8, false},
                       {"_MergedGlobals", 0x2020, 0, false},
                       {"_nextStorage", 0x2040, 8, false}};
    auto &Call = F.Pipeline.HighFuncs.back().Body[0].RetVal;
    Call->Operands[1] = HighExpr::makeConst(0x2030, 8);

    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    ASSERT_EQ(Plan.Getters.size(), 1U);
    ASSERT_EQ(Plan.CallbackHints.size(), 1U);
    const auto Bound = bindSwiftOnceSourceReferences(
        F.Pipeline.HighFuncs.back(), F.Image, Plan, F.functions());
    EXPECT_EQ(Bound.Dependencies, std::set<va_t>{0x1080});
    EXPECT_EQ(Bound.LocalStorageExtents,
              (std::map<va_t, uint64_t>{{0x2000, 8}, {0x2020, 24}}));
    const auto Storage = Bound.Function.Body[0].RetVal->Operands[1];
    ASSERT_EQ(Storage->Kind, ExprKind::BinOp);
    ASSERT_EQ(Storage->Operands.size(), 2U);
    ASSERT_TRUE(Storage->Operands[0]->SourceCallHint);
    EXPECT_EQ(Storage->Operands[0]->SourceCallHint->TargetAddress, 0x2020U);
    EXPECT_EQ(Storage->Operands[0]->SourceCallHint->ByteCount, 24U);
    EXPECT_EQ(Storage->Operands[1]->ConstVal, 16U);
    EXPECT_TRUE(
        objcSourceCallBound(*Storage->Operands[0], F.Image, F.functions()));
    EXPECT_TRUE(
        bindObjCSourceReferences(Bound.Function, F.Image).Limitation.empty());

    auto Overlap = F.Image;
    Overlap.Symbols.push_back({"_intervening", 0x2034, 4, false});
    EXPECT_TRUE(bindSwiftOnceSourceReferences(F.Pipeline.HighFuncs.back(),
                                              Overlap, Plan, F.functions())
                    .Dependencies.empty());

    auto Relocated = F.Image;
    Relocated.DataPtrRelocSlots.insert(0x2030);
    EXPECT_TRUE(bindSwiftOnceSourceReferences(F.Pipeline.HighFuncs.back(),
                                              Relocated, Plan, F.functions())
                    .Dependencies.empty());
  }
}

TEST(SwiftOnceSources, BindsContiguousSwiftStringStorageAsOneSharedObject) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    StringOnceFixture F(Architecture);
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    ASSERT_EQ(Plan.Getters.size(), 1U);
    ASSERT_EQ(Plan.CallbackHints.size(), 1U);
    const auto &Contract = Plan.Getters.begin()->second;
    EXPECT_EQ(Contract.parameterCount(), 4U);
    EXPECT_EQ(Contract.storageWidth(), 16U);
    const auto Bound = bindSwiftOnceSourceReferences(
        F.Pipeline.HighFuncs.back(), F.Image, Plan, F.functions());
    EXPECT_EQ(Bound.Dependencies, std::set<va_t>{0x1080});
    EXPECT_EQ(Bound.LocalStorageExtents,
              (std::map<va_t, uint64_t>{{0x2000, 8}, {0x2020, 16}}));
    const auto Call = Bound.Function.Body[0].RetVal;
    ASSERT_EQ(Call->Operands.size(), 4U);
    ASSERT_EQ(Call->Operands[2]->Kind, ExprKind::BinOp);
    EXPECT_EQ(Call->Operands[2]->Op, NdOp::INT_ADD);
    ASSERT_EQ(Call->Operands[2]->Operands.size(), 2U);
    EXPECT_EQ(Call->Operands[2]->Operands[1]->ConstVal, 8U);
    EXPECT_EQ(Call->Operands[1]->SourceCallHint->TargetAddress, 0x2020U);
    EXPECT_EQ(Call->Operands[2]->Operands[0]->SourceCallHint->TargetAddress,
              0x2020U);
    EXPECT_TRUE(swiftOnceCallbackBound(*Call->Operands[3], F.Image, Plan,
                                       F.functions()));
    EXPECT_TRUE(
        bindObjCSourceReferences(Bound.Function, F.Image).Limitation.empty());
  }
}

TEST(SwiftOnceSources, BindsInteriorSwiftStringWordsAsOneSharedObject) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    StringOnceFixture F(Architecture);
    F.Image.Symbols = {{"_predicate", 0x2000, 8, false},
                       {"_MergedGlobals", 0x2020, 0, false},
                       {"_nextStorage", 0x2050, 8, false}};
    auto &Call = F.Pipeline.HighFuncs.back().Body[0].RetVal;
    Call->Operands[1] = HighExpr::makeConst(0x2030, 8);
    Call->Operands[2] = HighExpr::makeConst(0x2038, 8);

    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    ASSERT_EQ(Plan.Getters.size(), 1U);
    ASSERT_EQ(Plan.CallbackHints.size(), 1U);
    const auto Bound = bindSwiftOnceSourceReferences(
        F.Pipeline.HighFuncs.back(), F.Image, Plan, F.functions());
    EXPECT_EQ(Bound.Dependencies, std::set<va_t>{0x1080});
    EXPECT_EQ(Bound.LocalStorageExtents,
              (std::map<va_t, uint64_t>{{0x2000, 8}, {0x2020, 32}}));
    for (unsigned Index = 1; Index <= 2; ++Index) {
      const auto Word = Bound.Function.Body[0].RetVal->Operands[Index];
      ASSERT_EQ(Word->Kind, ExprKind::BinOp);
      ASSERT_EQ(Word->Operands.size(), 2U);
      ASSERT_TRUE(Word->Operands[0]->SourceCallHint);
      EXPECT_EQ(Word->Operands[0]->SourceCallHint->TargetAddress, 0x2020U);
      EXPECT_EQ(Word->Operands[0]->SourceCallHint->ByteCount, 32U);
      EXPECT_EQ(Word->Operands[1]->ConstVal, Index == 1 ? 16U : 24U);
    }
    EXPECT_TRUE(
        bindObjCSourceReferences(Bound.Function, F.Image).Limitation.empty());

    auto Intervening = F.Image;
    Intervening.Symbols.push_back({"_intervening", 0x203c, 4, false});
    EXPECT_TRUE(bindSwiftOnceSourceReferences(F.Pipeline.HighFuncs.back(),
                                              Intervening, Plan, F.functions())
                    .Dependencies.empty());
  }
}

TEST(SwiftOnceSources,
     SharedStringGetterMayIgnoreTwoLeadingObjCRegisterCarriers) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    StringOnceFixture F(Architecture);
    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    const auto Integer = NdType::makeInt(8, false);
    auto Param = [&](unsigned Id, const TypeRef &Type) {
      MedVar V;
      V.Kind = MedVar::Param;
      V.Id = Id;
      V.Size = 8;
      return HighExpr::makeVar(V, Type);
    };
    auto &Getter = F.Pipeline.HighFuncs[0];
    Getter.Params = {{"objc_self", Pointer},      {"objc_cmd", Pointer},
                     {"predicate", Pointer},      {"string_word", Integer},
                     {"string_storage", Integer}, {"initializer", Pointer}};
    SourceFunctionTypeHint Signature;
    Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Signature.ReturnType = Pointer;
    for (const auto &P : Getter.Params)
      Signature.Parameters.push_back({P.Name, P.Type});
    std::string Error;
    ASSERT_TRUE(assignDarwinScalarSourceABI(Signature, Architecture, Error))
        << Error;
    Getter.SourceTypeHint = Signature;
    Getter.Body[0].Val =
        HighExpr::makeLoad(Param(2, Pointer), NdType::makeInt(8));
    F.Once->Operands = {Param(2, Pointer), Param(5, Pointer),
                        Param(2, Pointer)};
    F.Bridge->Operands = {HighExpr::makeLoad(Param(3, Integer), Integer),
                          HighExpr::makeLoad(Param(4, Integer), Integer)};
    auto &Call = F.Pipeline.HighFuncs.back().Body[0].RetVal;
    Call->Operands.insert(Call->Operands.begin(), {HighExpr::makeConst(0, 8),
                                                   HighExpr::makeConst(0, 8)});
    auto Hint = std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
    Hint->Signature = Signature;
    Call->SourceCallHint = Hint;

    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    ASSERT_EQ(Plan.Getters.size(), 1U);
    ASSERT_EQ(Plan.CallbackHints.size(), 1U);
    EXPECT_EQ(Plan.Getters.begin()->second.parameterCount(), 6U);
    const auto Bound = bindSwiftOnceSourceReferences(
        F.Pipeline.HighFuncs.back(), F.Image, Plan, F.functions());
    EXPECT_EQ(Bound.Dependencies, std::set<va_t>{0x1080});
    EXPECT_EQ(Bound.LocalStorageExtents,
              (std::map<va_t, uint64_t>{{0x2000, 8}, {0x2020, 16}}));
    EXPECT_TRUE(
        bindObjCSourceReferences(Bound.Function, F.Image).Limitation.empty());

    HighStmt Escape;
    Escape.Kind = StmtKind::ExprStmt;
    Escape.Val = Param(0, Pointer);
    Getter.Body.insert(Getter.Body.begin(), Escape);
    EXPECT_TRUE(discoverSwiftOnceSources(F.Image, F.Pipeline).Getters.empty());
  }
}

TEST(SwiftOnceSources,
     UntypedSharedStringGetterRequiresCompleteSixParameterUseProof) {
  for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    const auto Architecture = Mutation == 4 ? Arch::X64 : Arch::AArch64;
    StringOnceFixture F(Architecture);
    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    const auto Integer = NdType::makeInt(8, false);
    auto Param = [&](unsigned Id, const TypeRef &Type) {
      MedVar V;
      V.Kind = MedVar::Param;
      V.Id = Id;
      V.Size = 8;
      return HighExpr::makeVar(V, Type);
    };
    auto &Getter = F.Pipeline.HighFuncs[0];
    Getter.Params = {{"objc_self", Pointer},      {"objc_cmd", Pointer},
                     {"predicate", Pointer},      {"string_word", Integer},
                     {"string_storage", Integer}, {"initializer", Pointer}};
    Getter.SourceTypeHint.reset();
    Getter.Body[0].Val =
        HighExpr::makeLoad(Param(2, Pointer), NdType::makeInt(8));
    F.Once->Operands = {Param(2, Pointer), Param(5, Pointer),
                        Param(2, Pointer)};
    F.Bridge->Operands = {HighExpr::makeLoad(Param(3, Integer), Integer),
                          HighExpr::makeLoad(Param(4, Integer), Integer)};
    if (Mutation == 1) {
      HighStmt Escape;
      Escape.Kind = StmtKind::ExprStmt;
      Escape.Val = Param(0, Pointer);
      Getter.Body.insert(Getter.Body.begin(), Escape);
    } else if (Mutation == 2) {
      Getter.ReturnType = NdType::makeVoid();
    } else if (Mutation == 3) {
      F.Once->Operands[1] = Param(0, Pointer);
    } else if (Mutation == 5) {
      ObjCMethod Method;
      Method.Implementation = Getter.Entry;
      F.Image.ObjCMethods.push_back(Method);
    }
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    if (Mutation == 0) {
      ASSERT_EQ(Plan.Getters.size(), 1U);
      ASSERT_EQ(Plan.GetterHints.size(), 1U);
      PipelineOptions Options;
      EXPECT_GE(applySwiftOnceSourceHints(Plan, Options), 1U);
      ASSERT_EQ(Options.SourceTypeHints.size(), 1U);
      EXPECT_EQ(Options.SourceTypeHints.begin()->first, Getter.Entry);
      Getter.SourceTypeHint = Options.SourceTypeHints.at(Getter.Entry);
      auto &Call = F.Pipeline.HighFuncs.back().Body[0].RetVal;
      Call->Operands.insert(
          Call->Operands.begin(),
          {HighExpr::makeConst(0, 8), HighExpr::makeConst(0, 8)});
      auto Hint = std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
      Hint->Signature = *Getter.SourceTypeHint;
      Call->SourceCallHint = std::move(Hint);
      const auto TypedPlan = discoverSwiftOnceSources(F.Image, F.Pipeline);
      EXPECT_EQ(TypedPlan.Getters.size(), 1U);
      EXPECT_EQ(TypedPlan.CallbackHints.size(), 1U);
      EXPECT_TRUE(TypedPlan.GetterHints.empty());
    } else {
      EXPECT_TRUE(Plan.Getters.empty());
      EXPECT_TRUE(Plan.GetterHints.empty());
    }
  }
}

TEST(SwiftOnceSources, RejectsUnprovedSwiftStringOnceContractsAndStorage) {
  for (unsigned Case = 0; Case < 10; ++Case) {
    SCOPED_TRACE(Case);
    StringOnceFixture F(Arch::AArch64);
    auto &Getter = F.Pipeline.HighFuncs[0];
    auto &Call = F.Pipeline.HighFuncs.back().Body[0].RetVal;
    if (Case == 0)
      std::swap(F.Bridge->Operands[0], F.Bridge->Operands[1]);
    if (Case == 1)
      Call->Operands[2] = HighExpr::makeConst(0x2030, 8);
    if (Case == 2) {
      HighStmt Escape;
      Escape.Kind = StmtKind::ExprStmt;
      Escape.Val = F.Bridge->Operands[0]->Operands[0];
      Getter.Body.insert(Getter.Body.begin(), Escape);
    }
    if (Case == 3) {
      auto Hint =
          std::make_shared<SourceCallTypeHint>(*F.Bridge->SourceCallHint);
      Hint->CallKind = SourceCallTypeHint::Kind::SwiftRuntimeCall;
      F.Bridge->SourceCallHint = std::move(Hint);
    }
    if (Case == 4)
      F.Bridge->Operands[0]->Type = NdType::makeInt(4, false);
    if (Case == 5) {
      HighStmt Duplicate;
      Duplicate.Kind = StmtKind::ExprStmt;
      Duplicate.Val = F.Bridge;
      Getter.Body.insert(Getter.Body.end() - 1, Duplicate);
    }
    if (Case == 6)
      F.Image.Symbols.back().Size = 8;
    if (Case == 7)
      F.Image.Symbols.push_back({"_split_string", 0x2028, 8, false});
    if (Case == 8)
      Call->Operands[0] = HighExpr::makeConst(0x2028, 8);
    if (Case == 9) {
      auto Hint =
          std::make_shared<SourceCallTypeHint>(*F.Bridge->SourceCallHint);
      Hint->Signature.Parameters[1].Type = NdType::makeInt(8, false);
      F.Bridge->SourceCallHint = std::move(Hint);
    }
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    const auto Bound = bindSwiftOnceSourceReferences(
        F.Pipeline.HighFuncs.back(), F.Image, Plan, F.functions());
    EXPECT_TRUE(Bound.Dependencies.empty());
    EXPECT_TRUE(Bound.LocalStorageExtents.empty());
  }
}

TEST(SwiftOnceSources, RebuildsDispatchOncePredicateAndCallbackAddress) {
  OnceFixture F(Arch::AArch64);
  constexpr va_t DispatchSlot = 0x2088;
  auto &Wrapper = F.Pipeline.HighFuncs[0];
  auto &Callback = F.Pipeline.HighFuncs[1];
  F.Image.Symbols.erase(F.Image.Symbols.begin());
  F.Image.Sections[1].Type = llvm::MachO::S_ZEROFILL;
  F.Image.ImportPtrSlots[DispatchSlot] = "_dispatch_once_f";
  F.Image.DyldBindSlots[DispatchSlot] = {
      "_dispatch_once_f", 0, "/usr/lib/system/libdispatch.dylib", false};
  const auto Runtime = darwinRuntimeSourceCallHint(F.Image, DispatchSlot);
  ASSERT_TRUE(Runtime);
  auto Call = HighExpr::makeCall("dispatch_once_f", DispatchSlot,
                                 {HighExpr::makeConst(0x2000, 8),
                                  HighExpr::makeConst(0, 8),
                                  HighExpr::makeConst(Callback.Entry, 8)});
  Call->Type = NdType::makeVoid();
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Runtime);
  HighStmt Invoke;
  Invoke.Kind = StmtKind::Call;
  Invoke.CallExpr = Call;
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Wrapper.Params.clear();
  Wrapper.ReturnType = NdType::makeVoid();
  Wrapper.SourceTypeHint->Parameters.clear();
  Wrapper.SourceTypeHint->ReturnType = NdType::makeVoid();
  Wrapper.Body = {Invoke, Return};
  Callback.SourceTypeHint =
      swift_once_source_detail::dispatchCallbackHint(F.Image.Arch);
  Callback.Params = {{"once_context", NdType::makePtr(NdType::makeVoid())}};
  HighStmt Use;
  Use.Kind = StmtKind::ExprStmt;
  MedVar Context;
  Context.Kind = MedVar::Param;
  Context.Id = 0;
  Context.Size = 8;
  Use.Val = HighExpr::makeVar(Context, NdType::makePtr(NdType::makeVoid()));
  Callback.Body.insert(Callback.Body.begin(), Use);

  const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  EXPECT_EQ(Plan.DispatchOnceCallbacks, std::set<va_t>{Callback.Entry});
  ASSERT_EQ(Plan.CallbackHints.count(Callback.Entry), 1U);
  PipelineOptions Options;
  EXPECT_EQ(applySwiftOnceSourceHints(Plan, Options), 1U);
  EXPECT_EQ(Options.SourceTypeHints.at(Callback.Entry).Origin,
            SourceFunctionTypeHint::OriginKind::DarwinSDK);

  const auto Bound =
      bindSwiftOnceSourceReferences(Wrapper, F.Image, Plan, F.functions());
  EXPECT_EQ(Bound.Dependencies, std::set<va_t>{Callback.Entry});
  EXPECT_EQ(Bound.LocalStorageExtents, (std::map<va_t, uint64_t>{{0x2000, 8}}));
  const auto BoundCall = Bound.Function.Body[0].CallExpr;
  ASSERT_TRUE(BoundCall);
  ASSERT_EQ(BoundCall->Operands.size(), 3U);
  ASSERT_TRUE(BoundCall->Operands[0]->SourceCallHint);
  EXPECT_EQ(BoundCall->Operands[0]->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeLocalStorageAddress);
  EXPECT_EQ(BoundCall->Operands[0]->SourceCallHint->TargetAddress, 0x2000U);
  ASSERT_TRUE(BoundCall->Operands[2]->SourceCallHint);
  EXPECT_EQ(BoundCall->Operands[2]->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::NativeAddress);
  EXPECT_TRUE(swiftOnceCallbackBound(*BoundCall->Operands[2], F.Image, Plan,
                                     F.functions()));
  EXPECT_TRUE(
      bindObjCSourceReferences(Bound.Function, F.Image).Limitation.empty());
}

TEST(SwiftOnceSources, RejectsUnprovedDispatchOnceReferences) {
  for (unsigned Case = 0; Case < 8; ++Case) {
    OnceFixture F(Arch::AArch64);
    constexpr va_t DispatchSlot = 0x2088;
    F.Image.ImportPtrSlots[DispatchSlot] = "_dispatch_once_f";
    F.Image.DyldBindSlots[DispatchSlot] = {
        "_dispatch_once_f", 0, "/usr/lib/system/libdispatch.dylib", false};
    const auto Runtime = darwinRuntimeSourceCallHint(F.Image, DispatchSlot);
    ASSERT_TRUE(Runtime);
    auto &Wrapper = F.Pipeline.HighFuncs[0];
    auto &Callback = F.Pipeline.HighFuncs[1];
    F.Image.Symbols.erase(F.Image.Symbols.begin());
    F.Image.Sections[1].Type = llvm::MachO::S_ZEROFILL;
    auto Call = HighExpr::makeCall("dispatch_once_f", DispatchSlot,
                                   {HighExpr::makeConst(0x2000, 8),
                                    HighExpr::makeConst(0, 8),
                                    HighExpr::makeConst(Callback.Entry, 8)});
    Call->Type = NdType::makeVoid();
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Runtime);
    HighStmt Invoke;
    Invoke.Kind = StmtKind::Call;
    Invoke.CallExpr = Call;
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Wrapper.Params.clear();
    Wrapper.ReturnType = NdType::makeVoid();
    Wrapper.SourceTypeHint->Parameters.clear();
    Wrapper.SourceTypeHint->ReturnType = NdType::makeVoid();
    Wrapper.Body = {Invoke, Return};
    Callback.SourceTypeHint =
        swift_once_source_detail::dispatchCallbackHint(F.Image.Arch);
    Callback.Params = {{"once_context", NdType::makePtr(NdType::makeVoid())}};
    if (Case == 0)
      F.Image.DyldBindSlots[DispatchSlot].Module = "/tmp/libdispatch.dylib";
    if (Case == 1)
      Call->Operands[0] = HighExpr::makeConst(0x2001, 8);
    if (Case == 2)
      Call->Operands[2] = HighExpr::makeConst(0x2010, 8);
    if (Case == 3) {
      LowFunc Direct;
      Direct.Blocks.emplace_back();
      LowOp Op;
      Op.Opcode = NdOp::CALL;
      Op.addInput(NdVar::cst(Callback.Entry, 8));
      Direct.Blocks[0].Ops.push_back(Op);
      F.Pipeline.LowFuncs.push_back(Direct);
    }
    if (Case == 4)
      Callback.SourceTypeHint->ReturnType = NdType::makeInt(8);
    if (Case == 5)
      F.Image.Sections[1].Type = 0;
    if (Case == 6)
      F.Image.Segments[1].Data[0] = 1;
    if (Case == 7)
      F.Image.Symbols.push_back({"_overlap", 0x1ff8, 16, false});
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    const auto Bound =
        bindSwiftOnceSourceReferences(Wrapper, F.Image, Plan, F.functions());
    EXPECT_TRUE(Bound.Dependencies.empty()) << Case;
    EXPECT_TRUE(Bound.LocalStorageExtents.empty()) << Case;
  }
}

struct CopyOnceFixture : OnceFixture {
  ExprPtr PredicateValue, SourceValue, RetainedValue, SourceLoad, Retain;
  CopyOnceFixture() : OnceFixture(Arch::AArch64) {
    const auto Ptr = NdType::makePtr(NdType::makeVoid());
    const auto Int = NdType::makeInt(8);
    const auto Param = [&](unsigned Id) {
      MedVar V;
      V.Kind = MedVar::Param;
      V.Id = Id;
      V.Size = 8;
      V.RegOff = Id * 8;
      V.TheArch = Arch::AArch64;
      return HighExpr::makeVar(V, Ptr);
    };
    const auto Temp = [&](unsigned Id, TypeRef Type) {
      MedVar V;
      V.Kind = MedVar::Temp;
      V.Id = Id;
      V.Size = 8;
      return HighExpr::makeVar(V, Type);
    };
    auto &Helper = Pipeline.HighFuncs[0];
    Helper.Params.clear();
    Helper.SourceTypeHint->Parameters.clear();
    for (unsigned I = 0; I < 5; ++I) {
      Helper.Params.push_back({"arg" + std::to_string(I), Ptr});
      Helper.SourceTypeHint->Parameters.push_back(
          {"arg" + std::to_string(I), Ptr});
    }
    std::string Error;
    EXPECT_TRUE(
        assignDarwinScalarSourceABI(*Helper.SourceTypeHint, Image.Arch, Error));
    Once->Operands = {Param(1), Param(4), Param(2)};
    Image.ImportPtrSlots[0x2088] = "_objc_retain";
    Image.DynInfo.NeededLibs.push_back("/usr/lib/libobjc.A.dylib");
    Image.DyldBindSlots[0x2088] = {"_objc_retain", 0,
                                   "/usr/lib/libobjc.A.dylib", false};
    const auto RetainHint = objcRuntimeSourceCallHint(Image, 0x2088);
    EXPECT_TRUE(RetainHint);
    PredicateValue = Temp(10, Int);
    SourceValue = Temp(11, Int);
    RetainedValue = Temp(12, Ptr);
    SourceLoad = HighExpr::makeLoad(Param(2), Int);
    Retain = HighExpr::makeCall("objc_retain", 0x2088, {SourceValue});
    Retain->Type = Ptr;
    Retain->SourceCallHint = std::make_shared<SourceCallTypeHint>(*RetainHint);
    HighStmt Predicate, Conditional, Invoke, Load, Store, Keep, Return;
    Predicate.Kind = StmtKind::Assign;
    Predicate.Dst = PredicateValue;
    Predicate.Val = HighExpr::makeLoad(Param(1), Int);
    Conditional.Kind = StmtKind::If;
    Conditional.Cond =
        HighExpr::makeBinop(NdOp::INT_NOTEQUAL, PredicateValue,
                            HighExpr::makeConst(~uint64_t(0), 8));
    Invoke.Kind = StmtKind::Call;
    Invoke.CallExpr = Once;
    Conditional.Body = {Invoke};
    Load.Kind = StmtKind::Assign;
    Load.Dst = SourceValue;
    Load.Val = SourceLoad;
    Store.Kind = StmtKind::Store;
    Store.StoreAddr = Param(3);
    Store.StoreVal = SourceValue;
    Keep.Kind = StmtKind::Assign;
    Keep.Dst = RetainedValue;
    Keep.Val = Retain;
    Return.Kind = StmtKind::Return;
    Return.RetVal = RetainedValue;
    Helper.Body = {Predicate, Conditional, Load, Store, Keep, Return};
    auto &Callback = Pipeline.HighFuncs[1];
    Callback.Name = "_$s4Test6source_WZ";
    auto &Caller = Pipeline.HighFuncs[2];
    Caller.Name = "_$s4Test4copy_WZ";
    Caller.SourceTypeHint = swift_once_source_detail::callbackHint(Image.Arch);
    Caller.Params = {{"context", Ptr}};
    Caller.ReturnType = NdType::makeVoid();
    auto Call = HighExpr::makeCall(Helper.Name, Helper.Entry,
                                   {Param(0), HighExpr::makeConst(0x2000, 8),
                                    HighExpr::makeConst(0x2010, 8),
                                    HighExpr::makeConst(0x2020, 8),
                                    HighExpr::makeConst(Callback.Entry, 8)});
    Call->Type = Ptr;
    auto Hint = std::make_shared<SourceCallTypeHint>();
    Hint->CallKind = SourceCallTypeHint::Kind::Native;
    Hint->TargetAddress = Helper.Entry;
    Hint->Signature = *Helper.SourceTypeHint;
    Call->SourceCallHint = Hint;
    Invoke.CallExpr = Call;
    Return.RetVal.reset();
    Caller.Body = {Invoke, Return};
    Image.Symbols = {{"_$s4Test6source_Wz", 0x2000, 8, false},
                     {"_$s4Test6sourceSo8NSObjectCvpZ", 0x2010, 8, false},
                     {"_$s4Test4copySo8NSObjectCvpZ", 0x2020, 8, false},
                     {Callback.Name, Callback.Entry, 0, true},
                     {Caller.Name, Caller.Entry, 0, true}};
  }
};

TEST(SwiftOnceSources, CopyContractKeepsLoadStoreAndRetainEffects) {
  CopyOnceFixture F;
  const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  ASSERT_EQ(Plan.Copies.size(), 1U);
  const auto &Contract = Plan.Copies.at(0x1000);
  EXPECT_EQ(Contract.Predicate, 1U);
  EXPECT_EQ(Contract.Source, 2U);
  EXPECT_EQ(Contract.Destination, 3U);
  EXPECT_EQ(Contract.Initializer, 4U);
  ASSERT_EQ(Plan.CallbackHints.count(0x1080), 1U);
  const auto &Caller = F.Pipeline.HighFuncs[2];
  const auto Bound =
      bindSwiftOnceSourceReferences(Caller, F.Image, Plan, F.functions());
  EXPECT_EQ(Bound.Dependencies, std::set<va_t>{0x1080});
  EXPECT_EQ(Bound.LocalStorageExtents,
            (std::map<va_t, uint64_t>{{0x2000, 8}, {0x2010, 8}, {0x2020, 8}}));
  ASSERT_EQ(Bound.Function.Body.size(), Caller.Body.size());
  const auto Call = Bound.Function.Body[0].CallExpr;
  ASSERT_TRUE(Call);
  ASSERT_EQ(Call->Operands.size(), 5U);
  EXPECT_EQ(Call->Operands[0]->Var, Caller.Body[0].CallExpr->Operands[0]->Var);
  for (unsigned I = 1; I < 4; ++I) {
    ASSERT_TRUE(Call->Operands[I]->SourceCallHint);
    EXPECT_EQ(Call->Operands[I]->SourceCallHint->CallKind,
              SourceCallTypeHint::Kind::RuntimeLocalStorageAddress);
    EXPECT_EQ(Call->Operands[I]->SourceCallHint->TargetAddress,
              0x1ff0U + I * 16);
  }
  EXPECT_TRUE(
      swiftOnceCallbackBound(*Call->Operands[4], F.Image, Plan, F.functions()));
  const auto Functions = F.functions();
  EXPECT_TRUE(
      bindObjCSourceReferences(Bound.Function, F.Image, nullptr, &Functions)
          .Limitation.empty());
  EXPECT_EQ(F.Pipeline.HighFuncs[0].Body[2].Val.get(), F.SourceLoad.get());
  EXPECT_EQ(F.Pipeline.HighFuncs[0].Body[3].StoreVal.get(),
            F.SourceValue.get());
  EXPECT_EQ(F.Pipeline.HighFuncs[0].Body[4].Val.get(), F.Retain.get());
  EXPECT_TRUE(F.Pipeline.HighFuncs[2].Body[0].CallExpr->Operands[1]->Kind ==
              ExprKind::Const);
}

TEST(SwiftOnceSources, CopyContractUsesCurrentRefinedABIAndJoinLocals) {
  CopyOnceFixture F;
  auto &Helper = F.Pipeline.HighFuncs[0];
  Helper.Params.erase(Helper.Params.begin());
  Helper.SourceTypeHint->Parameters.erase(
      Helper.SourceTypeHint->Parameters.begin());
  std::string Error;
  ASSERT_TRUE(
      assignDarwinScalarSourceABI(*Helper.SourceTypeHint, F.Image.Arch, Error));
  std::set<HighExpr *> Seen;
  std::function<void(const ExprPtr &)> Remap = [&](const ExprPtr &E) {
    if (!E || !Seen.insert(E.get()).second)
      return;
    if (E->Kind == ExprKind::Var && E->Var.Kind == MedVar::Param)
      --E->Var.Id;
    for (const auto &Operand : E->Operands)
      Remap(Operand);
  };
  walkStmts(Helper.Body, [&](const HighStmt &Statement) {
    forEachRhsExpr(Statement, Remap);
    Remap(Statement.StoreAddr);
  });
  F.Once->Type.reset();
  const auto Local = [](unsigned Id) {
    MedVar V;
    V.Kind = MedVar::Reg;
    V.Id = Id;
    V.Size = 8;
    return HighExpr::makeVar(V, NdType::makePtr(NdType::makeVoid()));
  };
  auto Source = Local(50000), Destination = Local(50001);
  HighStmt AliasSource, AliasDestination, Jump;
  AliasSource.Kind = AliasDestination.Kind = StmtKind::Assign;
  AliasSource.Dst = Source;
  AliasSource.Val = F.SourceLoad->Operands[0];
  AliasDestination.Dst = Destination;
  AliasDestination.Val = Helper.Body[3].StoreAddr;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = 0x1060;
  Helper.Body[1].Body.insert(Helper.Body[1].Body.end(),
                             {AliasSource, AliasDestination, Jump});
  F.SourceLoad->Operands[0] = Source;
  Helper.Body[2].Addr = 0x1060;
  Helper.Body[3].StoreAddr = Destination;
  Helper.Body.insert(Helper.Body.begin() + 2, {AliasSource, AliasDestination});
  auto &Caller = F.Pipeline.HighFuncs[2];
  auto Call = Caller.Body[0].CallExpr;
  Call->Operands.erase(Call->Operands.begin());
  auto Hint = std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
  Hint->Signature = *Helper.SourceTypeHint;
  Call->SourceCallHint = Hint;
  const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  ASSERT_EQ(Plan.Copies.size(), 1U);
  EXPECT_EQ(Plan.Copies.at(Helper.Entry).ParameterCount, 4U);
  const auto Bound =
      bindSwiftOnceSourceReferences(Caller, F.Image, Plan, F.functions());
  EXPECT_EQ(Bound.Dependencies, std::set<va_t>{0x1080});
  EXPECT_EQ(Bound.LocalStorageExtents.size(), 3U);
  EXPECT_EQ(Bound.Function.Body[0].CallExpr->Operands.size(), 4U);
  EXPECT_EQ(Helper.Params.size(), 4U);
}

TEST(SwiftOnceSources, CopyContractRejectsExtraUsesAndChangedEffectOrder) {
  for (unsigned Case = 0; Case < 19; ++Case) {
    SCOPED_TRACE(Case);
    CopyOnceFixture F;
    auto &Helper = F.Pipeline.HighFuncs[0];
    ASSERT_TRUE(swift_once_source_detail::copyContract(Helper, F.Image));
    if (Case == 0) {
      auto Narrow = std::make_shared<HighExpr>();
      Narrow->Kind = ExprKind::Cast;
      Narrow->Type = NdType::makeInt(4);
      Narrow->Operands = {F.SourceValue};
      Helper.Body[3].StoreVal = Narrow;
    }
    if (Case == 1)
      Helper.Body[3].StoreAddr = F.Once->Operands[2];
    if (Case == 2)
      std::swap(Helper.Body[3], Helper.Body[4]);
    if (Case == 3)
      std::swap(Helper.Body[2], Helper.Body[3]);
    if (Case == 4) {
      HighStmt Extra;
      Extra.Kind = StmtKind::ExprStmt;
      Extra.Val = F.SourceLoad;
      Helper.Body.insert(Helper.Body.begin() + 3, Extra);
    }
    if (Case == 5)
      F.Retain->Operands[0] = F.PredicateValue;
    if (Case == 6) {
      HighStmt Extra;
      Extra.Kind = StmtKind::ExprStmt;
      Extra.Val = F.Pipeline.HighFuncs[2].Body[0].CallExpr->Operands[0];
      Helper.Body.insert(Helper.Body.begin(), Extra);
    }
    if (Case == 7) {
      auto Hint =
          std::make_shared<SourceCallTypeHint>(*F.Retain->SourceCallHint);
      Hint->TargetName = "objc_release";
      F.Retain->SourceCallHint = Hint;
    }
    if (Case == 8)
      F.SourceLoad->MemoryOrdering = NdMemoryOrdering::Acquire;
    if (Case == 9)
      Helper.Body[3].MemoryOrdering = NdMemoryOrdering::Release;
    if (Case == 10)
      F.Once->Operands[2] = F.Once->Operands[0];
    if (Case == 11)
      Helper.Body.erase(Helper.Body.begin());
    if (Case == 12)
      Helper.Body.back().RetVal = F.SourceValue;
    if (Case >= 13 && Case <= 16) {
      auto Cast = std::make_shared<HighExpr>();
      Cast->Kind = Case == 16 ? ExprKind::BitCast : ExprKind::Cast;
      Cast->Type = NdType::makeInt(8);
      Cast->CastTo = NdType::makeInt(4);
      Cast->Operands = {Case == 13   ? F.SourceLoad->Operands[0]
                        : Case == 14 ? Helper.Body[3].StoreAddr
                                     : F.SourceValue};
      if (Case == 13)
        F.SourceLoad->Operands[0] = Cast;
      else if (Case == 14)
        Helper.Body[3].StoreAddr = Cast;
      else
        Helper.Body[3].StoreVal = Cast;
    }
    if (Case == 17)
      Helper.SourceTypeHint->Architecture = Arch::X64;
    if (Case == 18)
      Helper.ReturnType = NdType::makeInt(8);
    EXPECT_FALSE(swift_once_source_detail::copyContract(Helper, F.Image));
  }
}

TEST(SwiftOnceSources, CopyReferencesRejectAliasedOrStaleIdentities) {
  for (unsigned Case = 0; Case < 22; ++Case) {
    SCOPED_TRACE(Case);
    CopyOnceFixture F;
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    ASSERT_EQ(Plan.Copies.size(), 1U);
    auto &Caller = F.Pipeline.HighFuncs[2];
    auto Call = Caller.Body[0].CallExpr;
    if (Case == 0)
      Call->Operands[3] = Call->Operands[2];
    if (Case == 1)
      Call->Operands[3] = HighExpr::makeConst(0x2014, 8);
    if (Case == 2)
      Call->Operands[2] = Call->Operands[1];
    if (Case == 3)
      Call->Operands[4] = HighExpr::makeConst(0x1084, 8);
    if (Case == 4)
      F.Image.Symbols[0].Name = "_$s4Test5other_Wz";
    if (Case == 5)
      F.Image.Symbols[1].Size = 16;
    if (Case == 6)
      F.Image.Symbols[1].Name = "_$s4Test5otherSo8NSObjectCvpZ";
    if (Case == 7)
      F.Image.Symbols.push_back(F.Image.Symbols[2]);
    if (Case == 8) {
      auto Duplicate = F.Image.Symbols[1];
      Duplicate.Addr += 0x30;
      F.Image.Symbols.push_back(Duplicate);
    }
    if (Case == 9)
      F.Image.Symbols[3].Name = "_$s4Test5other_WZ";
    if (Case == 10) {
      HighStmt Use;
      Use.Kind = StmtKind::ExprStmt;
      Use.Val = Call->Operands[0];
      F.Pipeline.HighFuncs[1].Body.insert(F.Pipeline.HighFuncs[1].Body.begin(),
                                          Use);
    }
    if (Case == 11)
      F.Pipeline.HighFuncs[1].SourceTypeHint.reset();
    if (Case == 12)
      std::swap(F.Pipeline.HighFuncs[0].Body[3],
                F.Pipeline.HighFuncs[0].Body[4]);
    if (Case == 13)
      F.Pipeline.HighFuncs[1].SourceTypeHint->ReturnType = NdType::makeInt(8);
    if (Case == 14)
      Call->CallAddr = 0x1100;
    if (Case == 15 || Case == 16) {
      auto Hint = std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
      if (Case == 15)
        Hint->TargetAddress = 0x1100;
      else
        Hint->ReturnedArgument = 0;
      Call->SourceCallHint = Hint;
    }
    if (Case == 17 || Case == 18) {
      auto Cast = std::make_shared<HighExpr>();
      Cast->Kind = ExprKind::Cast;
      Cast->Type = NdType::makeInt(8);
      Cast->CastTo = NdType::makeInt(Case == 17 ? 4 : 8);
      Cast->Operands = {Call->Operands[2]};
      Call->Operands[2] = Cast;
    }
    if (Case == 19)
      Call->Operands[2] = HighExpr::makeConst(0x2010, 4);
    if (Case == 20)
      Call->Operands[2]->SourceCallHint =
          std::make_shared<SourceCallTypeHint>();
    if (Case == 21)
      Call->IntrinsicOutputs.push_back(F.SourceValue->Var);
    const auto Bound =
        bindSwiftOnceSourceReferences(Caller, F.Image, Plan, F.functions());
    EXPECT_TRUE(Bound.Dependencies.empty());
    EXPECT_TRUE(Bound.LocalStorageExtents.empty());
    EXPECT_EQ(Bound.Function.Body[0].CallExpr->Operands[1]->Kind,
              ExprKind::Const);
  }
}

} // namespace
