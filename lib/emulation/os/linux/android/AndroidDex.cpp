//===- AndroidDex.cpp - Explicit DEX bodies in the workload VM ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared decoder admission and execution budgets for JNI-supplied DEX code.
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"

#include "neverd/dalvik/Runtime.h"

namespace neverd::emulation::android_model {
llvm::Error Bionic::initializeDex() {
  const auto &Native = *Options.Android;
  JNI.Heap = std::make_shared<mobile::dalvik::Heap>(Options.MemoryLimit / 2,
                                                    jni_abi::ObjectLimit);
  if (!Native.JNI && !Native.JNIClassLookupFailures.empty())
    return failure("JNI class lookup failures require JNI");
  if (!Native.JNI && Native.Application)
    return failure("Android application snapshot requires JNI");
  if (Native.DexImages.empty() && !Native.JNI)
    return llvm::Error::success();
  if (!Native.JNI || Native.DexImages.size() > 16)
    return failure("DEX images require JNI and at most 16 explicit images");
  uint64_t Total = 0;
  for (const auto &Image : Native.DexImages) {
    if (Image.empty() || Image.size() > 1024 * 1024 - Total)
      return failure("DEX image input exceeds the 1 MiB budget");
    Total += Image.size();
    if (Image.size() < 8 || Image[7] != 0 ||
        (std::string_view(reinterpret_cast<const char *>(Image.data()), 7) !=
             "dex\n035" &&
         std::string_view(reinterpret_cast<const char *>(Image.data()), 7) !=
             "dex\n037" &&
         std::string_view(reinterpret_cast<const char *>(Image.data()), 7) !=
             "dex\n038" &&
         std::string_view(reinterpret_cast<const char *>(Image.data()), 7) !=
             "dex\n039"))
      return failure(
          "Android API 28 requires DEX version 035, 037, 038 or 039");
  }
  // Materialized strings/instructions are larger than their compressed DEX
  // representation. Reserve a conservative bounded share before decoding.
  if (Total > Options.MemoryLimit / 256)
    return failure("DEX input exceeds the process model storage budget");
  try {
    // max_files bounds the shared reader's logical class inventory, not the
    // number of input DEX images. Debit it across all images in this workload.
    mobile::Budget DecodeBudget({1, 4096, std::max<uint64_t>(1, Total)});
    DecodeBudget.deadline =
        std::chrono::steady_clock::now() +
        std::chrono::microseconds(Budget.remainingMicroseconds());
    std::vector<mobile::dalvik::Class> Classes;
    for (size_t I = 0; I < Native.DexImages.size(); ++I) {
      const auto &Image = Native.DexImages[I];
      auto Part = mobile::dalvik::parseDex(
          std::string_view(reinterpret_cast<const char *>(Image.data()),
                           Image.size()),
          "supplied-dex-" + std::to_string(I), DecodeBudget);
      DecodeBudget.limits.max_files -= Part.size();
      Classes.insert(Classes.end(), std::make_move_iterator(Part.begin()),
                     std::make_move_iterator(Part.end()));
    }
    JNI.Dex = std::make_shared<mobile::dalvik::Program>(std::move(Classes),
                                                        Native.Properties);
    if (Native.JNIClassLookupFailures.size() > jni_abi::ReferenceLimit)
      return failure("JNI class lookup failure inventory exceeds its budget");
    for (const auto &[Name, Exception] : Native.JNIClassLookupFailures) {
      if (Name.empty() || Name.size() > 1024 || Name.front() == '/' ||
          Name.back() == '/' || Name.find("//") != std::string::npos ||
          Name.find_first_of(".;[\0", 0, 4) != std::string::npos ||
          (Exception != "java/lang/ClassNotFoundException" &&
           Exception != "java/lang/NoClassDefFoundError"))
        return failure("invalid JNI class lookup failure declaration");
      const auto Type = "L" + Name + ";";
      mobile::dalvik::descriptor(Type);
      if (mobile::dalvik::platformClass(Name) || JNI.Dex->containsClass(Type))
        return failure("JNI class lookup failure shadows a known class");
    }
    JNI.DexRuntime = std::make_shared<mobile::dalvik::Runtime>(
        *JNI.Dex, JNI.Heap, Native.Application);
  } catch (const mobile::Error &E) {
    return failure(llvm::Twine("invalid supplied DEX: ") + E.what());
  }
  return llvm::Error::success();
}
BionicResult Bionic::executeDex(const mobile::dalvik::Method *Method,
                                llvm::ArrayRef<uint64_t> Arguments,
                                llvm::StringRef Class) {
  return runDex([&](const auto &Admit, uint64_t Stack) {
    return Method
               ? JNI.DexRuntime->execute(*Method,
                                         {Arguments.data(), Arguments.size()},
                                         Admit, Stack)
               : JNI.DexRuntime->initialize(Class.str(), Admit, Stack);
  });
}
BionicResult Bionic::allocateDex(llvm::StringRef Class, bool Initialized) {
  return runDex([&](const auto &Admit, uint64_t Stack) {
    return JNI.DexRuntime->allocate(Class.str(), Initialized, Admit, Stack);
  });
}
BionicResult
Bionic::runDex(const std::function<mobile::dalvik::ExecutionResult(
                   const mobile::dalvik::AdmitInstruction &, uint64_t)> &Body) {
  using mobile::dalvik::ExecutionStop;
  auto Admit = [&] {
    if (!Budget.remainingMicroseconds())
      return ExecutionStop::Timeout;
    return Budget.consumeInstructions() ? ExecutionStop::Returned
                                        : ExecutionStop::InstructionLimit;
  };
  auto Executed = Body(
      Admit, std::min<uint64_t>(Options.StackSize, Options.MemoryLimit / 256));
  if (Executed.stop == ExecutionStop::Returned)
    return value(Executed.value);
  if (Executed.stop == ExecutionStop::Thrown) {
    if (Executed.throwable)
      JNI.Threads.at(threadID()).Exception = JNI.Heap->get(Executed.throwable);
    else if (auto E = jniException(Executed.exception))
      return std::move(E);
    return value(0);
  }
  if (Executed.stop == ExecutionStop::Invalid)
    return failure(Executed.diagnostic);
  Result.Diagnostic = Executed.diagnostic;
  if (Executed.stop == ExecutionStop::Timeout) {
    Expired = true;
    Result.Stop = ProcessStopReason::Timeout;
  } else if (Executed.stop == ExecutionStop::InstructionLimit)
    Result.Stop = ProcessStopReason::InstructionLimit;
  else
    Result.Stop = ProcessStopReason::UnsupportedService;
  return std::nullopt;
}
} // namespace neverd::emulation::android_model
