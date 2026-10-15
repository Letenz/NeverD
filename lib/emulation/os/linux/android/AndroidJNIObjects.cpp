//===- AndroidJNIObjects.cpp - Opaque JNI references and local frames -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// JNI references, local frames, intrinsic class identity and exceptions.
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"

#include "neverd/dalvik/Runtime.h"

namespace neverd::emulation::android_model {
namespace {
llvm::StringRef parent(llvm::StringRef Name) {
  if (Name == "java/lang/Object")
    return {};
  if (Name == "java/lang/RuntimeException")
    return "java/lang/Exception";
  if (Name == "java/lang/Exception" || Name == "java/lang/Error")
    return "java/lang/Throwable";
  if (Name == "java/lang/OutOfMemoryError")
    return "java/lang/Error";
  if (Name == "java/lang/ArrayIndexOutOfBoundsException" ||
      Name == "java/lang/StringIndexOutOfBoundsException")
    return "java/lang/IndexOutOfBoundsException";
  if (Name.starts_with("java/lang/") && Name.ends_with("Exception"))
    return "java/lang/RuntimeException";
  return "java/lang/Object";
}
} // namespace
bool jniAssignable(llvm::StringRef Source, llvm::StringRef Destination) {
  if (Source == "[Ljava/lang/String;" && Destination == "[Ljava/lang/Object;")
    return true;
  for (; !Source.empty(); Source = parent(Source))
    if (Source == Destination)
      return true;
  return false;
}

llvm::Expected<std::shared_ptr<JNIObject>>
Bionic::jniNewObject(JNIObject::Kind Kind, llvm::StringRef Class,
                     uint64_t Bytes) {
  const uint64_t Limit = Options.MemoryLimit / 2;
  const uint64_t Overhead = sizeof(JNIObject) + Class.size() + 64;
  if (JNI.CreatedObjects == jni_abi::ObjectLimit || Bytes > Limit ||
      Overhead > Limit - Bytes || JNI.ObjectBytes > Limit - Bytes - Overhead)
    return failure("JNI object storage budget exhausted");
  auto Object = std::make_shared<JNIObject>();
  Object->Type = Kind;
  Object->ClassName = Class.str();
  Object->Bytes.resize(Bytes);
  ++JNI.CreatedObjects;
  JNI.ObjectBytes += Bytes + Overhead;
  JNI.Objects.push_back(Object);
  return Object;
}

llvm::Expected<std::shared_ptr<JNIObject>>
Bionic::jniClass(llvm::StringRef Name) {
  if (auto I = JNI.Classes.find(Name.str()); I != JNI.Classes.end())
    return I->second;
  // Intrinsic and explicitly supplied classes are known. Absence of an
  // arbitrary application class is not evidence that FindClass should throw.
  bool Known = Name == "java/lang/Object" || Name == "java/lang/Class" ||
               Name == "java/lang/String" || Name == "java/lang/Throwable" ||
               Name == "java/lang/Exception" || Name == "java/lang/Error" ||
               Name == "java/lang/RuntimeException" ||
               Name == "java/lang/ArithmeticException" ||
               Name == "java/lang/NullPointerException" ||
               Name == "java/lang/IllegalArgumentException" ||
               Name == "java/lang/IndexOutOfBoundsException" ||
               Name == "java/lang/ArrayIndexOutOfBoundsException" ||
               Name == "java/lang/StringIndexOutOfBoundsException" ||
               Name == "java/lang/NegativeArraySizeException" ||
               Name == "java/lang/ArrayStoreException" ||
               Name == "java/lang/OutOfMemoryError" ||
               Name == "[Ljava/lang/Object;" || Name == "[Ljava/lang/String;" ||
               (Name.size() == 2 && Name[0] == '[' &&
                llvm::StringRef("ZBCSIJFD").contains(Name[1]));
  if (JNI.Dex && JNI.Dex->containsClass("L" + Name.str() + ";")) {
    if (auto Reason = JNI.Dex->unsupportedClass("L" + Name.str() + ";"))
      return failure(*Reason);
    Known = true;
  }
  if (!Known)
    return failure("JNI class resolution requires an unmodeled class: " + Name);
  auto Object = jniNewObject(JNIObject::Class, Name, 0);
  if (!Object)
    return Object.takeError();
  JNI.Classes.emplace(Name.str(), *Object);
  return Object;
}

llvm::Expected<std::shared_ptr<JNIObject>>
Bionic::jniObject(uint64_t Reference) {
  if (!Reference)
    return std::shared_ptr<JNIObject>();
  auto I = JNI.References.find(Reference);
  if (I == JNI.References.end() ||
      (I->second.Thread && I->second.Thread != threadID()))
    return failure(
        "JNI reference is invalid, retired or belongs to another thread");
  return I->second.Object;
}

llvm::Expected<uint64_t> Bionic::jniReference(std::shared_ptr<JNIObject> Object,
                                              bool Global) {
  if (!Object)
    return uint64_t(0);
  if (JNI.References.size() == jni_abi::ReferenceLimit)
    return failure("JNI reference budget exhausted");
  const uint64_t Reference = jni_abi::ReferenceBase + JNI.NextReference++ * 8;
  JNI.References.emplace(
      Reference, JNIState::Reference{
                     std::move(Object), Global ? 0 : threadID(),
                     Global ? 0 : JNI.Threads.at(threadID()).Frames.back()});
  return Reference;
}

llvm::Error Bionic::jniException(llvm::StringRef Class) {
  auto Object = jniNewObject(JNIObject::Throwable, Class, 0);
  if (!Object)
    return Object.takeError();
  JNI.Threads.at(threadID()).Exception = std::move(*Object);
  return llvm::Error::success();
}

BionicResult Bionic::jniObjects(unsigned Slot, const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  auto &Thread = JNI.Threads.at(threadID());
  auto Ref = [&](std::shared_ptr<JNIObject> Object,
                 bool Global = false) -> BionicResult {
    auto Handle = jniReference(std::move(Object), Global);
    if (!Handle)
      return Handle.takeError();
    return value(*Handle);
  };
  if (Slot == 6) {
    auto Name = string(A[1]);
    if (!Name)
      return Name.takeError();
    auto Class = jniClass(*Name);
    if (!Class)
      return Class.takeError();
    return Ref(*Class);
  }
  if (Slot == 15)
    return Ref(Thread.Exception);
  if (Slot == 17) {
    Thread.Exception.reset();
    return value(0);
  }
  if (Slot == 228)
    return value(bool(Thread.Exception));
  if (Slot == 19 || Slot == 26) {
    const int32_t Capacity = A[1];
    if (Capacity < 0)
      return value(uint32_t(-1));
    if (uint32_t(Capacity) > jni_abi::ReferenceLimit - JNI.References.size() ||
        Thread.Frames.size() == jni_abi::ReferenceLimit)
      return failure("JNI local frame capacity budget exhausted");
    if (Slot == 19)
      Thread.Frames.push_back(JNI.NextFrame++);
    return value(0);
  }
  if (Slot == 20) {
    if (Thread.Frames.size() == 1)
      return failure("JNI local frame stack underflow");
    auto Object = jniObject(A[1]);
    if (!Object)
      return Object.takeError();
    const uint64_t Frame = Thread.Frames.back();
    std::erase_if(JNI.References, [&](const auto &Entry) {
      return Entry.second.Thread == threadID() && Entry.second.Frame == Frame;
    });
    Thread.Frames.pop_back();
    return Ref(*Object);
  }
  if (Slot == 21 || Slot == 25 || Slot == 22 || Slot == 23 || Slot == 232) {
    auto Object = jniObject(A[1]);
    if (!Object)
      return Object.takeError();
    if (Slot == 21 || Slot == 25)
      return Ref(*Object, Slot == 21);
    if (!*Object)
      return value(0);
    const bool Global = !JNI.References.at(A[1]).Thread;
    if (Slot == 232)
      return value(Global ? 2 : 1);
    if (Global != (Slot == 22))
      return failure("JNI reference deletion kind mismatch");
    JNI.References.erase(A[1]);
    return value(0);
  }
  if (Slot == 24) {
    auto Left = jniObject(A[1]);
    if (!Left)
      return Left.takeError();
    auto Right = jniObject(A[2]);
    if (!Right)
      return Right.takeError();
    return value(*Left == *Right);
  }
  if (Slot == 10 || Slot == 11 || Slot == 31 || Slot == 32 || Slot == 13 ||
      Slot == 14) {
    auto Object = jniObject(A[1]);
    if (!Object)
      return Object.takeError();
    if (!*Object && Slot != 32)
      return failure("JNI operation requires a nonnull object");
    if (Slot == 31) {
      auto Class =
          jniClass((*Object)->Type == JNIObject::Class ? "java/lang/Class"
                                                       : (*Object)->ClassName);
      if (!Class)
        return Class.takeError();
      return Ref(*Class);
    }
    if (Slot == 13) {
      if ((*Object)->Type != JNIObject::Throwable)
        return failure("JNI Throw requires a throwable object");
      Thread.Exception = *Object;
      return value(0);
    }
    if (Slot == 10 || Slot == 11 || Slot == 14)
      if ((*Object)->Type != JNIObject::Class)
        return failure("JNI operation requires a class reference");
    if (Slot == 10) {
      auto Name = parent((*Object)->ClassName);
      if (Name.empty())
        return value(0);
      auto Class = jniClass(Name);
      if (!Class)
        return Class.takeError();
      return Ref(*Class);
    }
    if (Slot == 14) {
      if (!jniAssignable((*Object)->ClassName, "java/lang/Throwable"))
        return failure("JNI ThrowNew requires a throwable class");
      // Exception constructor/message semantics are not supplied by a host VM.
      // Admit the no-message constructor only; nonnull messages stop
      // explicitly.
      if (A[2])
        return unsupportedImport(Call);
      if (auto E = jniException((*Object)->ClassName))
        return std::move(E);
      return value(0);
    }
    auto Class = jniObject(A[2]);
    if (!Class)
      return Class.takeError();
    if (!*Class || (*Class)->Type != JNIObject::Class)
      return failure("JNI type check requires a class reference");
    return value(!*Object ||
                 jniAssignable(Slot == 32 && (*Object)->Type == JNIObject::Class
                                   ? llvm::StringRef("java/lang/Class")
                                   : llvm::StringRef((*Object)->ClassName),
                               (*Class)->ClassName));
  }
  return unsupportedImport(Call);
}
} // namespace neverd::emulation::android_model
