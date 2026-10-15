//===- KernelModelThreadPriorities.cpp - Thread priority objects ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "KernelThreadPriorities.h"

namespace neverd::emulation {

llvm::Expected<uint64_t> KernelModel::threadPriorityKey(uint64_t Object,
                                                        bool Changing) const {
  if (const auto Thread = ThreadObjects.find(Object);
      Thread != ThreadObjects.end()) {
    if (Changing && (Thread->second.Terminating || Thread->second.Exited))
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     thread_priority::ExitedThread);
    return Thread->second.CallbackID;
  }
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 thread_priority::UnknownThread);
}

llvm::Expected<uint64_t>
KernelModel::queryThreadPriority(uint64_t Object) const {
  if (const auto Thread = ThreadObjects.find(Object);
      Thread != ThreadObjects.end() && Thread->second.Exited)
    return uint64_t(Thread->second.ExitPriority);
  auto Key = threadPriorityKey(Object, false);
  if (!Key)
    return Key.takeError();
  return uint64_t(Scheduler.threadPriority(*Key));
}

llvm::Expected<uint64_t> KernelModel::setThreadPriority(uint64_t Object,
                                                        int32_t Priority) {
  auto Key = threadPriorityKey(Object, true);
  if (!Key)
    return Key.takeError();
  auto Previous = Scheduler.setThreadPriority(*Key, Priority);
  if (!Previous)
    return Previous.takeError();
  return uint64_t(*Previous);
}

} // namespace neverd::emulation
