//===- hvf-entry-counter.cpp - Opt-in ARM64 native-entry instrumentation
//---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include <Hypervisor/Hypervisor.h>
#include <atomic>
#include <cstdio>

namespace {
std::atomic<uint64_t> Entries{0};
hv_return_t countedRun(hv_vcpu_t CPU) {
  Entries.fetch_add(1, std::memory_order_relaxed);
  return hv_vcpu_run(CPU);
}
// dyld's interpose section pairs a replacement with its original symbol.
// The replacement's own call retains the original framework binding.
__attribute__((used, section("__DATA,__interpose,interposing"))) const struct {
  const void *Replacement;
  const void *Original;
} Interpose = {reinterpret_cast<const void *>(countedRun),
               reinterpret_cast<const void *>(hv_vcpu_run)};

// Whole-process count includes startup probes. Instrumented wall times must
// never be mixed with normal performance measurements.
__attribute__((destructor)) void report() {
  std::fprintf(stderr,
               "{\"instrumentation\":\"hv_vcpu_run\",\"entries\":%llu}\n",
               static_cast<unsigned long long>(Entries.load()));
}
} // namespace
