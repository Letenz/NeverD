//===- AndroidJNIFields.cpp - Supplied DEX field access -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"

namespace neverd::emulation::android_model {
BionicResult Bionic::jniFields(unsigned Slot, const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  auto Receiver = jniObject(A[1]);
  if (!Receiver)
    return Receiver.takeError();
  if (!*Receiver)
    return failure("JNI field access requires a nonnull receiver/class");
  if (!JNI.DexRuntime)
    return unsupportedImport(Call);
  const bool Static = Slot >= 144;
  if (Slot == 94 || Slot == 144) {
    if ((*Receiver)->Type != JNIObject::Class)
      return failure("JNI field lookup requires a class reference");
    auto Name = string(A[2]);
    if (!Name)
      return Name.takeError();
    auto Signature = string(A[3]);
    if (!Signature)
      return Signature.takeError();
    const auto Owner = "L" + (*Receiver)->ClassName + ";";
    auto Initialized = executeDex(nullptr, {}, Owner);
    if (!Initialized || !*Initialized || JNI.Threads.at(threadID()).Exception)
      return Initialized;
    const auto *Field = JNI.Dex->findField({Owner, *Name, *Signature}, Static);
    if (!Field) {
      auto Stopped = unsupportedImport(Call);
      Result.Diagnostic += " (field declaration not supplied)";
      return Stopped;
    }
    for (auto [ID, Existing] : JNI.DexFields)
      if (Existing == Field)
        return value(ID);
    if (JNI.DexFields.size() >= jni_abi::ReferenceLimit)
      return failure("JNI DEX field identity budget exhausted");
    const uint64_t ID = jni_abi::Base + 0x1200000 + JNI.DexFields.size() * 8;
    JNI.DexFields.emplace(ID, Field);
    return value(ID);
  }
  auto It = JNI.DexFields.find(A[2]);
  if (It == JNI.DexFields.end())
    return failure("JNI field ID was not resolved by this VM");
  const auto &Field = *It->second;
  unsigned Offset = Slot - (Static ? 145 : 95);
  const bool Write = Offset >= 9;
  const char Kind = "LZBCSIJFD"[Offset % 9];
  const auto &Type = Field.reference.type;
  if (Static != mobile::dalvik::has(Field.access, "static") ||
      (Kind == 'L' ? !mobile::dalvik::referenceType(Type)
                   : Type != std::string(1, Kind)))
    return failure("JNI field accessor type or storage kind mismatch");
  if (Static && ((*Receiver)->Type != JNIObject::Class ||
                 "L" + (*Receiver)->ClassName + ";" != Field.reference.owner))
    return failure("JNI static field class mismatch");
  uint64_t Bits = A[3];
  if (Write && Kind == 'L') {
    auto Object = jniObject(Bits);
    if (!Object)
      return Object.takeError();
    Bits = *Object ? (*Object)->Identity : 0;
  } else if (Write && Kind == 'Z')
    Bits = uint8_t(Bits);
  auto Executed = runDex([&](const auto &Admit, uint64_t Stack) {
    return JNI.DexRuntime->field(
        Field, Static ? 0 : (*Receiver)->Identity,
        Write ? std::optional<uint64_t>(Bits) : std::nullopt, Admit, Stack);
  });
  if (!Executed || !*Executed || JNI.Threads.at(threadID()).Exception)
    return Executed;
  if (Write)
    return value(0);
  if (Kind == 'L') {
    auto Ref = jniReference(JNI.Heap->get(std::get<uint64_t>(**Executed)));
    if (!Ref)
      return Ref.takeError();
    return value(*Ref);
  }
  return Executed;
}
} // namespace neverd::emulation::android_model
