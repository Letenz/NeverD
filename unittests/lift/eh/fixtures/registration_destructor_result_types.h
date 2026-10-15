//===- registration_destructor_result_types.h - PE32 results --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Observe scalar child results through lifetime count, order and contents.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TEST_REGISTRATION_DESTRUCTOR_RESULT_TYPES_H
#define NEVERD_TEST_REGISTRATION_DESTRUCTOR_RESULT_TYPES_H

extern "C" volatile unsigned callback_destructors;
extern "C" volatile unsigned callback_destructor_trace;

struct ValueObject {
  int Head;
  int Tail;
  __attribute__((noinline)) void beginDestruction() noexcept {
    ++callback_destructors;
  }
  __attribute__((noinline)) unsigned destructionEvent() noexcept {
    return 0x100u + unsigned(Head) * 7u + unsigned(Tail);
  }
  ~ValueObject() {
    beginDestruction();
    callback_destructor_trace =
        (callback_destructor_trace * 33u) ^ destructionEvent();
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
  __attribute__((noinline)) unsigned destructionEvent() noexcept {
    return 0x200u + Head * 7u + Tail + Tag;
  }
  ~ReferenceObject() {
    beginDestruction();
    callback_destructor_trace =
        (callback_destructor_trace * 33u) ^ destructionEvent();
    callback_destructors = callback_destructors;
  }
};

#endif
