// The Linux types and prototypes for ARM64 (LP64), as a program declares them
// with musl's headers and _GNU_SOURCE.  Generated on 2026-10-18 by
// scripts/generate_linux_types.py --target musl-arm64 from the headers of musl
// 1.2.5: changed from them, as NeverD reads and renders their declarations
// again.  The headers' notices are in LICENSES/musl-headers.txt.  Do not edit;
// generate it again.
// 331 types, 1354 prototypes, rendered by NeverD.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef NEVERD_GNU_STRUCT
#if defined(__MINGW32__) && (defined(__i386__) || defined(__x86_64__))
#define NEVERD_GNU_STRUCT __attribute__((gcc_struct))
#else
#define NEVERD_GNU_STRUCT
#endif
#endif

#if !defined(__cplusplus) && !defined(static_assert)
#define static_assert _Static_assert
#endif

static_assert(sizeof(long) == 8, "these types need a 64-bit long");

struct _IO_FILE;
union _G_fpos64_t;
struct _IO_cookie_io_functions_t;
typedef struct div_t div_t;
typedef struct ldiv_t ldiv_t;
typedef struct lldiv_t lldiv_t;
struct __locale_struct;
struct iovec;
struct flock;
struct file_handle;
struct f_owner_ex;
struct __mbstate_t;
struct tm;
struct lconv;
struct timespec;
struct itimerspec;
struct sigevent;
struct __pthread;
struct __sigset_t;
typedef struct pthread_attr_t pthread_attr_t;
struct sigaltstack;
typedef struct fpregset_t fpregset_t;
struct sigcontext;
struct _aarch64_ctx;
struct fpsimd_context;
struct esr_context;
struct extra_context;
struct sve_context;
struct ucontext;
union sigval;
typedef struct siginfo_t siginfo_t;
struct sigaction;
struct __jmp_buf_tag;
typedef struct fenv_t fenv_t;
typedef struct imaxdiv_t imaxdiv_t;
struct dirent;
struct __dirstream;
typedef struct Dl_info Dl_info;
typedef struct pthread_mutexattr_t pthread_mutexattr_t;
typedef struct pthread_condattr_t pthread_condattr_t;
typedef struct pthread_barrierattr_t pthread_barrierattr_t;
typedef struct pthread_rwlockattr_t pthread_rwlockattr_t;
typedef struct pthread_mutex_t pthread_mutex_t;
typedef struct pthread_cond_t pthread_cond_t;
typedef struct pthread_rwlock_t pthread_rwlock_t;
typedef struct pthread_barrier_t pthread_barrier_t;
struct sched_param;
typedef struct cpu_set_t cpu_set_t;
struct __ptcb;
typedef struct sem_t sem_t;
struct pollfd;
struct winsize;
struct termios;
struct passwd;
struct group;
typedef struct glob_t glob_t;
struct re_pattern_buffer;
typedef struct regmatch_t regmatch_t;
struct option;
typedef struct posix_spawnattr_t posix_spawnattr_t;
typedef struct posix_spawn_file_actions_t posix_spawn_file_actions_t;
typedef struct Elf32_Ehdr Elf32_Ehdr;
typedef struct Elf64_Ehdr Elf64_Ehdr;
typedef struct Elf32_Shdr Elf32_Shdr;
typedef struct Elf64_Shdr Elf64_Shdr;
typedef struct Elf32_Chdr Elf32_Chdr;
typedef struct Elf64_Chdr Elf64_Chdr;
typedef struct Elf32_Sym Elf32_Sym;
typedef struct Elf64_Sym Elf64_Sym;
typedef struct Elf32_Syminfo Elf32_Syminfo;
typedef struct Elf64_Syminfo Elf64_Syminfo;
typedef struct Elf32_Rel Elf32_Rel;
typedef struct Elf64_Rel Elf64_Rel;
typedef struct Elf32_Rela Elf32_Rela;
typedef struct Elf64_Rela Elf64_Rela;
typedef struct Elf32_Phdr Elf32_Phdr;
typedef struct Elf64_Phdr Elf64_Phdr;
typedef struct Elf32_Dyn Elf32_Dyn;
typedef struct Elf64_Dyn Elf64_Dyn;
typedef struct Elf32_Verdef Elf32_Verdef;
typedef struct Elf64_Verdef Elf64_Verdef;
typedef struct Elf32_Verdaux Elf32_Verdaux;
typedef struct Elf64_Verdaux Elf64_Verdaux;
typedef struct Elf32_Verneed Elf32_Verneed;
typedef struct Elf64_Verneed Elf64_Verneed;
typedef struct Elf32_Vernaux Elf32_Vernaux;
typedef struct Elf64_Vernaux Elf64_Vernaux;
typedef struct Elf32_auxv_t Elf32_auxv_t;
typedef struct Elf64_auxv_t Elf64_auxv_t;
typedef struct Elf32_Nhdr Elf32_Nhdr;
typedef struct Elf64_Nhdr Elf64_Nhdr;
typedef struct Elf32_Move Elf32_Move;
typedef struct Elf64_Move Elf64_Move;
typedef union Elf32_gptab Elf32_gptab;
typedef struct Elf32_RegInfo Elf32_RegInfo;
typedef struct Elf_Options Elf_Options;
typedef struct Elf_Options_Hw Elf_Options_Hw;
typedef struct Elf32_Lib Elf32_Lib;
typedef struct Elf64_Lib Elf64_Lib;
typedef struct Elf_MIPS_ABIFlags_v0 Elf_MIPS_ABIFlags_v0;
struct dl_phdr_info;
struct link_map;
struct r_debug;
struct msghdr;
struct cmsghdr;
struct ucred;
struct mmsghdr;
struct linger;
struct sockaddr;
struct sockaddr_storage;
struct in_addr;
struct sockaddr_in;
struct in6_addr;
struct sockaddr_in6;
struct ipv6_mreq;
struct ip_opts;
struct ip_mreq;
struct ip_mreqn;
struct ip_mreq_source;
struct ip_msfilter;
struct group_req;
struct group_source_req;
struct group_filter;
struct in_pktinfo;
struct in6_pktinfo;
struct ip6_mtuinfo;
struct addrinfo;
struct netent;
struct hostent;
struct servent;
struct protoent;
struct ifaddrs;
struct entry;
struct __tab;
struct hsearch_data;
struct qelem;
struct utimbuf;
struct timeval;
struct utmpx;
struct lastlog;
typedef struct fd_set fd_set;
struct stat;
struct statx_timestamp;
struct statx;
struct statvfs;
struct itimerval;
struct timezone;
struct tms;
struct rlimit;
struct rusage;
struct sockaddr_un;
union epoll_data;
struct epoll_event;
struct inotify_event;
struct utsname;
struct prctl_mm_map;
struct __ptrace_peeksiginfo_args;
struct __ptrace_seccomp_metadata;
struct __ptrace_syscall_info;
struct __ptrace_rseq_configuration;
struct sysinfo;
struct signalfd_siginfo;
struct ipc_perm;
struct semid_ds;
struct seminfo;
struct sembuf;
struct shmid_ds;
struct shminfo;
struct shm_info;
struct msqid_ds;
struct msginfo;
struct msgbuf;
struct tcphdr;
struct tcp_info;
struct tcp_md5sig;
struct tcp_diag_md5sig;
struct tcp_repair_window;
struct tcp_zerocopy_receive;
struct if_nameindex;
struct iface;
struct ifaddr;
struct ifmap;
struct ifreq;
struct ifconf;
struct aiocb;
struct mq_attr;
struct FTW;
typedef struct wordexp_t wordexp_t;
struct spwd;

#pragma neverd_header("search.h")
// ACTION: 4 bytes
enum ACTION
{
    FIND = 0,
    ENTER = 1,
};
typedef enum ACTION ACTION;

// VISIT: 4 bytes
enum VISIT
{
    preorder = 0,
    postorder = 1,
    endorder = 2,
    leaf = 3,
};
typedef enum VISIT VISIT;

#pragma neverd_header("sys/wait.h")
// idtype_t: 4 bytes
enum idtype_t
{
    P_ALL = 0,
    P_PID = 1,
    P_PGID = 2,
    P_PIDFD = 3,
};
typedef enum idtype_t idtype_t;

#pragma neverd_header("sys/epoll.h")
// EPOLL_EVENTS: 4 bytes
enum EPOLL_EVENTS
{
    __EPOLL_DUMMY = 0,
};

#pragma neverd_header("netinet/tcp.h")
// tcp_fastopen_client_fail: 4 bytes
enum tcp_fastopen_client_fail
{
    TFO_STATUS_UNSPEC = 0,
    TFO_COOKIE_UNAVAILABLE = 1,
    TFO_DATA_NOT_ACKED = 2,
    TFO_SYN_RETRANSMITTED = 3,
};

#pragma neverd_header("stdio.h")
typedef unsigned long size_t;

#pragma neverd_header()
typedef long ptrdiff_t;

typedef __int128 __int128_t;

typedef unsigned __int128 __uint128_t;

#pragma neverd_header("stdio.h")
typedef long ssize_t;

typedef long off_t;

#pragma neverd_header()
// Declared only: its members are unknown.
struct _IO_FILE;

#pragma neverd_header("stdio.h")
typedef struct _IO_FILE FILE;

// _G_fpos64_t: 0x10 (16) bytes
union _G_fpos64_t
{
    char __opaque[16];
    long long __lldata;
    double __align;
};

typedef union _G_fpos64_t fpos_t;

typedef ssize_t cookie_read_function_t(void *, char *, size_t);

typedef ssize_t cookie_write_function_t(void *, const char *, size_t);

typedef int cookie_seek_function_t(void *, off_t *, int);

typedef int cookie_close_function_t(void *);

// _IO_cookie_io_functions_t: 0x20 (32) bytes
struct _IO_cookie_io_functions_t
{
    cookie_read_function_t *read;
    cookie_write_function_t *write;
    cookie_seek_function_t *seek;
    cookie_close_function_t *close;
};

typedef struct _IO_cookie_io_functions_t cookie_io_functions_t;

#pragma neverd_header("stdlib.h")
// div_t: 0x8 (8) bytes
struct div_t
{
    int quot;
    int rem;
};

// ldiv_t: 0x10 (16) bytes
struct ldiv_t
{
    long quot;
    long rem;
};

// lldiv_t: 0x10 (16) bytes
struct lldiv_t
{
    long long quot;
    long long rem;
};

#pragma neverd_header()
// Declared only: its members are unknown.
struct __locale_struct;

#pragma neverd_header("string.h")
typedef struct __locale_struct *locale_t;

#pragma neverd_header("unistd.h")
typedef long intptr_t;

typedef int pid_t;

typedef unsigned int uid_t;

typedef unsigned int gid_t;

typedef unsigned int useconds_t;

#pragma neverd_header("fcntl.h")
typedef unsigned int mode_t;

// iovec: 0x10 (16) bytes
struct iovec
{
    void *iov_base;
    size_t iov_len;
};

// flock: 0x20 (32) bytes
struct flock
{
    short l_type;
    short l_whence;
    off_t l_start;
    off_t l_len;
    pid_t l_pid;
};

// file_handle: 0x8 (8) bytes
struct file_handle
{
    unsigned int handle_bytes;
    int handle_type;
    unsigned char f_handle[];
};

// f_owner_ex: 0x8 (8) bytes
struct f_owner_ex
{
    int type;
    pid_t pid;
};

#pragma neverd_header("wchar.h")
typedef unsigned int wint_t;

typedef unsigned long wctype_t;

// __mbstate_t: 0x8 (8) bytes
struct __mbstate_t
{
    unsigned int __opaque1;
    unsigned int __opaque2;
};

typedef struct __mbstate_t mbstate_t;

#pragma neverd_header("time.h")
// tm: 0x38 (56) bytes
struct tm
{
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};

#pragma neverd_header("wctype.h")
typedef const int *wctrans_t;

#pragma neverd_header("locale.h")
// lconv: 0x60 (96) bytes
struct lconv
{
    char *decimal_point;
    char *thousands_sep;
    char *grouping;
    char *int_curr_symbol;
    char *currency_symbol;
    char *mon_decimal_point;
    char *mon_thousands_sep;
    char *mon_grouping;
    char *positive_sign;
    char *negative_sign;
    char int_frac_digits;
    char frac_digits;
    char p_cs_precedes;
    char p_sep_by_space;
    char n_cs_precedes;
    char n_sep_by_space;
    char p_sign_posn;
    char n_sign_posn;
    char int_p_cs_precedes;
    char int_p_sep_by_space;
    char int_n_cs_precedes;
    char int_n_sep_by_space;
    char int_p_sign_posn;
    char int_n_sign_posn;
};

#pragma neverd_header("time.h")
typedef long time_t;

typedef void *timer_t;

typedef int clockid_t;

typedef long clock_t;

// timespec: 0x10 (16) bytes
struct timespec
{
    time_t tv_sec;
    long tv_nsec;
};

// itimerspec: 0x20 (32) bytes
struct itimerspec
{
    struct timespec it_interval;
    struct timespec it_value;
};

#pragma neverd_header("signal.h")
// sigval: 0x8 (8) bytes
union sigval
{
    int sival_int;
    void *sival_ptr;
};

// sigevent: 0x40 (64) bytes
struct sigevent
{
    union sigval sigev_value;
    int sigev_signo;
    int sigev_notify;
    union
    {
        char __pad[48];
        pid_t sigev_notify_thread_id;
        struct
        {
            void (*sigev_notify_function)(union sigval);
            pthread_attr_t *sigev_notify_attributes;
        } __sev_thread;
    } __sev_fields;
};

#pragma neverd_header()
// Declared only: its members are unknown.
struct __pthread;

#pragma neverd_header("signal.h")
typedef struct __pthread *pthread_t;

// __sigset_t: 0x80 (128) bytes
struct __sigset_t
{
    unsigned long __bits[16];
};

typedef struct __sigset_t sigset_t;

// pthread_attr_t: 0x38 (56) bytes
struct pthread_attr_t
{
    union
    {
        int __i[14];
        volatile int __vi[14];
        unsigned long __s[7];
    } __u;
};

// sigaltstack: 0x18 (24) bytes
struct sigaltstack
{
    void *ss_sp;
    int ss_flags;
    size_t ss_size;
};

typedef struct sigaltstack stack_t;

typedef unsigned long greg_t;

typedef unsigned long gregset_t[34];

// fpregset_t: 0x210 (528) bytes
struct fpregset_t
{
    __uint128_t vregs[32];
    unsigned int fpsr;
    unsigned int fpcr;
};

// sigcontext: 0x1120 (4384) bytes
struct sigcontext
{
    unsigned long fault_address;
    unsigned long regs[31];
    unsigned long sp;
    unsigned long pc;
    unsigned long pstate;
    long double __reserved[256];
};

typedef struct sigcontext mcontext_t;

// _aarch64_ctx: 0x8 (8) bytes
struct _aarch64_ctx
{
    unsigned int magic;
    unsigned int size;
};

// fpsimd_context: 0x210 (528) bytes
struct fpsimd_context
{
    struct _aarch64_ctx head;
    unsigned int fpsr;
    unsigned int fpcr;
    __uint128_t vregs[32];
};

// esr_context: 0x10 (16) bytes
struct esr_context
{
    struct _aarch64_ctx head;
    unsigned long esr;
};

