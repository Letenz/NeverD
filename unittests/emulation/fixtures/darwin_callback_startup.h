//===- darwin_callback_startup.h - Original startup/TLS workload
//---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file SDK-free dependency order, startup ABI and TLS callback controls.
typedef unsigned long U64;
struct ProgramVars {
  const void *Header;
  int *Argc;
  char ***Argv;
  char ***Envp;
  char **Progname;
};
extern long write(int, const void *, U64);
extern int getpid(void);
extern int Order, Errors;
static int equal(const char *A, const char *B) {
  for (unsigned I = 0; I < 128; ++I) {
    if (A[I] != B[I])
      return 0;
    if (!A[I])
      return 1;
  }
  return 0;
}
static void startup(int Argc, char **Argv, char **Envp, char **Apple,
                    struct ProgramVars *Vars) {
  if (Argc != 2 || !Argv || !Argv[0] || !Argv[1] || Argv[2] || !Envp ||
      !Apple || !Apple[0] || !Vars || !Vars->Header ||
      *(const unsigned *)Vars->Header != 0xfeedfacf || !Vars->Argc ||
      *Vars->Argc != Argc || !Vars->Argv || *Vars->Argv != Argv ||
      !Vars->Envp || *Vars->Envp != Envp || !Vars->Progname ||
      !*Vars->Progname) {
    Errors += 1;
    return;
  }
  if (!equal(*Vars->Progname, "main"))
    Errors += 2;
  int Found = 0;
  for (unsigned I = 0; I < 256 && Envp[I]; ++I)
    Found |= equal(Envp[I], "NEVERD_CALLBACK_SCOPE=one");
  if (!Found)
    Errors += 4;
}
