//===- registration_destructor_types.h - Observable destruction types -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Source records with observable destruction of both catch copies and throws.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TEST_REGISTRATION_DESTRUCTOR_TYPES_H
#define NEVERD_TEST_REGISTRATION_DESTRUCTOR_TYPES_H

extern "C" volatile unsigned callback_destructors;
extern "C" volatile unsigned callback_destructor_trace;

struct ValueObject {
  int Head;
  int Tail;
  ~ValueObject() {
    ++callback_destructors;
    callback_destructor_trace = (callback_destructor_trace * 33u) ^
                                (0x100u + unsigned(Head) * 7u + unsigned(Tail));
  }
};
struct ReferenceObject {
  unsigned Head;
  unsigned Tail;
  unsigned Tag;
  ~ReferenceObject() {
    ++callback_destructors;
    callback_destructor_trace =
        (callback_destructor_trace * 33u) ^ (0x200u + Head * 7u + Tail + Tag);
  }
};

#endif
