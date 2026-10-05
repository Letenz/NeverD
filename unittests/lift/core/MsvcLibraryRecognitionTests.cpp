//===- MsvcLibraryRecognitionTests.cpp - Published MSVC evidence --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "LibrarySourceTest.h"
#include "gtest/gtest.h"

#include "neverd/debug/DebugInfoDiscovery.h"
#include "neverd/debug/PDBLoader.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedLibraryRecognition.h"
#include "neverd/loader/Loader.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/sigs/SignatureDB.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/raw_ostream.h"

#include <filesystem>

using namespace neverd;
using namespace neverd::sigs;

namespace {
constexpr const char *Stl = "msvc-14.44.35207-x64-release-stl";
class MsvcLibraryRecognitionTest : public testing::Test {
protected:
  void load(llvm::StringRef Fixture, llvm::StringRef Pack = Stl) {
    auto Error =
        DB.loadFeaturePack(std::filesystem::path(NEVERD_LIBRARY_FEATURE_DIR) /
                           (Pack.str() + ".json"));
    ASSERT_FALSE(static_cast<bool>(Error)) << llvm::toString(std::move(Error));
    auto Path = std::filesystem::path(NEVERD_LIBRARY_MSVC_FIXTURE_DIR) /
                (Fixture.str() + ".dll");
    auto Reader = Loader::create(Path);
    ASSERT_TRUE(Reader);
    auto Loaded = Reader->load(Path);
    ASSERT_TRUE(static_cast<bool>(Loaded))
        << llvm::toString(Loaded.takeError());
    Image = std::move(*Loaded);
    Debug = loadDebugInfo(Path, Image);
    ASSERT_TRUE(Debug) << Debug.Error;
    ASSERT_EQ(Debug.Kind, DebugInfoKind::PDB);
    applyDebugSymbols(Image, *Debug.Context);
  }
  PipelineResult analyze(llvm::StringRef Name) {
    const auto *Symbol = Image.findSymbol(Name);
    EXPECT_NE(Symbol, nullptr) << Name.str();
    if (!Symbol)
      return {};
    PipelineOptions Options;
    Options.DumpMed = true;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries.insert(Symbol->Addr);
    Options.LibraryFeatures = &DB.featurePacks();
    auto Result = Pipeline().run(Image, Context, Options, Debug.Context.get());
    EXPECT_TRUE(Result.Success) << Result.Error;
    return Result;
  }
  static std::string dump(const PipelineResult &Result) {
    std::string Text;
    llvm::raw_string_ostream Out(Text);
    for (const auto &F : Result.MedFuncs)
      for (const auto &B : F.Blocks) {
        Out << "block " << B.Id << '\n';
        for (const auto &O : B.Ops) {
          Out << O.Addr << ':' << O.OriginSeq << " " << ndOpName(O.Opcode)
              << " -> " << O.Output.display() << " " << O.Output.Size << " (";
          for (unsigned I = 0; I < O.NumInputs; ++I)
            Out << O.Inputs[I].display() << ":" << O.Inputs[I].Size << " ";
          Out << ")\n";
        }
      }
    return Text;
  }
  SignatureDB DB;
  BinaryImage Image;
  DebugInfoResult Debug;
  llvm::LLVMContext Context;
};

TEST_F(MsvcLibraryRecognitionTest, PdbBindsOriginalRecordToExactEntryRegister) {
  load("accessors-inline");
  const auto *F = Image.findSymbol("nd_vector_u32_data_inline");
  ASSERT_NE(F, nullptr);
  const auto Params =
      Debug.Context->resolveAuthenticatedRecordParameters(F->Addr);
  ASSERT_EQ(Params.size(), 1u);
  EXPECT_EQ(Params.front().QualifiedType,
            "std::vector<unsigned int,std::allocator<unsigned int> >");
  EXPECT_EQ(Params.front().ObjectBytes, 24u);
  EXPECT_TRUE(
      Debug.Context->resolveAuthenticatedRecordParameters(F->Addr + 1).empty());
  EXPECT_FALSE(Debug.Context->hasAuthenticatedFunctionSignatures());
  EXPECT_FALSE(Debug.Context->hasAuthenticatedObjectExtents());
}

TEST_F(MsvcLibraryRecognitionTest, PublishedStandaloneAccessors) {
  load("accessors-inline");
  for (const auto &Rule : DB.featurePacks().at(Stl).Rules) {
    SCOPED_TRACE(Rule.Id);
    ASSERT_EQ(Rule.WholeFunctionSymbols.size(), 1u);
    auto Result = analyze(Rule.WholeFunctionSymbols.front());
    auto I = llvm::find_if(Result.LibraryRecognitions, [&](const auto &R) {
      return R.Rule == Rule.Id && R.Scope == LibraryFeatureScope::WholeFunction;
    });
    EXPECT_NE(I, Result.LibraryRecognitions.end()) << dump(Result);
    verifyLibrarySource(Image, Debug.Context.get(), DB,
                        Rule.WholeFunctionSymbols.front(), Rule.Id, "");
  }
}

TEST_F(MsvcLibraryRecognitionTest, PublishedInlineAccessors) {
  load("accessors-inline");
  for (const auto &Rule : DB.featurePacks().at(Stl).Rules) {
    SCOPED_TRACE(Rule.Id);
    std::string Wrapper = "nd_" + Rule.Id.substr(5) + "_inline";
    std::replace(Wrapper.begin(), Wrapper.end(), '-', '_');
    std::replace(Wrapper.begin(), Wrapper.end(), '.', '_');
    if (auto At = Wrapper.find("wchar"); At != std::string::npos)
      Wrapper.replace(At, 5, "wide");
    auto Result = analyze(Wrapper);
    auto I = llvm::find_if(Result.LibraryRecognitions, [&](const auto &R) {
      return R.Rule == Rule.Id &&
             R.Scope == LibraryFeatureScope::InlineExpression;
    });
    EXPECT_NE(I, Result.LibraryRecognitions.end()) << dump(Result);
  }
}

TEST_F(MsvcLibraryRecognitionTest,
       SharedStringAccessorsRequireTheAdmittedBase) {
  constexpr auto Pack = "msvc-14.44.35207-x64-release-atl-mfc-string";
  load("atl_string-inline", Pack);
  for (const auto &Rule : DB.featurePacks().at(Pack).Rules) {
    if (Rule.PatternKind != LibraryFeatureRule::Kind::Expression)
      continue;
    SCOPED_TRACE(Rule.Id);
    auto Standalone = analyze(Rule.WholeFunctionSymbols.front());
    EXPECT_TRUE(llvm::any_of(Standalone.LibraryRecognitions, [&](const auto
                                                                     &R) {
      return R.Rule == Rule.Id && R.Scope == LibraryFeatureScope::WholeFunction;
    })) << dump(Standalone);
    const std::string Character =
        Rule.ReceiverType.find("wchar_t") == std::string::npos ? "char"
                                                               : "wide";
    const std::string Operation = Rule.Operation == "GetLength" ? "length"
                                  : Rule.Operation == "IsEmpty" ? "empty"
                                                                : "data";
    auto Inline =
        analyze("nd_cstring_" + Character + "_" + Operation + "_inline");
    EXPECT_TRUE(llvm::any_of(Inline.LibraryRecognitions, [&](const auto &R) {
      return R.Rule == Rule.Id &&
             R.Scope == LibraryFeatureScope::InlineExpression;
    })) << dump(Inline);
  }
  auto Lookalike = analyze("nd_user_GetLength");
  EXPECT_TRUE(Lookalike.LibraryRecognitions.empty());
}

TEST_F(MsvcLibraryRecognitionTest,
       DifferentIteratorConfigurationRemainsUnknown) {
  load("accessors-negative-configuration");
  for (const auto &Rule : DB.featurePacks().at(Stl).Rules) {
    std::string Wrapper = "nd_" + Rule.Id.substr(5) + "_inline";
    std::replace(Wrapper.begin(), Wrapper.end(), '-', '_');
    std::replace(Wrapper.begin(), Wrapper.end(), '.', '_');
    if (auto At = Wrapper.find("wchar"); At != std::string::npos)
      Wrapper.replace(At, 5, "wide");
    auto Result = analyze(Wrapper);
    EXPECT_TRUE(Result.LibraryRecognitions.empty()) << Wrapper;
  }
}

TEST_F(MsvcLibraryRecognitionTest,
       SharedStringByteFeaturesUseTheOriginalMatcher) {
  constexpr auto Pack = "msvc-14.44.35207-x64-release-atl-mfc-string";
  load("atl_string-inline", Pack);
  for (const auto &Rule : DB.featurePacks().at(Pack).Rules) {
    if (Rule.PatternKind != LibraryFeatureRule::Kind::BytePattern)
      continue;
    SCOPED_TRACE(Rule.Id);
    auto Result = analyze(Rule.LinkageName);
    EXPECT_TRUE(llvm::any_of(Result.LibraryRecognitions, [&](const auto &R) {
      return R.Rule == Rule.Id &&
             R.Scope == LibraryFeatureScope::WholeFunction &&
             !R.Occurrences.empty();
    })) << dump(Result);
    verifyLibrarySource(Image, Debug.Context.get(), DB, Rule.LinkageName,
                        Rule.Id, "", true);
  }
}

TEST_F(MsvcLibraryRecognitionTest, ByteAndStructuralProfilesCannotDisagree) {
  constexpr auto Pack = "msvc-14.44.35207-x64-release-atl-mfc-string";
  load("atl_string-inline", Pack);
  auto Packs = DB.featurePacks();
  auto Other = Packs.at(Pack);
  Other.Id += "-conflicting-source";
  std::erase_if(Other.Rules, [](const auto &R) {
    return R.PatternKind != LibraryFeatureRule::Kind::Expression;
  });
  std::erase_if(Packs.at(Pack).Rules, [](const auto &R) {
    return R.PatternKind != LibraryFeatureRule::Kind::BytePattern;
  });
  Packs.emplace(Other.Id, std::move(Other));
  bool SawOverlap = false;
  for (const auto &Rule : Packs.at(Pack).Rules) {
    auto Baseline = analyze(Rule.LinkageName);
    if (!llvm::any_of(Baseline.LibraryRecognitions, [](const auto &R) {
          return R.Scope != LibraryFeatureScope::WholeFunction;
        }))
      continue;
    SawOverlap = true;
    PipelineOptions Options;
    Options.DumpMed = true;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries.insert(
        Image.findSymbol(Rule.LinkageName)->Addr);
    Options.LibraryFeatures = &Packs;
    auto Result = Pipeline().run(Image, Context, Options, Debug.Context.get());
    ASSERT_TRUE(Result.Success) << Result.Error;
    EXPECT_TRUE(Result.LibraryRecognitions.empty()) << Rule.Id;
    EXPECT_EQ(dump(Result), dump(Baseline));
  }
  EXPECT_TRUE(SawOverlap);
}

TEST_F(MsvcLibraryRecognitionTest, PublishedComLifetimePolicies) {
  constexpr auto Pack = "msvc-14.44.35207-x64-release-atl-com";
  load("atl_com-inline", Pack);
  for (const auto &Rule : DB.featurePacks().at(Pack).Rules) {
    std::vector<std::string> Names = Rule.WholeFunctionSymbols;
    if (Rule.ReceiverType == "ATL::CComPtr<IUnknown>")
      Names.push_back(Rule.Operation == "Release" ? "nd_com_release_inline"
                      : Rule.Operation == "assign"
                          ? "nd_com_assign_inline"
                          : "nd_com_" + Rule.Operation);
    for (const auto &Name : Names) {
      SCOPED_TRACE(Rule.Id + " " + Name);
      auto Result = analyze(Name);
      EXPECT_TRUE(llvm::any_of(Result.LibraryRecognitions, [&](const auto &R) {
        return R.Rule == Rule.Id && R.Isolated;
      })) << dump(Result);
      if (!llvm::StringRef(Name).starts_with("nd_"))
        verifyLibrarySource(Image, Debug.Context.get(), DB, Name, Rule.Id, "",
                            true);
    }
  }
}

TEST_F(MsvcLibraryRecognitionTest, ComReturnTypeIsIndependentEntryEvidence) {
  load("atl_com-inline", "msvc-14.44.35207-x64-release-atl-com");
  const auto *F = Image.findSymbol("nd_com_construct");
  ASSERT_NE(F, nullptr);
  EXPECT_TRUE(
      Debug.Context->resolveAuthenticatedRecordParameters(F->Addr).empty());
  const auto Return = Debug.Context->resolveAuthenticatedRecordReturn(F->Addr);
  ASSERT_TRUE(Return);
  EXPECT_EQ(Return->QualifiedType, "ATL::CComPtr<IUnknown>");
  EXPECT_EQ(Return->ObjectBytes, 8u);
  EXPECT_EQ(Return->Register, getTargetRegInfo(Arch::X64).IntReturnReg);
  EXPECT_FALSE(Debug.Context->resolveAuthenticatedRecordReturn(F->Addr + 1));
  auto Result = analyze("nd_com_construct");
  ASSERT_EQ(Result.LibraryRecognitions.size(), 1u);
  EXPECT_EQ(Result.LibraryRecognitions.front().IdentityEvidence,
            "authenticated-debug-return-alias+com-lifetime");
  auto &Function = Result.MedFuncs.front();
  EXPECT_TRUE(recognizeMedLibraryOperations(Function, Image, DB.featurePacks())
                  .Matches.empty());
}

TEST_F(MsvcLibraryRecognitionTest, ComSlotTypeAndOrderingNearMissesAbstain) {
  load("atl_com-inline", "msvc-14.44.35207-x64-release-atl-com");
  auto Result = analyze("nd_com_release_inline");
  ASSERT_GE(Result.MedFuncs.size(), 1u);
  ASSERT_FALSE(Result.LibraryRecognitions.empty());
  const auto Register =
      getTargetRegInfo(Arch::X64).integerParamRegs(BinaryFormat::COFF).front();
  MedLibraryReceiver Receiver{Register, "ATL::CComPtr<IUnknown>", 8,
                              "test-authenticated-record"};
  const auto Original = Result.MedFuncs.front();
  ASSERT_FALSE(recognizeMedLibraryOperations(Original, Image, DB.featurePacks(),
                                             {Receiver})
                   .Matches.empty());
  for (unsigned BadExit = 0; BadExit != 3; ++BadExit) {
    auto F = Original;
    ASSERT_TRUE(F.ExceptionMetadata);
    bool Changed = false;
    if (BadExit == 0) {
      F.ExceptionMetadata->ParseStatus = ExceptionParseStatus::Partial;
      Changed = true;
    } else {
      for (auto &B : F.Blocks)
        for (auto &E : B.ExceptionalSuccs) {
          if (BadExit == 1)
            E.TargetVA += 1;
          else
            E.Kind = ExceptionalEdgeKind::CxxCatch;
          Changed = true;
        }
    }
    ASSERT_TRUE(Changed);
    EXPECT_TRUE(
        recognizeMedLibraryOperations(F, Image, DB.featurePacks(), {Receiver})
            .Matches.empty());
  }
  for (bool WrongSlot : {false, true}) {
    auto F = Original;
    bool Changed = false;
    for (auto &B : F.Blocks)
      for (auto &O : B.Ops) {
        if (WrongSlot && O.Opcode == NdOp::INT_ADD && O.NumInputs == 2 &&
            O.Inputs[1].isConst() && O.Inputs[1].ConstVal == 16) {
          O.Inputs[1].ConstVal = 24;
          Changed = true;
        }
        if (!WrongSlot && O.Opcode == NdOp::STORE && O.NumInputs == 2 &&
            O.Inputs[1].isConst() && !O.Inputs[1].ConstVal) {
          O.Inputs[1].ConstVal = 1;
          Changed = true;
        }
      }
    ASSERT_TRUE(Changed);
    EXPECT_TRUE(
        recognizeMedLibraryOperations(F, Image, DB.featurePacks(), {Receiver})
            .Matches.empty());
  }
  Receiver.Type = "UserPtr<IUnknown>";
  EXPECT_TRUE(recognizeMedLibraryOperations(Original, Image, DB.featurePacks(),
                                            {Receiver})
                  .Matches.empty());
  Receiver.Type = "ATL::CComPtr<IUnknown>";
  Receiver.ObjectBytes = 16;
  EXPECT_TRUE(recognizeMedLibraryOperations(Original, Image, DB.featurePacks(),
                                            {Receiver})
                  .Matches.empty());
}

TEST_F(MsvcLibraryRecognitionTest, InlineAccessorsMapBothSourceRoutes) {
  load("accessors-inline");
  for (const auto &Rule : DB.featurePacks().at(Stl).Rules) {
    std::string Wrapper = "nd_" + Rule.Id.substr(5) + "_inline";
    std::replace(Wrapper.begin(), Wrapper.end(), '-', '_');
    std::replace(Wrapper.begin(), Wrapper.end(), '.', '_');
    if (auto At = Wrapper.find("wchar"); At != std::string::npos)
      Wrapper.replace(At, 5, "wide");
    verifyLibrarySource(Image, Debug.Context.get(), DB, Wrapper, Rule.Id);
  }
}

TEST_F(MsvcLibraryRecognitionTest, SharedStringAccessorsMapBothSourceRoutes) {
  constexpr auto Pack = "msvc-14.44.35207-x64-release-atl-mfc-string";
  load("atl_string-inline", Pack);
  for (const auto &Rule : DB.featurePacks().at(Pack).Rules) {
    if (Rule.PatternKind != LibraryFeatureRule::Kind::Expression)
      continue;
    const std::string Character =
        Rule.ReceiverType.find("wchar_t") == std::string::npos ? "char"
                                                               : "wide";
    const std::string Operation = Rule.Operation == "GetLength" ? "length"
                                  : Rule.Operation == "IsEmpty" ? "empty"
                                                                : "data";
    verifyLibrarySource(Image, Debug.Context.get(), DB,
                        "nd_cstring_" + Character + "_" + Operation + "_inline",
                        Rule.Id);
  }
}

TEST_F(MsvcLibraryRecognitionTest, StringSelectionRejectsAnExternalArmEntry) {
  load("accessors-inline");
  auto Result = analyze("nd_string_char_data_inline");
  ASSERT_EQ(Result.MedFuncs.size(), 1u);
  auto Function = Result.MedFuncs.front();
  ASSERT_EQ(Function.Blocks.size(), 3u);
  const auto Params =
      Debug.Context->resolveAuthenticatedRecordParameters(Function.Entry);
  ASSERT_EQ(Params.size(), 1u);
  const auto &R = DB.featurePacks().at(Stl).Rules;
  auto Rule = llvm::find_if(
      R, [](const auto &R) { return R.Id == "msvc.string-char.data"; });
  ASSERT_NE(Rule, R.end());
  MedLibraryReceiver Receiver{Params[0].Register, Rule->ReceiverType,
                              Params[0].ObjectBytes,
                              "test-authenticated-record"};
  auto Recognized = recognizeMedLibraryOperations(
      Function, Image, DB.featurePacks(), {Receiver});
  ASSERT_TRUE(llvm::any_of(Recognized.Matches, [](const auto &M) {
    return M.Rule == "msvc.string-char.data" && M.Isolated;
  }));
  for (auto &B : Function.Blocks)
    if (B.Preds.size() == 1 && B.Succs.size() == 1)
      B.Preds.push_back(99);
  Recognized = recognizeMedLibraryOperations(Function, Image, DB.featurePacks(),
                                             {Receiver});
  EXPECT_FALSE(llvm::any_of(Recognized.Matches, [](const auto &M) {
    return M.Rule == "msvc.string-char.data";
  }));
}

TEST_F(MsvcLibraryRecognitionTest, ComLifetimeRegionsMapBothSourceRoutes) {
  constexpr auto Pack = "msvc-14.44.35207-x64-release-atl-com";
  load("atl_com-inline", Pack);
  for (const auto &Rule : DB.featurePacks().at(Pack).Rules) {
    if (Rule.ReceiverType != "ATL::CComPtr<IUnknown>")
      continue;
    const auto Name = Rule.Operation == "Release"  ? "nd_com_release_inline"
                      : Rule.Operation == "assign" ? "nd_com_assign_inline"
                                                   : "nd_com_" + Rule.Operation;
    // Machine ABI calls can carry otherwise unused source arguments. Their
    // spelling stays intact; the wrapper's independent return stays visible.
    verifyLibrarySource(Image, Debug.Context.get(), DB, Name, Rule.Id, "return",
                        true);
  }
}

TEST_F(MsvcLibraryRecognitionTest, ComLookalikesDoNotGainAnOwnershipPolicy) {
  load("atl_com-inline", "msvc-14.44.35207-x64-release-atl-com");
  const auto Registers =
      getTargetRegInfo(Arch::X64).integerParamRegs(BinaryFormat::COFF);
  const std::vector<MedLibraryReceiver> Claimed{
      {Registers[0], "ATL::CComPtr<IUnknown>", 8, "test-counterfactual-type"},
      {Registers[1], "ATL::CComPtr<IUnknown>", 8, "test-counterfactual-type"}};
  for (auto Name :
       {"nd_unknown_release", "nd_user_release_then_clear",
        "nd_user_release_without_guard", "nd_user_clear_then_addref",
        "nd_user_assign_release_first", "nd_user_assign"}) {
    SCOPED_TRACE(Name);
    auto Result = analyze(Name);
    ASSERT_EQ(Result.MedFuncs.size(), 1u);
    EXPECT_TRUE(Result.LibraryRecognitions.empty());
    // Even a claimed library type cannot excuse a different effect ordering.
    EXPECT_TRUE(recognizeMedLibraryOperations(Result.MedFuncs.front(), Image,
                                              DB.featurePacks(), Claimed)
                    .Matches.empty());
  }
}
} // namespace
