//===- registration_initializer_driver.cpp - PE32 constructors ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep ordinary scalar constructors separate from runtime copy callbacks.
//===----------------------------------------------------------------------===//

#define NEVERD_REGISTRATION_INITIALIZER __attribute__((noinline))
#include "registration_copy_driver.cpp"
