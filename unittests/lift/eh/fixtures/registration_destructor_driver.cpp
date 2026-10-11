// Observe destructor count, order and object contents through real CRT search.
extern "C" __declspec(noreturn) void registration_destructor_finish(bool);
#define NEVERD_REGISTRATION_OBJECT_TYPES "registration_destructor_types.h"
#define NEVERD_REGISTRATION_OBJECT_FINISH registration_destructor_finish
#include "registration_object_driver.cpp"

extern "C" {
volatile unsigned callback_destructors = 0;
volatile unsigned callback_destructor_trace = 0;
}

#ifndef EXPECTED_DESTRUCTIONS
#define EXPECTED_DESTRUCTIONS 12
#endif

extern "C"
    __declspec(noreturn) void registration_destructor_finish(bool Passed) {
  // Each round destroys the value catch copy, then its original, then the
  // modified reference object. With rethrow, the uncaught value dies first,
  // followed by the outer reference copy and the shared original exception.
#ifdef RETHROW_SEARCH
  const unsigned Events[] = {318, 669, 665};
#else
  const unsigned Events[] = {322, 318, 665};
#endif
  unsigned ExpectedTrace = 0;
  for (unsigned Round = 0; Round != 4; ++Round)
    for (unsigned Event : Events)
      ExpectedTrace = (ExpectedTrace * 33u) ^ Event;
  const unsigned Results[] = {callback_destructors, callback_destructor_trace};
  char Message[] = "LIFETIME 00000000 00000000\n";
  for (unsigned I = 0; I != 2; ++I)
    for (unsigned J = 0; J != 8; ++J)
      Message[9 + I * 9 + J] =
          "0123456789ABCDEF"[(Results[I] >> ((7 - J) * 4)) & 15];
  unsigned Written = 0;
  const bool Printed = WriteFile(GetStdHandle(unsigned(-11)), Message,
                                 sizeof(Message) - 1, &Written, nullptr) &&
                       Written == sizeof(Message) - 1;
  ExitProcess(Passed && Printed && Results[0] == EXPECTED_DESTRUCTIONS &&
                      Results[1] == ExpectedTrace
                  ? 0
                  : 1);
}
