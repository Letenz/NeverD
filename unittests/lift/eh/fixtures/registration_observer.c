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
#if REGISTRATION_CASE == 5 || REGISTRATION_CASE == 6
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
#else
#define EXPECTED_TRACE 0
#endif

void registration_raise(void) {
  observed_caller = (unsigned)_ReturnAddress();
  RaiseException(0xe0420042, 0, 0, 0);
}

void registration_no_raise(void) {
  observed_caller = (unsigned)_ReturnAddress();
}

void mainCRTStartup(void) {
  unsigned chain = __readfsdword(0);
  unsigned iterations = 0;
  int value = 0, chain_ok = 1;
  for (; iterations != 4; ++iterations) {
    registration_trace = 0;
#if REGISTRATION_CASE == 6
    registration_parameter_after = 0;
    value = registration_call_entry();
    registration_trace += registration_parameter_after * 10;
#elif REGISTRATION_CASE == 5
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
