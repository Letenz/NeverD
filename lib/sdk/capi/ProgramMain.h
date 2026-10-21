//===- ProgramMain.h - The C program's main ---------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The function a C program's start code hands its C library's start routine
/// as main: glibc's and musl's `_start` calls __libc_start_main(main, argc,
/// argv, ...), and uClibc's, bionic's and FreeBSD's routines take main too
/// (libc/LibCStartup.h).  The runtime calls that function as
/// libc::kProgramMainDeclaration declares it.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_SDK_CAPI_PROGRAMMAIN_H
#define NEVERD_LIB_SDK_CAPI_PROGRAMMAIN_H

#include "neverd/loader/BinaryImage.h"

#include <optional>

namespace neverd::sdk {

/// The function the image's entry passes a start routine as main: the value
/// the entry's straight-line code puts in that routine's main argument, a
/// register or a stack word, before it calls the routine, read through the
/// pointer slot it loads the value from, if it loads it.  Else the function
/// a symbol names main.  None when neither says: the entry calls something
/// else first, or the value is no code address.
std::optional<va_t> findProgramMain(const BinaryImage &Img);

} // namespace neverd::sdk

#endif // NEVERD_LIB_SDK_CAPI_PROGRAMMAIN_H
