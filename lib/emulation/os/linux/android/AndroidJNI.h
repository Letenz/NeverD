//===- AndroidJNI.h - Workload-owned JNI state ------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Workload-owned JNI identities, references and bounded object storage.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_ANDROIDJNI_H
#define NEVERD_EMULATION_ANDROIDJNI_H

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace neverd::mobile::dalvik {
class Program;
class Runtime;
struct Method;
} // namespace neverd::mobile::dalvik

namespace neverd::emulation::android_model {
namespace jni_abi {
// Model placement, not an ART object layout. Reference handles are opaque.
inline constexpr uint64_t Base = 0x7020000000;
inline constexpr uint64_t VMTable = Base + 64, EnvTable = Base + 256;
inline constexpr uint64_t Environments = Base + 4096, ArenaSize = 8192;
inline constexpr uint64_t ReferenceBase = Base + 65536;
inline constexpr unsigned EnvSlots = 233, AttachmentLimit = 512;
inline constexpr unsigned ReferenceLimit = 4096, ObjectLimit = 4096;
inline constexpr uint32_t Version = 0x00010006;
} // namespace jni_abi

struct JNIObject {
  enum Kind { Class, String, PrimitiveArray, ObjectArray, Throwable };
  Kind Type;
  std::string ClassName;
  std::vector<uint8_t> Bytes;
  std::vector<std::weak_ptr<JNIObject>> Elements;
  uint64_t Length = 0;
  unsigned ElementSize = 0;
};
struct JNIState {
  struct Thread {
    uint64_t Environment = 0;
    std::vector<uint64_t> Frames{0};
    std::shared_ptr<JNIObject> Exception;
  };
  struct Reference {
    std::shared_ptr<JNIObject> Object;
    uint64_t Thread, Frame;
  };
  struct Borrow {
    std::shared_ptr<JNIObject> Object;
    uint64_t Thread, Size;
    unsigned Slot;
  };
  std::map<uint64_t, Thread> Threads;
  std::map<uint64_t, Reference> References;
  std::map<uint64_t, Borrow> Borrows;
  std::map<std::string, std::shared_ptr<JNIObject>> Classes;
  // IDs are separate from references and survive local-frame/thread teardown.
  std::set<unsigned> ResolvedMethods;
  std::shared_ptr<const mobile::dalvik::Program> Dex;
  std::shared_ptr<mobile::dalvik::Runtime> DexRuntime;
  std::map<uint64_t, const mobile::dalvik::Method *> DexMethods;
  std::map<std::string, std::shared_ptr<JNIObject>> MethodConstants;
  // Retain bounded objects until workload teardown. Weak element edges avoid
  // host ownership cycles; this profile deliberately makes no GC claim.
  std::vector<std::shared_ptr<JNIObject>> Objects;
  uint64_t NextEnvironment = 0, NextReference = 1, NextFrame = 1;
  uint64_t CreatedObjects = 0, ObjectBytes = 0;
};
bool jniAssignable(llvm::StringRef Source, llvm::StringRef Destination);
} // namespace neverd::emulation::android_model
#endif
