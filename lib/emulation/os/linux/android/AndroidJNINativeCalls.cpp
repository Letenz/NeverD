//===- AndroidJNINativeCalls.cpp - Registered guest JNI calls -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// JNI method dispatch to original guest code, with implicit local frames.
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"

#include <bit>

namespace neverd::emulation::android_model {
namespace {
bool scalar(std::string_view Type) {
  return Type.size() == 1 &&
         std::string_view("VZBCSIJ").find(Type[0]) != std::string_view::npos;
}
uint64_t narrow(uint64_t Value, std::string_view Type) {
  if (Type == "V")
    return 0;
  if (Type == "Z")
    return uint8_t(Value);
  if (Type == "C")
    return uint16_t(Value);
  if (Type == "B")
    return uint32_t(int32_t(std::bit_cast<int8_t>(uint8_t(Value))));
  if (Type == "S")
    return uint32_t(int32_t(std::bit_cast<int16_t>(uint16_t(Value))));
  return Type == "I" ? uint32_t(Value) : Value;
}
} // namespace

BionicResult
Bionic::callNativeJNI(const mobile::dalvik::Method &Method,
                      llvm::ArrayRef<uint64_t> Arguments,
                      std::shared_ptr<DexContinuation> Continuation) {
  using namespace mobile::dalvik;
  auto Unsupported = [&](llvm::StringRef Reason) -> BionicResult {
    return resumeDex(Continuation,
                     {ExecutionStop::Unsupported,
                      0,
                      {},
                      "JNI native invocation " + Method.reference.identity() +
                          ": " + Reason.str()});
  };
  if (has(Method.access, "synchronized") ||
      has(Method.access, "declared-synchronized"))
    return Unsupported("synchronized native methods require Java monitors");
  const auto &Ref = Method.reference;
  const bool Static = has(Method.access, "static");
  if (Arguments.size() != Ref.parameters.size() + !Static)
    return failure("JNI native argument count mismatch");
  // The callback transport deliberately admits register arguments only.
  // It must not reinterpret floating or overflowing arguments as GP values.
  if (Ref.parameters.size() > 6 ||
      (!scalar(Ref.returns) && !referenceType(Ref.returns)))
    return Unsupported("signature exceeds the scalar register ABI");
  for (const auto &Type : Ref.parameters)
    if (Type == "V" || (!scalar(Type) && !referenceType(Type)))
      return Unsupported("signature exceeds the scalar register ABI");
  auto Binding = JNI.Registrations.find(Ref.identity());
  if (Binding == JNI.Registrations.end())
    return Unsupported("no registered target; native symbol lookup is unknown");
  const uint64_t Target = Binding->second.Address;
  if (Target % 4)
    return failure("JNI native target is not instruction aligned");
  if (auto E = access(Target, 4, Execute))
    return std::move(E);
  auto &Thread = JNI.Threads.at(threadID());
  if (Thread.Frames.size() == jni_abi::ReferenceLimit ||
      JNI.NextFrame == UINT64_MAX ||
      Thread.NativeFrames.size() >= jni_abi::NativeFrameLimit ||
      JNI.References.size() + Ref.parameters.size() + 1 >
          jni_abi::ReferenceLimit)
    return failure("JNI native frame/reference budget exhausted");
  auto Compatible = [&](uint64_t ID, std::string_view Type) {
    if (!ID)
      return true;
    auto Object = JNI.Heap->get(ID);
    return Object && Object->Initialized &&
           JNI.Dex->assignable(objectDescriptor(*Object), Type);
  };
  if (!Static && (!Arguments[0] || !Compatible(Arguments[0], Ref.owner)))
    return failure("JNI native receiver type or initialization mismatch");
  for (size_t I = 0; I < Ref.parameters.size(); ++I) {
    const auto &Type = Ref.parameters[I];
    const uint64_t Arg = Arguments[I + !Static];
    if ((referenceType(Type) && !Compatible(Arg, Type)) ||
        (Type == "Z" && uint8_t(Arg) > 1))
      return failure("JNI native argument type or initialization mismatch");
  }
  std::shared_ptr<JNIObject> Receiver;
  if (Static) {
    auto Class = jniClass(llvm::StringRef(Ref.owner).drop_front().drop_back());
    if (!Class)
      return Class.takeError();
    Receiver = std::move(*Class);
  } else
    Receiver = JNI.Heap->get(Arguments[0]);
  const size_t Depth = Thread.Frames.size();
  const uint64_t Frame = JNI.NextFrame++;
  Thread.Frames.push_back(Frame);
  Thread.NativeFrames.push_back(Frame);
  auto Handle = jniReference(std::move(Receiver));
  if (!Handle)
    return Handle.takeError();
  std::vector<uint64_t> Values{Thread.Environment, *Handle};
  for (size_t I = 0; I < Ref.parameters.size(); ++I) {
    const auto &Type = Ref.parameters[I];
    const uint64_t Arg = Arguments[I + !Static];
    if (referenceType(Type)) {
      auto Local = jniReference(JNI.Heap->get(Arg));
      if (!Local)
        return Local.takeError();
      Values.push_back(*Local);
    } else
      Values.push_back(narrow(Arg, Type));
  }
  return std::optional<BionicValue>(
      GuestCallback{Target, std::move(Values),
                    JNINativeCallback{&Method, threadID(), Frame, Depth,
                                      std::move(Continuation)}});
}

BionicResult Bionic::finishNativeJNI(const JNINativeCallback &Continuation) {
  const auto &Type = Continuation.Method->reference.returns;
  auto &Thread = JNI.Threads.at(threadID());
  if (Continuation.Thread != threadID() || Thread.NativeFrames.empty() ||
      Thread.NativeFrames.back() != Continuation.Frame ||
      Thread.Frames.size() <= Continuation.FrameDepth ||
      Thread.Frames[Continuation.FrameDepth] != Continuation.Frame)
    return failure("JNI native continuation frame mismatch");
  uint64_t Returned = 0;
  std::shared_ptr<JNIObject> Object;
  // A pending Java exception supersedes an unspecified native return carrier.
  if (!Thread.Exception && Type != "V") {
    auto Register = CPU.readRegister(CPURegister::AArch64X0);
    if (!Register)
      return Register.takeError();
    Returned = (*Register)[0];
    if (mobile::dalvik::referenceType(Type)) {
      auto Reference = jniObject(Returned);
      if (!Reference)
        return Reference.takeError();
      Object = std::move(*Reference);
      if (Object && (!Object->Initialized ||
                     !JNI.Dex->assignable(
                         mobile::dalvik::objectDescriptor(*Object), Type)))
        return failure("JNI native return reference type mismatch");
    } else {
      Returned = narrow(Returned, Type);
      if (Type == "Z" && Returned > 1)
        return failure("JNI native return boolean is not canonical");
    }
  }
  // All frames pushed by this native invocation retire, even if the guest
  // left explicit frames open. Caller locals and global references survive.
  std::erase_if(JNI.References, [&](const auto &Entry) {
    return Entry.second.Thread == threadID() &&
           Entry.second.Frame >= Continuation.Frame;
  });
  Thread.Frames.resize(Continuation.FrameDepth);
  Thread.NativeFrames.pop_back();
  mobile::dalvik::ExecutionResult Completion;
  if (Thread.Exception) {
    Completion.stop = mobile::dalvik::ExecutionStop::Thrown;
    Completion.throwable = Thread.Exception->Identity;
    Completion.exception = Thread.Exception->ClassName;
    Completion.diagnostic = "exception returned from guest JNI native";
    Thread.Exception.reset();
  } else
    Completion.value = mobile::dalvik::referenceType(Type)
                           ? (Object ? Object->Identity : 0)
                           : Returned;
  return resumeDex(Continuation.Dex, std::move(Completion));
}
} // namespace neverd::emulation::android_model
