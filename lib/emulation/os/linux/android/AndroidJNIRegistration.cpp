//===- AndroidJNIRegistration.cpp - Retained JNI native bindings ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// API 28 native registration against supplied declarations. Registration
/// does not initialize a class or execute the registered function.
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::android_model {
BionicResult Bionic::jniRegistration(unsigned Slot,
                                     const NativeCallEvent &Call) {
  using namespace mobile::dalvik;
  const auto &A = Call.Arguments;
  auto Class = jniObject(A[1]);
  if (!Class)
    return Class.takeError();
  if (!*Class || (*Class)->Type != JNIObject::Class)
    return failure("JNI native registration requires a class reference");
  const auto Owner = "L" + (*Class)->ClassName + ";";
  if ((Slot == 216 || int32_t(A[3]) > 0) && !JNI.Dex->containsClass(Owner))
    return unsupportedImport(Call);
  if (Slot == 216) {
    // Only declarations in this class, not bindings inherited from its parent.
    std::erase_if(JNI.Registrations, [&](const auto &Entry) {
      return Entry.second.Class == (*Class)->ClassName;
    });
    return value(0);
  }
  const int32_t Count = A[3];
  if (Count < 0 || uint32_t(Count) > jni_abi::ReferenceLimit)
    return failure("JNI native registration count exceeds its bounded domain");
  if (Count && !A[2])
    return failure("JNI native registration requires a method table");
  for (int32_t I = 0; I < Count; ++I) {
    const uint64_t Offset = uint64_t(I) * 24;
    if (A[2] > UINT64_MAX - Offset)
      return failure("JNI native method table address overflow");
    if (auto E = access(A[2] + Offset, 24, Read))
      return std::move(E);
    uint8_t Bytes[24];
    if (auto E = CPU.read(A[2] + Offset, Bytes))
      return std::move(E);
    const uint64_t NameAddress = llvm::support::endian::read64le(Bytes);
    const uint64_t SignatureAddress =
        llvm::support::endian::read64le(Bytes + 8);
    const uint64_t Target = llvm::support::endian::read64le(Bytes + 16);
    if (!NameAddress || !SignatureAddress || !Target)
      return failure("JNI native registration contains a null member");
    auto Name = string(NameAddress);
    if (!Name)
      return Name.takeError();
    auto Signature = string(SignatureAddress);
    if (!Signature)
      return Signature.takeError();
    // ART API 28 accepts the deprecated fast-JNI marker as an ordinary binding.
    llvm::StringRef Prototype = *Signature;
    Prototype.consume_front("!");
    try {
      auto [Parameters, Returns] = prototype(Prototype.str());
      MethodRef Reference{Owner, *Name, std::move(Parameters),
                          std::move(Returns)};
      const auto *Method = JNI.Dex->findMethod(Reference, std::nullopt);
      if (!Method || !has(Method->access, "native")) {
        const std::string Message =
            std::string("no ") + (Method ? "native" : "static or non-static") +
            " method \"" + Owner + "." + *Name + Prototype.str() + "\"";
        if (auto E = jniException("java/lang/NoSuchMethodError", Message))
          return std::move(E);
        // ART publishes each entry in order; previous entries are not rolled
        // back when a later name or non-native declaration fails.
        return value(uint32_t(-1));
      }
      const auto Key = Method->reference.identity();
      if (!JNI.Registrations.contains(Key) &&
          JNI.Registrations.size() == jni_abi::ReferenceLimit)
        return failure("JNI native binding inventory budget exhausted");
      JNI.Registrations[Key] = NativeJNIRegistration{
          Method->reference.owner.substr(1, Method->reference.owner.size() - 2),
          Method->reference.name, Method->reference.signature(), Target};
    } catch (const mobile::Error &E) {
      return failure(E.what());
    }
  }
  return value(0);
}

void Bionic::reportJNIState() {
  for (const auto &[Key, Binding] : JNI.Registrations)
    Result.JNIRegistrations.push_back(Binding);
  if (Options.Android->ObserveJavaState && JNI.DexRuntime)
    Result.JavaState = JNI.DexRuntime->snapshot();
}
} // namespace neverd::emulation::android_model
