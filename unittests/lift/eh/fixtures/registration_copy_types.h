// Observable nontrivial copy construction with separate source/destination.
#ifndef NEVERD_TEST_REGISTRATION_COPY_TYPES_H
#define NEVERD_TEST_REGISTRATION_COPY_TYPES_H

#ifndef NEVERD_REGISTRATION_INITIALIZER
#define NEVERD_REGISTRATION_INITIALIZER __attribute__((always_inline))
#endif

extern "C" volatile unsigned callback_copies;
extern "C" volatile unsigned callback_copy_trace;
extern "C" volatile unsigned callback_destructors;
extern "C" volatile unsigned callback_destructor_trace;

struct ValueObject {
  int Head;
  int Tail;
  // Copy-only fixtures inline initialization; constructor fixtures keep the
  // call.
#ifdef NEVERD_REGISTRATION_DELEGATING_CONSTRUCTION
  __attribute__((noinline)) ValueObject(int H, int T, unsigned)
      : Head(H), Tail(T) {}
  NEVERD_REGISTRATION_INITIALIZER ValueObject(int H, int T)
      : ValueObject(H, T, 0) {}
#else
  NEVERD_REGISTRATION_INITIALIZER ValueObject(int H, int T)
      : Head(H), Tail(T) {}
#endif
  __attribute__((noinline)) ValueObject(const ValueObject &Other)
      : Head(Other.Head), Tail(Other.Tail) {
    ++callback_copies;
    callback_copy_trace =
        (callback_copy_trace * 33u) ^
        (0x400u + unsigned(Other.Head) * 7u + unsigned(Other.Tail));
  }
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
#ifdef NEVERD_REGISTRATION_DELEGATING_CONSTRUCTION
  __attribute__((noinline)) ReferenceObject(unsigned H, unsigned T, unsigned G,
                                            unsigned)
      : Head(H), Tail(T), Tag(G) {}
  NEVERD_REGISTRATION_INITIALIZER ReferenceObject(unsigned H, unsigned T,
                                                  unsigned G)
      : ReferenceObject(H, T, G, 0) {}
#else
  NEVERD_REGISTRATION_INITIALIZER ReferenceObject(unsigned H, unsigned T,
                                                  unsigned G)
      : Head(H), Tail(T), Tag(G) {}
#endif
  __attribute__((noinline)) ReferenceObject(const ReferenceObject &Other)
      : Head(Other.Head), Tail(Other.Tail), Tag(Other.Tag) {
    ++callback_copies;
    callback_copy_trace = (callback_copy_trace * 33u) ^
                          (0x800u + Other.Head * 7u + Other.Tail + Other.Tag);
  }
  ~ReferenceObject() {
    ++callback_destructors;
    callback_destructor_trace =
        (callback_destructor_trace * 33u) ^ (0x200u + Head * 7u + Tail + Tag);
  }
};

#endif
