// clang-format off: this include contains x86 assembly.
  .long REGISTRATION_SENTINEL,.Lfilter,.Lhandler
#if REGISTRATION_CASE == 1 || REGISTRATION_CASE == 4
  .long 0,0,.Lfinally
#elif REGISTRATION_CASE == 2
  .long 0,.Linnerfilter,.Linnerhandler
#endif
  .long 0,0,0
