//===- AndroidJNI.cpp - Explicit JNI invocation and thread ownership -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// JNI invocation and thread ownership.
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::android_model {
namespace {
bool isJNIEntry(const LinkedImage &Linked, uint64_t PC) {
  return PC >= Linked.JNIThunkBegin && PC < Linked.JNIThunkEnd &&
         (PC - Linked.JNIThunkBegin) % 8 == 0;
}
} // namespace
llvm::Error initializeJNI(AddressSpace &Space, LinkedImage &Linked) {
  auto &Bytes = Linked.JNIImage;
  Linked.JNIThunkBegin = ThunkBase + (Linked.Imports.size() + 1) * 8;
  Bytes.resize(jni_abi::ArenaSize);
  auto Put = [&](uint64_t Address, uint64_t Value) {
    llvm::support::endian::write64le(Bytes.data() + Address - jni_abi::Base,
                                     Value);
  };
  auto Trap = [&](std::string Name) {
    const uint64_t PC = ThunkBase + (Linked.Imports.size() + 1) * 8;
    Linked.Imports.emplace(PC, std::move(Name));
    return PC;
  };
  Put(jni_abi::Base, jni_abi::VMTable);
  const char *VMNames[] = {"DestroyJavaVM", "AttachCurrentThread",
                           "DetachCurrentThread", "GetEnv",
                           "AttachCurrentThreadAsDaemon"};
  for (unsigned I = 3; I != 8; ++I)
    Put(jni_abi::VMTable + I * 8,
        Trap(std::string("JavaVM.") + VMNames[I - 3]));
  // Preserve the standard table offsets, including unsupported entries. Calling
  // one of those entries reaches an explicit unsupported-service stop.
  for (unsigned I = 4; I != jni_abi::EnvSlots; ++I)
    Put(jni_abi::EnvTable + I * 8, Trap("JNIEnv." + std::to_string(I)));
  Linked.JNIThunkEnd = ThunkBase + (Linked.Imports.size() + 1) * 8;
  for (unsigned I = 0; I != jni_abi::AttachmentLimit; ++I)
    Put(jni_abi::Environments + I * 8, jni_abi::EnvTable);
  if (auto E = Space.map(jni_abi::Base, Bytes.size(), Read | Write))
    return E;
  if (auto E = Space.write(jni_abi::Base, Bytes))
    return E;
  return Space.protect(jni_abi::Base, Bytes.size(), Read | UserAccessible);
}

llvm::Error Bionic::jniIdentity(uint64_t Address, uint64_t Expected) {
  if (Address != Expected)
    return failure("JNI VM/environment identity does not belong to this call");
  if (auto E = access(jni_abi::Base, Linked.JNIImage.size(), Read))
    return E;
  std::vector<uint8_t> Bytes(Linked.JNIImage.size());
  if (auto E = CPU.read(jni_abi::Base, Bytes))
    return E;
  if (Bytes != Linked.JNIImage)
    return failure("JNI interface storage was modified or replaced");
  return llvm::Error::success();
}

llvm::Error Bionic::jniPut(uint64_t Address, uint64_t Value, unsigned Size) {
  if (auto E = access(Address, Size, Write))
    return E;
  uint8_t Bytes[8];
  llvm::support::endian::write64le(Bytes, Value);
  return CPU.write(Address, llvm::ArrayRef(Bytes, Size));
}

llvm::Expected<JNIState::Thread *> Bionic::jniThread(uint64_t Environment) {
  auto I = JNI.Threads.find(threadID());
  if (I == JNI.Threads.end() || !I->second.Environment)
    return failure("JNI call on a detached native thread");
  if (auto E = jniIdentity(Environment, I->second.Environment))
    return std::move(E);
  return &I->second;
}

llvm::Error Bionic::jniThreadExit() {
  auto I = JNI.Threads.find(threadID());
  if (I != JNI.Threads.end() && I->second.Environment)
    return failure("JNI attached native thread exited without detaching");
  return llvm::Error::success();
}

BionicResult Bionic::jniVM(const NativeCallEvent &Call) {
  if (!Options.Android->JNI)
    return unsupportedImport(Call);
  const auto &A = Call.Arguments;
  auto Status = [&](int32_t S) { return value(uint32_t(S)); };
  if (Call.Name == "JNI_GetCreatedJavaVMs") {
    const int32_t Length = A[1];
    if (Length < 0)
      return failure("JNI VM enumeration has a negative buffer length");
    // ART reports the number actually copied. Preserve its count-first write
    // order, including when the caller aliases the two output parameters.
    if (auto E = jniPut(A[2], Length ? 1 : 0, 4))
      return std::move(E);
    if (Length) {
      if (auto E = jniPut(A[0], jni_abi::Base, 8))
        return std::move(E);
    }
    return Status(0);
  }
  const bool GetEnv = Call.Name == "JavaVM.GetEnv";
  if (!isJNIEntry(Linked, Call.PC))
    return failure("JNI invocation did not enter its owned function table");
  const bool Attach = Call.Name == "JavaVM.AttachCurrentThread" ||
                      Call.Name == "JavaVM.AttachCurrentThreadAsDaemon";
  const bool Detach = Call.Name == "JavaVM.DetachCurrentThread";
  if (!GetEnv && !Attach && !Detach)
    return unsupportedImport(Call);
  auto Version = [](uint32_t V) {
    return V == 0x10002 || V == 0x10004 || V == jni_abi::Version;
  };
  // ART rejects an unsupported GetEnv version before touching its output.
  if (GetEnv && !Version(uint32_t(A[2])) && uint32_t(A[2]) != 0x10001)
    return Status(-3);
  if (!A[0] || (!Detach && !A[1]))
    return Status(-1);
  if (auto E = jniIdentity(A[0], jni_abi::Base))
    return std::move(E);
  auto &Thread = JNI.Threads[threadID()];
  if (Detach) {
    if (!Thread.Environment)
      return Status(-1);
    if (!Thread.NativeFrames.empty())
      return Status(-1);
    for (const auto &[Address, Borrow] : JNI.Borrows)
      if (Borrow.Thread == threadID())
        return failure("JNI detach with outstanding borrowed storage");
    std::erase_if(JNI.References, [&](const auto &Entry) {
      return Entry.second.Thread == threadID();
    });
    Thread = {};
    return Status(0);
  }
  if (GetEnv || Thread.Environment) {
    if (auto E = jniPut(A[1], Thread.Environment, 8))
      return std::move(E);
    return Status(Thread.Environment ? 0 : -2);
  }
  if (A[2]) {
    if (auto E = access(A[2], 24, Read))
      return std::move(E);
    uint8_t Args[24];
    if (auto E = CPU.read(A[2], Args))
      return std::move(E);
    if (!Version(llvm::support::endian::read32le(Args)))
      return Status(-3);
    // Names/groups have observable Java and kernel thread effects. They need
    // an explicit model rather than being silently discarded on attachment.
    if (llvm::support::endian::read64le(Args + 8) ||
        llvm::support::endian::read64le(Args + 16))
      return unsupportedImport(Call);
  }
  if (JNI.NextEnvironment == jni_abi::AttachmentLimit)
    return failure("JNI attachment identity budget exhausted");
  const uint64_t Environment = jni_abi::Environments + JNI.NextEnvironment * 8;
  if (auto E = jniPut(A[1], Environment, 8))
    return std::move(E);
  ++JNI.NextEnvironment;
  Thread.Environment = Environment;
  return Status(0);
}

BionicResult Bionic::jniCall(const NativeCallEvent &Call) {
  if (!Options.Android->JNI)
    return unsupportedImport(Call);
  if (!isJNIEntry(Linked, Call.PC))
    return failure("JNI operation did not enter its owned function table");
  llvm::StringRef Name = Call.Name;
  unsigned Slot = 0;
  if (!Name.consume_front("JNIEnv.") || Name.getAsInteger(10, Slot))
    return unsupportedImport(Call);
  auto Thread = jniThread(Call.Arguments[0]);
  if (!Thread)
    return Thread.takeError();
  if ((*Thread)->Exception && Slot != 15 && Slot != 17 && Slot != 228 &&
      Slot != 22 && Slot != 23 && Slot != 19 && Slot != 20 && Slot != 166 &&
      Slot != 170 && !(Slot >= 191 && Slot <= 198))
    return failure("JNI operation is not admitted with a pending exception");
  if (Slot == 215 || Slot == 216)
    return jniRegistration(Slot, Call);
  if (Slot == 4)
    return value(jni_abi::Version);
  if (Slot == 219) {
    if (auto E = jniPut(Call.Arguments[1], jni_abi::Base, 8))
      return std::move(E);
    return value(0);
  }
  if ((Slot >= 163 && Slot <= 170) || Slot == 220 || Slot == 221)
    return jniStrings(Slot, Call);
  if (Slot >= 171 && Slot <= 214)
    return jniArrays(Slot, Call);
  if ((Slot >= 94 && Slot <= 112) || (Slot >= 144 && Slot <= 162))
    return jniFields(Slot, Call);
  if ((Slot >= 28 && Slot <= 30) || (Slot >= 33 && Slot <= 93) ||
      (Slot >= 113 && Slot <= 143))
    return jniMethods(Slot, Call);
  return jniObjects(Slot, Call);
}
} // namespace neverd::emulation::android_model
