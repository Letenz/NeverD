// Check runtime copy construction and destruction with independent oracles.
extern "C" __declspec(noreturn) void registration_copy_finish(bool);
#define NEVERD_REGISTRATION_OBJECT_TYPES "registration_copy_types.h"
#define NEVERD_REGISTRATION_OBJECT_FINISH registration_copy_finish
#include "registration_object_driver.cpp"

extern "C" {
volatile unsigned callback_copies = 0;
volatile unsigned callback_copy_trace = 0;
volatile unsigned callback_destructors = 0;
volatile unsigned callback_destructor_trace = 0;
}

#ifndef EXPECTED_COPIES
#define EXPECTED_COPIES 4
#endif

extern "C" __declspec(noreturn) void registration_copy_finish(bool Passed) {
#ifdef SECONDARY_SEARCH
  const unsigned CopyEvent = 2201;
  const unsigned Destruction[] = {318, 665, 669, 665};
#elif defined(RETHROW_SEARCH)
  const unsigned CopyEvent = 2201;
  const unsigned Destruction[] = {318, 669, 665};
#else
  const unsigned CopyEvent = 1086;
  const unsigned Destruction[] = {322, 318, 665};
#endif
  unsigned CopyTrace = 0, DestructorTrace = 0;
  for (unsigned Round = 0; Round != 4; ++Round) {
    CopyTrace = (CopyTrace * 33u) ^ CopyEvent;
    for (unsigned Event : Destruction)
      DestructorTrace = (DestructorTrace * 33u) ^ Event;
  }
  const unsigned Values[] = {callback_copies, callback_copy_trace,
                             callback_destructors, callback_destructor_trace};
  char Message[] = "COPY 00000000 00000000\nLIFETIME 00000000 00000000\n";
  const unsigned Offsets[] = {5, 14, 32, 41};
  for (unsigned I = 0; I != 4; ++I)
    for (unsigned J = 0; J != 8; ++J)
      Message[Offsets[I] + J] =
          "0123456789ABCDEF"[(Values[I] >> ((7 - J) * 4)) & 15];
  unsigned Written = 0;
  const bool Printed = WriteFile(GetStdHandle(unsigned(-11)), Message,
                                 sizeof(Message) - 1, &Written, nullptr) &&
                       Written == sizeof(Message) - 1;
  ExitProcess(Passed && Printed && Values[0] == EXPECTED_COPIES &&
                      Values[1] == CopyTrace &&
                      Values[2] ==
                          4 * (sizeof(Destruction) / sizeof(Destruction[0])) &&
                      Values[3] == DestructorTrace
                  ? 0
                  : 1);
}
