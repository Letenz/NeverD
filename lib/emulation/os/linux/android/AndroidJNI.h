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

#include "neverd/dalvik/Runtime.h"
#include "neverd/emulation/AndroidNative.h"
#include "neverd/emulation/Limits.h"

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
inline constexpr unsigned
    EnvSlots = 233,
    AttachmentLimit = ::neverd::limits::emulation::kAndroidJNIAttachmentLimit;
inline constexpr unsigned
    ReferenceLimit = ::neverd::limits::emulation::kAndroidJNIReferenceLimit,
    ObjectLimit = ::neverd::limits::emulation::kAndroidJNIObjectLimit;
inline constexpr uint32_t Version = 0x00010006;
} // namespace jni_abi

using JNIObject = mobile::dalvik::Object;
struct JNIState {
  struct Thread {
    uint64_t Environment = 0;
    std::vector<uint64_t> Frames{0};
    // Implicit native frames cannot be popped by PopLocalFrame or detached.
    std::vector<uint64_t> NativeFrames;
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
  std::map<uint64_t, const mobile::dalvik::Field *> DexFields;
  std::map<std::string, NativeJNIRegistration> Registrations;
  std::shared_ptr<mobile::dalvik::Heap> Heap;
  uint64_t NextEnvironment = 0, NextReference = 1, NextFrame = 1;
};
} // namespace neverd::emulation::android_model
// Preserve interface spellings; resource values are defined by Limits.h.
namespace neverd::emulation::android_model {
namespace jni_abi {
inline constexpr unsigned NativeFrameLimit =
    ::neverd::limits::emulation::kAndroidJNINativeFrameLimit;
}
} // namespace neverd::emulation::android_model

#endif
