// Genuine MSVC RTTI, throw, catch-object and cleanup ABI baseline.
#include <intrin.h>
#include <malloc.h>
#include <stdio.h>

#pragma intrinsic(_ReturnAddress)
#pragma intrinsic(__readfsdword)

extern "C" {
__declspec(dllexport) volatile unsigned registration_cxx_trace;
__declspec(dllexport) volatile unsigned registration_cxx_caught;
__declspec(dllexport) unsigned registration_cxx_caller;
}

struct RegistrationGuard {
  unsigned Tag;
  __declspec(dllexport) __declspec(noinline) ~RegistrationGuard();
};

RegistrationGuard::~RegistrationGuard() {
  registration_cxx_trace = registration_cxx_trace * 10 + Tag;
}

extern "C" __declspec(dllexport) __declspec(noinline) void
registration_cxx_throw() noexcept(false) {
  registration_cxx_caller = (unsigned)_ReturnAddress();
  throw 7;
}

extern "C" __declspec(dllexport) __declspec(noinline) int
registration_cxx_probe() {
  try {
    RegistrationGuard Outer{1};
    RegistrationGuard Inner{2};
    registration_cxx_throw();
  }
#if REGISTRATION_CXX_REFERENCE
  catch (int &Value) {
    Value += 11;
#else
  catch (int Value) {
#endif
    registration_cxx_trace = registration_cxx_trace * 10 + 3;
    registration_cxx_caught = Value;
#if REGISTRATION_CXX_REFERENCE
    return Value - 11;
#else
    return Value;
#endif
  }
  return 9;
}

extern "C" {
int registration_cxx_generated_value();
int registration_cxx_generated_reference();
int registration_cxx_generated_aligned_value();
int registration_cxx_generated_aligned_reference();
}

__declspec(noinline) int registration_cxx_call(int (*Probe)(),
                                               unsigned Padding) {
  auto *Space = static_cast<volatile unsigned char *>(_alloca(Padding));
  Space[0] = static_cast<unsigned char>(Padding);
  const int Value = Probe();
  return Space[0] == Padding ? Value : -1;
}

int main() {
#if REGISTRATION_CXX_GENERATED
#if REGISTRATION_CXX_ALIGNED
  const auto Probe = REGISTRATION_CXX_REFERENCE
                         ? registration_cxx_generated_aligned_reference
                         : registration_cxx_generated_aligned_value;
#else
  const auto Probe = REGISTRATION_CXX_REFERENCE
                         ? registration_cxx_generated_reference
                         : registration_cxx_generated_value;
#endif
#else
  const auto Probe = registration_cxx_probe;
#endif
  const unsigned Chain = __readfsdword(0);
  unsigned Iterations = 0;
  int Value = 0;
  bool ChainOK = true;
  for (; Iterations != 4; ++Iterations) {
    registration_cxx_trace = registration_cxx_caught = 0;
    Value = registration_cxx_call(Probe, (Iterations + 1) * 4);
    ChainOK &= __readfsdword(0) == Chain;
    if (Value != 7 || !ChainOK || registration_cxx_trace != 213 ||
        registration_cxx_caught != (REGISTRATION_CXX_REFERENCE ? 18u : 7u))
      break;
  }
  printf("neverd-registration-cxx: value=%d caller=%08x entry=%08x "
         "chain=%d iterations=%u trace=%u caught=%u\n",
         Value, registration_cxx_caller, (unsigned)Probe, ChainOK, Iterations,
         registration_cxx_trace, registration_cxx_caught);
  return Value == 7 && ChainOK && Iterations == 4 ? 0 : 1;
}
