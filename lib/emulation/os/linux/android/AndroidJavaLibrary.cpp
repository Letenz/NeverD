//===- AndroidJavaLibrary.cpp - Guest ELF Java library lifecycle ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The supplied DEX namespace loads an explicitly bound, initialized guest ELF.
/// No pathname search, host dlopen, replacement JNI_OnLoad or inferred APK.
#include "AndroidInternal.h"

namespace neverd::emulation::android_model {
llvm::Expected<std::variant<mobile::dalvik::ExecutionResult, GuestCallback>>
Bionic::loadJavaLibrary(const mobile::dalvik::ExternalCall &Call,
                        std::shared_ptr<DexContinuation> Continuation) {
  using mobile::dalvik::ExecutionResult;
  using mobile::dalvik::ExecutionStop;
  auto Refuse = [&](llvm::StringRef Reason) {
    return ExecutionResult{ExecutionStop::Unsupported, 0, {}, Reason.str()};
  };
  if (!Options.Android->JavaLibrary ||
      Call.library != *Options.Android->JavaLibrary)
    return Refuse("System.loadLibrary has no explicit guest ELF binding");
  if (!JNI.Dex->containsClass(Call.caller))
    return Refuse("System.loadLibrary requires a supplied DEX caller");
  if (JavaLibraryState == JavaLibraryStatus::Loaded ||
      (JavaLibraryState == JavaLibraryStatus::Loading &&
       JavaLibraryThread == threadID()))
    return ExecutionResult{};
  if (JavaLibraryState == JavaLibraryStatus::Loading)
    return Refuse("Java library initialization waits on another thread");
  if (JavaLibraryState == JavaLibraryStatus::Failed)
    return ExecutionResult{ExecutionStop::Thrown, 0,
                           "java/lang/UnsatisfiedLinkError",
                           "JNI_OnLoad previously failed"};
  if (!Linked.JNIOnLoad) {
    JavaLibraryState = JavaLibraryStatus::Loaded;
    return ExecutionResult{};
  }
  auto &Thread = JNI.Threads.at(threadID());
  if (Thread.Frames.size() >= jni_abi::ReferenceLimit ||
      Thread.NativeFrames.size() >= jni_abi::NativeFrameLimit ||
      JNI.NextFrame == UINT64_MAX)
    return Refuse("JNI_OnLoad frame budget exhausted");
  const size_t Depth = Thread.Frames.size();
  const uint64_t Frame = JNI.NextFrame++;
  Thread.Frames.push_back(Frame);
  Thread.NativeFrames.push_back(Frame);
  JavaLibraryState = JavaLibraryStatus::Loading;
  JavaLibraryThread = threadID();
  return GuestCallback{
      *Linked.JNIOnLoad,
      {jni_abi::Base, 0},
      JNIOnLoadCallback{threadID(), Frame, Depth, std::move(Continuation)}};
}

BionicResult Bionic::finishJavaLibrary(const JNIOnLoadCallback &Continuation) {
  using mobile::dalvik::ExecutionStop;
  auto &Thread = JNI.Threads.at(threadID());
  if (Continuation.Thread != threadID() ||
      JavaLibraryState != JavaLibraryStatus::Loading ||
      JavaLibraryThread != threadID() || Thread.NativeFrames.empty() ||
      Thread.NativeFrames.back() != Continuation.Frame ||
      Thread.Frames.size() <= Continuation.FrameDepth ||
      Thread.Frames[Continuation.FrameDepth] != Continuation.Frame)
    return failure("JNI_OnLoad continuation frame mismatch");
  auto Register = CPU.readRegister(CPURegister::AArch64X0);
  if (!Register)
    return Register.takeError();
  uint32_t Version = (*Register)[0];
  std::erase_if(JNI.References, [&](const auto &Entry) {
    return Entry.second.Thread == threadID() &&
           Entry.second.Frame >= Continuation.Frame;
  });
  Thread.Frames.resize(Continuation.FrameDepth);
  Thread.NativeFrames.pop_back();
  mobile::dalvik::ExecutionResult Completion;
  // API 28 ART accepts 1.2, 1.4 and 1.6 from JNI_OnLoad, not 1.1 or 1.8.
  const bool Supported =
      Version == 0x10002 || Version == 0x10004 || Version == 0x10006;
  JavaLibraryState =
      Supported ? JavaLibraryStatus::Loaded : JavaLibraryStatus::Failed;
  if (Thread.Exception) {
    Completion.stop = ExecutionStop::Thrown;
    Completion.throwable = Thread.Exception->Identity;
    Completion.exception = Thread.Exception->ClassName;
    Completion.diagnostic = "exception returned from JNI_OnLoad";
    Thread.Exception.reset();
  } else if (!Supported) {
    Completion.stop = ExecutionStop::Thrown;
    Completion.exception = "java/lang/UnsatisfiedLinkError";
    Completion.diagnostic = "unsupported JNI_OnLoad version";
  }
  return resumeDex(Continuation.Dex, std::move(Completion));
}
} // namespace neverd::emulation::android_model
