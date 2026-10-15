//===- registration_destructor_graph_types.h - PE32 object graphs --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep lifetime count, order and contents observable through two member calls.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TEST_REGISTRATION_DESTRUCTOR_GRAPH_TYPES_H
#define NEVERD_TEST_REGISTRATION_DESTRUCTOR_GRAPH_TYPES_H

extern "C" volatile unsigned callback_destructors;
extern "C" volatile unsigned callback_destructor_trace;

struct ValueObject {
  int Head;
  int Tail;
  __attribute__((noinline)) void beginDestruction() noexcept {
    ++callback_destructors;
  }
  __attribute__((noinline)) void recordDestruction() noexcept {
    callback_destructor_trace = (callback_destructor_trace * 33u) ^
                                (0x100u + unsigned(Head) * 7u + unsigned(Tail));
  }
  ~ValueObject() {
    beginDestruction();
    recordDestruction();
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
  __attribute__((noinline)) void recordDestruction() noexcept {
    callback_destructor_trace =
        (callback_destructor_trace * 33u) ^ (0x200u + Head * 7u + Tail + Tag);
  }
  ~ReferenceObject() {
    beginDestruction();
    recordDestruction();
    callback_destructors = callback_destructors;
  }
};

#endif
