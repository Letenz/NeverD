//===- KernelAPIKind.h - Kernel service identifiers -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_KERNELAPIKIND_H
#define NEVERD_EMULATION_KERNELAPIKIND_H
namespace neverd::emulation {
enum class KernelAPIKind {
#define NEVERD_KERNEL_API(Symbol, Arity, Availability) Symbol,
#include "KernelAPIs.def"
#undef NEVERD_KERNEL_API
};
} // namespace neverd::emulation
#endif
