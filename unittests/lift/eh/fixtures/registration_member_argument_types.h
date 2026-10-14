//===- registration_member_argument_types.h - Scalar words ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Observe scalar child results through lifetime count, order and contents.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TEST_REGISTRATION_MEMBER_ARGUMENT_TYPES_H
#define NEVERD_TEST_REGISTRATION_MEMBER_ARGUMENT_TYPES_H

extern "C" volatile unsigned callback_destructors;
extern "C" volatile unsigned callback_destructor_trace;

struct ValueObject {
  int Head;
  int Tail;
  __attribute__((noinline)) void beginDestruction() noexcept {
    ++callback_destructors;
  }
  __attribute__((noinline)) unsigned destructionEvent(unsigned Bias) noexcept {
    return 0x100u + unsigned(Head) * 7u + unsigned(Tail) + Bias;
  }
  ~ValueObject() {
    beginDestruction();
    callback_destructor_trace =
        (callback_destructor_trace * 33u) ^ destructionEvent(0u);
    callback_destructors = callback_destructors;
  }
};
struct ReferenceObject {
  unsigned Head;
  unsigned Tail;
  unsigned Tag;
  __attribute__((noinline)) void beginDestruction() noexcept {
    ++callback_destructors;
  }
  __attribute__((noinline)) unsigned destructionEvent(unsigned Bias) noexcept {
    return 0x200u + Head * 7u + Tail + Tag + Bias;
  }
  ~ReferenceObject() {
    beginDestruction();
    callback_destructor_trace =
        (callback_destructor_trace * 33u) ^ destructionEvent(0u);
    callback_destructors = callback_destructors;
  }
};

#endif
