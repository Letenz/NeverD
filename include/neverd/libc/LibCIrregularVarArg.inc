// Irregular variadic functions whose fixed parameters no name suffix gives:
// the ABI category (VarArgFixedParamKind) of each fixed parameter the
// recovery pass can prove, one per parameter.
// Included with LIBC_IRREGULAR_VARARG(Name, Kinds...).

LIBC_IRREGULAR_VARARG("syslog", Integer)
LIBC_IRREGULAR_VARARG("err", Integer)
LIBC_IRREGULAR_VARARG("errx", Integer)
LIBC_IRREGULAR_VARARG("warn", Pointer)
LIBC_IRREGULAR_VARARG("warnx", Pointer)
LIBC_IRREGULAR_VARARG("open", Pointer, Integer)
LIBC_IRREGULAR_VARARG("openat", Integer)
LIBC_IRREGULAR_VARARG("fcntl", Integer)
LIBC_IRREGULAR_VARARG("ioctl", Integer)
LIBC_IRREGULAR_VARARG("execl", Pointer)
LIBC_IRREGULAR_VARARG("execlp", Pointer)
LIBC_IRREGULAR_VARARG("execle", Pointer)
LIBC_IRREGULAR_VARARG("mq_open", Pointer)
LIBC_IRREGULAR_VARARG("sem_open", Pointer)
LIBC_IRREGULAR_VARARG("prctl", Integer)
LIBC_IRREGULAR_VARARG("syscall", Integer)