// extra_context: 0x20 (32) bytes
struct extra_context
{
    struct _aarch64_ctx head;
    unsigned long datap;
    unsigned int size;
    unsigned int __reserved[3];
};

// sve_context: 0x10 (16) bytes
struct sve_context
{
    struct _aarch64_ctx head;
    unsigned short vl;
    unsigned short __reserved[3];
};

// ucontext: 0x11D0 (4560) bytes
struct ucontext
{
    unsigned long uc_flags;
    struct ucontext *uc_link;
    stack_t uc_stack;
    sigset_t uc_sigmask;
    mcontext_t uc_mcontext;
};

typedef struct ucontext ucontext_t;

// siginfo_t: 0x80 (128) bytes
struct siginfo_t
{
    int si_signo;
    int si_errno;
    int si_code;
    union
    {
        char __pad[112];
        struct
        {
            union
            {
                struct
                {
                    pid_t si_pid;
                    uid_t si_uid;
                } __piduid;
                struct
                {
                    int si_timerid;
                    int si_overrun;
                } __timer;
            } __first;
            union
            {
                union sigval si_value;
                struct
                {
                    int si_status;
                    clock_t si_utime;
                    clock_t si_stime;
                } __sigchld;
            } __second;
        } __si_common;
        struct
        {
            void *si_addr;
            short si_addr_lsb;
            union
            {
                struct
                {
                    void *si_lower;
                    void *si_upper;
                } __addr_bnd;
                unsigned int si_pkey;
            } __first;
        } __sigfault;
        struct
        {
            long si_band;
            int si_fd;
        } __sigpoll;
        struct
        {
            void *si_call_addr;
            int si_syscall;
            unsigned int si_arch;
        } __sigsys;
    } __si_fields;
};

// sigaction: 0x98 (152) bytes
struct sigaction
{
    union
    {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t *, void *);
    } __sa_handler;
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};

typedef void (*sig_t)(int);

typedef void (*sighandler_t)(int);

typedef int sig_atomic_t;

#pragma neverd_header("setjmp.h")
typedef unsigned long __jmp_buf[22];

// __jmp_buf_tag: 0x138 (312) bytes
struct __jmp_buf_tag
{
    __jmp_buf __jb;
    unsigned long __fl;
    unsigned long __ss[16];
};

typedef struct __jmp_buf_tag jmp_buf[1];

typedef jmp_buf sigjmp_buf;

#pragma neverd_header("math.h")
typedef float float_t;

typedef double double_t;

#pragma neverd_header("fenv.h")
typedef unsigned int fexcept_t;

// fenv_t: 0x8 (8) bytes
struct fenv_t
{
    unsigned int __fpcr;
    unsigned int __fpsr;
};

#pragma neverd_header("inttypes.h")
typedef unsigned long uintptr_t;

typedef long intmax_t;

typedef unsigned long uintmax_t;

typedef signed char int_fast8_t;

typedef long long int_fast64_t;

typedef signed char int_least8_t;

typedef short int_least16_t;

typedef int int_least32_t;

typedef long long int_least64_t;

typedef unsigned char uint_fast8_t;

typedef unsigned long long uint_fast64_t;

typedef unsigned char uint_least8_t;

typedef unsigned short uint_least16_t;

typedef unsigned int uint_least32_t;

typedef unsigned long long uint_least64_t;

typedef int int_fast16_t;

typedef int int_fast32_t;

typedef unsigned int uint_fast16_t;

typedef unsigned int uint_fast32_t;

// imaxdiv_t: 0x10 (16) bytes
struct imaxdiv_t
{
    intmax_t quot;
    intmax_t rem;
};

#pragma neverd_header("dirent.h")
typedef unsigned long ino_t;

// dirent: 0x118 (280) bytes
struct dirent
{
    ino_t d_ino;
    off_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[256];
};

#pragma neverd_header()
// Declared only: its members are unknown.
struct __dirstream;

#pragma neverd_header("dirent.h")
typedef struct __dirstream DIR;

#pragma neverd_header("dlfcn.h")
// Dl_info: 0x20 (32) bytes
struct Dl_info
{
    const char *dli_fname;
    void *dli_fbase;
    const char *dli_sname;
    void *dli_saddr;
};

#pragma neverd_header("pthread.h")
typedef int pthread_once_t;

typedef unsigned int pthread_key_t;

typedef int pthread_spinlock_t;

// pthread_mutexattr_t: 0x4 (4) bytes
struct pthread_mutexattr_t
{
    unsigned int __attr;
};

// pthread_condattr_t: 0x4 (4) bytes
struct pthread_condattr_t
{
    unsigned int __attr;
};

// pthread_barrierattr_t: 0x4 (4) bytes
struct pthread_barrierattr_t
{
    unsigned int __attr;
};

// pthread_rwlockattr_t: 0x8 (8) bytes
struct pthread_rwlockattr_t
{
    unsigned int __attr[2];
};

// pthread_mutex_t: 0x28 (40) bytes
struct pthread_mutex_t
{
    union
    {
        int __i[10];
        volatile int __vi[10];
        volatile void *volatile __p[5];
    } __u;
};

// pthread_cond_t: 0x30 (48) bytes
struct pthread_cond_t
{
    union
    {
        int __i[12];
        volatile int __vi[12];
        void *__p[6];
    } __u;
};

// pthread_rwlock_t: 0x38 (56) bytes
struct pthread_rwlock_t
{
    union
    {
        int __i[14];
        volatile int __vi[14];
        void *__p[7];
    } __u;
};

// pthread_barrier_t: 0x20 (32) bytes
struct pthread_barrier_t
{
    union
    {
        int __i[8];
        volatile int __vi[8];
        void *__p[4];
    } __u;
};

// sched_param: 0x30 (48) bytes
struct sched_param
{
    int sched_priority;
    int __reserved1;
    struct
    {
        time_t __reserved1;
        long __reserved2;
    } __reserved2[2];
    int __reserved3;
};

// cpu_set_t: 0x80 (128) bytes
struct cpu_set_t
{
    unsigned long __bits[16];
};

// __ptcb: 0x18 (24) bytes
struct __ptcb
{
    void (*__f)(void *);
    void *__x;
    struct __ptcb *__next;
};

#pragma neverd_header("semaphore.h")
// sem_t: 0x20 (32) bytes
struct sem_t
{
    volatile int __val[8];
};

#pragma neverd_header("poll.h")
typedef unsigned long nfds_t;

// pollfd: 0x8 (8) bytes
struct pollfd
{
    int fd;
    short events;
    short revents;
};

