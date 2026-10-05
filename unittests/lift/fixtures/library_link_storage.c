/* Storage required by the MSVC thread-init relocations in analysis fixtures.
 * These DLLs are never executed and no library call is implemented here. */
__declspec(thread) int _Init_thread_epoch;
unsigned long _tls_index;
