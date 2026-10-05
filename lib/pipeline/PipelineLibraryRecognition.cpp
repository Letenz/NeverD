//===- PipelineLibraryRecognition.cpp - Shared presentation evidence -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedLibraryRecognition.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/STLExtras.h"

#include <cctype>

namespace neverd {
namespace {

std::string typeSpelling(llvm::StringRef Name) {
  // Only whitespace around punctuation is immaterial. Keep word boundaries:
  // the built-in 'unsigned int' must not equal a user type 'unsignedint'.
  std::string Result;
  bool Space = false;
  auto Word = [](unsigned char C) { return std::isalnum(C) || C == '_'; };
  for (unsigned char C : Name) {
    if (std::isspace(C)) {
      Space = true;
      continue;
    }
    if (Space && !Result.empty() && Word(Result.back()) && Word(C))
      Result += ' ';
    Space = false;
    Result += C;
  }
  return Result;
}

std::vector<MedLibraryReceiver>
receivers(const BinaryImage &Image, const MedFunc &Function, DebugContext *Dbg,
          const std::map<std::string, sigs::LibraryFeaturePack> &Packs) {
  std::vector<MedLibraryReceiver> Result;
  if (!Dbg)
    return Result;
  for (const auto &Parameter :
       Dbg->resolveAuthenticatedRecordParameters(Function.Entry)) {
    const std::string Spelling = typeSpelling(Parameter.QualifiedType);
    std::string Identity = Parameter.QualifiedType;
    for (const auto &[ID, Pack] : Packs) {
      (void)ID;
      if (!Pack.accepts(Image.Arch, Image.Format, Image.Bits))
        continue;
      for (const auto &[LayoutID, Layout] : Pack.Layouts) {
        (void)LayoutID;
        if (typeSpelling(Layout.ReceiverType) == Spelling)
          Identity = Layout.ReceiverType;
      }
    }
    Result.push_back({Parameter.Register, Identity, Parameter.ObjectBytes,
                      "authenticated-debug-parameter"});
  }
  return Result;
}

} // namespace

void Pipeline::recognizeLibraries(const BinaryImage &Img,
                                  const PipelineOptions &Opts,
                                  PipelineResult &Result, DebugContext *Dbg) {
  if (!Opts.LibraryFeatures || Opts.LibraryFeatures->empty() ||
      (Img.Arch != Arch::X64 && Img.Arch != Arch::AArch64) ||
      ((Opts.PatchMode || Opts.LiftMode) && !Opts.SourceProjection))
    return;
  MedLibraryIdentityIndex Identities(Img);
  for (const MedFunc &Function : Result.MedFuncs) {
    auto Receivers = receivers(Img, Function, Dbg, *Opts.LibraryFeatures);
    auto Evidence = recognizeMedLibraryOperations(
        Function, Img, *Opts.LibraryFeatures, Receivers, 250000, &Identities);
    if (Evidence.BudgetExhausted)
      Result.LibraryRecognitionBudgetExhausted.insert(Function.Entry);
    llvm::append_range(Result.LibraryRecognitions, std::move(Evidence.Matches));
  }
}

} // namespace neverd
