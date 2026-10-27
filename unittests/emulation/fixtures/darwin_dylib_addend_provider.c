// Original portable provider call with an admitted libSystem import.
extern int getpid(void);
int provider_probe(void) { return getpid() > 0 ? 23 : 0; }
