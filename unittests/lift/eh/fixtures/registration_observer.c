// Executes the same source entry repeatedly and observes its actual caller PC.
__declspec(dllimport) void __stdcall RaiseException(unsigned long,
                                                    unsigned long,
                                                    unsigned long,
                                                    const unsigned long *);
__declspec(dllimport) void __stdcall ExitProcess(unsigned);
__declspec(dllimport) int __cdecl printf(const char *, ...);
void *_ReturnAddress(void);
unsigned long __readfsdword(unsigned long);
#pragma intrinsic(_ReturnAddress)
#pragma intrinsic(__readfsdword)
#if REGISTRATION_CASE == 5 || REGISTRATION_CASE == 6 || REGISTRATION_CASE == 12
extern int registration_entry(int);
#else
extern int registration_entry(void);
#endif
#if REGISTRATION_CASE == 6
extern int registration_call_entry(void);
unsigned registration_parameter_after;
#endif
static unsigned observed_caller;
unsigned registration_trace;
#if (REGISTRATION_CASE >= 36 && REGISTRATION_CASE <= 49) ||                    \
    (REGISTRATION_CASE == 52 || REGISTRATION_CASE == 57)
unsigned registration_iteration;
#endif
#if REGISTRATION_CASE >= 48 && REGISTRATION_CASE <= 51
unsigned registration_helper_visits;
#endif
#if REGISTRATION_CASE == 1 || REGISTRATION_CASE == 2
#define EXPECTED_TRACE 123
#elif REGISTRATION_CASE == 3
#define EXPECTED_TRACE 1
#elif REGISTRATION_CASE == 4
#define EXPECTED_TRACE 2
#elif REGISTRATION_CASE == 5
#define EXPECTED_TRACE 7
#elif REGISTRATION_CASE == 6
#define EXPECTED_TRACE 177
#elif REGISTRATION_CASE == 7 || REGISTRATION_CASE == 8
#define EXPECTED_TRACE 7u
#elif REGISTRATION_CASE == 28 || REGISTRATION_CASE == 29 ||                    \
    REGISTRATION_CASE == 30 || REGISTRATION_CASE == 31 ||                      \
    REGISTRATION_CASE == 34 || REGISTRATION_CASE == 35
#define EXPECTED_TRACE 243
#elif REGISTRATION_CASE >= 43 && REGISTRATION_CASE <= 49
#define EXPECTED_TRACE ((iterations & 1) + 1)
#elif REGISTRATION_CASE >= 36 && REGISTRATION_CASE <= 42
#define EXPECTED_TRACE 2345
#elif REGISTRATION_CASE == 57
#define EXPECTED_TRACE ((iterations & 1) ? 23u : 235u)
#elif REGISTRATION_CASE == 54 || REGISTRATION_CASE == 58 ||                    \
    REGISTRATION_CASE == 60 || REGISTRATION_CASE == 62
#define EXPECTED_TRACE 236u
#elif REGISTRATION_CASE == 59 || REGISTRATION_CASE == 61 ||                    \
    REGISTRATION_CASE == 63
#define EXPECTED_TRACE 2367u
#elif REGISTRATION_CASE == 55
#define EXPECTED_TRACE 234u
#elif REGISTRATION_CASE == 9 || REGISTRATION_CASE == 10 ||                     \
    REGISTRATION_CASE == 12 || REGISTRATION_CASE >= 13
#define EXPECTED_TRACE 23u
#elif REGISTRATION_CASE == 11
#define EXPECTED_TRACE 3u
#else
#define EXPECTED_TRACE 0
#endif

void registration_raise(void) {
  observed_caller = (unsigned)_ReturnAddress();
#if REGISTRATION_CASE == 8
  RaiseException(0xe0420042, 1, 0, 0);
#else
  RaiseException(0xe0420042, 0, 0, 0);
#endif
}

void registration_no_raise(void) {
  observed_caller = (unsigned)_ReturnAddress();
#if REGISTRATION_CASE == 48 || REGISTRATION_CASE == 50
  ++registration_helper_visits;
#endif
}

void mainCRTStartup(void) {
  unsigned chain = __readfsdword(0);
  unsigned iterations = 0;
  int value = 0, chain_ok = 1;
  for (; iterations != 4; ++iterations) {
    registration_trace = 0;
#if (REGISTRATION_CASE >= 36 && REGISTRATION_CASE <= 49) ||                    \
    (REGISTRATION_CASE == 52 || REGISTRATION_CASE == 57)
    registration_iteration = iterations;
#endif
#if REGISTRATION_CASE >= 48 && REGISTRATION_CASE <= 51
    registration_helper_visits = 0;
#endif
#if REGISTRATION_CASE == 6
    registration_parameter_after = 0;
    value = registration_call_entry();
    registration_trace += registration_parameter_after * 10;
#elif REGISTRATION_CASE == 5 || REGISTRATION_CASE == 12
    value = registration_entry(7);
#else
    value = registration_entry();
#endif
    chain_ok &= __readfsdword(0) == chain;
    if (value != 7 || !chain_ok || registration_trace != EXPECTED_TRACE)
      break;
  }
  printf("neverd-registration-rewrite: value=%d caller=%08x entry=%08x "
         "chain=%d iterations=%u trace=%u\n",
         value, observed_caller, (unsigned)&registration_entry, chain_ok,
         iterations, registration_trace);
  ExitProcess(value == 7 && chain_ok && iterations == 4 ? 0 : 1);
}