#pragma neverd_header("termios.h")
// winsize: 0x8 (8) bytes
struct winsize
{
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

typedef unsigned char cc_t;

typedef unsigned int speed_t;

typedef unsigned int tcflag_t;

// termios: 0x3C (60) bytes
struct termios
{
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_line;
    cc_t c_cc[32];
    speed_t __c_ispeed;
    speed_t __c_ospeed;
};

#pragma neverd_header("pwd.h")
// passwd: 0x30 (48) bytes
struct passwd
{
    char *pw_name;
    char *pw_passwd;
    uid_t pw_uid;
    gid_t pw_gid;
    char *pw_gecos;
    char *pw_dir;
    char *pw_shell;
};

#pragma neverd_header("grp.h")
// group: 0x20 (32) bytes
struct group
{
    char *gr_name;
    char *gr_passwd;
    gid_t gr_gid;
    char **gr_mem;
};

#pragma neverd_header("glob.h")
// glob_t: 0x48 (72) bytes
struct glob_t
{
    size_t gl_pathc;
    char **gl_pathv;
    size_t gl_offs;
    int __dummy1;
    void *__dummy2[5];
};

#pragma neverd_header("regex.h")
typedef long regoff_t;

// re_pattern_buffer: 0x40 (64) bytes
struct re_pattern_buffer
{
    size_t re_nsub;
    void *__opaque;
    void *__padding[4];
    size_t __nsub2;
    char __padding2;
};

typedef struct re_pattern_buffer regex_t;

// regmatch_t: 0x10 (16) bytes
struct regmatch_t
{
    regoff_t rm_so;
    regoff_t rm_eo;
};

#pragma neverd_header("getopt.h")
// option: 0x20 (32) bytes
struct option
{
    const char *name;
    int has_arg;
    int *flag;
    int val;
};

#pragma neverd_header("spawn.h")
// posix_spawnattr_t: 0x150 (336) bytes
struct posix_spawnattr_t
{
    int __flags;
    pid_t __pgrp;
    sigset_t __def;
    sigset_t __mask;
    int __prio;
    int __pol;
    void *__fn;
    char __pad[56];
};

// posix_spawn_file_actions_t: 0x50 (80) bytes
struct posix_spawn_file_actions_t
{
    int __pad0[2];
    void *__actions;
    int __pad[16];
};

#pragma neverd_header("iconv.h")
typedef void *iconv_t;

#pragma neverd_header("langinfo.h")
typedef int nl_item;

typedef void *nl_catd;

#pragma neverd_header("link.h")
typedef unsigned short Elf32_Half;

typedef unsigned short Elf64_Half;

typedef unsigned int Elf32_Word;

typedef int Elf32_Sword;

typedef unsigned int Elf64_Word;

typedef int Elf64_Sword;

typedef unsigned long long Elf32_Xword;

typedef long long Elf32_Sxword;

typedef unsigned long long Elf64_Xword;

typedef long long Elf64_Sxword;

typedef unsigned int Elf32_Addr;

typedef unsigned long long Elf64_Addr;

typedef unsigned int Elf32_Off;

typedef unsigned long long Elf64_Off;

typedef unsigned short Elf32_Section;

typedef unsigned short Elf64_Section;

typedef Elf32_Half Elf32_Versym;

typedef Elf64_Half Elf64_Versym;

// Elf32_Ehdr: 0x34 (52) bytes
struct Elf32_Ehdr
{
    unsigned char e_ident[16];
    Elf32_Half e_type;
    Elf32_Half e_machine;
    Elf32_Word e_version;
    Elf32_Addr e_entry;
    Elf32_Off e_phoff;
    Elf32_Off e_shoff;
    Elf32_Word e_flags;
    Elf32_Half e_ehsize;
    Elf32_Half e_phentsize;
    Elf32_Half e_phnum;
    Elf32_Half e_shentsize;
    Elf32_Half e_shnum;
    Elf32_Half e_shstrndx;
};

// Elf64_Ehdr: 0x40 (64) bytes
struct Elf64_Ehdr
{
    unsigned char e_ident[16];
    Elf64_Half e_type;
    Elf64_Half e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off e_phoff;
    Elf64_Off e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize;
    Elf64_Half e_phentsize;
    Elf64_Half e_phnum;
    Elf64_Half e_shentsize;
    Elf64_Half e_shnum;
    Elf64_Half e_shstrndx;
};

// Elf32_Shdr: 0x28 (40) bytes
struct Elf32_Shdr
{
    Elf32_Word sh_name;
    Elf32_Word sh_type;
    Elf32_Word sh_flags;
    Elf32_Addr sh_addr;
    Elf32_Off sh_offset;
    Elf32_Word sh_size;
    Elf32_Word sh_link;
    Elf32_Word sh_info;
    Elf32_Word sh_addralign;
    Elf32_Word sh_entsize;
};

// Elf64_Shdr: 0x40 (64) bytes
struct Elf64_Shdr
{
    Elf64_Word sh_name;
    Elf64_Word sh_type;
    Elf64_Xword sh_flags;
    Elf64_Addr sh_addr;
    Elf64_Off sh_offset;
    Elf64_Xword sh_size;
    Elf64_Word sh_link;
    Elf64_Word sh_info;
    Elf64_Xword sh_addralign;
    Elf64_Xword sh_entsize;
};

// Elf32_Chdr: 0xC (12) bytes
struct Elf32_Chdr
{
    Elf32_Word ch_type;
    Elf32_Word ch_size;
    Elf32_Word ch_addralign;
};

// Elf64_Chdr: 0x18 (24) bytes
struct Elf64_Chdr
{
    Elf64_Word ch_type;
    Elf64_Word ch_reserved;
    Elf64_Xword ch_size;
    Elf64_Xword ch_addralign;
};

// Elf32_Sym: 0x10 (16) bytes
struct Elf32_Sym
{
    Elf32_Word st_name;
    Elf32_Addr st_value;
    Elf32_Word st_size;
    unsigned char st_info;
    unsigned char st_other;
    Elf32_Section st_shndx;
};

// Elf64_Sym: 0x18 (24) bytes
struct Elf64_Sym
{
    Elf64_Word st_name;
    unsigned char st_info;
    unsigned char st_other;
    Elf64_Section st_shndx;
    Elf64_Addr st_value;
    Elf64_Xword st_size;
};

// Elf32_Syminfo: 0x4 (4) bytes
struct Elf32_Syminfo
{
    Elf32_Half si_boundto;
    Elf32_Half si_flags;
};

// Elf64_Syminfo: 0x4 (4) bytes
struct Elf64_Syminfo
{
    Elf64_Half si_boundto;
    Elf64_Half si_flags;
};

// Elf32_Rel: 0x8 (8) bytes
struct Elf32_Rel
{
    Elf32_Addr r_offset;
    Elf32_Word r_info;
};

// Elf64_Rel: 0x10 (16) bytes
struct Elf64_Rel
{
    Elf64_Addr r_offset;
    Elf64_Xword r_info;
};

// Elf32_Rela: 0xC (12) bytes
struct Elf32_Rela
{
    Elf32_Addr r_offset;
    Elf32_Word r_info;
    Elf32_Sword r_addend;
};

// Elf64_Rela: 0x18 (24) bytes
struct Elf64_Rela
{
    Elf64_Addr r_offset;
    Elf64_Xword r_info;
    Elf64_Sxword r_addend;
};

typedef Elf32_Word Elf32_Relr;

typedef Elf64_Xword Elf64_Relr;

// Elf32_Phdr: 0x20 (32) bytes
struct Elf32_Phdr
{
    Elf32_Word p_type;
    Elf32_Off p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
};

// Elf64_Phdr: 0x38 (56) bytes
struct Elf64_Phdr
{
    Elf64_Word p_type;
    Elf64_Word p_flags;
    Elf64_Off p_offset;
    Elf64_Addr p_vaddr;
    Elf64_Addr p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
};

// Elf32_Dyn: 0x8 (8) bytes
struct Elf32_Dyn
{
    Elf32_Sword d_tag;
    union
    {
        Elf32_Word d_val;
        Elf32_Addr d_ptr;
    } d_un;
};

// Elf64_Dyn: 0x10 (16) bytes
struct Elf64_Dyn
{
    Elf64_Sxword d_tag;
    union
    {
        Elf64_Xword d_val;
        Elf64_Addr d_ptr;
    } d_un;
};

// Elf32_Verdef: 0x14 (20) bytes
struct Elf32_Verdef
{
    Elf32_Half vd_version;
    Elf32_Half vd_flags;
    Elf32_Half vd_ndx;
    Elf32_Half vd_cnt;
    Elf32_Word vd_hash;
    Elf32_Word vd_aux;
    Elf32_Word vd_next;
};

// Elf64_Verdef: 0x14 (20) bytes
struct Elf64_Verdef
{
    Elf64_Half vd_version;
    Elf64_Half vd_flags;
    Elf64_Half vd_ndx;
    Elf64_Half vd_cnt;
    Elf64_Word vd_hash;
    Elf64_Word vd_aux;
    Elf64_Word vd_next;
};

// Elf32_Verdaux: 0x8 (8) bytes
struct Elf32_Verdaux
{
    Elf32_Word vda_name;
    Elf32_Word vda_next;
};

// Elf64_Verdaux: 0x8 (8) bytes
struct Elf64_Verdaux
{
    Elf64_Word vda_name;
    Elf64_Word vda_next;
};

// Elf32_Verneed: 0x10 (16) bytes
struct Elf32_Verneed
{
    Elf32_Half vn_version;
    Elf32_Half vn_cnt;
    Elf32_Word vn_file;
    Elf32_Word vn_aux;
    Elf32_Word vn_next;
};

// Elf64_Verneed: 0x10 (16) bytes
struct Elf64_Verneed
{
    Elf64_Half vn_version;
    Elf64_Half vn_cnt;
    Elf64_Word vn_file;
    Elf64_Word vn_aux;
    Elf64_Word vn_next;
};

// Elf32_Vernaux: 0x10 (16) bytes
struct Elf32_Vernaux
{
    Elf32_Word vna_hash;
    Elf32_Half vna_flags;
    Elf32_Half vna_other;
    Elf32_Word vna_name;
    Elf32_Word vna_next;
};

// Elf64_Vernaux: 0x10 (16) bytes
struct Elf64_Vernaux
{
    Elf64_Word vna_hash;
    Elf64_Half vna_flags;
    Elf64_Half vna_other;
    Elf64_Word vna_name;
    Elf64_Word vna_next;
};

// Elf32_auxv_t: 0x8 (8) bytes
struct Elf32_auxv_t
{
    unsigned int a_type;
    union
    {
        unsigned int a_val;
    } a_un;
};

// Elf64_auxv_t: 0x10 (16) bytes
struct Elf64_auxv_t
{
    unsigned long long a_type;
    union
    {
        unsigned long long a_val;
    } a_un;
};

// Elf32_Nhdr: 0xC (12) bytes
struct Elf32_Nhdr
{
    Elf32_Word n_namesz;
    Elf32_Word n_descsz;
    Elf32_Word n_type;
};

// Elf64_Nhdr: 0xC (12) bytes
struct Elf64_Nhdr
{
    Elf64_Word n_namesz;
    Elf64_Word n_descsz;
    Elf64_Word n_type;
};

// Elf32_Move: 0x18 (24) bytes
struct Elf32_Move
{
    Elf32_Xword m_value;
    Elf32_Word m_info;
    Elf32_Word m_poffset;
    Elf32_Half m_repeat;
    Elf32_Half m_stride;
};

// Elf64_Move: 0x20 (32) bytes
struct Elf64_Move
{
    Elf64_Xword m_value;
    Elf64_Xword m_info;
    Elf64_Xword m_poffset;
    Elf64_Half m_repeat;
    Elf64_Half m_stride;
};

// Elf32_gptab: 0x8 (8) bytes
union Elf32_gptab
{
    struct
    {
        Elf32_Word gt_current_g_value;
        Elf32_Word gt_unused;
    } gt_header;
    struct
    {
        Elf32_Word gt_g_value;
        Elf32_Word gt_bytes;
    } gt_entry;
};

// Elf32_RegInfo: 0x18 (24) bytes
struct Elf32_RegInfo
{
    Elf32_Word ri_gprmask;
    Elf32_Word ri_cprmask[4];
    Elf32_Sword ri_gp_value;
};

// Elf_Options: 0x8 (8) bytes
struct Elf_Options
{
    unsigned char kind;
    unsigned char size;
    Elf32_Section section;
    Elf32_Word info;
};

// Elf_Options_Hw: 0x8 (8) bytes
struct Elf_Options_Hw
{
    Elf32_Word hwp_flags1;
    Elf32_Word hwp_flags2;
};

// Elf32_Lib: 0x14 (20) bytes
struct Elf32_Lib
{
    Elf32_Word l_name;
    Elf32_Word l_time_stamp;
    Elf32_Word l_checksum;
    Elf32_Word l_version;
    Elf32_Word l_flags;
};

// Elf64_Lib: 0x14 (20) bytes
struct Elf64_Lib
{
    Elf64_Word l_name;
    Elf64_Word l_time_stamp;
    Elf64_Word l_checksum;
    Elf64_Word l_version;
    Elf64_Word l_flags;
};

typedef Elf32_Addr Elf32_Conflict;

// Elf_MIPS_ABIFlags_v0: 0x18 (24) bytes
struct Elf_MIPS_ABIFlags_v0
{
    Elf32_Half version;
    unsigned char isa_level;
    unsigned char isa_rev;
    unsigned char gpr_size;
    unsigned char cpr1_size;
    unsigned char cpr2_size;
    unsigned char fp_abi;
    Elf32_Word isa_ext;
    Elf32_Word ases;
    Elf32_Word flags1;
    Elf32_Word flags2;
};

typedef unsigned int Elf_Symndx;

// dl_phdr_info: 0x40 (64) bytes
struct dl_phdr_info
{
    Elf64_Addr dlpi_addr;
    const char *dlpi_name;
    const Elf64_Phdr *dlpi_phdr;
    Elf64_Half dlpi_phnum;
    unsigned long long dlpi_adds;
    unsigned long long dlpi_subs;
    size_t dlpi_tls_modid;
    void *dlpi_tls_data;
};

// link_map: 0x28 (40) bytes
struct link_map
{
    Elf64_Addr l_addr;
    char *l_name;
    Elf64_Dyn *l_ld;
    struct link_map *l_next;
    struct link_map *l_prev;
};

// r_debug: 0x28 (40) bytes
struct r_debug
{
    int r_version;
    struct link_map *r_map;
    Elf64_Addr r_brk;
    enum
    {
        RT_CONSISTENT = 0,
        RT_ADD = 1,
        RT_DELETE = 2,
    } r_state;
    Elf64_Addr r_ldbase;
};

#pragma neverd_header("netdb.h")
typedef unsigned int socklen_t;

typedef unsigned short sa_family_t;

// msghdr: 0x38 (56) bytes
struct msghdr
{
    void *msg_name;
    socklen_t msg_namelen;
    struct iovec *msg_iov;
    int msg_iovlen;
    int __pad1;
    void *msg_control;
    socklen_t msg_controllen;
    int __pad2;
    int msg_flags;
};

// cmsghdr: 0x10 (16) bytes
struct cmsghdr
{
    socklen_t cmsg_len;
    int __pad1;
    int cmsg_level;
    int cmsg_type;
};

// ucred: 0xC (12) bytes
struct ucred
{
    pid_t pid;
    uid_t uid;
    gid_t gid;
};

// mmsghdr: 0x40 (64) bytes
struct mmsghdr
{
    struct msghdr msg_hdr;
    unsigned int msg_len;
};

// linger: 0x8 (8) bytes
struct linger
{
    int l_onoff;
    int l_linger;
};

// sockaddr: 0x10 (16) bytes
struct sockaddr
{
    sa_family_t sa_family;
    char sa_data[14];
};

// sockaddr_storage: 0x80 (128) bytes
struct sockaddr_storage
{
    sa_family_t ss_family;
    char __ss_padding[118];
    unsigned long __ss_align;
};

typedef unsigned short in_port_t;

typedef unsigned int in_addr_t;

// in_addr: 0x4 (4) bytes
struct in_addr
{
    in_addr_t s_addr;
};

// sockaddr_in: 0x10 (16) bytes
struct sockaddr_in
{
    sa_family_t sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    unsigned char sin_zero[8];
};

// in6_addr: 0x10 (16) bytes
struct in6_addr
{
    union
    {
        unsigned char __s6_addr[16];
        unsigned short __s6_addr16[8];
        unsigned int __s6_addr32[4];
    } __in6_union;
};

// sockaddr_in6: 0x1C (28) bytes
struct sockaddr_in6
{
    sa_family_t sin6_family;
    in_port_t sin6_port;
    unsigned int sin6_flowinfo;
    struct in6_addr sin6_addr;
    unsigned int sin6_scope_id;
};

// ipv6_mreq: 0x14 (20) bytes
struct ipv6_mreq
{
    struct in6_addr ipv6mr_multiaddr;
    unsigned int ipv6mr_interface;
};

// ip_opts: 0x2C (44) bytes
struct ip_opts
{
    struct in_addr ip_dst;
    char ip_opts[40];
};

// ip_mreq: 0x8 (8) bytes
struct ip_mreq
{
    struct in_addr imr_multiaddr;
    struct in_addr imr_interface;
};

// ip_mreqn: 0xC (12) bytes
struct ip_mreqn
{
    struct in_addr imr_multiaddr;
    struct in_addr imr_address;
    int imr_ifindex;
};

// ip_mreq_source: 0xC (12) bytes
struct ip_mreq_source
{
    struct in_addr imr_multiaddr;
    struct in_addr imr_interface;
    struct in_addr imr_sourceaddr;
};

// ip_msfilter: 0x14 (20) bytes
struct ip_msfilter
{
    struct in_addr imsf_multiaddr;
    struct in_addr imsf_interface;
    unsigned int imsf_fmode;
    unsigned int imsf_numsrc;
    struct in_addr imsf_slist[1];
};

// group_req: 0x88 (136) bytes
struct group_req
{
    unsigned int gr_interface;
    struct sockaddr_storage gr_group;
};

// group_source_req: 0x108 (264) bytes
struct group_source_req
{
    unsigned int gsr_interface;
    struct sockaddr_storage gsr_group;
    struct sockaddr_storage gsr_source;
};

// group_filter: 0x110 (272) bytes
struct group_filter
{
    unsigned int gf_interface;
    struct sockaddr_storage gf_group;
    unsigned int gf_fmode;
    unsigned int gf_numsrc;
    struct sockaddr_storage gf_slist[1];
};

// in_pktinfo: 0xC (12) bytes
struct in_pktinfo
{
    int ipi_ifindex;
    struct in_addr ipi_spec_dst;
    struct in_addr ipi_addr;
};

// in6_pktinfo: 0x14 (20) bytes
struct in6_pktinfo
{
    struct in6_addr ipi6_addr;
    unsigned int ipi6_ifindex;
};

// ip6_mtuinfo: 0x20 (32) bytes
struct ip6_mtuinfo
{
    struct sockaddr_in6 ip6m_addr;
    unsigned int ip6m_mtu;
};

// addrinfo: 0x30 (48) bytes
struct addrinfo
{
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr *ai_addr;
    char *ai_canonname;
    struct addrinfo *ai_next;
};

// netent: 0x18 (24) bytes
struct netent
{
    char *n_name;
    char **n_aliases;
    int n_addrtype;
    unsigned int n_net;
};

// hostent: 0x20 (32) bytes
struct hostent
{
    char *h_name;
    char **h_aliases;
    int h_addrtype;
    int h_length;
    char **h_addr_list;
};

// servent: 0x20 (32) bytes
struct servent
{
    char *s_name;
    char **s_aliases;
    int s_port;
    char *s_proto;
};

// protoent: 0x18 (24) bytes
struct protoent
{
    char *p_name;
    char **p_aliases;
    int p_proto;
};

#pragma neverd_header("ifaddrs.h")
// ifaddrs: 0x38 (56) bytes
struct ifaddrs
{
    struct ifaddrs *ifa_next;
    char *ifa_name;
    unsigned int ifa_flags;
    struct sockaddr *ifa_addr;
    struct sockaddr *ifa_netmask;
    union
    {
        struct sockaddr *ifu_broadaddr;
        struct sockaddr *ifu_dstaddr;
    } ifa_ifu;
    void *ifa_data;
};

#pragma neverd_header("search.h")
// entry: 0x10 (16) bytes
struct entry
{
    char *key;
    void *data;
};

typedef struct entry ENTRY;

#pragma neverd_header()
// Declared only: its members are unknown.
struct __tab;

#pragma neverd_header("search.h")
// hsearch_data: 0x10 (16) bytes
struct hsearch_data
{
    struct __tab *__tab;
    unsigned int __unused1;
    unsigned int __unused2;
};

// qelem: 0x18 (24) bytes
struct qelem
{
    struct qelem *q_forw;
    struct qelem *q_back;
    char q_data[1];
};

#pragma neverd_header("utime.h")
// utimbuf: 0x10 (16) bytes
struct utimbuf
{
    time_t actime;
    time_t modtime;
};

#pragma neverd_header("utmp.h")
typedef long suseconds_t;

// timeval: 0x10 (16) bytes
struct timeval
{
    time_t tv_sec;
    suseconds_t tv_usec;
};

// utmpx: 0x190 (400) bytes
struct utmpx
{
    short ut_type;
    short __ut_pad1;
    pid_t ut_pid;
    char ut_line[32];
    char ut_id[4];
    char ut_user[32];
    char ut_host[256];
    struct
    {
        short __e_termination;
        short __e_exit;
    } ut_exit;
    int ut_session;
    int __ut_pad2;
    struct timeval ut_tv;
    unsigned int ut_addr_v6[4];
    char __unused[20];
};

// lastlog: 0x128 (296) bytes
struct lastlog
{
    time_t ll_time;
    char ll_line[32];
    char ll_host[256];
};

#pragma neverd_header("sys/types.h")
typedef int blksize_t;

typedef unsigned int nlink_t;

typedef long register_t;

typedef unsigned long u_int64_t;

typedef unsigned long dev_t;

typedef long blkcnt_t;

typedef unsigned long fsblkcnt_t;

typedef unsigned long fsfilcnt_t;

typedef unsigned int id_t;

typedef int key_t;

typedef unsigned char u_int8_t;

typedef unsigned short u_int16_t;

typedef unsigned int u_int32_t;

typedef char *caddr_t;

typedef unsigned char u_char;

typedef unsigned short u_short;

typedef unsigned short ushort;

typedef unsigned int u_int;

typedef unsigned int uint;

typedef unsigned long u_long;

typedef unsigned long ulong;

typedef long long quad_t;

typedef unsigned long long u_quad_t;

typedef unsigned long fd_mask;

// fd_set: 0x80 (128) bytes
struct fd_set
{
    unsigned long fds_bits[16];
};

#pragma neverd_header("sys/stat.h")
// stat: 0x80 (128) bytes
struct stat
{
    dev_t st_dev;
    ino_t st_ino;
    mode_t st_mode;
    nlink_t st_nlink;
    uid_t st_uid;
    gid_t st_gid;
    dev_t st_rdev;
    unsigned long __pad;
    off_t st_size;
    blksize_t st_blksize;
    int __pad2;
    blkcnt_t st_blocks;
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
    unsigned int __unused[2];
};

// statx_timestamp: 0x10 (16) bytes
struct statx_timestamp
{
    long long tv_sec;
    unsigned int tv_nsec;
    unsigned int __pad;
};

// statx: 0x100 (256) bytes
struct statx
{
    unsigned int stx_mask;
    unsigned int stx_blksize;
    unsigned long long stx_attributes;
    unsigned int stx_nlink;
    unsigned int stx_uid;
    unsigned int stx_gid;
    unsigned short stx_mode;
    unsigned short __pad0[1];
    unsigned long long stx_ino;
    unsigned long long stx_size;
    unsigned long long stx_blocks;
    unsigned long long stx_attributes_mask;
    struct statx_timestamp stx_atime;
    struct statx_timestamp stx_btime;
    struct statx_timestamp stx_ctime;
    struct statx_timestamp stx_mtime;
    unsigned int stx_rdev_major;
    unsigned int stx_rdev_minor;
    unsigned int stx_dev_major;
    unsigned int stx_dev_minor;
    unsigned long long __pad1[14];
};

#pragma neverd_header("sys/statvfs.h")
// statvfs: 0x70 (112) bytes
struct statvfs
{
    unsigned long f_bsize;
    unsigned long f_frsize;
    fsblkcnt_t f_blocks;
    fsblkcnt_t f_bfree;
    fsblkcnt_t f_bavail;
    fsfilcnt_t f_files;
    fsfilcnt_t f_ffree;
    fsfilcnt_t f_favail;
    unsigned long f_fsid;
    unsigned long f_flag;
    unsigned long f_namemax;
    unsigned int f_type;
    int __reserved[5];
};

#pragma neverd_header("sys/time.h")
// itimerval: 0x20 (32) bytes
struct itimerval
{
    struct timeval it_interval;
    struct timeval it_value;
};

// timezone: 0x8 (8) bytes
struct timezone
{
    int tz_minuteswest;
    int tz_dsttime;
};

#pragma neverd_header("sys/times.h")
// tms: 0x20 (32) bytes
struct tms
{
    clock_t tms_utime;
    clock_t tms_stime;
    clock_t tms_cutime;
    clock_t tms_cstime;
};

#pragma neverd_header("sys/wait.h")
typedef unsigned long long rlim_t;

// rlimit: 0x10 (16) bytes
struct rlimit
{
    rlim_t rlim_cur;
    rlim_t rlim_max;
};

// rusage: 0x110 (272) bytes
struct rusage
{
    struct timeval ru_utime;
    struct timeval ru_stime;
    long ru_maxrss;
    long ru_ixrss;
    long ru_idrss;
    long ru_isrss;
    long ru_minflt;
    long ru_majflt;
    long ru_nswap;
    long ru_inblock;
    long ru_oublock;
    long ru_msgsnd;
    long ru_msgrcv;
    long ru_nsignals;
    long ru_nvcsw;
    long ru_nivcsw;
    long __reserved[16];
};

#pragma neverd_header("sys/un.h")
// sockaddr_un: 0x6E (110) bytes
struct sockaddr_un
{
    sa_family_t sun_family;
    char sun_path[108];
};

#pragma neverd_header("sys/epoll.h")
// epoll_data: 0x8 (8) bytes
union epoll_data
{
    void *ptr;
    int fd;
    unsigned int u32;
    unsigned long long u64;
};

typedef union epoll_data epoll_data_t;

// epoll_event: 0x10 (16) bytes
struct epoll_event
{
    unsigned int events;
    epoll_data_t data;
};

#pragma neverd_header("sys/eventfd.h")
typedef unsigned long long eventfd_t;

#pragma neverd_header("sys/inotify.h")
// inotify_event: 0x10 (16) bytes
struct inotify_event
{
    int wd;
    unsigned int mask;
    unsigned int cookie;
    unsigned int len;
    char name[];
};

#pragma neverd_header("sys/utsname.h")
// utsname: 0x186 (390) bytes
struct utsname
{
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

#pragma neverd_header("sys/prctl.h")
// prctl_mm_map: 0x68 (104) bytes
struct prctl_mm_map
{
    unsigned long long start_code;
    unsigned long long end_code;
    unsigned long long start_data;
    unsigned long long end_data;
    unsigned long long start_brk;
    unsigned long long brk;
    unsigned long long start_stack;
    unsigned long long arg_start;
    unsigned long long arg_end;
    unsigned long long env_start;
    unsigned long long env_end;
    unsigned long long *auxv;
    unsigned int auxv_size;
    unsigned int exe_fd;
};

#pragma neverd_header("sys/ptrace.h")
// __ptrace_peeksiginfo_args: 0x10 (16) bytes
struct __ptrace_peeksiginfo_args
{
    unsigned long long off;
    unsigned int flags;
    int nr;
};

// __ptrace_seccomp_metadata: 0x10 (16) bytes
struct __ptrace_seccomp_metadata
{
    unsigned long long filter_off;
    unsigned long long flags;
};

// __ptrace_syscall_info: 0x58 (88) bytes
struct __ptrace_syscall_info
{
    unsigned char op;
    unsigned char __pad[3];
    unsigned int arch;
    unsigned long long instruction_pointer;
    unsigned long long stack_pointer;
    union
    {
        struct
        {
            unsigned long long nr;
            unsigned long long args[6];
        } entry;
        struct
        {
            long long rval;
            unsigned char is_error;
        } exit;
        struct
        {
            unsigned long long nr;
            unsigned long long args[6];
            unsigned int ret_data;
        } seccomp;
    };
};

// __ptrace_rseq_configuration: 0x18 (24) bytes
struct __ptrace_rseq_configuration
{
    unsigned long long rseq_abi_pointer;
    unsigned int rseq_abi_size;
    unsigned int signature;
    unsigned int flags;
    unsigned int pad;
};

#pragma neverd_header("sys/sysinfo.h")
// sysinfo: 0x170 (368) bytes
struct sysinfo
{
    unsigned long uptime;
    unsigned long loads[3];
    unsigned long totalram;
    unsigned long freeram;
    unsigned long sharedram;
    unsigned long bufferram;
    unsigned long totalswap;
    unsigned long freeswap;
    unsigned short procs;
    unsigned short pad;
    unsigned long totalhigh;
    unsigned long freehigh;
    unsigned int mem_unit;
    char __reserved[256];
};

#pragma neverd_header("sys/signalfd.h")
// signalfd_siginfo: 0x80 (128) bytes
struct signalfd_siginfo
{
    unsigned int ssi_signo;
    int ssi_errno;
    int ssi_code;
    unsigned int ssi_pid;
    unsigned int ssi_uid;
    int ssi_fd;
    unsigned int ssi_tid;
    unsigned int ssi_band;
    unsigned int ssi_overrun;
    unsigned int ssi_trapno;
    int ssi_status;
    int ssi_int;
    unsigned long long ssi_ptr;
    unsigned long long ssi_utime;
    unsigned long long ssi_stime;
    unsigned long long ssi_addr;
    unsigned short ssi_addr_lsb;
    unsigned short __pad2;
    int ssi_syscall;
    unsigned long long ssi_call_addr;
    unsigned int ssi_arch;
    unsigned char __pad[28];
};

#pragma neverd_header("sys/sem.h")
// ipc_perm: 0x30 (48) bytes
struct ipc_perm
{
    key_t key;
    uid_t uid;
    gid_t gid;
    uid_t cuid;
    gid_t cgid;
    mode_t mode;
    int seq;
    long __pad1;
    long __pad2;
};

// semid_ds: 0x58 (88) bytes
struct semid_ds
{
    struct ipc_perm sem_perm;
    time_t sem_otime;
    time_t sem_ctime;
    unsigned short sem_nsems;
    char __sem_nsems_pad[6];
    long __unused3;
    long __unused4;
};

// seminfo: 0x28 (40) bytes
struct seminfo
{
    int semmap;
    int semmni;
    int semmns;
    int semmnu;
    int semmsl;
    int semopm;
    int semume;
    int semusz;
    int semvmx;
    int semaem;
};

// sembuf: 0x6 (6) bytes
struct sembuf
{
    unsigned short sem_num;
    short sem_op;
    short sem_flg;
};

#pragma neverd_header("sys/shm.h")
// shmid_ds: 0x70 (112) bytes
struct shmid_ds
{
    struct ipc_perm shm_perm;
    size_t shm_segsz;
    time_t shm_atime;
    time_t shm_dtime;
    time_t shm_ctime;
    pid_t shm_cpid;
    pid_t shm_lpid;
    unsigned long shm_nattch;
    unsigned long __pad1;
    unsigned long __pad2;
};

// shminfo: 0x48 (72) bytes
struct shminfo
{
    unsigned long shmmax;
    unsigned long shmmin;
    unsigned long shmmni;
    unsigned long shmseg;
    unsigned long shmall;
    unsigned long __unused[4];
};

// shm_info: 0x30 (48) bytes
struct shm_info
{
    int used_ids;
    unsigned long shm_tot;
    unsigned long shm_rss;
    unsigned long shm_swp;
    unsigned long swap_attempts;
    unsigned long swap_successes;
};

typedef unsigned long shmatt_t;

#pragma neverd_header("sys/msg.h")
typedef unsigned long msgqnum_t;

typedef unsigned long msglen_t;

// msqid_ds: 0x78 (120) bytes
struct msqid_ds
{
    struct ipc_perm msg_perm;
    time_t msg_stime;
    time_t msg_rtime;
    time_t msg_ctime;
    unsigned long msg_cbytes;
    msgqnum_t msg_qnum;
    msglen_t msg_qbytes;
    pid_t msg_lspid;
    pid_t msg_lrpid;
    unsigned long __unused[2];
};

// msginfo: 0x20 (32) bytes
struct msginfo
{
    int msgpool;
    int msgmap;
    int msgmax;
    int msgmnb;
    int msgmni;
    int msgssz;
    int msgtql;
    unsigned short msgseg;
};

// msgbuf: 0x10 (16) bytes
struct msgbuf
{
    long mtype;
    char mtext[1];
};

#pragma neverd_header("netinet/tcp.h")
typedef unsigned int tcp_seq;

// tcphdr: 0x14 (20) bytes
struct tcphdr
{
    union
    {
        struct NEVERD_GNU_STRUCT
        {
            unsigned short source;
            unsigned short dest;
            unsigned int seq;
            unsigned int ack_seq;
            unsigned short res1 : 4;
            unsigned short doff : 4;
            unsigned short fin : 1;
            unsigned short syn : 1;
            unsigned short rst : 1;
            unsigned short psh : 1;
            unsigned short ack : 1;
            unsigned short urg : 1;
            unsigned short res2 : 2;
            unsigned short window;
            unsigned short check;
            unsigned short urg_ptr;
        };
        struct NEVERD_GNU_STRUCT
        {
            unsigned short th_sport;
            unsigned short th_dport;
            unsigned int th_seq;
            unsigned int th_ack;
            unsigned char th_x2 : 4;
            unsigned char th_off : 4;
            unsigned char th_flags;
            unsigned short th_win;
            unsigned short th_sum;
            unsigned short th_urp;
        };
    };
};

// tcp_info: 0xE8 (232) bytes
struct NEVERD_GNU_STRUCT tcp_info
{
    unsigned char tcpi_state;
    unsigned char tcpi_ca_state;
    unsigned char tcpi_retransmits;
    unsigned char tcpi_probes;
    unsigned char tcpi_backoff;
    unsigned char tcpi_options;
    unsigned char tcpi_snd_wscale : 4;
    unsigned char tcpi_rcv_wscale : 4;
    unsigned char tcpi_delivery_rate_app_limited : 1;
    unsigned char tcpi_fastopen_client_fail : 2;
    unsigned int tcpi_rto;
    unsigned int tcpi_ato;
    unsigned int tcpi_snd_mss;
    unsigned int tcpi_rcv_mss;
    unsigned int tcpi_unacked;
    unsigned int tcpi_sacked;
    unsigned int tcpi_lost;
    unsigned int tcpi_retrans;
    unsigned int tcpi_fackets;
    unsigned int tcpi_last_data_sent;
    unsigned int tcpi_last_ack_sent;
    unsigned int tcpi_last_data_recv;
    unsigned int tcpi_last_ack_recv;
    unsigned int tcpi_pmtu;
    unsigned int tcpi_rcv_ssthresh;
    unsigned int tcpi_rtt;
    unsigned int tcpi_rttvar;
    unsigned int tcpi_snd_ssthresh;
    unsigned int tcpi_snd_cwnd;
    unsigned int tcpi_advmss;
    unsigned int tcpi_reordering;
    unsigned int tcpi_rcv_rtt;
    unsigned int tcpi_rcv_space;
    unsigned int tcpi_total_retrans;
    unsigned long long tcpi_pacing_rate;
    unsigned long long tcpi_max_pacing_rate;
    unsigned long long tcpi_bytes_acked;
    unsigned long long tcpi_bytes_received;
    unsigned int tcpi_segs_out;
    unsigned int tcpi_segs_in;
    unsigned int tcpi_notsent_bytes;
    unsigned int tcpi_min_rtt;
    unsigned int tcpi_data_segs_in;
    unsigned int tcpi_data_segs_out;
    unsigned long long tcpi_delivery_rate;
    unsigned long long tcpi_busy_time;
    unsigned long long tcpi_rwnd_limited;
    unsigned long long tcpi_sndbuf_limited;
    unsigned int tcpi_delivered;
    unsigned int tcpi_delivered_ce;
    unsigned long long tcpi_bytes_sent;
    unsigned long long tcpi_bytes_retrans;
    unsigned int tcpi_dsack_dups;
    unsigned int tcpi_reord_seen;
    unsigned int tcpi_rcv_ooopack;
    unsigned int tcpi_snd_wnd;
};

// tcp_md5sig: 0xD8 (216) bytes
struct tcp_md5sig
{
    struct sockaddr_storage tcpm_addr;
    unsigned char tcpm_flags;
    unsigned char tcpm_prefixlen;
    unsigned short tcpm_keylen;
    int tcpm_ifindex;
    unsigned char tcpm_key[80];
};

// tcp_diag_md5sig: 0x64 (100) bytes
struct tcp_diag_md5sig
{
    unsigned char tcpm_family;
    unsigned char tcpm_prefixlen;
    unsigned short tcpm_keylen;
    unsigned int tcpm_addr[4];
    unsigned char tcpm_key[80];
};

// tcp_repair_window: 0x14 (20) bytes
struct tcp_repair_window
{
    unsigned int snd_wl1;
    unsigned int snd_wnd;
    unsigned int max_window;
    unsigned int rcv_wnd;
    unsigned int rcv_wup;
};

// tcp_zerocopy_receive: 0x40 (64) bytes
struct tcp_zerocopy_receive
{
    unsigned long long address;
    unsigned int length;
    unsigned int recv_skip_hint;
    unsigned int inq;
    int err;
    unsigned long long copybuf_address;
    int copybuf_len;
    unsigned int flags;
    unsigned long long msg_control;
    unsigned long long msg_controllen;
    unsigned int msg_flags;
    unsigned int reserved;
};

#pragma neverd_header("net/if.h")
// if_nameindex: 0x10 (16) bytes
struct if_nameindex
{
    unsigned int if_index;
    char *if_name;
};

#pragma neverd_header()
// Declared only: its members are unknown.
struct iface;

#pragma neverd_header("net/if.h")
// ifaddr: 0x30 (48) bytes
struct ifaddr
{
    struct sockaddr ifa_addr;
    union
    {
        struct sockaddr ifu_broadaddr;
        struct sockaddr ifu_dstaddr;
    } ifa_ifu;
    struct iface *ifa_ifp;
    struct ifaddr *ifa_next;
};

// ifmap: 0x18 (24) bytes
struct ifmap
{
    unsigned long mem_start;
    unsigned long mem_end;
    unsigned short base_addr;
    unsigned char irq;
    unsigned char dma;
    unsigned char port;
};

// ifreq: 0x28 (40) bytes
struct ifreq
{
    union
    {
        char ifrn_name[16];
    } ifr_ifrn;
    union
    {
        struct sockaddr ifru_addr;
        struct sockaddr ifru_dstaddr;
        struct sockaddr ifru_broadaddr;
        struct sockaddr ifru_netmask;
        struct sockaddr ifru_hwaddr;
        short ifru_flags;
        int ifru_ivalue;
        int ifru_mtu;
        struct ifmap ifru_map;
        char ifru_slave[16];
        char ifru_newname[16];
        char *ifru_data;
    } ifr_ifru;
};

// ifconf: 0x10 (16) bytes
struct ifconf
{
    int ifc_len;
    union
    {
        char *ifcu_buf;
        struct ifreq *ifcu_req;
    } ifc_ifcu;
};

#pragma neverd_header("aio.h")
// aiocb: 0xA8 (168) bytes
struct aiocb
{
    int aio_fildes;
    int aio_lio_opcode;
    int aio_reqprio;
    volatile void *aio_buf;
    size_t aio_nbytes;
    struct sigevent aio_sigevent;
    void *__td;
    int __lock[2];
    volatile int __err;
    ssize_t __ret;
    off_t aio_offset;
    void *__next;
    void *__prev;
    char __dummy4[16];
};

#pragma neverd_header("mqueue.h")
typedef int mqd_t;

// mq_attr: 0x40 (64) bytes
struct mq_attr
{
    long mq_flags;
    long mq_maxmsg;
    long mq_msgsize;
    long mq_curmsgs;
    long __unused[4];
};

#pragma neverd_header("ftw.h")
// FTW: 0x8 (8) bytes
struct FTW
{
    int base;
    int level;
};

#pragma neverd_header("wordexp.h")
// wordexp_t: 0x18 (24) bytes
struct wordexp_t
{
    size_t we_wordc;
    char **we_wordv;
    size_t we_offs;
};

#pragma neverd_header("shadow.h")
// spwd: 0x48 (72) bytes
struct spwd
{
    char *sp_namp;
    char *sp_pwdp;
    long sp_lstchg;
    long sp_min;
    long sp_max;
    long sp_warn;
    long sp_inact;
    long sp_expire;
    unsigned long sp_flag;
};


_Noreturn void _Exit(int);
pid_t _Fork(void);
void __CPU_AND_S(size_t __size, cpu_set_t *__dest, const cpu_set_t *__src1, const cpu_set_t *__src2);
void __CPU_OR_S(size_t __size, cpu_set_t *__dest, const cpu_set_t *__src1, const cpu_set_t *__src2);
void __CPU_XOR_S(size_t __size, cpu_set_t *__dest, const cpu_set_t *__src1, const cpu_set_t *__src2);
unsigned long long __DOUBLE_BITS(double __f);
unsigned int __FLOAT_BITS(float __f);
_Noreturn void __assert_fail(const char *, const char *, int, const char *);
unsigned short __bswap16(unsigned short __x);
unsigned int __bswap32(unsigned int __x);
unsigned long long __bswap64(unsigned long long __x);
size_t __ctype_get_mb_cur_max(void);
int *__errno_location(void);
int __fpclassify(double);
int __fpclassifyf(float);
int __fpclassifyl(long double);
int *__h_errno_location(void);
int __isgreater(double_t __x, double_t __y);
int __isgreaterequal(double_t __x, double_t __y);
int __isgreaterequalf(float_t __x, float_t __y);
int __isgreaterequall(long double __x, long double __y);
int __isgreaterf(float_t __x, float_t __y);
int __isgreaterl(long double __x, long double __y);
int __isless(double_t __x, double_t __y);
int __islessequal(double_t __x, double_t __y);
int __islessequalf(float_t __x, float_t __y);
int __islessequall(long double __x, long double __y);
int __islessf(float_t __x, float_t __y);
int __islessgreater(double_t __x, double_t __y);
int __islessgreaterf(float_t __x, float_t __y);
int __islessgreaterl(long double __x, long double __y);
int __islessl(long double __x, long double __y);
int __isspace(int _c);
int __libc_current_sigrtmax(void);
int __libc_current_sigrtmin(void);
int __sched_cpucount(size_t, const cpu_set_t *);
int __signbit(double);
int __signbitf(float);
int __signbitl(long double);
_Noreturn void _exit(int);
_Noreturn void _longjmp(jmp_buf, int);
void _pthread_cleanup_pop(struct __ptcb *, int);
void _pthread_cleanup_push(struct __ptcb *, void (*)(void *), void *);
int _setjmp(jmp_buf);
long a64l(const char *);
_Noreturn void abort(void);
int abs(int);
int accept(int, struct sockaddr *, socklen_t *);
int accept4(int, struct sockaddr *, socklen_t *, int);
int access(const char *, int);
int acct(const char *);
double acos(double);
float acosf(float);
double acosh(double);
float acoshf(float);
long double acoshl(long double);
long double acosl(long double);
int adjtime(const struct timeval *, struct timeval *);
int aio_cancel(int, struct aiocb *);
int aio_error(const struct aiocb *);
int aio_fsync(int, struct aiocb *);
int aio_read(struct aiocb *);
ssize_t aio_return(struct aiocb *);
int aio_suspend(const struct aiocb *const *, int, const struct timespec *);
int aio_write(struct aiocb *);
unsigned int alarm(unsigned int);
void *aligned_alloc(size_t, size_t);
void *alloca(size_t);
int alphasort(const struct dirent **, const struct dirent **);
char *asctime(const struct tm *);
char *asctime_r(const struct tm *, char *);
double asin(double);
float asinf(float);
double asinh(double);
float asinhf(float);
long double asinhl(long double);
long double asinl(long double);
int asprintf(char **, const char *, ...);
int at_quick_exit(void (*)(void));
double atan(double);
double atan2(double, double);
float atan2f(float, float);
long double atan2l(long double, long double);
float atanf(float);
double atanh(double);
float atanhf(float);
long double atanhl(long double);
long double atanl(long double);
int atexit(void (*)(void));
double atof(const char *);
int atoi(const char *);
long atol(const char *);
long long atoll(const char *);
char *basename(char *);
int bcmp(const void *, const void *, size_t);
void bcopy(const void *, void *, size_t);
int bind(int, const struct sockaddr *, socklen_t);
int brk(void *);
void (*bsd_signal(int, void (*)(int)))(int);
void *bsearch(const void *, const void *, size_t, size_t, int (*)(const void *, const void *));
wint_t btowc(int);
void bzero(void *, size_t);
void *calloc(size_t, size_t);
int catclose(nl_catd);
char *catgets(nl_catd, int, int, const char *);
nl_catd catopen(const char *, int);
double cbrt(double);
float cbrtf(float);
long double cbrtl(long double);
double ceil(double);
float ceilf(float);
long double ceill(long double);
speed_t cfgetispeed(const struct termios *);
speed_t cfgetospeed(const struct termios *);
void cfmakeraw(struct termios *);
int cfsetispeed(struct termios *, speed_t);
int cfsetospeed(struct termios *, speed_t);
int cfsetspeed(struct termios *, speed_t);
int chdir(const char *);
int chmod(const char *, mode_t);
int chown(const char *, uid_t, gid_t);
int chroot(const char *);
int clearenv(void);
void clearerr(FILE *);
void clearerr_unlocked(FILE *);
clock_t clock(void);
int clock_getcpuclockid(pid_t, clockid_t *);
int clock_getres(clockid_t, struct timespec *);
int clock_gettime(clockid_t, struct timespec *);
int clock_nanosleep(clockid_t, int, const struct timespec *, struct timespec *);
int clock_settime(clockid_t, const struct timespec *);
int clone(int (*)(void *), void *, int, void *, ...);
int close(int);
int closedir(DIR *);
void closelog(void);
size_t confstr(int, char *, size_t);
int connect(int, const struct sockaddr *, socklen_t);
ssize_t copy_file_range(int, off_t *, int, off_t *, size_t, unsigned int);
double copysign(double, double);
float copysignf(float, float);
long double copysignl(long double, long double);
double cos(double);
float cosf(float);
double cosh(double);
float coshf(float);
long double coshl(long double);
long double cosl(long double);
int creat(const char *, mode_t);
char *crypt(const char *, const char *);
char *ctermid(char *);
char *ctime(const time_t *);
char *ctime_r(const time_t *, char *);
char *cuserid(char *);
int daemon(int, int);
double difftime(time_t, time_t);
int dirfd(DIR *);
char *dirname(char *);
div_t div(int, int);
int dl_iterate_phdr(int (*)(struct dl_phdr_info *, size_t, void *), void *);
int dladdr(const void *, Dl_info *);
int dlclose(void *);
char *dlerror(void);
int dlinfo(void *, int, void *);
void *dlopen(const char *, int);
void *dlsym(void *, const char *);
int dprintf(int, const char *, ...);
double drand48(void);
double drem(double, double);
float dremf(float, float);
int dup(int);
int dup2(int, int);
int dup3(int, int, int);
locale_t duplocale(locale_t);
int eaccess(const char *, int);
char *ecvt(double, int, int *, int *);
void encrypt(char *, int);
void endgrent(void);
void endhostent(void);
void endnetent(void);
void endprotoent(void);
void endpwent(void);
void endservent(void);
void endspent(void);
void endusershell(void);
void endutent(void);
void endutxent(void);
int epoll_create(int);
int epoll_create1(int);
int epoll_ctl(int, int, int, struct epoll_event *);
int epoll_pwait(int, struct epoll_event *, int, int, const sigset_t *);
int epoll_wait(int, struct epoll_event *, int, int);
double erand48(unsigned short *);
double erf(double);
double erfc(double);
float erfcf(float);
long double erfcl(long double);
float erff(float);
long double erfl(long double);
_Noreturn void err(int, const char *, ...);
_Noreturn void errx(int, const char *, ...);
int euidaccess(const char *, int);
int eventfd(unsigned int, int);
int eventfd_read(int, eventfd_t *);
int eventfd_write(int, eventfd_t);
int execl(const char *, const char *, ...);
int execle(const char *, const char *, ...);
int execlp(const char *, const char *, ...);
int execv(const char *, char *const *);
int execve(const char *, char *const *, char *const *);
int execvp(const char *, char *const *);
int execvpe(const char *, char *const *, char *const *);
_Noreturn void exit(int);
double exp(double);
double exp10(double);
float exp10f(float);
long double exp10l(long double);
double exp2(double);
float exp2f(float);
long double exp2l(long double);
float expf(float);
long double expl(long double);
void explicit_bzero(void *, size_t);
double expm1(double);
float expm1f(float);
long double expm1l(long double);
double fabs(double);
float fabsf(float);
long double fabsl(long double);
int faccessat(int, const char *, int, int);
int fallocate(int, int, off_t, off_t);
int fchdir(int);
int fchmod(int, mode_t);
int fchmodat(int, const char *, mode_t, int);
int fchown(int, uid_t, gid_t);
int fchownat(int, const char *, uid_t, gid_t, int);
int fclose(FILE *);
int fcntl(int, int, ...);
char *fcvt(double, int, int *, int *);
int fdatasync(int);
double fdim(double, double);
float fdimf(float, float);
long double fdiml(long double, long double);
FILE *fdopen(int, const char *);
DIR *fdopendir(int);
int feclearexcept(int);
int fegetenv(fenv_t *);
int fegetexceptflag(fexcept_t *, int);
int fegetround(void);
int feholdexcept(fenv_t *);
int feof(FILE *);
int feof_unlocked(FILE *);
int feraiseexcept(int);
int ferror(FILE *);
int ferror_unlocked(FILE *);
int fesetenv(const fenv_t *);
int fesetexceptflag(const fexcept_t *, int);
int fesetround(int);
int fetestexcept(int);
int feupdateenv(const fenv_t *);
int fexecve(int, char *const *, char *const *);
int fflush(FILE *);
int fflush_unlocked(FILE *);
int ffs(int);
int ffsl(long);
int ffsll(long long);
int fgetc(FILE *);
int fgetc_unlocked(FILE *);
struct group *fgetgrent(FILE *);
char *fgetln(FILE *, size_t *);
int fgetpos(FILE *, fpos_t *);
struct passwd *fgetpwent(FILE *);
char *fgets(char *, int, FILE *);
char *fgets_unlocked(char *, int, FILE *);
struct spwd *fgetspent(FILE *);
wint_t fgetwc(FILE *);
wint_t fgetwc_unlocked(FILE *);
unsigned int *fgetws(unsigned int *, int, FILE *);
unsigned int *fgetws_unlocked(unsigned int *, int, FILE *);
ssize_t fgetxattr(int, const char *, void *, size_t);
int fileno(FILE *);
int fileno_unlocked(FILE *);
int finite(double);
int finitef(float);
ssize_t flistxattr(int, char *, size_t);
int flock(int, int);
void flockfile(FILE *);
double floor(double);
float floorf(float);
long double floorl(long double);
double fma(double, double, double);
float fmaf(float, float, float);
long double fmal(long double, long double, long double);
double fmax(double, double);
float fmaxf(float, float);
long double fmaxl(long double, long double);
FILE *fmemopen(void *, size_t, const char *);
double fmin(double, double);
float fminf(float, float);
long double fminl(long double, long double);
double fmod(double, double);
float fmodf(float, float);
long double fmodl(long double, long double);
int fnmatch(const char *, const char *, int);
FILE *fopen(const char *, const char *);
FILE *fopencookie(void *, const char *, cookie_io_functions_t);
pid_t fork(void);
long fpathconf(int, int);
int fprintf(FILE *, const char *, ...);
int fputc(int, FILE *);
int fputc_unlocked(int, FILE *);
int fputs(const char *, FILE *);
int fputs_unlocked(const char *, FILE *);
wint_t fputwc(unsigned int, FILE *);
wint_t fputwc_unlocked(unsigned int, FILE *);
int fputws(const unsigned int *, FILE *);
int fputws_unlocked(const unsigned int *, FILE *);
size_t fread(void *, size_t, size_t, FILE *);
size_t fread_unlocked(void *, size_t, size_t, FILE *);
void free(void *);
void freeaddrinfo(struct addrinfo *);
void freeifaddrs(struct ifaddrs *);
void freelocale(locale_t);
int fremovexattr(int, const char *);
FILE *freopen(const char *, const char *, FILE *);
double frexp(double, int *);
float frexpf(float, int *);
long double frexpl(long double, int *);
int fscanf(FILE *, const char *, ...);
int fseek(FILE *, long, int);
int fseeko(FILE *, off_t, int);
int fsetpos(FILE *, const fpos_t *);
int fsetxattr(int, const char *, const void *, size_t, int);
int fstat(int, struct stat *);
int fstatat(int, const char *, struct stat *, int);
int fstatvfs(int, struct statvfs *);
int fsync(int);
long ftell(FILE *);
off_t ftello(FILE *);
key_t ftok(const char *, int);
int ftruncate(int, off_t);
int ftrylockfile(FILE *);
int ftw(const char *, int (*)(const char *, const struct stat *, int), int);
void funlockfile(FILE *);
int futimens(int, const struct timespec *);
int futimes(int, const struct timeval *);
int futimesat(int, const char *, const struct timeval *);
int fwide(FILE *, int);
int fwprintf(FILE *, const unsigned int *, ...);
size_t fwrite(const void *, size_t, size_t, FILE *);
size_t fwrite_unlocked(const void *, size_t, size_t, FILE *);
int fwscanf(FILE *, const unsigned int *, ...);
const char *gai_strerror(int);
char *gcvt(double, int, char *);
long get_avphys_pages(void);
char *get_current_dir_name(void);
int get_nprocs(void);
int get_nprocs_conf(void);
long get_phys_pages(void);
int getaddrinfo(const char *, const char *, const struct addrinfo *, struct addrinfo **);
unsigned long getauxval(unsigned long);
int getc(FILE *);
int getc_unlocked(FILE *);
int getchar(void);
int getchar_unlocked(void);
int getcontext(struct ucontext *);
char *getcwd(char *, size_t);
struct tm *getdate(const char *);
ssize_t getdelim(char **, size_t *, int, FILE *);
int getdents(int, struct dirent *, size_t);
int getdomainname(char *, size_t);
int getdtablesize(void);
gid_t getegid(void);
int getentropy(void *, size_t);
char *getenv(const char *);
uid_t geteuid(void);
gid_t getgid(void);
struct group *getgrent(void);
struct group *getgrgid(gid_t);
int getgrgid_r(gid_t, struct group *, char *, size_t, struct group **);
struct group *getgrnam(const char *);
int getgrnam_r(const char *, struct group *, char *, size_t, struct group **);
int getgrouplist(const char *, gid_t, gid_t *, int *);
int getgroups(int, gid_t *);
struct hostent *gethostbyaddr(const void *, socklen_t, int);
int gethostbyaddr_r(const void *, socklen_t, int, struct hostent *, char *, size_t, struct hostent **, int *);
struct hostent *gethostbyname(const char *);
struct hostent *gethostbyname2(const char *, int);
int gethostbyname2_r(const char *, int, struct hostent *, char *, size_t, struct hostent **, int *);
int gethostbyname_r(const char *, struct hostent *, char *, size_t, struct hostent **, int *);
struct hostent *gethostent(void);
long gethostid(void);
int gethostname(char *, size_t);
int getifaddrs(struct ifaddrs **);
int getitimer(int, struct itimerval *);
ssize_t getline(char **, size_t *, FILE *);
int getloadavg(double *, int);
char *getlogin(void);
int getlogin_r(char *, size_t);
int getnameinfo(const struct sockaddr *, socklen_t, char *, socklen_t, char *, socklen_t, int);
struct netent *getnetbyaddr(unsigned int, int);
struct netent *getnetbyname(const char *);
struct netent *getnetent(void);
int getopt(int, char *const *, const char *);
int getopt_long(int, char *const *, const char *, const struct option *, int *);
int getopt_long_only(int, char *const *, const char *, const struct option *, int *);
int getpagesize(void);
char *getpass(const char *);
int getpeername(int, struct sockaddr *, socklen_t *);
pid_t getpgid(pid_t);
pid_t getpgrp(void);
pid_t getpid(void);
pid_t getppid(void);
int getpriority(int, id_t);
struct protoent *getprotobyname(const char *);
struct protoent *getprotobynumber(int);
struct protoent *getprotoent(void);
struct passwd *getpwent(void);
struct passwd *getpwnam(const char *);
int getpwnam_r(const char *, struct passwd *, char *, size_t, struct passwd **);
struct passwd *getpwuid(uid_t);
int getpwuid_r(uid_t, struct passwd *, char *, size_t, struct passwd **);
ssize_t getrandom(void *, size_t, unsigned int);
int getresgid(gid_t *, gid_t *, gid_t *);
int getresuid(uid_t *, uid_t *, uid_t *);
int getrlimit(int, struct rlimit *);
int getrusage(int, struct rusage *);
struct servent *getservbyname(const char *, const char *);
int getservbyname_r(const char *, const char *, struct servent *, char *, size_t, struct servent **);
struct servent *getservbyport(int, const char *);
int getservbyport_r(int, const char *, struct servent *, char *, size_t, struct servent **);
struct servent *getservent(void);
pid_t getsid(pid_t);
int getsockname(int, struct sockaddr *, socklen_t *);
int getsockopt(int, int, int, void *, socklen_t *);
struct spwd *getspent(void);
struct spwd *getspnam(const char *);
int getspnam_r(const char *, struct spwd *, char *, size_t, struct spwd **);
int getsubopt(char **, char *const *, char **);
pid_t gettid(void);
int gettimeofday(struct timeval *, void *);
uid_t getuid(void);
char *getusershell(void);
struct utmpx *getutent(void);
struct utmpx *getutid(const struct utmpx *);
struct utmpx *getutline(const struct utmpx *);
struct utmpx *getutxent(void);
struct utmpx *getutxid(const struct utmpx *);
struct utmpx *getutxline(const struct utmpx *);
int getw(FILE *);
wint_t getwc(FILE *);
wint_t getwc_unlocked(FILE *);
wint_t getwchar(void);
wint_t getwchar_unlocked(void);
ssize_t getxattr(const char *, const char *, void *, size_t);
int glob(const char *, int, int (*)(const char *, int), glob_t *);
void globfree(glob_t *);
struct tm *gmtime(const time_t *);
struct tm *gmtime_r(const time_t *, struct tm *);
int grantpt(int);
int hcreate(size_t);
int hcreate_r(size_t, struct hsearch_data *);
void hdestroy(void);
void hdestroy_r(struct hsearch_data *);
void herror(const char *);
ENTRY *hsearch(ENTRY, ACTION);
int hsearch_r(ENTRY, ACTION, ENTRY **, struct hsearch_data *);
const char *hstrerror(int);
unsigned int htonl(unsigned int);
unsigned short htons(unsigned short);
double hypot(double, double);
float hypotf(float, float);
long double hypotl(long double, long double);
size_t iconv(iconv_t, char **, size_t *, char **, size_t *);
int iconv_close(iconv_t);
iconv_t iconv_open(const char *, const char *);
void if_freenameindex(struct if_nameindex *);
char *if_indextoname(unsigned int, char *);
struct if_nameindex *if_nameindex(void);
unsigned int if_nametoindex(const char *);
int ilogb(double);
int ilogbf(float);
int ilogbl(long double);
intmax_t imaxabs(intmax_t);
imaxdiv_t imaxdiv(intmax_t, intmax_t);
char *index(const char *, int);
in_addr_t inet_addr(const char *);
int inet_aton(const char *, struct in_addr *);
in_addr_t inet_lnaof(struct in_addr);
struct in_addr inet_makeaddr(in_addr_t, in_addr_t);
in_addr_t inet_netof(struct in_addr);
in_addr_t inet_network(const char *);
char *inet_ntoa(struct in_addr);
const char *inet_ntop(int, const void *, char *, socklen_t);
int inet_pton(int, const char *, void *);
int initgroups(const char *, gid_t);
char *initstate(unsigned int, char *, size_t);
int inotify_add_watch(int, const char *, unsigned int);
int inotify_init(void);
int inotify_init1(int);
int inotify_rm_watch(int, int);
void insque(void *, void *);
int ioctl(int, int, ...);
int isalnum(int);
int isalnum_l(int, locale_t);
int isalpha(int);
int isalpha_l(int, locale_t);
int isascii(int);
int isatty(int);
int isblank(int);
int isblank_l(int, locale_t);
int iscntrl(int);
int iscntrl_l(int, locale_t);
int isdigit(int);
int isdigit_l(int, locale_t);
int isgraph(int);
int isgraph_l(int, locale_t);
int islower(int);
int islower_l(int, locale_t);
int isprint(int);
int isprint_l(int, locale_t);
int ispunct(int);
int ispunct_l(int, locale_t);
int issetugid(void);
int isspace(int);
int isspace_l(int, locale_t);
int isupper(int);
int isupper_l(int, locale_t);
int iswalnum(wint_t);
int iswalnum_l(wint_t, locale_t);
int iswalpha(wint_t);
int iswalpha_l(wint_t, locale_t);
int iswblank(wint_t);
int iswblank_l(wint_t, locale_t);
int iswcntrl(wint_t);
int iswcntrl_l(wint_t, locale_t);
int iswctype(wint_t, wctype_t);
int iswctype_l(wint_t, wctype_t, locale_t);
int iswdigit(wint_t);
int iswdigit_l(wint_t, locale_t);
int iswgraph(wint_t);
int iswgraph_l(wint_t, locale_t);
int iswlower(wint_t);
int iswlower_l(wint_t, locale_t);
int iswprint(wint_t);
int iswprint_l(wint_t, locale_t);
int iswpunct(wint_t);
int iswpunct_l(wint_t, locale_t);
int iswspace(wint_t);
int iswspace_l(wint_t, locale_t);
int iswupper(wint_t);
int iswupper_l(wint_t, locale_t);
int iswxdigit(wint_t);
int iswxdigit_l(wint_t, locale_t);
int isxdigit(int);
int isxdigit_l(int, locale_t);
double j0(double);
float j0f(float);
double j1(double);
float j1f(float);
double jn(int, double);
float jnf(int, float);
long jrand48(unsigned short *);
int kill(pid_t, int);
int killpg(pid_t, int);
char *l64a(long);
long labs(long);
int lchmod(const char *, mode_t);
int lchown(const char *, uid_t, gid_t);
int lckpwdf(void);
void lcong48(unsigned short *);
double ldexp(double, int);
float ldexpf(float, int);
long double ldexpl(long double, int);
ldiv_t ldiv(long, long);
void *lfind(const void *, const void *, size_t *, size_t, int (*)(const void *, const void *));
double lgamma(double);
double lgamma_r(double, int *);
float lgammaf(float);
float lgammaf_r(float, int *);
long double lgammal(long double);
long double lgammal_r(long double, int *);
ssize_t lgetxattr(const char *, const char *, void *, size_t);
int link(const char *, const char *);
int linkat(int, const char *, int, const char *, int);
int lio_listio(int, struct aiocb *const *, int, struct sigevent *);
int listen(int, int);
ssize_t listxattr(const char *, char *, size_t);
long long llabs(long long);
lldiv_t lldiv(long long, long long);
ssize_t llistxattr(const char *, char *, size_t);
long long llrint(double);
long long llrintf(float);
long long llrintl(long double);
long long llround(double);
long long llroundf(float);
long long llroundl(long double);
struct lconv *localeconv(void);
struct tm *localtime(const time_t *);
struct tm *localtime_r(const time_t *, struct tm *);
int lockf(int, int, off_t);
double log(double);
double log10(double);
float log10f(float);
long double log10l(long double);
double log1p(double);
float log1pf(float);
long double log1pl(long double);
double log2(double);
float log2f(float);
long double log2l(long double);
double logb(double);
float logbf(float);
long double logbl(long double);
float logf(float);
int login_tty(int);
long double logl(long double);
_Noreturn void longjmp(jmp_buf, int);
long lrand48(void);
int lremovexattr(const char *, const char *);
long lrint(double);
long lrintf(float);
long lrintl(long double);
long lround(double);
long lroundf(float);
long lroundl(long double);
void *lsearch(const void *, void *, size_t *, size_t, int (*)(const void *, const void *));
off_t lseek(int, off_t, int);
int lsetxattr(const char *, const char *, const void *, size_t, int);
int lstat(const char *, struct stat *);
int lutimes(const char *, const struct timeval *);
int madvise(void *, size_t, int);
void makecontext(struct ucontext *, void (*)(void), int, ...);
void *malloc(size_t);
size_t malloc_usable_size(void *);
int mblen(const char *, size_t);
size_t mbrlen(const char *, size_t, mbstate_t *);
size_t mbrtowc(unsigned int *, const char *, size_t, mbstate_t *);
int mbsinit(const mbstate_t *);
size_t mbsnrtowcs(unsigned int *, const char **, size_t, size_t, mbstate_t *);
size_t mbsrtowcs(unsigned int *, const char **, size_t, mbstate_t *);
size_t mbstowcs(unsigned int *, const char *, size_t);
int mbtowc(unsigned int *, const char *, size_t);
void *memalign(size_t, size_t);
void *memccpy(void *, const void *, int, size_t);
void *memchr(const void *, int, size_t);
int memcmp(const void *, const void *, size_t);
void *memcpy(void *, const void *, size_t);
int memfd_create(const char *, unsigned int);
void *memmem(const void *, size_t, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *mempcpy(void *, const void *, size_t);
void *memrchr(const void *, int, size_t);
void *memset(void *, int, size_t);
int mincore(void *, size_t, unsigned char *);
int mkdir(const char *, mode_t);
int mkdirat(int, const char *, mode_t);
char *mkdtemp(char *);
int mkfifo(const char *, mode_t);
int mkfifoat(int, const char *, mode_t);
int mknod(const char *, mode_t, dev_t);
int mknodat(int, const char *, mode_t, dev_t);
int mkostemp(char *, int);
int mkostemps(char *, int, int);
int mkstemp(char *);
int mkstemps(char *, int);
char *mktemp(char *);
time_t mktime(struct tm *);
int mlock(const void *, size_t);
int mlock2(const void *, size_t, unsigned int);
int mlockall(int);
void *mmap(void *, size_t, int, int, int, off_t);
double modf(double, double *);
float modff(float, float *);
long double modfl(long double, long double *);
int mprotect(void *, size_t, int);
int mq_close(mqd_t);
int mq_getattr(mqd_t, struct mq_attr *);
int mq_notify(mqd_t, const struct sigevent *);
mqd_t mq_open(const char *, int, ...);
ssize_t mq_receive(mqd_t, char *, size_t, unsigned int *);
int mq_send(mqd_t, const char *, size_t, unsigned int);
int mq_setattr(mqd_t, const struct mq_attr *, struct mq_attr *);
ssize_t mq_timedreceive(mqd_t, char *, size_t, unsigned int *, const struct timespec *);
int mq_timedsend(mqd_t, const char *, size_t, unsigned int, const struct timespec *);
int mq_unlink(const char *);
long mrand48(void);
void *mremap(void *, size_t, size_t, int, ...);
int msgctl(int, int, struct msqid_ds *);
int msgget(key_t, int);
ssize_t msgrcv(int, void *, size_t, long, int);
int msgsnd(int, const void *, size_t, int);
int msync(void *, size_t, int);
int munlock(const void *, size_t);
int munlockall(void);
int munmap(void *, size_t);
int name_to_handle_at(int, const char *, struct file_handle *, int *, int);
double nan(const char *);
float nanf(const char *);
long double nanl(const char *);
int nanosleep(const struct timespec *, struct timespec *);
double nearbyint(double);
float nearbyintf(float);
long double nearbyintl(long double);
locale_t newlocale(int, const char *, locale_t);
double nextafter(double, double);
float nextafterf(float, float);
long double nextafterl(long double, long double);
double nexttoward(double, long double);
float nexttowardf(float, long double);
long double nexttowardl(long double, long double);
int nftw(const char *, int (*)(const char *, const struct stat *, int, struct FTW *), int, int);
int nice(int);
char *nl_langinfo(nl_item);
char *nl_langinfo_l(nl_item, locale_t);
long nrand48(unsigned short *);
unsigned int ntohl(unsigned int);
unsigned short ntohs(unsigned short);
int open(const char *, int, ...);
int open_by_handle_at(int, struct file_handle *, int);
FILE *open_memstream(char **, size_t *);
FILE *open_wmemstream(unsigned int **, size_t *);
int openat(int, const char *, int, ...);
DIR *opendir(const char *);
void openlog(const char *, int, int);
long pathconf(const char *, int);
int pause(void);
int pclose(FILE *);
void perror(const char *);
int pipe(int *);
int pipe2(int *, int);
int poll(struct pollfd *, nfds_t, int);
FILE *popen(const char *, const char *);
int posix_close(int, int);
int posix_fadvise(int, off_t, off_t, int);
int posix_fallocate(int, off_t, off_t);
int posix_madvise(void *, size_t, int);
int posix_memalign(void **, size_t, size_t);
int posix_openpt(int);
int posix_spawn(pid_t *, const char *, const posix_spawn_file_actions_t *, const posix_spawnattr_t *, char *const *, char *const *);
int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t *, const char *);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *, int);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *, int, int);
int posix_spawn_file_actions_addfchdir_np(posix_spawn_file_actions_t *, int);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *, int, const char *, int, mode_t);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *);
int posix_spawn_file_actions_init(posix_spawn_file_actions_t *);
int posix_spawnattr_destroy(posix_spawnattr_t *);
int posix_spawnattr_getflags(const posix_spawnattr_t *, short *);
int posix_spawnattr_getpgroup(const posix_spawnattr_t *, pid_t *);
int posix_spawnattr_getschedparam(const posix_spawnattr_t *, struct sched_param *);
int posix_spawnattr_getschedpolicy(const posix_spawnattr_t *, int *);
int posix_spawnattr_getsigdefault(const posix_spawnattr_t *, sigset_t *);
int posix_spawnattr_getsigmask(const posix_spawnattr_t *, sigset_t *);
int posix_spawnattr_init(posix_spawnattr_t *);
int posix_spawnattr_setflags(posix_spawnattr_t *, short);
int posix_spawnattr_setpgroup(posix_spawnattr_t *, pid_t);
int posix_spawnattr_setschedparam(posix_spawnattr_t *, const struct sched_param *);
int posix_spawnattr_setschedpolicy(posix_spawnattr_t *, int);
int posix_spawnattr_setsigdefault(posix_spawnattr_t *, const sigset_t *);
int posix_spawnattr_setsigmask(posix_spawnattr_t *, const sigset_t *);
int posix_spawnp(pid_t *, const char *, const posix_spawn_file_actions_t *, const posix_spawnattr_t *, char *const *, char *const *);
double pow(double, double);
double pow10(double);
float pow10f(float);
long double pow10l(long double);
float powf(float, float);
long double powl(long double, long double);
int ppoll(struct pollfd *, nfds_t, const struct timespec *, const sigset_t *);
int prctl(int, ...);
ssize_t pread(int, void *, size_t, off_t);
ssize_t preadv(int, const struct iovec *, int, off_t);
ssize_t preadv2(int, const struct iovec *, int, off_t, int);
int printf(const char *, ...);
int prlimit(pid_t, int, const struct rlimit *, struct rlimit *);
ssize_t process_vm_readv(pid_t, const struct iovec *, unsigned long, const struct iovec *, unsigned long, unsigned long);
ssize_t process_vm_writev(pid_t, const struct iovec *, unsigned long, const struct iovec *, unsigned long, unsigned long);
int pselect(int, fd_set *, fd_set *, fd_set *, const struct timespec *, const sigset_t *);
void psiginfo(const siginfo_t *, const char *);
void psignal(int, const char *);
int pthread_atfork(void (*)(void), void (*)(void), void (*)(void));
int pthread_attr_destroy(pthread_attr_t *);
int pthread_attr_getdetachstate(const pthread_attr_t *, int *);
int pthread_attr_getguardsize(const pthread_attr_t *, size_t *);
int pthread_attr_getinheritsched(const pthread_attr_t *, int *);
int pthread_attr_getschedparam(const pthread_attr_t *, struct sched_param *);
int pthread_attr_getschedpolicy(const pthread_attr_t *, int *);
int pthread_attr_getscope(const pthread_attr_t *, int *);
int pthread_attr_getstack(const pthread_attr_t *, void **, size_t *);
int pthread_attr_getstacksize(const pthread_attr_t *, size_t *);
int pthread_attr_init(pthread_attr_t *);
int pthread_attr_setdetachstate(pthread_attr_t *, int);
int pthread_attr_setguardsize(pthread_attr_t *, size_t);
int pthread_attr_setinheritsched(pthread_attr_t *, int);
int pthread_attr_setschedparam(pthread_attr_t *, const struct sched_param *);
int pthread_attr_setschedpolicy(pthread_attr_t *, int);
int pthread_attr_setscope(pthread_attr_t *, int);
int pthread_attr_setstack(pthread_attr_t *, void *, size_t);
int pthread_attr_setstacksize(pthread_attr_t *, size_t);
int pthread_barrier_destroy(pthread_barrier_t *);
int pthread_barrier_init(pthread_barrier_t *, const pthread_barrierattr_t *, unsigned int);
int pthread_barrier_wait(pthread_barrier_t *);
int pthread_barrierattr_destroy(pthread_barrierattr_t *);
int pthread_barrierattr_getpshared(const pthread_barrierattr_t *, int *);
int pthread_barrierattr_init(pthread_barrierattr_t *);
int pthread_barrierattr_setpshared(pthread_barrierattr_t *, int);
int pthread_cancel(pthread_t);
int pthread_cond_broadcast(pthread_cond_t *);
int pthread_cond_destroy(pthread_cond_t *);
int pthread_cond_init(pthread_cond_t *, const pthread_condattr_t *);
int pthread_cond_signal(pthread_cond_t *);
int pthread_cond_timedwait(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
int pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
int pthread_condattr_destroy(pthread_condattr_t *);
int pthread_condattr_getclock(const pthread_condattr_t *, clockid_t *);
int pthread_condattr_getpshared(const pthread_condattr_t *, int *);
int pthread_condattr_init(pthread_condattr_t *);
int pthread_condattr_setclock(pthread_condattr_t *, clockid_t);
int pthread_condattr_setpshared(pthread_condattr_t *, int);
int pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int pthread_detach(pthread_t);
int pthread_equal(pthread_t, pthread_t);
_Noreturn void pthread_exit(void *);
int pthread_getaffinity_np(pthread_t, size_t, cpu_set_t *);
int pthread_getattr_default_np(pthread_attr_t *);
int pthread_getattr_np(pthread_t, pthread_attr_t *);
int pthread_getconcurrency(void);
int pthread_getcpuclockid(pthread_t, clockid_t *);
int pthread_getname_np(pthread_t, char *, size_t);
int pthread_getschedparam(pthread_t, int *, struct sched_param *);
void *pthread_getspecific(pthread_key_t);
int pthread_join(pthread_t, void **);
int pthread_key_create(pthread_key_t *, void (*)(void *));
int pthread_key_delete(pthread_key_t);
int pthread_kill(pthread_t, int);
int pthread_mutex_consistent(pthread_mutex_t *);
int pthread_mutex_destroy(pthread_mutex_t *);
int pthread_mutex_getprioceiling(const pthread_mutex_t *, int *);
int pthread_mutex_init(pthread_mutex_t *, const pthread_mutexattr_t *);
int pthread_mutex_lock(pthread_mutex_t *);
int pthread_mutex_setprioceiling(pthread_mutex_t *, int, int *);
int pthread_mutex_timedlock(pthread_mutex_t *, const struct timespec *);
int pthread_mutex_trylock(pthread_mutex_t *);
int pthread_mutex_unlock(pthread_mutex_t *);
int pthread_mutexattr_destroy(pthread_mutexattr_t *);
int pthread_mutexattr_getprioceiling(const pthread_mutexattr_t *, int *);
int pthread_mutexattr_getprotocol(const pthread_mutexattr_t *, int *);
int pthread_mutexattr_getpshared(const pthread_mutexattr_t *, int *);
int pthread_mutexattr_getrobust(const pthread_mutexattr_t *, int *);
int pthread_mutexattr_gettype(const pthread_mutexattr_t *, int *);
int pthread_mutexattr_init(pthread_mutexattr_t *);
int pthread_mutexattr_setprioceiling(pthread_mutexattr_t *, int);
int pthread_mutexattr_setprotocol(pthread_mutexattr_t *, int);
int pthread_mutexattr_setpshared(pthread_mutexattr_t *, int);
int pthread_mutexattr_setrobust(pthread_mutexattr_t *, int);
int pthread_mutexattr_settype(pthread_mutexattr_t *, int);
int pthread_once(pthread_once_t *, void (*)(void));
int pthread_rwlock_destroy(pthread_rwlock_t *);
int pthread_rwlock_init(pthread_rwlock_t *, const pthread_rwlockattr_t *);
int pthread_rwlock_rdlock(pthread_rwlock_t *);
int pthread_rwlock_timedrdlock(pthread_rwlock_t *, const struct timespec *);
int pthread_rwlock_timedwrlock(pthread_rwlock_t *, const struct timespec *);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *);
int pthread_rwlock_trywrlock(pthread_rwlock_t *);
int pthread_rwlock_unlock(pthread_rwlock_t *);
int pthread_rwlock_wrlock(pthread_rwlock_t *);
int pthread_rwlockattr_destroy(pthread_rwlockattr_t *);
int pthread_rwlockattr_getpshared(const pthread_rwlockattr_t *, int *);
int pthread_rwlockattr_init(pthread_rwlockattr_t *);
int pthread_rwlockattr_setpshared(pthread_rwlockattr_t *, int);
pthread_t pthread_self(void);
int pthread_setaffinity_np(pthread_t, size_t, const cpu_set_t *);
int pthread_setattr_default_np(const pthread_attr_t *);
int pthread_setcancelstate(int, int *);
int pthread_setcanceltype(int, int *);
int pthread_setconcurrency(int);
int pthread_setname_np(pthread_t, const char *);
int pthread_setschedparam(pthread_t, int, const struct sched_param *);
int pthread_setschedprio(pthread_t, int);
int pthread_setspecific(pthread_key_t, const void *);
int pthread_sigmask(int, const sigset_t *, sigset_t *);
int pthread_spin_destroy(pthread_spinlock_t *);
int pthread_spin_init(pthread_spinlock_t *, int);
int pthread_spin_lock(pthread_spinlock_t *);
int pthread_spin_trylock(pthread_spinlock_t *);
int pthread_spin_unlock(pthread_spinlock_t *);
void pthread_testcancel(void);
int pthread_timedjoin_np(pthread_t, void **, const struct timespec *);
int pthread_tryjoin_np(pthread_t, void **);
long ptrace(int, ...);
char *ptsname(int);
int ptsname_r(int, char *, size_t);
int putc(int, FILE *);
int putc_unlocked(int, FILE *);
int putchar(int);
int putchar_unlocked(int);
int putenv(char *);
int putgrent(const struct group *, FILE *);
int putpwent(const struct passwd *, FILE *);
int puts(const char *);
int putspent(const struct spwd *, FILE *);
struct utmpx *pututline(const struct utmpx *);
struct utmpx *pututxline(const struct utmpx *);
int putw(int, FILE *);
wint_t putwc(unsigned int, FILE *);
wint_t putwc_unlocked(unsigned int, FILE *);
wint_t putwchar(unsigned int);
wint_t putwchar_unlocked(unsigned int);
ssize_t pwrite(int, const void *, size_t, off_t);
ssize_t pwritev(int, const struct iovec *, int, off_t);
ssize_t pwritev2(int, const struct iovec *, int, off_t, int);
void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
void qsort_r(void *, size_t, size_t, int (*)(const void *, const void *, void *), void *);
_Noreturn void quick_exit(int);
int raise(int);
int rand(void);
int rand_r(unsigned int *);
long random(void);
ssize_t read(int, void *, size_t);
ssize_t readahead(int, off_t, size_t);
struct dirent *readdir(DIR *);
int readdir_r(DIR *, struct dirent *, struct dirent **);
ssize_t readlink(const char *, char *, size_t);
ssize_t readlinkat(int, const char *, char *, size_t);
ssize_t readv(int, const struct iovec *, int);
void *realloc(void *, size_t);
void *reallocarray(void *, size_t, size_t);
char *realpath(const char *, char *);
ssize_t recv(int, void *, size_t, int);
ssize_t recvfrom(int, void *, size_t, int, struct sockaddr *, socklen_t *);
int recvmmsg(int, struct mmsghdr *, unsigned int, unsigned int, struct timespec *);
ssize_t recvmsg(int, struct msghdr *, int);
int regcomp(regex_t *, const char *, int);
size_t regerror(int, const regex_t *, char *, size_t);
int regexec(const regex_t *, const char *, size_t, regmatch_t *, int);
void regfree(regex_t *);
double remainder(double, double);
float remainderf(float, float);
long double remainderl(long double, long double);
int remap_file_pages(void *, size_t, int, size_t, int);
int remove(const char *);
int removexattr(const char *, const char *);
void remque(void *);
double remquo(double, double, int *);
float remquof(float, float, int *);
long double remquol(long double, long double, int *);
int rename(const char *, const char *);
int renameat(int, const char *, int, const char *);
void rewind(FILE *);
void rewinddir(DIR *);
char *rindex(const char *, int);
double rint(double);
float rintf(float);
long double rintl(long double);
int rmdir(const char *);
double round(double);
float roundf(float);
long double roundl(long double);
void *sbrk(intptr_t);
double scalb(double, double);
float scalbf(float, float);
double scalbln(double, long);
float scalblnf(float, long);
long double scalblnl(long double, long);
double scalbn(double, int);
float scalbnf(float, int);
long double scalbnl(long double, int);
int scandir(const char *, struct dirent ***, int (*)(const struct dirent *), int (*)(const struct dirent **, const struct dirent **));
int scanf(const char *, ...);
int sched_get_priority_max(int);
int sched_get_priority_min(int);
int sched_getaffinity(pid_t, size_t, cpu_set_t *);
int sched_getcpu(void);
int sched_getparam(pid_t, struct sched_param *);
int sched_getscheduler(pid_t);
int sched_rr_get_interval(pid_t, struct timespec *);
int sched_setaffinity(pid_t, size_t, const cpu_set_t *);
int sched_setparam(pid_t, const struct sched_param *);
int sched_setscheduler(pid_t, int, const struct sched_param *);
int sched_yield(void);
char *secure_getenv(const char *);
unsigned short *seed48(unsigned short *);
void seekdir(DIR *, long);
int select(int, fd_set *, fd_set *, fd_set *, struct timeval *);
int sem_close(sem_t *);
int sem_destroy(sem_t *);
int sem_getvalue(sem_t *, int *);
int sem_init(sem_t *, int, unsigned int);
sem_t *sem_open(const char *, int, ...);
int sem_post(sem_t *);
int sem_timedwait(sem_t *, const struct timespec *);
int sem_trywait(sem_t *);
int sem_unlink(const char *);
int sem_wait(sem_t *);
int semctl(int, int, int, ...);
int semget(key_t, int, int);
int semop(int, struct sembuf *, size_t);
int semtimedop(int, struct sembuf *, size_t, const struct timespec *);
ssize_t send(int, const void *, size_t, int);
ssize_t sendfile(int, int, off_t *, size_t);
int sendmmsg(int, struct mmsghdr *, unsigned int, unsigned int);
ssize_t sendmsg(int, const struct msghdr *, int);
ssize_t sendto(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
void setbuf(FILE *, char *);
void setbuffer(FILE *, char *, size_t);
int setcontext(const struct ucontext *);
int setdomainname(const char *, size_t);
int setegid(gid_t);
int setenv(const char *, const char *, int);
int seteuid(uid_t);
int setgid(gid_t);
void setgrent(void);
int setgroups(size_t, const gid_t *);
void sethostent(int);
int sethostname(const char *, size_t);
int setitimer(int, const struct itimerval *, struct itimerval *);
int setjmp(jmp_buf);
void setkey(const char *);
void setlinebuf(FILE *);
char *setlocale(int, const char *);
int setlogmask(int);
void setnetent(int);
int setns(int, int);
int setpgid(pid_t, pid_t);
pid_t setpgrp(void);
int setpriority(int, id_t, int);
void setprotoent(int);
void setpwent(void);
int setregid(gid_t, gid_t);
int setresgid(gid_t, gid_t, gid_t);
int setresuid(uid_t, uid_t, uid_t);
int setreuid(uid_t, uid_t);
int setrlimit(int, const struct rlimit *);
void setservent(int);
pid_t setsid(void);
int setsockopt(int, int, int, const void *, socklen_t);
void setspent(void);
char *setstate(char *);
int settimeofday(const struct timeval *, const struct timezone *);
int setuid(uid_t);
void setusershell(void);
void setutent(void);
void setutxent(void);
int setvbuf(FILE *, char *, int, size_t);
int setxattr(const char *, const char *, const void *, size_t, int);
struct spwd *sgetspent(const char *);
int shm_open(const char *, int, mode_t);
int shm_unlink(const char *);
void *shmat(int, const void *, int);
int shmctl(int, int, struct shmid_ds *);
int shmdt(const void *);
int shmget(key_t, size_t, int);
int shutdown(int, int);
int sigaction(int, const struct sigaction *, struct sigaction *);
int sigaddset(sigset_t *, int);
int sigaltstack(const stack_t *, stack_t *);
int sigandset(sigset_t *, const sigset_t *, const sigset_t *);
int sigdelset(sigset_t *, int);
int sigemptyset(sigset_t *);
int sigfillset(sigset_t *);
int sighold(int);
int sigignore(int);
int siginterrupt(int, int);
int sigisemptyset(const sigset_t *);
int sigismember(const sigset_t *, int);
_Noreturn void siglongjmp(sigjmp_buf, int);
void (*signal(int, void (*)(int)))(int);
int signalfd(int, const sigset_t *, int);
double significand(double);
float significandf(float);
int sigorset(sigset_t *, const sigset_t *, const sigset_t *);
int sigpause(int);
int sigpending(sigset_t *);
int sigprocmask(int, const sigset_t *, sigset_t *);
int sigqueue(pid_t, int, union sigval);
int sigrelse(int);
void (*sigset(int, void (*)(int)))(int);
int sigsetjmp(sigjmp_buf, int);
int sigsuspend(const sigset_t *);
int sigtimedwait(const sigset_t *, siginfo_t *, const struct timespec *);
int sigwait(const sigset_t *, int *);
int sigwaitinfo(const sigset_t *, siginfo_t *);
double sin(double);
void sincos(double, double *, double *);
void sincosf(float, float *, float *);
void sincosl(long double, long double *, long double *);
float sinf(float);
double sinh(double);
float sinhf(float);
long double sinhl(long double);
long double sinl(long double);
unsigned int sleep(unsigned int);
int snprintf(char *, size_t, const char *, ...);
int sockatmark(int);
int socket(int, int, int);
int socketpair(int, int, int, int *);
ssize_t splice(int, off_t *, int, off_t *, size_t, unsigned int);
int sprintf(char *, const char *, ...);
double sqrt(double);
float sqrtf(float);
long double sqrtl(long double);
void srand(unsigned int);
void srand48(long);
void srandom(unsigned int);
int sscanf(const char *, const char *, ...);
int stat(const char *, struct stat *);
int statvfs(const char *, struct statvfs *);
int statx(int, const char *, int, unsigned int, struct statx *);
int stime(const time_t *);
char *stpcpy(char *, const char *);
char *stpncpy(char *, const char *, size_t);
int strcasecmp(const char *, const char *);
int strcasecmp_l(const char *, const char *, locale_t);
char *strcasestr(const char *, const char *);
char *strcat(char *, const char *);
char *strchr(const char *, int);
char *strchrnul(const char *, int);
int strcmp(const char *, const char *);
int strcoll(const char *, const char *);
int strcoll_l(const char *, const char *, locale_t);
char *strcpy(char *, const char *);
size_t strcspn(const char *, const char *);
char *strdup(const char *);
char *strerror(int);
char *strerror_l(int, locale_t);
int strerror_r(int, char *, size_t);
size_t strftime(char *, size_t, const char *, const struct tm *);
size_t strftime_l(char *, size_t, const char *, const struct tm *, locale_t);
size_t strlcat(char *, const char *, size_t);
size_t strlcpy(char *, const char *, size_t);
size_t strlen(const char *);
int strncasecmp(const char *, const char *, size_t);
int strncasecmp_l(const char *, const char *, size_t, locale_t);
char *strncat(char *, const char *, size_t);
int strncmp(const char *, const char *, size_t);
char *strncpy(char *, const char *, size_t);
char *strndup(const char *, size_t);
size_t strnlen(const char *, size_t);
char *strpbrk(const char *, const char *);
char *strptime(const char *, const char *, struct tm *);
char *strrchr(const char *, int);
char *strsep(char **, const char *);
char *strsignal(int);
size_t strspn(const char *, const char *);
char *strstr(const char *, const char *);
double strtod(const char *, char **);
double strtod_l(const char *, char **, struct __locale_struct *);
float strtof(const char *, char **);
float strtof_l(const char *, char **, struct __locale_struct *);
intmax_t strtoimax(const char *, char **, int);
char *strtok(char *, const char *);
char *strtok_r(char *, const char *, char **);
long strtol(const char *, char **, int);
long double strtold(const char *, char **);
long double strtold_l(const char *, char **, struct __locale_struct *);
long long strtoll(const char *, char **, int);
unsigned long strtoul(const char *, char **, int);
unsigned long long strtoull(const char *, char **, int);
uintmax_t strtoumax(const char *, char **, int);
int strverscmp(const char *, const char *);
size_t strxfrm(char *, const char *, size_t);
size_t strxfrm_l(char *, const char *, size_t, locale_t);
void swab(const void *, void *, ssize_t);
int swapcontext(struct ucontext *, const struct ucontext *);
int swprintf(unsigned int *, size_t, const unsigned int *, ...);
int swscanf(const unsigned int *, const unsigned int *, ...);
int symlink(const char *, const char *);
int symlinkat(const char *, int, const char *);
void sync(void);
int sync_file_range(int, off_t, off_t, unsigned int);
int syncfs(int);
long syscall(long, ...);
long sysconf(int);
int sysinfo(struct sysinfo *);
void syslog(int, const char *, ...);
int system(const char *);
double tan(double);
float tanf(float);
double tanh(double);
float tanhf(float);
long double tanhl(long double);
long double tanl(long double);
int tcdrain(int);
int tcflow(int, int);
int tcflush(int, int);
int tcgetattr(int, struct termios *);
pid_t tcgetpgrp(int);
pid_t tcgetsid(int);
int tcgetwinsize(int, struct winsize *);
int tcsendbreak(int, int);
int tcsetattr(int, int, const struct termios *);
int tcsetpgrp(int, pid_t);
int tcsetwinsize(int, const struct winsize *);
void *tdelete(const void *, void **, int (*)(const void *, const void *));
void tdestroy(void *, void (*)(void *));
ssize_t tee(int, int, size_t, unsigned int);
long telldir(DIR *);
char *tempnam(const char *, const char *);
void *tfind(const void *, void *const *, int (*)(const void *, const void *));
double tgamma(double);
float tgammaf(float);
long double tgammal(long double);
time_t time(time_t *);
time_t timegm(struct tm *);
int timer_create(clockid_t, struct sigevent *, timer_t *);
int timer_delete(timer_t);
int timer_getoverrun(timer_t);
int timer_gettime(timer_t, struct itimerspec *);
int timer_settime(timer_t, int, const struct itimerspec *, struct itimerspec *);
int timerfd_create(int, int);
int timerfd_gettime(int, struct itimerspec *);
int timerfd_settime(int, int, const struct itimerspec *, struct itimerspec *);
clock_t times(struct tms *);
int timespec_get(struct timespec *, int);
FILE *tmpfile(void);
char *tmpnam(char *);
int toascii(int);
int tolower(int);
int tolower_l(int, locale_t);
int toupper(int);
int toupper_l(int, locale_t);
wint_t towctrans(wint_t, wctrans_t);
wint_t towctrans_l(wint_t, wctrans_t, locale_t);
wint_t towlower(wint_t);
wint_t towlower_l(wint_t, locale_t);
wint_t towupper(wint_t);
wint_t towupper_l(wint_t, locale_t);
double trunc(double);
int truncate(const char *, off_t);
float truncf(float);
long double truncl(long double);
void *tsearch(const void *, void **, int (*)(const void *, const void *));
char *ttyname(int);
int ttyname_r(int, char *, size_t);
void twalk(const void *, void (*)(const void *, VISIT, int));
void tzset(void);
unsigned int ualarm(unsigned int, unsigned int);
int ulckpwdf(void);
mode_t umask(mode_t);
int uname(struct utsname *);
int ungetc(int, FILE *);
wint_t ungetwc(wint_t, FILE *);
int unlink(const char *);
int unlinkat(int, const char *, int);
int unlockpt(int);
int unsetenv(const char *);
int unshare(int);
void updwtmp(const char *, const struct utmpx *);
void updwtmpx(const char *, const struct utmpx *);
locale_t uselocale(locale_t);
int usleep(unsigned int);
int utime(const char *, const struct utimbuf *);
int utimensat(int, const char *, const struct timespec *, int);
int utimes(const char *, const struct timeval *);
int utmpname(const char *);
int utmpxname(const char *);
void *valloc(size_t);
int vasprintf(char **, const char *, char *);
int vdprintf(int, const char *, char *);
_Noreturn void verr(int, const char *, char *);
_Noreturn void verrx(int, const char *, char *);
int versionsort(const struct dirent **, const struct dirent **);
pid_t vfork(void);
int vfprintf(FILE *, const char *, char *);
int vfscanf(FILE *, const char *, char *);
int vfwprintf(FILE *, const unsigned int *, char *);
int vfwscanf(FILE *, const unsigned int *, char *);
int vhangup(void);
ssize_t vmsplice(int, const struct iovec *, size_t, unsigned int);
int vprintf(const char *, char *);
int vscanf(const char *, char *);
int vsnprintf(char *, size_t, const char *, char *);
int vsprintf(char *, const char *, char *);
int vsscanf(const char *, const char *, char *);
int vswprintf(unsigned int *, size_t, const unsigned int *, char *);
int vswscanf(const unsigned int *, const unsigned int *, char *);
void vsyslog(int, const char *, char *);
void vwarn(const char *, char *);
void vwarnx(const char *, char *);
int vwprintf(const unsigned int *, char *);
int vwscanf(const unsigned int *, char *);
pid_t wait(int *);
pid_t wait3(int *, int, struct rusage *);
pid_t wait4(pid_t, int *, int, struct rusage *);
int waitid(idtype_t, id_t, siginfo_t *, int);
pid_t waitpid(pid_t, int *, int);
void warn(const char *, ...);
void warnx(const char *, ...);
unsigned int *wcpcpy(unsigned int *, const unsigned int *);
unsigned int *wcpncpy(unsigned int *, const unsigned int *, size_t);
size_t wcrtomb(char *, unsigned int, mbstate_t *);
int wcscasecmp(const unsigned int *, const unsigned int *);
int wcscasecmp_l(const unsigned int *, const unsigned int *, locale_t);
unsigned int *wcscat(unsigned int *, const unsigned int *);
unsigned int *wcschr(const unsigned int *, unsigned int);
int wcscmp(const unsigned int *, const unsigned int *);
int wcscoll(const unsigned int *, const unsigned int *);
int wcscoll_l(const unsigned int *, const unsigned int *, locale_t);
unsigned int *wcscpy(unsigned int *, const unsigned int *);
size_t wcscspn(const unsigned int *, const unsigned int *);
unsigned int *wcsdup(const unsigned int *);
size_t wcsftime(unsigned int *, size_t, const unsigned int *, const struct tm *);
size_t wcsftime_l(unsigned int *, size_t, const unsigned int *, const struct tm *, locale_t);
size_t wcslen(const unsigned int *);
int wcsncasecmp(const unsigned int *, const unsigned int *, size_t);
int wcsncasecmp_l(const unsigned int *, const unsigned int *, size_t, locale_t);
unsigned int *wcsncat(unsigned int *, const unsigned int *, size_t);
int wcsncmp(const unsigned int *, const unsigned int *, size_t);
unsigned int *wcsncpy(unsigned int *, const unsigned int *, size_t);
size_t wcsnlen(const unsigned int *, size_t);
size_t wcsnrtombs(char *, const unsigned int **, size_t, size_t, mbstate_t *);
unsigned int *wcspbrk(const unsigned int *, const unsigned int *);
unsigned int *wcsrchr(const unsigned int *, unsigned int);
size_t wcsrtombs(char *, const unsigned int **, size_t, mbstate_t *);
size_t wcsspn(const unsigned int *, const unsigned int *);
unsigned int *wcsstr(const unsigned int *, const unsigned int *);
double wcstod(const unsigned int *, unsigned int **);
float wcstof(const unsigned int *, unsigned int **);
intmax_t wcstoimax(const unsigned int *, unsigned int **, int);
unsigned int *wcstok(unsigned int *, const unsigned int *, unsigned int **);
long wcstol(const unsigned int *, unsigned int **, int);
long double wcstold(const unsigned int *, unsigned int **);
long long wcstoll(const unsigned int *, unsigned int **, int);
size_t wcstombs(char *, const unsigned int *, size_t);
unsigned long wcstoul(const unsigned int *, unsigned int **, int);
unsigned long long wcstoull(const unsigned int *, unsigned int **, int);
uintmax_t wcstoumax(const unsigned int *, unsigned int **, int);
unsigned int *wcswcs(const unsigned int *, const unsigned int *);
int wcswidth(const unsigned int *, size_t);
size_t wcsxfrm(unsigned int *, const unsigned int *, size_t);
size_t wcsxfrm_l(unsigned int *, const unsigned int *, size_t, locale_t);
int wctob(wint_t);
int wctomb(char *, unsigned int);
wctrans_t wctrans(const char *);
wctrans_t wctrans_l(const char *, locale_t);
wctype_t wctype(const char *);
wctype_t wctype_l(const char *, locale_t);
int wcwidth(unsigned int);
unsigned int *wmemchr(const unsigned int *, unsigned int, size_t);
int wmemcmp(const unsigned int *, const unsigned int *, size_t);
unsigned int *wmemcpy(unsigned int *, const unsigned int *, size_t);
unsigned int *wmemmove(unsigned int *, const unsigned int *, size_t);
unsigned int *wmemset(unsigned int *, unsigned int, size_t);
int wordexp(const char *, wordexp_t *, int);
void wordfree(wordexp_t *);
int wprintf(const unsigned int *, ...);
ssize_t write(int, const void *, size_t);
ssize_t writev(int, const struct iovec *, int);
int wscanf(const unsigned int *, ...);
double y0(double);
float y0f(float);
double y1(double);
float y1f(float);
double yn(int, double);
float ynf(int, float);
