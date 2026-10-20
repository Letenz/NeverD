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
bool Bionic::jniAssignable(llvm::StringRef Source,
                           llvm::StringRef Destination) const {
  auto Descriptor = [](llvm::StringRef Name) {
    return Name.starts_with("[") ? Name.str() : "L" + Name.str() + ";";
  };
  if (JNI.Dex)
    return JNI.Dex->assignable(Descriptor(Source), Descriptor(Destination));
  return mobile::dalvik::referenceAssignable(Descriptor(Source),
                                             Descriptor(Destination));
}

llvm::Expected<std::shared_ptr<JNIObject>>
Bionic::jniNewObject(JNIObject::Kind Kind, llvm::StringRef Class,
                     uint64_t Bytes) {
  auto Object = JNI.Heap->allocate(Kind, Class.str(), Bytes);
  if (!Object)
    return failure("JNI object storage budget exhausted");
  return Object;
}

llvm::Expected<std::shared_ptr<JNIObject>>
Bionic::jniClass(llvm::StringRef Name) {
  if (auto I = JNI.Classes.find(Name.str()); I != JNI.Classes.end())
    return I->second;
  // Intrinsic and explicitly supplied classes are known. Absence of an
  // arbitrary application class is not evidence that FindClass should throw.
  bool Known = mobile::dalvik::platformClass(Name.str()) ||
               (Name.size() == 2 && Name[0] == '[' &&
                llvm::StringRef("ZBCSIJFD").contains(Name[1]));
  if (JNI.Dex && JNI.Dex->containsClass("L" + Name.str() + ";")) {
    if (auto Reason = JNI.Dex->unsupportedClass("L" + Name.str() + ";"))
      return failure(*Reason);
    Known = true;
  }
  if (!Known && Name.starts_with("[")) {
    try {
      mobile::dalvik::descriptor(Name.str());
      auto Component = Name;
      while (Component.consume_front("[")) {
      }
      if (Component.size() == 1 &&
          llvm::StringRef("ZBCSIJFD").contains(Component[0]))
        Known = true;
      else if (Component.starts_with("L") && Component.ends_with(";")) {
        auto Class = jniClass(Component.drop_front().drop_back());
        if (!Class)
          return Class.takeError();
        Known = true;
      }
    } catch (const mobile::Error &E) {
      return failure(E.what());
    }
  }
  if (!Known)
    return failure("JNI class resolution requires an unmodeled class: " + Name);
  auto Object = JNI.Heap->classObject(Name.str());
  if (!Object)
    return failure("JNI class storage budget exhausted");
  JNI.Classes.emplace(Name.str(), Object);
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

llvm::Error Bionic::jniException(llvm::StringRef Class,
                                 std::optional<llvm::StringRef> Message) {
  auto Object = jniNewObject(JNIObject::Throwable, Class, 0);
  if (!Object)
    return Object.takeError();
  if (Message) {
    try {
      auto Bytes = mobile::dalvik::javaStringBytes(Message->str());
      auto Text =
          jniNewObject(JNIObject::String, "java/lang/String", Bytes.size());
      if (!Text)
        return Text.takeError();
      (*Text)->Bytes = std::move(Bytes);
      (*Object)->Message = (*Text)->Identity;
    } catch (const mobile::Error &E) {
      return failure(E.what());
    }
  }
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
  if (Slot == 27) {
    auto Class = jniObject(A[1]);
    if (!Class)
      return Class.takeError();
    if (!*Class || (*Class)->Type != JNIObject::Class)
      return failure("JNI AllocObject requires a class reference");
    if (!JNI.DexRuntime)
      return unsupportedImport(Call);
    return allocateDex("L" + (*Class)->ClassName + ";", true,
                       [this](uint64_t ID) { return dexReference(ID); });
  }
  if (Slot == 6) {
    auto Name = string(A[1]);
    if (!Name)
      return Name.takeError();
    const auto &Failures = Options.Android->JNIClassLookupFailures;
    if (auto I = Failures.find(*Name); I != Failures.end()) {
      if (auto E = jniException(I->second))
        return std::move(E);
      return value(0);
    }
    auto Class = jniClass(*Name);
    if (!Class)
      return Class.takeError();
    // API 28 ART resolves FindClass without initializing it. GetMethodID,
    // field lookup, allocation and invocation retain their own active uses.
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
    if (Thread.Frames.size() == 1 ||
        (!Thread.NativeFrames.empty() &&
         Thread.Frames.back() == Thread.NativeFrames.back()))
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
      if (!jniAssignable((*Object)->ClassName, "java/lang/Throwable") ||
          !(*Object)->Initialized)
        return failure("JNI Throw requires an initialized throwable object");
      Thread.Exception = *Object;
      return value(0);
    }
    if (Slot == 10 || Slot == 11 || Slot == 14)
      if ((*Object)->Type != JNIObject::Class)
        return failure("JNI operation requires a class reference");
    if (Slot == 10) {
      if ((*Object)->ClassName == "java/lang/Cloneable" ||
          (*Object)->ClassName == "java/io/Serializable")
        return value(0);
      std::string Name;
      if (JNI.Dex && JNI.Dex->containsClass("L" + (*Object)->ClassName + ";")) {
        const auto *Declaration =
            JNI.Dex->findClass("L" + (*Object)->ClassName + ";");
        if (mobile::dalvik::has(Declaration->access, "interface"))
          return value(0);
        auto Parent = JNI.Dex->parent(Declaration->name);
        if (!Parent.empty())
          Name = Parent.substr(1, Parent.size() - 2);
      } else
        Name = mobile::dalvik::javaParent((*Object)->ClassName);
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
