// Parameters through which standard C and POSIX functions take a function:
// the function name without leading underscores, the zero-based parameter
// position and the C type the function's header declares for it.  Included
// by LibCCallTraits.cpp with LIBC_FUNCTION_POINTER_PARAM defined.

// stdlib.h
LIBC_FUNCTION_POINTER_PARAM("atexit", 0, "void (*)(void)")
LIBC_FUNCTION_POINTER_PARAM("at_quick_exit", 0, "void (*)(void)")
LIBC_FUNCTION_POINTER_PARAM("on_exit", 0, "void (*)(int, void *)")
LIBC_FUNCTION_POINTER_PARAM("qsort", 3, "int (*)(const void *, const void *)")
LIBC_FUNCTION_POINTER_PARAM("qsort_r", 3,
                            "int (*)(const void *, const void *, void *)")
LIBC_FUNCTION_POINTER_PARAM("bsearch", 4, "int (*)(const void *, const void *)")

// signal.h
LIBC_FUNCTION_POINTER_PARAM("signal", 1, "void (*)(int)")

// pthread.h
LIBC_FUNCTION_POINTER_PARAM("pthread_create", 2, "void *(*)(void *)")
LIBC_FUNCTION_POINTER_PARAM("pthread_once", 1, "void (*)(void)")
LIBC_FUNCTION_POINTER_PARAM("pthread_key_create", 1, "void (*)(void *)")
LIBC_FUNCTION_POINTER_PARAM("pthread_atfork", 0, "void (*)(void)")
LIBC_FUNCTION_POINTER_PARAM("pthread_atfork", 1, "void (*)(void)")
LIBC_FUNCTION_POINTER_PARAM("pthread_atfork", 2, "void (*)(void)")

// threads.h
LIBC_FUNCTION_POINTER_PARAM("thrd_create", 1, "int (*)(void *)")
LIBC_FUNCTION_POINTER_PARAM("call_once", 1, "void (*)(void)")
LIBC_FUNCTION_POINTER_PARAM("tss_create", 1, "void (*)(void *)")

// search.h
LIBC_FUNCTION_POINTER_PARAM("tsearch", 2, "int (*)(const void *, const void *)")
LIBC_FUNCTION_POINTER_PARAM("tfind", 2, "int (*)(const void *, const void *)")
LIBC_FUNCTION_POINTER_PARAM("tdelete", 2, "int (*)(const void *, const void *)")
LIBC_FUNCTION_POINTER_PARAM("twalk", 1, "void (*)(const void *, VISIT, int)")
LIBC_FUNCTION_POINTER_PARAM("tdestroy", 1, "void (*)(void *)")
LIBC_FUNCTION_POINTER_PARAM("lfind", 4, "int (*)(const void *, const void *)")
LIBC_FUNCTION_POINTER_PARAM("lsearch", 4, "int (*)(const void *, const void *)")

// dirent.h
LIBC_FUNCTION_POINTER_PARAM("scandir", 2, "int (*)(const struct dirent *)")
LIBC_FUNCTION_POINTER_PARAM(
    "scandir", 3, "int (*)(const struct dirent **, const struct dirent **)")

// link.h
LIBC_FUNCTION_POINTER_PARAM("dl_iterate_phdr", 0,
                            "int (*)(struct dl_phdr_info *, size_t, void *)")

// ucontext.h
LIBC_FUNCTION_POINTER_PARAM("makecontext", 1, "void (*)(void)")
