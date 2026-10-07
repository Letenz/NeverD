// Microsoft runtime and Windows functions that never return to their caller.
// Included by LibCCallTraits.cpp with LIBC_NO_RETURN_SYMBOL defined; names
// are written without their leading underscores.

LIBC_NO_RETURN_SYMBOL("CxxThrowException")
LIBC_NO_RETURN_SYMBOL("report_gsfailure")
LIBC_NO_RETURN_SYMBOL("raise_securityfailure")
LIBC_NO_RETURN_SYMBOL("TerminateProcess")
// Windows kernel routines the WDK declares DECLSPEC_NORETURN.  Routines that
// can return (RtlRaiseException when the exception is continued,
// PsTerminateSystemThread on failure) are deliberately absent.
LIBC_NO_RETURN_SYMBOL("KeBugCheck")
LIBC_NO_RETURN_SYMBOL("KeBugCheckEx")
LIBC_NO_RETURN_SYMBOL("ExRaiseStatus")
LIBC_NO_RETURN_SYMBOL("ExRaiseAccessViolation")
LIBC_NO_RETURN_SYMBOL("ExRaiseDatatypeMisalignment")
LIBC_NO_RETURN_SYMBOL("RtlRaiseStatus")
