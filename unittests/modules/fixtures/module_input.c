// File/symbol fixture only. No host execution or OS services.
#ifndef MODULE_VALUE
#define MODULE_VALUE 42
#endif
__declspec(dllexport) unsigned module_global = MODULE_VALUE;
__declspec(dllexport) unsigned module_probe(void) { return module_global + 1; }
