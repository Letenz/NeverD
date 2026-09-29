//===- ProcessReport.h - Process options and result JSON -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_PROCESSREPORT_H
#define NEVERD_EMULATION_PROCESSREPORT_H
#include "neverd/emulation/ProcessSession.h"

namespace neverd::emulation {
llvm::Expected<ProcessOptions> processOptionsFromJSON(llvm::StringRef Text);
/// Binary streams use lowercase hexadecimal. Addresses, service numbers,
/// argument registers and raw results use hexadecimal strings to preserve all
/// 64 bits even in consumers whose JSON number type is a double.
std::string processResultJSON(const ProcessResult &Result);
} // namespace neverd::emulation
#endif
