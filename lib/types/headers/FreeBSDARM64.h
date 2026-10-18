// The FreeBSD types and prototypes for ARM64 (LP64), as a program declares them
// with FreeBSD's headers.  Generated on 2026-10-18 by
// scripts/generate_freebsd_types.py --target freebsd-arm64 from the headers of
// FreeBSD 15.1-RELEASE (FreeBSD 15, base.txz of
// https://download.freebsd.org/releases/): changed from them, as NeverD reads
// and renders their declarations again.  The headers' notices are in
// LICENSES/freebsd-headers.txt.  Do not edit; generate it again.
// 701 types, 2013 prototypes, rendered by NeverD.
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

struct __timer;
struct __mq;
typedef struct __max_align_t __max_align_t;
typedef union __mbstate_t __mbstate_t;
struct pthread;
struct pthread_attr;
struct pthread_cond;
struct pthread_cond_attr;
struct pthread_mutex;
struct pthread_mutex_attr;
struct pthread_once;
struct pthread_rwlock;
struct pthread_rwlockattr;
struct pthread_barrier;
struct pthread_barrier_attr;
struct pthread_spinlock;
struct pthread_barrierattr;
struct cap_rights;
struct __sigset;
struct timeval;
struct timespec;
struct itimerspec;
typedef struct fd_set fd_set;
struct sigaltstack;
union sigval;
struct sigcontext;
struct sigevent;
struct __siginfo;
struct sigaction;
struct sigvec;
struct sigstack;
struct __sbuf;
struct __sFILE;
typedef struct cookie_io_functions_t cookie_io_functions_t;
typedef struct div_t div_t;
typedef struct ldiv_t ldiv_t;
typedef struct lldiv_t lldiv_t;
struct _xlocale;
struct crypt_data;
struct flock;
struct __oflock;
struct spacectl_range;
typedef struct _RuneEntry _RuneEntry;
typedef struct _RuneRange _RuneRange;
typedef struct _RuneLocale _RuneLocale;
struct tm;
struct lconv;
struct gpregs;
struct fpregs;
struct arm64_reg_context;
struct sve_context;
struct __mcontext;
struct __ucontext;
struct _sigjmp_buf;
struct _jmp_buf;
union __infinity_un;
union __nan_un;
typedef struct imaxdiv_t imaxdiv_t;
struct dirent;
struct _dirdesc;
struct dl_info;
struct __dlfunc_arg;
struct dl_serpath;
struct dl_serinfo;
struct sched_param;
struct _cpuset;
struct _pthread_cleanup_info;
struct umtx;
struct umutex;
struct ucond;
struct urwlock;
struct _usem;
struct _usem2;
struct _umtx_time;
struct _sem;
struct pollfd;
struct termios;
struct winsize;
struct passwd;
struct group;
struct stat;
typedef struct glob_t glob_t;
struct re_guts;
typedef struct regex_t regex_t;
typedef struct regmatch_t regmatch_t;
struct option;
struct __posix_spawnattr;
struct __posix_spawn_file_actions;
struct __tag_iconv_t;
typedef struct iconv_allocation_t iconv_allocation_t;
struct iconv_hooks;
struct iconv_fallbacks;
typedef struct Elf_Note Elf_Note;
typedef struct Elf_GNU_Hash_Header Elf_GNU_Hash_Header;
typedef struct Elf32_Ehdr Elf32_Ehdr;
typedef struct Elf32_Lib Elf32_Lib;
typedef struct Elf32_Shdr Elf32_Shdr;
typedef struct Elf32_Phdr Elf32_Phdr;
typedef struct Elf32_Dyn Elf32_Dyn;
typedef struct Elf32_Rel Elf32_Rel;
typedef struct Elf32_Rela Elf32_Rela;
typedef struct Elf32_Move Elf32_Move;
typedef struct Elf32_Cap Elf32_Cap;
typedef struct Elf32_Sym Elf32_Sym;
typedef struct Elf32_Verdef Elf32_Verdef;
typedef struct Elf32_Verdaux Elf32_Verdaux;
typedef struct Elf32_Verneed Elf32_Verneed;
typedef struct Elf32_Vernaux Elf32_Vernaux;
typedef struct Elf32_Syminfo Elf32_Syminfo;
typedef struct Elf32_Chdr Elf32_Chdr;
typedef struct Elf64_Ehdr Elf64_Ehdr;
typedef struct Elf64_Lib Elf64_Lib;
typedef struct Elf64_Shdr Elf64_Shdr;
typedef struct Elf64_Phdr Elf64_Phdr;
typedef struct Elf64_Dyn Elf64_Dyn;
typedef struct Elf64_Rel Elf64_Rel;
typedef struct Elf64_Rela Elf64_Rela;
typedef struct Elf64_Move Elf64_Move;
typedef struct Elf64_Cap Elf64_Cap;
typedef struct Elf64_Sym Elf64_Sym;
typedef struct Elf64_Verdef Elf64_Verdef;
typedef struct Elf64_Verdaux Elf64_Verdaux;
typedef struct Elf64_Verneed Elf64_Verneed;
typedef struct Elf64_Vernaux Elf64_Vernaux;
typedef struct Elf64_Syminfo Elf64_Syminfo;
typedef struct Elf64_Chdr Elf64_Chdr;
typedef struct Elf32_Auxinfo Elf32_Auxinfo;
typedef struct Elf64_Auxinfo Elf64_Auxinfo;
struct link_map;
struct r_debug;
struct dl_phdr_info;
struct hostent;
struct netent;
struct servent;
struct protoent;
struct sockaddr;
struct addrinfo;
struct ifaddrs;
struct ifmaddrs;
struct entry;
struct __hsearch;
struct hsearch_data;
struct utimbuf;
struct utmpx;
struct timezone;
struct bintime;
struct itimerval;
struct clockinfo;
struct statvfs;
struct tms;
struct rusage;
struct __wrusage;
struct shm_largepage_conf;
struct fiodgname_arg;
struct fiobmap2_arg;
struct iovec;
struct linger;
struct accept_filter_arg;
struct sockproto;
struct sockaddr_storage;
struct msghdr;
struct cmsghdr;
struct cmsgcred;
struct sockcred;
struct sockcred2;
struct sock_timestamp_info;
struct osockaddr;
struct omsghdr;
struct sf_hdtr;
struct mmsghdr;
struct splice;
struct sockaddr_un;
struct kevent;
struct knote;
struct klist;
struct kqueue;
struct kqlist;
struct knlist;
struct inotify_event;
struct rlimit;
struct orlimit;
struct loadavg;
struct utsname;
struct xfile;
struct reg;
struct reg32;
struct fpreg;
struct fpreg32;
struct svereg_header;
struct dbreg;
struct dbreg32;
struct arm64_addr_mask;
struct ptrace_io_desc;
struct ptrace_lwpinfo;
struct ptrace_sc_ret;
struct ptrace_vm_entry;
struct ptrace_coredump;
struct ptrace_sc_remote;
struct ipc_perm;
struct sem;
struct semid_ds;
struct sembuf;
struct shmid_ds;
struct shm_info;
struct msg;
struct msqid_ds;
struct procctl_reaper_status;
struct procctl_reaper_pidinfo;
struct procctl_reaper_pids;
struct procctl_reaper_kill;
struct au_tid;
struct au_tid_addr;
struct au_mask;
struct auditinfo;
struct auditinfo_addr;
struct auditpinfo;
struct auditpinfo_addr;
struct au_session;
struct au_token;
struct au_qctrl;
struct audit_stat;
struct audit_fstat;
struct au_evclass_map;
struct au_evname_map;
struct xucred;
struct mac;
struct setcred;
struct fsid;
struct fid;
struct statfs;
struct fhandle;
struct oexport_args;
struct o2export_args;
struct export_args;
struct mount;
struct nfs_public;
struct vfsops;
struct vfsoptdecl;
struct vfsconf;
struct xvfsconf;
struct ovfsconf;
struct vfsidctl;
struct vfsquery;
struct uio;
struct kld_file_stat;
struct kld_sym_lookup;
struct module;
struct moduledata;
union modspecific;
struct mod_depend;
struct mod_version;
struct mod_metadata;
struct mod_pnp_match_info;
struct module_stat;
struct in_addr;
struct in6_addr;
struct jail;
struct xprison_v1;
struct xprison;
struct sockaddr_in;
struct ip_mreq;
struct ip_mreqn;
struct ip_mreq_source;
struct group_req;
struct group_source_req;
struct __msfilterreq;
struct sockaddr_in6;
struct nhop_object;
struct llentry;
struct route_in6;
struct ipv6_mreq;
struct in6_pktinfo;
struct ip6_mtuinfo;
struct tcphdr;
struct tcp_perf_info;
struct tcp_info;
struct tcp_fastopen;
struct stack_specific_info;
struct tcp_function_set;
struct tcp_snd_req;
union tcp_log_userdata;
struct tcp_log_user;
struct tcp_hybrid_req;
struct if_clonereq;
struct if_data;
struct if_msghdr;
struct if_msghdrl;
struct ifa_msghdr;
struct ifa_msghdrl;
struct ifma_msghdr;
struct if_announcemsghdr;
struct ifreq_buffer;
struct ifreq_nv_req;
struct ifreq;
struct ifaliasreq;
struct oifaliasreq;
struct ifmediareq;
struct ifdrv;
struct ifstat;
struct ifconf;
struct ifg_req;
struct ifgroupreq;
struct ifi2creq;
struct ifrsskey;
struct ifrsshash;
struct ifdownreason;
struct ifnet;
struct if_nameindex;
struct __aiocb_private;
struct aiocb;
struct mq_attr;
struct extent_hooks_s;
struct _ftsent;
typedef struct FTS FTS;
struct FTW;
typedef struct wordexp_t wordexp_t;
struct _property;
struct pidfh;
struct kinfo_file;
struct kinfo_vmentry;
struct kinfo_vmobject;
struct kinfo_proc;
struct nlist;
struct kvm_nlist;
struct __kvm;
struct proc;
struct kvm_swap;
struct kvm_page;
struct MD5Context;
struct SHA256Context;

#pragma neverd_header("pthread.h")
// pthread_mutextype: 4 bytes
enum pthread_mutextype
{
    PTHREAD_MUTEX_ERRORCHECK = 1,
    PTHREAD_MUTEX_RECURSIVE = 2,
    PTHREAD_MUTEX_NORMAL = 3,
    PTHREAD_MUTEX_ADAPTIVE_NP = 4,
    PTHREAD_MUTEX_TYPE_MAX = 5,
};

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
// idtype: 4 bytes
enum idtype
{
    P_PID = 0,
    P_PPID = 1,
    P_PGID = 2,
    P_SID = 3,
    P_CID = 4,
    P_UID = 5,
    P_GID = 6,
    P_ALL = 7,
    P_LWPID = 8,
    P_TASKID = 9,
    P_PROJID = 10,
    P_POOLID = 11,
    P_JAILID = 12,
    P_CTID = 13,
    P_CPUID = 14,
    P_PSETID = 15,
};

#pragma neverd_header("sys/socket.h")
// shutdown_how: 4 bytes
enum shutdown_how
{
    SHUT_RD = 0,
    SHUT_WR = 1,
    SHUT_RDWR = 2,
};

#pragma neverd_header("sys/uio.h")
// uio_rw: 4 bytes
enum uio_rw
{
    UIO_READ = 0,
    UIO_WRITE = 1,
};

// uio_seg: 4 bytes
enum uio_seg
{
    UIO_USERSPACE = 0,
    UIO_SYSSPACE = 1,
    UIO_NOCOPY = 2,
};

#pragma neverd_header("sys/module.h")
// modeventtype: 4 bytes
enum modeventtype
{
    MOD_LOAD = 0,
    MOD_UNLOAD = 1,
    MOD_SHUTDOWN = 2,
    MOD_QUIESCE = 3,
};

#pragma neverd_header("sys/jail.h")
// prison_state: 4 bytes
enum prison_state
{
    PRISON_STATE_INVALID = 0,
    PRISON_STATE_ALIVE = 1,
    PRISON_STATE_DYING = 2,
};

#pragma neverd_header("sys/types.h")
typedef unsigned long __uint64_t;

typedef __uint64_t __size_t;

typedef __size_t size_t;

#pragma neverd_header()
typedef long ptrdiff_t;

typedef __int128 __int128_t;

typedef unsigned __int128 __uint128_t;

#pragma neverd_header("sys/types.h")
typedef signed char __int8_t;

typedef unsigned char __uint8_t;

typedef short __int16_t;

typedef unsigned short __uint16_t;

typedef int __int32_t;

typedef unsigned int __uint32_t;

typedef long __int64_t;

typedef __int8_t __int_least8_t;

typedef __int16_t __int_least16_t;

typedef __int32_t __int_least32_t;

typedef __int64_t __int_least64_t;

typedef __int64_t __intmax_t;

typedef __uint8_t __uint_least8_t;

typedef __uint16_t __uint_least16_t;

typedef __uint32_t __uint_least32_t;

typedef __uint64_t __uint_least64_t;

typedef __uint64_t __uintmax_t;

typedef __int64_t __intptr_t;

typedef __int64_t __intfptr_t;

typedef __uint64_t __uintptr_t;

typedef __uint64_t __uintfptr_t;

typedef __uint64_t __vm_offset_t;

typedef __uint64_t __vm_size_t;

typedef __int64_t __ssize_t;

typedef __int64_t __ptrdiff_t;

typedef __int32_t __clock_t;

typedef __int64_t __critical_t;

typedef double __double_t;

typedef float __float_t;

typedef __int32_t __int_fast8_t;

typedef __int32_t __int_fast16_t;

typedef __int32_t __int_fast32_t;

typedef __int64_t __int_fast64_t;

typedef __int64_t __register_t;

typedef __int64_t __segsz_t;

typedef __int64_t __time_t;

typedef __uint32_t __uint_fast8_t;

typedef __uint32_t __uint_fast16_t;

typedef __uint32_t __uint_fast32_t;

typedef __uint64_t __uint_fast64_t;

typedef __uint64_t __u_register_t;

typedef __uint64_t __vm_paddr_t;

typedef unsigned int ___wchar_t;

typedef __int32_t __blksize_t;

typedef __int64_t __blkcnt_t;

typedef __int32_t __clockid_t;

typedef __uint32_t __fflags_t;

typedef __uint64_t __fsblkcnt_t;

typedef __uint64_t __fsfilcnt_t;

typedef __uint32_t __gid_t;

typedef __int64_t __id_t;

typedef __uint64_t __ino_t;

typedef long __key_t;

typedef __int32_t __lwpid_t;

typedef __uint16_t __mode_t;

typedef int __accmode_t;

typedef int __nl_item;

typedef __uint64_t __nlink_t;

typedef __int64_t __off_t;

typedef __int64_t __off64_t;

typedef __int32_t __pid_t;

typedef __int64_t __sbintime_t;

typedef __int64_t __rlim_t;

typedef __uint8_t __sa_family_t;

typedef __uint32_t __socklen_t;

typedef long __suseconds_t;

#pragma neverd_header()
// Declared only: its members are unknown.
struct __timer;

#pragma neverd_header("sys/types.h")
typedef struct __timer *__timer_t;

#pragma neverd_header()
// Declared only: its members are unknown.
struct __mq;

#pragma neverd_header("sys/types.h")
typedef struct __mq *__mqd_t;

typedef __uint32_t __uid_t;

typedef unsigned int __useconds_t;

typedef int __cpuwhich_t;

typedef int __cpulevel_t;

typedef int __cpusetid_t;

typedef __int64_t __daddr_t;

typedef int __ct_rune_t;

typedef __ct_rune_t __rune_t;

typedef __ct_rune_t __wint_t;

typedef __uint_least16_t __char16_t;

typedef __uint_least32_t __char32_t;

// __max_align_t: 0x20 (32) bytes
struct __max_align_t
{
    long long __max_align1;
    long double __max_align2;
};

typedef __uint32_t __acl_tag_t;

typedef __uint32_t __acl_perm_t;

typedef __uint16_t __acl_entry_type_t;

typedef __uint16_t __acl_flag_t;

typedef __uint32_t __acl_type_t;

typedef __uint32_t *__acl_permset_t;

typedef __uint16_t *__acl_flagset_t;

typedef __uint64_t __dev_t;

typedef __uint32_t __fixpt_t;

// __mbstate_t: 0x80 (128) bytes
union __mbstate_t
{
    char __mbstate8[128];
    __int64_t _mbstateL;
};

typedef __uintmax_t __rman_res_t;

#pragma neverd_header()
// Declared only: its members are unknown.
struct pthread;

// Declared only: its members are unknown.
struct pthread_attr;

// Declared only: its members are unknown.
struct pthread_cond;

// Declared only: its members are unknown.
struct pthread_cond_attr;

// Declared only: its members are unknown.
struct pthread_mutex;

// Declared only: its members are unknown.
struct pthread_mutex_attr;

#pragma neverd_header("sys/types.h")
typedef struct pthread_mutex *pthread_mutex_t;

// pthread_once: 0x10 (16) bytes
struct pthread_once
{
    int state;
    pthread_mutex_t mutex;
};

#pragma neverd_header()
// Declared only: its members are unknown.
struct pthread_rwlock;

// Declared only: its members are unknown.
struct pthread_rwlockattr;

// Declared only: its members are unknown.
struct pthread_barrier;

// Declared only: its members are unknown.
struct pthread_barrier_attr;

// Declared only: its members are unknown.
struct pthread_spinlock;

#pragma neverd_header("sys/types.h")
typedef struct pthread *pthread_t;

typedef struct pthread_attr *pthread_attr_t;

typedef struct pthread_mutex_attr *pthread_mutexattr_t;

typedef struct pthread_cond *pthread_cond_t;

typedef struct pthread_cond_attr *pthread_condattr_t;

typedef int pthread_key_t;

typedef struct pthread_once pthread_once_t;

typedef struct pthread_rwlock *pthread_rwlock_t;

typedef struct pthread_rwlockattr *pthread_rwlockattr_t;

typedef struct pthread_barrier *pthread_barrier_t;

#pragma neverd_header()
// Declared only: its members are unknown.
struct pthread_barrierattr;

#pragma neverd_header("sys/types.h")
typedef struct pthread_barrierattr *pthread_barrierattr_t;

typedef struct pthread_spinlock *pthread_spinlock_t;

typedef void *pthread_addr_t;

typedef void *(*pthread_startroutine_t)(void *);

typedef unsigned char u_char;

typedef unsigned short u_short;

typedef unsigned int u_int;

typedef unsigned long u_long;

typedef unsigned short ushort;

typedef unsigned int uint;

typedef __intptr_t intptr_t;

typedef __uintptr_t uintptr_t;

typedef __intmax_t intmax_t;

typedef __uintmax_t uintmax_t;

typedef __uint8_t u_int8_t;

typedef __uint16_t u_int16_t;

typedef __uint32_t u_int32_t;

typedef __uint64_t u_int64_t;

typedef __uint64_t u_quad_t;

typedef __int64_t quad_t;

typedef quad_t *qaddr_t;

typedef char *caddr_t;

typedef const char *c_caddr_t;

typedef __blksize_t blksize_t;

typedef __cpuwhich_t cpuwhich_t;

typedef __cpulevel_t cpulevel_t;

typedef __cpusetid_t cpusetid_t;

typedef __blkcnt_t blkcnt_t;

typedef __clock_t clock_t;

typedef __clockid_t clockid_t;

typedef __critical_t critical_t;

typedef __daddr_t daddr_t;

typedef __dev_t dev_t;

typedef __fflags_t fflags_t;

typedef __fixpt_t fixpt_t;

typedef __fsblkcnt_t fsblkcnt_t;

typedef __fsfilcnt_t fsfilcnt_t;

typedef __gid_t gid_t;

typedef __uint32_t in_addr_t;

typedef __uint16_t in_port_t;

typedef __id_t id_t;

typedef __ino_t ino_t;

typedef __key_t key_t;

typedef __lwpid_t lwpid_t;

typedef __mode_t mode_t;

typedef __accmode_t accmode_t;

typedef __nlink_t nlink_t;

typedef __off_t off_t;

typedef __off64_t off64_t;

typedef __pid_t pid_t;

typedef __register_t register_t;

typedef __rlim_t rlim_t;

typedef __sbintime_t sbintime_t;

typedef __segsz_t segsz_t;

typedef __ssize_t ssize_t;

typedef __suseconds_t suseconds_t;

typedef __time_t time_t;

typedef __timer_t timer_t;

typedef __mqd_t mqd_t;

typedef __u_register_t u_register_t;

typedef __uid_t uid_t;

typedef __useconds_t useconds_t;

typedef unsigned long cap_ioctl_t;

#pragma neverd_header("sys/capsicum.h")
// cap_rights: 0x10 (16) bytes
struct cap_rights
{
    unsigned long long cr_rights[2];
};

#pragma neverd_header("sys/types.h")
typedef struct cap_rights cap_rights_t;

typedef __uint64_t kpaddr_t;

typedef __uint64_t kvaddr_t;

typedef __uint64_t ksize_t;

typedef __int64_t kssize_t;

typedef __vm_offset_t vm_offset_t;

typedef __uint64_t vm_ooffset_t;

typedef __vm_paddr_t vm_paddr_t;

typedef __uint64_t vm_pindex_t;

typedef __vm_size_t vm_size_t;

typedef __rman_res_t rman_res_t;

typedef __register_t syscallarg_t;

// __sigset: 0x10 (16) bytes
struct __sigset
{
    __uint32_t __bits[4];
};

typedef struct __sigset __sigset_t;

// timeval: 0x10 (16) bytes
struct timeval
{
    time_t tv_sec;
    suseconds_t tv_usec;
};

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

typedef unsigned long __fd_mask;

typedef __fd_mask fd_mask;

typedef __sigset_t sigset_t;

// fd_set: 0x80 (128) bytes
struct fd_set
{
    __fd_mask __fds_bits[16];
};

#pragma neverd_header("sys/param.h")
// sigaltstack: 0x18 (24) bytes
struct sigaltstack
{
    void *ss_sp;
    __size_t ss_size;
    int ss_flags;
};

typedef struct sigaltstack stack_t;

// sigval: 0x8 (8) bytes
union sigval
{
    int sival_int;
    void *sival_ptr;
    int sigval_int;
    void *sigval_ptr;
};

typedef long sig_atomic_t;

// sigcontext: 0x4 (4) bytes
struct sigcontext
{
    int _dummy;
};

typedef void __sighandler_t(int);

// sigevent: 0x50 (80) bytes
struct sigevent
{
    int sigev_notify;
    int sigev_signo;
    union sigval sigev_value;
    union
    {
        __lwpid_t _threadid;
        struct
        {
            void (*_function)(union sigval);
            struct pthread_attr **_attribute;
        } _sigev_thread;
        unsigned short _kevent_flags;
        long __spare__[8];
    } _sigev_un;
};

// __siginfo: 0x50 (80) bytes
struct __siginfo
{
    int si_signo;
    int si_errno;
    int si_code;
    __pid_t si_pid;
    __uid_t si_uid;
    int si_status;
    void *si_addr;
    union sigval si_value;
    union
    {
        struct
        {
            int _trapno;
        } _fault;
        struct
        {
            int _timerid;
            int _overrun;
        } _timer;
        struct
        {
            int _mqd;
        } _mesgq;
        struct
        {
            long _band;
        } _poll;
        struct
        {
            int _syscall;
        } _capsicum;
        struct
        {
            long __spare1__;
            int __spare2__[7];
        } __spare__;
    } _reason;
};

typedef struct __siginfo siginfo_t;

// sigaction: 0x20 (32) bytes
struct sigaction
{
    union
    {
        void (*__sa_handler)(int);
        void (*__sa_sigaction)(int, struct __siginfo *, void *);
    } __sigaction_u;
    int sa_flags;
    sigset_t sa_mask;
};

typedef __sighandler_t *sig_t;

typedef void __siginfohandler_t(int, struct __siginfo *, void *);

// sigvec: 0x10 (16) bytes
struct sigvec
{
    __sighandler_t *sv_handler;
    int sv_mask;
    int sv_flags;
};

// sigstack: 0x10 (16) bytes
struct sigstack
{
    void *ss_sp;
    int ss_onstack;
};

#pragma neverd_header("stdio.h")
typedef __off_t fpos_t;

typedef size_t rsize_t;

// __sbuf: 0x10 (16) bytes
struct __sbuf
{
    unsigned char *_base;
    int _size;
};

// __sFILE: 0x138 (312) bytes
struct __sFILE
{
    unsigned char *_p;
    int _r;
    int _w;
    short _flags;
    short _file;
    struct __sbuf _bf;
    int _lbfsize;
    void *_cookie;
    int (*_close)(void *);
    int (*_read)(void *, char *, int);
    fpos_t (*_seek)(void *, fpos_t, int);
    int (*_write)(void *, const char *, int);
    struct __sbuf _ub;
    unsigned char *_up;
    int _ur;
    unsigned char _ubuf[3];
    unsigned char _nbuf[1];
    struct __sbuf _lb;
    int _blksize;
    fpos_t _offset;
    struct pthread_mutex *_fl_mutex;
    struct pthread *_fl_owner;
    int _fl_count;
    int _orientation;
    __mbstate_t _mbstate;
    int _flags2;
};

typedef struct __sFILE FILE;

typedef __ssize_t cookie_read_function_t(void *, char *, size_t);

typedef __ssize_t cookie_write_function_t(void *, const char *, size_t);

typedef int cookie_seek_function_t(void *, off64_t *, int);

typedef int cookie_close_function_t(void *);

// cookie_io_functions_t: 0x20 (32) bytes
struct cookie_io_functions_t
{
    cookie_read_function_t *read;
    cookie_write_function_t *write;
    cookie_seek_function_t *seek;
    cookie_close_function_t *close;
};

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

typedef int errno_t;

typedef void (*constraint_handler_t)(const char *, void *, errno_t);

#pragma neverd_header()
// Declared only: its members are unknown.
struct _xlocale;

#pragma neverd_header("string.h")
typedef struct _xlocale *locale_t;

#pragma neverd_header("unistd.h")
// crypt_data: 0x104 (260) bytes
struct crypt_data
{
    int initialized;
    char __buf[256];
};

#pragma neverd_header("fcntl.h")
// flock: 0x20 (32) bytes
struct flock
{
    off_t l_start;
    off_t l_len;
    pid_t l_pid;
    short l_type;
    short l_whence;
    int l_sysid;
};

// __oflock: 0x18 (24) bytes
struct __oflock
{
    off_t l_start;
    off_t l_len;
    pid_t l_pid;
    short l_type;
    short l_whence;
};

// spacectl_range: 0x10 (16) bytes
struct spacectl_range
{
    off_t r_offset;
    off_t r_len;
};

#pragma neverd_header("ctype.h")
// _RuneEntry: 0x18 (24) bytes
struct _RuneEntry
{
    __rune_t __min;
    __rune_t __max;
    __rune_t __map;
    unsigned long *__types;
};

// _RuneRange: 0x10 (16) bytes
struct _RuneRange
{
    int __nranges;
    _RuneEntry *__ranges;
};

// _RuneLocale: 0x1080 (4224) bytes
struct _RuneLocale
{
    char __magic[8];
    char __encoding[32];
    __rune_t (*__sgetrune)(const char *, __size_t, const char **);
    int (*__sputrune)(__rune_t, char *, __size_t, char **);
    __rune_t __invalid_rune;
    unsigned long __runetype[256];
    __rune_t __maplower[256];
    __rune_t __mapupper[256];
    _RuneRange __runetype_ext;
    _RuneRange __maplower_ext;
    _RuneRange __mapupper_ext;
    void *__variable;
    int __variable_len;
};

#pragma neverd_header("wchar.h")
typedef __mbstate_t mbstate_t;

typedef __wint_t wint_t;

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
    char *tm_zone;
};

#pragma neverd_header("wctype.h")
typedef int wctrans_t;

typedef unsigned long wctype_t;

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
    char int_n_cs_precedes;
    char int_p_sep_by_space;
    char int_n_sep_by_space;
    char int_p_sign_posn;
    char int_n_sign_posn;
};

#pragma neverd_header("signal.h")
// gpregs: 0x110 (272) bytes
struct gpregs
{
    __register_t gp_x[30];
    __register_t gp_lr;
    __register_t gp_sp;
    __register_t gp_elr;
    __uint64_t gp_spsr;
};

// fpregs: 0x210 (528) bytes
struct fpregs
{
    __uint128_t fp_q[32];
    __uint32_t fp_sr;
    __uint32_t fp_cr;
    int fp_flags;
    int fp_pad;
};

// arm64_reg_context: 0x8 (8) bytes
struct arm64_reg_context
{
    __uint32_t ctx_id;
    __uint32_t ctx_size;
};

// sve_context: 0x10 (16) bytes
struct sve_context
{
    struct arm64_reg_context sve_ctx;
    __uint16_t sve_vector_len;
    __uint16_t sve_flags;
    __uint16_t sve_reserved[2];
};

// __mcontext: 0x370 (880) bytes
struct __mcontext
{
    struct gpregs mc_gpregs;
    struct fpregs mc_fpregs;
    int mc_flags;
    int mc_pad;
    __uint64_t mc_ptr;
    __uint64_t mc_spare[7];
};

typedef struct __mcontext mcontext_t;

// __ucontext: 0x3C0 (960) bytes
struct __ucontext
{
    __sigset_t uc_sigmask;
    mcontext_t uc_mcontext;
    struct __ucontext *uc_link;
    struct sigaltstack uc_stack;
    int uc_flags;
    int __spare__[4];
};

typedef struct __ucontext ucontext_t;

typedef struct pthread *__pthread_t;

#pragma neverd_header("setjmp.h")
// _sigjmp_buf: 0x200 (512) bytes
struct _sigjmp_buf
{
    __int128_t _sjb[32];
};

typedef struct _sigjmp_buf sigjmp_buf[1];

// _jmp_buf: 0x200 (512) bytes
struct _jmp_buf
{
    __int128_t _jb[32];
};

typedef struct _jmp_buf jmp_buf[1];

#pragma neverd_header("math.h")
// __infinity_un: 0x8 (8) bytes
union __infinity_un
{
    unsigned char __uc[8];
    double __ud;
};

// __nan_un: 0x4 (4) bytes
union __nan_un
{
    unsigned char __uc[4];
    float __uf;
};

typedef __double_t double_t;

typedef __float_t float_t;

#pragma neverd_header("fenv.h")
typedef __uint64_t fenv_t;

typedef __uint64_t fexcept_t;

#pragma neverd_header("inttypes.h")
typedef __int_least8_t int_least8_t;

typedef __int_least16_t int_least16_t;

typedef __int_least32_t int_least32_t;

typedef __int_least64_t int_least64_t;

typedef __uint_least8_t uint_least8_t;

typedef __uint_least16_t uint_least16_t;

typedef __uint_least32_t uint_least32_t;

typedef __uint_least64_t uint_least64_t;

typedef __int_fast8_t int_fast8_t;

typedef __int_fast16_t int_fast16_t;

typedef __int_fast32_t int_fast32_t;

typedef __int_fast64_t int_fast64_t;

typedef __uint_fast8_t uint_fast8_t;

typedef __uint_fast16_t uint_fast16_t;

typedef __uint_fast32_t uint_fast32_t;

typedef __uint_fast64_t uint_fast64_t;

// imaxdiv_t: 0x10 (16) bytes
struct imaxdiv_t
{
    intmax_t quot;
    intmax_t rem;
};

#pragma neverd_header("dirent.h")
// dirent: 0x118 (280) bytes
struct dirent
{
    ino_t d_fileno;
    off_t d_off;
    __uint16_t d_reclen;
    __uint8_t d_type;
    __uint8_t d_pad0;
    __uint16_t d_namlen;
    __uint16_t d_pad1;
    char d_name[256];
};

#pragma neverd_header()
// Declared only: its members are unknown.
struct _dirdesc;

#pragma neverd_header("dirent.h")
typedef struct _dirdesc DIR;

#pragma neverd_header("dlfcn.h")
// dl_info: 0x20 (32) bytes
struct dl_info
{
    const char *dli_fname;
    void *dli_fbase;
    const char *dli_sname;
    void *dli_saddr;
};

typedef struct dl_info Dl_info;

// __dlfunc_arg: 0x4 (4) bytes
struct __dlfunc_arg
{
    int __dlfunc_dummy;
};

typedef void (*dlfunc_t)(struct __dlfunc_arg);

// dl_serpath: 0x10 (16) bytes
struct dl_serpath
{
    char *dls_name;
    unsigned int dls_flags;
};

typedef struct dl_serpath Dl_serpath;

// dl_serinfo: 0x20 (32) bytes
struct dl_serinfo
{
    size_t dls_size;
    unsigned int dls_cnt;
    Dl_serpath dls_serpath[1];
};

typedef struct dl_serinfo Dl_serinfo;

#pragma neverd_header("pthread.h")
// sched_param: 0x4 (4) bytes
struct sched_param
{
    int sched_priority;
};

// _cpuset: 0x80 (128) bytes
struct _cpuset
{
    unsigned long __bits[16];
};

typedef struct _cpuset cpuset_t;

typedef struct _cpuset cpu_set_t;

// _pthread_cleanup_info: 0x40 (64) bytes
struct _pthread_cleanup_info
{
    __uintptr_t pthread_cleanup_pad[8];
};

#pragma neverd_header("semaphore.h")
// umtx: 0x8 (8) bytes
struct umtx
{
    volatile unsigned long u_owner;
};

// umutex: 0x20 (32) bytes
struct umutex
{
    volatile __lwpid_t m_owner;
    __uint32_t m_flags;
    __uint32_t m_ceilings[2];
    __uintptr_t m_rb_lnk;
    __uint32_t m_spare[2];
};

// ucond: 0x10 (16) bytes
struct ucond
{
    volatile __uint32_t c_has_waiters;
    __uint32_t c_flags;
    __uint32_t c_clockid;
    __uint32_t c_spare[1];
};

// urwlock: 0x20 (32) bytes
struct urwlock
{
    volatile __int32_t rw_state;
    __uint32_t rw_flags;
    __uint32_t rw_blocked_readers;
    __uint32_t rw_blocked_writers;
    __uint32_t rw_spare[4];
};

// _usem: 0xC (12) bytes
struct _usem
{
    volatile __uint32_t _has_waiters;
    volatile __uint32_t _count;
    __uint32_t _flags;
};

// _usem2: 0x8 (8) bytes
struct _usem2
{
    volatile __uint32_t _count;
    __uint32_t _flags;
};

// _umtx_time: 0x18 (24) bytes
struct _umtx_time
{
    struct timespec _timeout;
    __uint32_t _flags;
    __uint32_t _clockid;
};

// _sem: 0x10 (16) bytes
struct _sem
{
    __uint32_t _magic;
    struct _usem2 _kern;
    __uint32_t _padding;
};

typedef struct _sem sem_t;

#pragma neverd_header("poll.h")
typedef unsigned int nfds_t;

// pollfd: 0x8 (8) bytes
struct pollfd
{
    int fd;
    short events;
    short revents;
};

#pragma neverd_header("termios.h")
typedef unsigned int tcflag_t;

typedef unsigned char cc_t;

typedef unsigned int speed_t;

// termios: 0x2C (44) bytes
struct termios
{
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_cc[20];
    speed_t c_ispeed;
    speed_t c_ospeed;
};

// winsize: 0x8 (8) bytes
struct winsize
{
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

#pragma neverd_header("pwd.h")
// passwd: 0x50 (80) bytes
struct passwd
{
    char *pw_name;
    char *pw_passwd;
    uid_t pw_uid;
    gid_t pw_gid;
    time_t pw_change;
    char *pw_class;
    char *pw_gecos;
    char *pw_dir;
    char *pw_shell;
    time_t pw_expire;
    int pw_fields;
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

#pragma neverd_header("sys/stat.h")
// stat: 0xE0 (224) bytes
struct stat
{
    dev_t st_dev;
    ino_t st_ino;
    nlink_t st_nlink;
    mode_t st_mode;
    __int16_t st_bsdflags;
    uid_t st_uid;
    gid_t st_gid;
    __int32_t st_padding1;
    dev_t st_rdev;
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
    struct timespec st_birthtim;
    off_t st_size;
    blkcnt_t st_blocks;
    blksize_t st_blksize;
    fflags_t st_flags;
    __uint64_t st_gen;
    __uint64_t st_filerev;
    __uint64_t st_spare[9];
};

#pragma neverd_header("glob.h")
// glob_t: 0x58 (88) bytes
struct glob_t
{
    size_t gl_pathc;
    size_t gl_matchc;
    size_t gl_offs;
    int gl_flags;
    char **gl_pathv;
    union
    {
        int (*gl_errfunc)(const char *, int);
        void *gl_errblk;
    };
    void (*gl_closedir)(void *);
    struct dirent *(*gl_readdir)(void *);
    void *(*gl_opendir)(const char *);
    int (*gl_lstat)(const char *, struct stat *);
    int (*gl_stat)(const char *, struct stat *);
};

#pragma neverd_header("regex.h")
typedef __off_t regoff_t;

#pragma neverd_header()
// Declared only: its members are unknown.
struct re_guts;

#pragma neverd_header("regex.h")
// regex_t: 0x20 (32) bytes
struct regex_t
{
    int re_magic;
    size_t re_nsub;
    const char *re_endp;
    struct re_guts *re_g;
};

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

#pragma neverd_header()
// Declared only: its members are unknown.
struct __posix_spawnattr;

#pragma neverd_header("spawn.h")
typedef struct __posix_spawnattr *posix_spawnattr_t;

#pragma neverd_header()
// Declared only: its members are unknown.
struct __posix_spawn_file_actions;

#pragma neverd_header("spawn.h")
typedef struct __posix_spawn_file_actions *posix_spawn_file_actions_t;

#pragma neverd_header("iconv.h")
typedef bool __iconv_bool;

#pragma neverd_header()
// Declared only: its members are unknown.
struct __tag_iconv_t;

#pragma neverd_header("iconv.h")
typedef struct __tag_iconv_t *iconv_t;

// iconv_allocation_t: 0x200 (512) bytes
struct iconv_allocation_t
{
    void *spaceholder[64];
};

typedef void (*iconv_unicode_char_hook)(unsigned int mbr, void *data);

typedef void (*iconv_wide_char_hook)(unsigned int wc, void *data);

// iconv_hooks: 0x18 (24) bytes
struct iconv_hooks
{
    iconv_unicode_char_hook uc_hook;
    iconv_wide_char_hook wc_hook;
    void *data;
};

typedef void (*iconv_unicode_mb_to_uc_fallback)(const char *, size_t, void (*write_replacement)(const unsigned int *, size_t, void *), void *, void *);

typedef void (*iconv_unicode_uc_to_mb_fallback)(unsigned int, void (*write_replacement)(const char *, size_t, void *), void *, void *);

typedef void (*iconv_wchar_mb_to_wc_fallback)(const char *, size_t, void (*write_replacement)(const unsigned int *, size_t, void *), void *, void *);

typedef void (*iconv_wchar_wc_to_mb_fallback)(unsigned int, void (*write_replacement)(const char *, size_t, void *), void *, void *);

// iconv_fallbacks: 0x28 (40) bytes
struct iconv_fallbacks
{
    iconv_unicode_mb_to_uc_fallback mb_to_uc_fallback;
    iconv_unicode_uc_to_mb_fallback uc_to_mb_fallback;
    iconv_wchar_mb_to_wc_fallback mb_to_wc_fallback;
    iconv_wchar_wc_to_mb_fallback wc_to_mb_fallback;
    void *data;
};

#pragma neverd_header("langinfo.h")
typedef __nl_item nl_item;

#pragma neverd_header("link.h")
// Elf_Note: 0xC (12) bytes
struct Elf_Note
{
    u_int32_t n_namesz;
    u_int32_t n_descsz;
    u_int32_t n_type;
};

typedef Elf_Note Elf_Nhdr;

// Elf_GNU_Hash_Header: 0x10 (16) bytes
struct Elf_GNU_Hash_Header
{
    u_int32_t gh_nbuckets;
    u_int32_t gh_symndx;
    u_int32_t gh_maskwords;
    u_int32_t gh_shift2;
};

typedef unsigned int Elf32_Addr;

typedef unsigned short Elf32_Half;

typedef unsigned int Elf32_Off;

typedef int Elf32_Sword;

typedef unsigned int Elf32_Word;

typedef unsigned long long Elf32_Lword;

typedef Elf32_Word Elf32_Hashelt;

typedef Elf32_Word Elf32_Size;

typedef Elf32_Sword Elf32_Ssize;

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

// Elf32_Lib: 0x14 (20) bytes
struct Elf32_Lib
{
    Elf32_Word l_name;
    Elf32_Word l_time_stamp;
    Elf32_Word l_checksum;
    Elf32_Word l_version;
    Elf32_Word l_flags;
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

// Elf32_Rel: 0x8 (8) bytes
struct Elf32_Rel
{
    Elf32_Addr r_offset;
    Elf32_Word r_info;
};

// Elf32_Rela: 0xC (12) bytes
struct Elf32_Rela
{
    Elf32_Addr r_offset;
    Elf32_Word r_info;
    Elf32_Sword r_addend;
};

typedef Elf32_Word Elf32_Relr;

typedef Elf_Note Elf32_Nhdr;

// Elf32_Move: 0x18 (24) bytes
struct Elf32_Move
{
    Elf32_Lword m_value;
    Elf32_Word m_info;
    Elf32_Word m_poffset;
    Elf32_Half m_repeat;
    Elf32_Half m_stride;
};

// Elf32_Cap: 0x8 (8) bytes
struct Elf32_Cap
{
    Elf32_Word c_tag;
    union
    {
        Elf32_Word c_val;
        Elf32_Addr c_ptr;
    } c_un;
};

// Elf32_Sym: 0x10 (16) bytes
struct Elf32_Sym
{
    Elf32_Word st_name;
    Elf32_Addr st_value;
    Elf32_Word st_size;
    unsigned char st_info;
    unsigned char st_other;
    Elf32_Half st_shndx;
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

// Elf32_Verdaux: 0x8 (8) bytes
struct Elf32_Verdaux
{
    Elf32_Word vda_name;
    Elf32_Word vda_next;
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

// Elf32_Vernaux: 0x10 (16) bytes
struct Elf32_Vernaux
{
    Elf32_Word vna_hash;
    Elf32_Half vna_flags;
    Elf32_Half vna_other;
    Elf32_Word vna_name;
    Elf32_Word vna_next;
};

typedef Elf32_Half Elf32_Versym;

// Elf32_Syminfo: 0x4 (4) bytes
struct Elf32_Syminfo
{
    Elf32_Half si_boundto;
    Elf32_Half si_flags;
};

// Elf32_Chdr: 0xC (12) bytes
struct Elf32_Chdr
{
    Elf32_Word ch_type;
    Elf32_Word ch_size;
    Elf32_Word ch_addralign;
};

typedef unsigned long long Elf64_Addr;

typedef unsigned short Elf64_Half;

typedef unsigned long long Elf64_Off;

typedef int Elf64_Sword;

typedef long long Elf64_Sxword;

typedef unsigned int Elf64_Word;

typedef unsigned long long Elf64_Lword;

typedef unsigned long long Elf64_Xword;

typedef Elf64_Word Elf64_Hashelt;

typedef Elf64_Xword Elf64_Size;

typedef Elf64_Sxword Elf64_Ssize;

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

// Elf64_Lib: 0x14 (20) bytes
struct Elf64_Lib
{
    Elf64_Word l_name;
    Elf64_Word l_time_stamp;
    Elf64_Word l_checksum;
    Elf64_Word l_version;
    Elf64_Word l_flags;
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

// Elf64_Rel: 0x10 (16) bytes
struct Elf64_Rel
{
    Elf64_Addr r_offset;
    Elf64_Xword r_info;
};

// Elf64_Rela: 0x18 (24) bytes
struct Elf64_Rela
{
    Elf64_Addr r_offset;
    Elf64_Xword r_info;
    Elf64_Sxword r_addend;
};

typedef Elf64_Xword Elf64_Relr;

typedef Elf_Note Elf64_Nhdr;

// Elf64_Move: 0x20 (32) bytes
struct Elf64_Move
{
    Elf64_Lword m_value;
    Elf64_Xword m_info;
    Elf64_Xword m_poffset;
    Elf64_Half m_repeat;
    Elf64_Half m_stride;
};

// Elf64_Cap: 0x10 (16) bytes
struct Elf64_Cap
{
    Elf64_Xword c_tag;
    union
    {
        Elf64_Xword c_val;
        Elf64_Addr c_ptr;
    } c_un;
};

// Elf64_Sym: 0x18 (24) bytes
struct Elf64_Sym
{
    Elf64_Word st_name;
    unsigned char st_info;
    unsigned char st_other;
    Elf64_Half st_shndx;
    Elf64_Addr st_value;
    Elf64_Xword st_size;
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

// Elf64_Verdaux: 0x8 (8) bytes
struct Elf64_Verdaux
{
    Elf64_Word vda_name;
    Elf64_Word vda_next;
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

// Elf64_Vernaux: 0x10 (16) bytes
struct Elf64_Vernaux
{
    Elf64_Word vna_hash;
    Elf64_Half vna_flags;
    Elf64_Half vna_other;
    Elf64_Word vna_name;
    Elf64_Word vna_next;
};

typedef Elf64_Half Elf64_Versym;

// Elf64_Syminfo: 0x4 (4) bytes
struct Elf64_Syminfo
{
    Elf64_Half si_boundto;
    Elf64_Half si_flags;
};

// Elf64_Chdr: 0x18 (24) bytes
struct Elf64_Chdr
{
    Elf64_Word ch_type;
    Elf64_Word ch_reserved;
    Elf64_Xword ch_size;
    Elf64_Xword ch_addralign;
};

typedef Elf64_Addr Elf_Addr;

typedef Elf64_Half Elf_Half;

typedef Elf64_Off Elf_Off;

typedef Elf64_Sword Elf_Sword;

typedef Elf64_Word Elf_Word;

typedef Elf64_Ehdr Elf_Ehdr;

typedef Elf64_Shdr Elf_Shdr;

typedef Elf64_Phdr Elf_Phdr;

typedef Elf64_Dyn Elf_Dyn;

typedef Elf64_Rel Elf_Rel;

typedef Elf64_Rela Elf_Rela;

typedef Elf64_Relr Elf_Relr;

typedef Elf64_Sym Elf_Sym;

typedef Elf64_Verdef Elf_Verdef;

typedef Elf64_Verdaux Elf_Verdaux;

typedef Elf64_Verneed Elf_Verneed;

typedef Elf64_Vernaux Elf_Vernaux;

typedef Elf64_Versym Elf_Versym;

typedef Elf64_Hashelt Elf_Hashelt;

typedef Elf64_Size Elf_Size;

typedef Elf64_Ssize Elf_Ssize;

// Elf32_Auxinfo: 0x8 (8) bytes
struct Elf32_Auxinfo
{
    int a_type;
    union
    {
        int a_val;
    } a_un;
};

// Elf64_Auxinfo: 0x10 (16) bytes
struct Elf64_Auxinfo
{
    long a_type;
    union
    {
        long a_val;
        void *a_ptr;
        void (*a_fcn)(void);
    } a_un;
};

typedef Elf64_Auxinfo Elf_Auxinfo;

// link_map: 0x38 (56) bytes
struct link_map
{
    caddr_t l_base;
    const char *l_name;
    const void *l_ld;
    struct link_map *l_next;
    struct link_map *l_prev;
    caddr_t l_addr;
    const char *l_refname;
};

typedef struct link_map Link_map;

// r_debug: 0x28 (40) bytes
struct r_debug
{
    int r_version;
    struct link_map *r_map;
    void (*r_brk)(struct r_debug *, struct link_map *);
    enum
    {
        RT_CONSISTENT = 0,
        RT_ADD = 1,
        RT_DELETE = 2,
    } r_state;
    void *r_ldbase;
};

// dl_phdr_info: 0x40 (64) bytes
struct dl_phdr_info
{
    Elf_Addr dlpi_addr;
    const char *dlpi_name;
    const Elf_Phdr *dlpi_phdr;
    Elf_Half dlpi_phnum;
    unsigned long long dlpi_adds;
    unsigned long long dlpi_subs;
    size_t dlpi_tls_modid;
    void *dlpi_tls_data;
};

typedef int (*__dl_iterate_hdr_callback)(struct dl_phdr_info *, size_t, void *);

#pragma neverd_header("netdb.h")
typedef __socklen_t socklen_t;

// hostent: 0x20 (32) bytes
struct hostent
{
    char *h_name;
    char **h_aliases;
    int h_addrtype;
    int h_length;
    char **h_addr_list;
};

// netent: 0x18 (24) bytes
struct netent
{
    char *n_name;
    char **n_aliases;
    int n_addrtype;
    unsigned int n_net;
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

#pragma neverd_header("sys/socket.h")
typedef __sa_family_t sa_family_t;

// sockaddr: 0x10 (16) bytes
struct sockaddr
{
    unsigned char sa_len;
    sa_family_t sa_family;
    char sa_data[14];
};

#pragma neverd_header("netdb.h")
// addrinfo: 0x30 (48) bytes
struct addrinfo
{
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    socklen_t ai_addrlen;
    char *ai_canonname;
    struct sockaddr *ai_addr;
    struct addrinfo *ai_next;
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
    struct sockaddr *ifa_dstaddr;
    void *ifa_data;
};

// ifmaddrs: 0x20 (32) bytes
struct ifmaddrs
{
    struct ifmaddrs *ifma_next;
    struct sockaddr *ifma_name;
    struct sockaddr *ifma_addr;
    struct sockaddr *ifma_lladdr;
};

#pragma neverd_header("search.h")
// entry: 0x10 (16) bytes
struct entry
{
    char *key;
    void *data;
};

typedef struct entry ENTRY;

typedef void posix_tnode;

#pragma neverd_header()
// Declared only: its members are unknown.
struct __hsearch;

#pragma neverd_header("search.h")
// hsearch_data: 0x8 (8) bytes
struct hsearch_data
{
    struct __hsearch *__hsearch;
};

#pragma neverd_header("utime.h")
// utimbuf: 0x10 (16) bytes
struct utimbuf
{
    time_t actime;
    time_t modtime;
};

#pragma neverd_header("utmpx.h")
// utmpx: 0x118 (280) bytes
struct utmpx
{
    short ut_type;
    struct timeval ut_tv;
    char ut_id[8];
    pid_t ut_pid;
    char ut_user[32];
    char ut_line[16];
    char ut_host[128];
    char __ut_spare[64];
};

#pragma neverd_header("sys/stat.h")
// timezone: 0x8 (8) bytes
struct timezone
{
    int tz_minuteswest;
    int tz_dsttime;
};

// bintime: 0x10 (16) bytes
struct bintime
{
    time_t sec;
    unsigned long long frac;
};

// itimerval: 0x20 (32) bytes
struct itimerval
{
    struct timeval it_interval;
    struct timeval it_value;
};

// clockinfo: 0x14 (20) bytes
struct clockinfo
{
    int hz;
    int tick;
    int spare;
    int stathz;
    int profhz;
};

#pragma neverd_header("sys/statvfs.h")
// statvfs: 0x58 (88) bytes
struct statvfs
{
    fsblkcnt_t f_bavail;
    fsblkcnt_t f_bfree;
    fsblkcnt_t f_blocks;
    fsfilcnt_t f_favail;
    fsfilcnt_t f_ffree;
    fsfilcnt_t f_files;
    unsigned long f_bsize;
    unsigned long f_flag;
    unsigned long f_frsize;
    unsigned long f_fsid;
    unsigned long f_namemax;
};

#pragma neverd_header("sys/times.h")
// tms: 0x10 (16) bytes
struct tms
{
    clock_t tms_utime;
    clock_t tms_stime;
    clock_t tms_cutime;
    clock_t tms_cstime;
};

#pragma neverd_header("sys/wait.h")
typedef enum idtype idtype_t;

#pragma neverd_header("sys/resource.h")
// rusage: 0x90 (144) bytes
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
};

// __wrusage: 0x120 (288) bytes
struct __wrusage
{
    struct rusage wru_self;
    struct rusage wru_children;
};

#pragma neverd_header("sys/mman.h")
// shm_largepage_conf: 0x30 (48) bytes
struct shm_largepage_conf
{
    int psind;
    int alloc_policy;
    int pad[10];
};

#pragma neverd_header("sys/ioctl.h")
// fiodgname_arg: 0x10 (16) bytes
struct fiodgname_arg
{
    int len;
    void *buf;
};

// fiobmap2_arg: 0x10 (16) bytes
struct fiobmap2_arg
{
    __daddr_t bn;
    int runp;
    int runb;
};

#pragma neverd_header("sys/socket.h")
// iovec: 0x10 (16) bytes
struct iovec
{
    void *iov_base;
    size_t iov_len;
};

// linger: 0x8 (8) bytes
struct linger
{
    int l_onoff;
    int l_linger;
};

// accept_filter_arg: 0x100 (256) bytes
struct accept_filter_arg
{
    char af_name[16];
    char af_arg[240];
};

// sockproto: 0x4 (4) bytes
struct sockproto
{
    unsigned short sp_family;
    unsigned short sp_protocol;
};

// sockaddr_storage: 0x80 (128) bytes
struct sockaddr_storage
{
    unsigned char ss_len;
    sa_family_t ss_family;
    char __ss_pad1[6];
    __int64_t __ss_align;
    char __ss_pad2[112];
};

// msghdr: 0x30 (48) bytes
struct msghdr
{
    void *msg_name;
    socklen_t msg_namelen;
    struct iovec *msg_iov;
    int msg_iovlen;
    void *msg_control;
    socklen_t msg_controllen;
    int msg_flags;
};

// cmsghdr: 0xC (12) bytes
struct cmsghdr
{
    socklen_t cmsg_len;
    int cmsg_level;
    int cmsg_type;
};

// cmsgcred: 0x54 (84) bytes
struct cmsgcred
{
    pid_t cmcred_pid;
    uid_t cmcred_uid;
    uid_t cmcred_euid;
    gid_t cmcred_gid;
    short cmcred_ngroups;
    gid_t cmcred_groups[16];
};

// sockcred: 0x18 (24) bytes
struct sockcred
{
    uid_t sc_uid;
    uid_t sc_euid;
    gid_t sc_gid;
    gid_t sc_egid;
    int sc_ngroups;
    gid_t sc_groups[1];
};

// sockcred2: 0x20 (32) bytes
struct sockcred2
{
    int sc_version;
    pid_t sc_pid;
    uid_t sc_uid;
    uid_t sc_euid;
    gid_t sc_gid;
    gid_t sc_egid;
    int sc_ngroups;
    gid_t sc_groups[1];
};

// sock_timestamp_info: 0x40 (64) bytes
struct sock_timestamp_info
{
    __uint32_t st_info_flags;
    __uint32_t st_info_pad0;
    __uint64_t st_info_rsv[7];
};

// osockaddr: 0x10 (16) bytes
struct osockaddr
{
    unsigned short sa_family;
    char sa_data[14];
};

// omsghdr: 0x30 (48) bytes
struct omsghdr
{
    char *msg_name;
    int msg_namelen;
    struct iovec *msg_iov;
    int msg_iovlen;
    char *msg_accrights;
    int msg_accrightslen;
};

// sf_hdtr: 0x20 (32) bytes
struct sf_hdtr
{
    struct iovec *headers;
    int hdr_cnt;
    struct iovec *trailers;
    int trl_cnt;
};

// mmsghdr: 0x38 (56) bytes
struct mmsghdr
{
    struct msghdr msg_hdr;
    ssize_t msg_len;
};

// splice: 0x20 (32) bytes
struct splice
{
    int sp_fd;
    off_t sp_max;
    struct timeval sp_idle;
};

#pragma neverd_header("sys/un.h")
// sockaddr_un: 0x6A (106) bytes
struct sockaddr_un
{
    unsigned char sun_len;
    sa_family_t sun_family;
    char sun_path[104];
};

#pragma neverd_header("sys/event.h")
// kevent: 0x40 (64) bytes
struct kevent
{
    __uintptr_t ident;
    short filter;
    unsigned short flags;
    unsigned int fflags;
    __int64_t data;
    void *udata;
    __uint64_t ext[4];
};

#pragma neverd_header()
// Declared only: its members are unknown.
struct knote;

#pragma neverd_header("sys/event.h")
// klist: 0x8 (8) bytes
struct klist
{
    struct knote *slh_first;
};

#pragma neverd_header()
// Declared only: its members are unknown.
struct kqueue;

#pragma neverd_header("sys/event.h")
// kqlist: 0x10 (16) bytes
struct kqlist
{
    struct kqueue *tqh_first;
    struct kqueue **tqh_last;
};

// knlist: 0x30 (48) bytes
struct knlist
{
    struct klist kl_list;
    void (*kl_lock)(void *);
    void (*kl_unlock)(void *);
    void (*kl_assert_lock)(void *, int);
    void *kl_lockarg;
    int kl_autodestroy;
};

#pragma neverd_header("sys/eventfd.h")
typedef unsigned long long eventfd_t;

#pragma neverd_header("sys/inotify.h")
// inotify_event: 0x10 (16) bytes
struct inotify_event
{
    int wd;
    __uint32_t mask;
    __uint32_t cookie;
    __uint32_t len;
    char name[0];
};

#pragma neverd_header("sys/timerfd.h")
typedef unsigned long long timerfd_t;

#pragma neverd_header("sys/resource.h")
// rlimit: 0x10 (16) bytes
struct rlimit
{
    rlim_t rlim_cur;
    rlim_t rlim_max;
};

// orlimit: 0x8 (8) bytes
struct orlimit
{
    __int32_t rlim_cur;
    __int32_t rlim_max;
};

// loadavg: 0x18 (24) bytes
struct loadavg
{
    __fixpt_t ldavg[3];
    long fscale;
};

#pragma neverd_header("sys/utsname.h")
// utsname: 0x500 (1280) bytes
struct utsname
{
    char sysname[256];
    char nodename[256];
    char release[256];
    char version[256];
    char machine[256];
};

#pragma neverd_header("sys/file.h")
// xfile: 0x80 (128) bytes
struct xfile
{
    ksize_t xf_size;
    pid_t xf_pid;
    uid_t xf_uid;
    int xf_fd;
    int _xf_int_pad1;
    kvaddr_t xf_file;
    short xf_type;
    short _xf_short_pad1;
    int xf_count;
    int xf_msgcount;
    int _xf_int_pad2;
    off_t xf_offset;
    kvaddr_t xf_data;
    kvaddr_t xf_vnode;
    u_int xf_flag;
    int _xf_int_pad3;
    long long _xf_int64_pad[6];
};

#pragma neverd_header("sys/ptrace.h")
// reg: 0x110 (272) bytes
struct reg
{
    __uint64_t x[30];
    __uint64_t lr;
    __uint64_t sp;
    __uint64_t elr;
    __uint64_t spsr;
};

// reg32: 0x44 (68) bytes
struct reg32
{
    unsigned int r[13];
    unsigned int r_sp;
    unsigned int r_lr;
    unsigned int r_pc;
    unsigned int r_cpsr;
};

// fpreg: 0x210 (528) bytes
struct fpreg
{
    __uint128_t fp_q[32];
    __uint32_t fp_sr;
    __uint32_t fp_cr;
};

// fpreg32: 0x4 (4) bytes
struct fpreg32
{
    int dummy;
};

// svereg_header: 0x10 (16) bytes
struct svereg_header
{
    __uint32_t sve_size;
    __uint32_t sve_maxsize;
    __uint16_t sve_vec_len;
    __uint16_t sve_max_vec_len;
    __uint16_t sve_flags;
    __uint16_t sve_reserved;
};

// dbreg: 0x208 (520) bytes
struct dbreg
{
    __uint8_t db_debug_ver;
    __uint8_t db_nbkpts;
    __uint8_t db_nwtpts;
    __uint8_t db_pad[5];
    struct
    {
        __uint64_t dbr_addr;
        __uint32_t dbr_ctrl;
        __uint32_t dbr_pad;
    } db_breakregs[16];
    struct
    {
        __uint64_t dbw_addr;
        __uint32_t dbw_ctrl;
        __uint32_t dbw_pad;
    } db_watchregs[16];
};

// dbreg32: 0x4 (4) bytes
struct dbreg32
{
    int dummy;
};

// arm64_addr_mask: 0x10 (16) bytes
struct arm64_addr_mask
{
    __uint64_t code;
    __uint64_t data;
};

// ptrace_io_desc: 0x20 (32) bytes
struct ptrace_io_desc
{
    int piod_op;
    void *piod_offs;
    void *piod_addr;
    size_t piod_len;
};

// ptrace_lwpinfo: 0xA0 (160) bytes
struct ptrace_lwpinfo
{
    lwpid_t pl_lwpid;
    int pl_event;
    int pl_flags;
    sigset_t pl_sigmask;
    sigset_t pl_siglist;
    struct __siginfo pl_siginfo;
    char pl_tdname[20];
    pid_t pl_child_pid;
    u_int pl_syscall_code;
    u_int pl_syscall_narg;
};

// ptrace_sc_ret: 0x18 (24) bytes
struct ptrace_sc_ret
{
    syscallarg_t sr_retval[2];
    int sr_error;
};

// ptrace_vm_entry: 0x40 (64) bytes
struct ptrace_vm_entry
{
    int pve_entry;
    int pve_timestamp;
    u_long pve_start;
    u_long pve_end;
    u_long pve_offset;
    u_int pve_prot;
    u_int pve_pathlen;
    long pve_fileid;
    unsigned int pve_fsid;
    char *pve_path;
};

// ptrace_coredump: 0x10 (16) bytes
struct ptrace_coredump
{
    int pc_fd;
    unsigned int pc_flags;
    off_t pc_limit;
};

// ptrace_sc_remote: 0x28 (40) bytes
struct ptrace_sc_remote
{
    struct ptrace_sc_ret pscr_ret;
    u_int pscr_syscall;
    u_int pscr_nargs;
    syscallarg_t *pscr_args;
};

#pragma neverd_header("sys/sem.h")
// ipc_perm: 0x20 (32) bytes
struct ipc_perm
{
    uid_t cuid;
    gid_t cgid;
    uid_t uid;
    gid_t gid;
    mode_t mode;
    unsigned short seq;
    key_t key;
};

#pragma neverd_header()
// Declared only: its members are unknown.
struct sem;

#pragma neverd_header("sys/sem.h")
// semid_ds: 0x40 (64) bytes
struct semid_ds
{
    struct ipc_perm sem_perm;
    struct sem *__sem_base;
    unsigned short sem_nsems;
    time_t sem_otime;
    time_t sem_ctime;
};

// sembuf: 0x6 (6) bytes
struct sembuf
{
    unsigned short sem_num;
    short sem_op;
    short sem_flg;
};

#pragma neverd_header("sys/shm.h")
typedef unsigned int shmatt_t;

// shmid_ds: 0x50 (80) bytes
struct shmid_ds
{
    struct ipc_perm shm_perm;
    size_t shm_segsz;
    pid_t shm_lpid;
    pid_t shm_cpid;
    shmatt_t shm_nattch;
    time_t shm_atime;
    time_t shm_dtime;
    time_t shm_ctime;
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

#pragma neverd_header("sys/msg.h")
typedef unsigned long msglen_t;

typedef unsigned long msgqnum_t;

#pragma neverd_header()
// Declared only: its members are unknown.
struct msg;

#pragma neverd_header("sys/msg.h")
// msqid_ds: 0x68 (104) bytes
struct msqid_ds
{
    struct ipc_perm msg_perm;
    struct msg *__msg_first;
    struct msg *__msg_last;
    msglen_t msg_cbytes;
    msgqnum_t msg_qnum;
    msglen_t msg_qbytes;
    pid_t msg_lspid;
    pid_t msg_lrpid;
    time_t msg_stime;
    time_t msg_rtime;
    time_t msg_ctime;
};

#pragma neverd_header("sys/procctl.h")
// procctl_reaper_status: 0x50 (80) bytes
struct procctl_reaper_status
{
    u_int rs_flags;
    u_int rs_children;
    u_int rs_descendants;
    pid_t rs_reaper;
    pid_t rs_pid;
    u_int rs_pad0[15];
};

// procctl_reaper_pidinfo: 0x48 (72) bytes
struct procctl_reaper_pidinfo
{
    pid_t pi_pid;
    pid_t pi_subtree;
    u_int pi_flags;
    u_int pi_pad0[15];
};

// procctl_reaper_pids: 0x48 (72) bytes
struct procctl_reaper_pids
{
    u_int rp_count;
    u_int rp_pad0[15];
    struct procctl_reaper_pidinfo *rp_pids;
};

// procctl_reaper_kill: 0x50 (80) bytes
struct procctl_reaper_kill
{
    int rk_sig;
    u_int rk_flags;
    pid_t rk_subtree;
    u_int rk_killed;
    pid_t rk_fpid;
    u_int rk_pad0[15];
};

#pragma neverd_header("sys/mount.h")
typedef uid_t au_id_t;

typedef pid_t au_asid_t;

typedef u_int16_t au_event_t;

typedef u_int16_t au_emod_t;

typedef u_int32_t au_class_t;

typedef u_int64_t au_asflgs_t;

// au_tid: 0x8 (8) bytes
struct au_tid
{
    u_int32_t port;
    u_int32_t machine;
};

typedef struct au_tid au_tid_t;

// au_tid_addr: 0x18 (24) bytes
struct au_tid_addr
{
    u_int32_t at_port;
    u_int32_t at_type;
    u_int32_t at_addr[4];
};

typedef struct au_tid_addr au_tid_addr_t;

// au_mask: 0x8 (8) bytes
struct au_mask
{
    unsigned int am_success;
    unsigned int am_failure;
};

typedef struct au_mask au_mask_t;

// auditinfo: 0x18 (24) bytes
struct auditinfo
{
    au_id_t ai_auid;
    au_mask_t ai_mask;
    au_tid_t ai_termid;
    au_asid_t ai_asid;
};

typedef struct auditinfo auditinfo_t;

// auditinfo_addr: 0x30 (48) bytes
struct auditinfo_addr
{
    au_id_t ai_auid;
    au_mask_t ai_mask;
    au_tid_addr_t ai_termid;
    au_asid_t ai_asid;
    au_asflgs_t ai_flags;
};

typedef struct auditinfo_addr auditinfo_addr_t;

// auditpinfo: 0x1C (28) bytes
struct auditpinfo
{
    pid_t ap_pid;
    au_id_t ap_auid;
    au_mask_t ap_mask;
    au_tid_t ap_termid;
    au_asid_t ap_asid;
};

typedef struct auditpinfo auditpinfo_t;

// auditpinfo_addr: 0x38 (56) bytes
struct auditpinfo_addr
{
    pid_t ap_pid;
    au_id_t ap_auid;
    au_mask_t ap_mask;
    au_tid_addr_t ap_termid;
    au_asid_t ap_asid;
    au_asflgs_t ap_flags;
};

typedef struct auditpinfo_addr auditpinfo_addr_t;

// au_session: 0x10 (16) bytes
struct au_session
{
    auditinfo_addr_t *as_aia_p;
    au_mask_t as_mask;
};

typedef struct au_session au_session_t;

#pragma neverd_header()
// Declared only: its members are unknown.
struct au_token;

#pragma neverd_header("sys/mount.h")
typedef struct au_token token_t;

// au_qctrl: 0x14 (20) bytes
struct au_qctrl
{
    int aq_hiwater;
    int aq_lowater;
    int aq_bufsz;
    int aq_delay;
    int aq_minfree;
};

typedef struct au_qctrl au_qctrl_t;

// audit_stat: 0x38 (56) bytes
struct audit_stat
{
    unsigned int as_version;
    unsigned int as_numevent;
    int as_generated;
    int as_nonattrib;
    int as_kernel;
    int as_audit;
    int as_auditctl;
    int as_enqueue;
    int as_written;
    int as_wblocked;
    int as_rblocked;
    int as_dropped;
    int as_totalsize;
    unsigned int as_memused;
};

typedef struct audit_stat au_stat_t;

// audit_fstat: 0x10 (16) bytes
struct audit_fstat
{
    u_int64_t af_filesz;
    u_int64_t af_currsz;
};

typedef struct audit_fstat au_fstat_t;

// au_evclass_map: 0x8 (8) bytes
struct au_evclass_map
{
    au_event_t ec_number;
    au_class_t ec_class;
};

typedef struct au_evclass_map au_evclass_map_t;

// au_evname_map: 0x42 (66) bytes
struct au_evname_map
{
    au_event_t en_number;
    char en_name[64];
};

typedef struct au_evname_map au_evname_map_t;

// xucred: 0x58 (88) bytes
struct xucred
{
    u_int cr_version;
    uid_t cr_uid;
    short cr_ngroups;
    union
    {
        struct
        {
            gid_t cr_gid;
            gid_t cr_sgroups[15];
        };
        gid_t cr_groups[16];
    };
    union
    {
        void *_cr_unused1;
        pid_t cr_pid;
    };
};

#pragma neverd_header()
// Declared only: its members are unknown.
struct mac;

#pragma neverd_header("sys/mount.h")
// setcred: 0x30 (48) bytes
struct setcred
{
    uid_t sc_uid;
    uid_t sc_ruid;
    uid_t sc_svuid;
    gid_t sc_gid;
    gid_t sc_rgid;
    gid_t sc_svgid;
    u_int sc_pad;
    u_int sc_supp_groups_nb;
    gid_t *sc_supp_groups;
    struct mac *sc_label;
};

// fsid: 0x8 (8) bytes
struct fsid
{
    int val[2];
};

typedef struct fsid fsid_t;

// fid: 0x14 (20) bytes
struct fid
{
    u_short fid_len;
    u_short fid_data0;
    char fid_data[16];
};

// statfs: 0x928 (2344) bytes
struct statfs
{
    unsigned int f_version;
    unsigned int f_type;
    unsigned long long f_flags;
    unsigned long long f_bsize;
    unsigned long long f_iosize;
    unsigned long long f_blocks;
    unsigned long long f_bfree;
    long long f_bavail;
    unsigned long long f_files;
    long long f_ffree;
    unsigned long long f_syncwrites;
    unsigned long long f_asyncwrites;
    unsigned long long f_syncreads;
    unsigned long long f_asyncreads;
    unsigned int f_nvnodelistsize;
    unsigned int f_spare0;
    unsigned long long f_spare[9];
    unsigned int f_namemax;
    uid_t f_owner;
    fsid_t f_fsid;
    char f_charspare[80];
    char f_fstypename[16];
    char f_mntfromname[1024];
    char f_mntonname[1024];
};

// fhandle: 0x1C (28) bytes
struct fhandle
{
    fsid_t fh_fsid;
    struct fid fh_fid;
};

typedef struct fhandle fhandle_t;

// oexport_args: 0x88 (136) bytes
struct oexport_args
{
    int ex_flags;
    uid_t ex_root;
    struct xucred ex_anon;
    struct sockaddr *ex_addr;
    u_char ex_addrlen;
    struct sockaddr *ex_mask;
    u_char ex_masklen;
    char *ex_indexfile;
};

// o2export_args: 0xA0 (160) bytes
struct o2export_args
{
    int ex_flags;
    uid_t ex_root;
    struct xucred ex_anon;
    struct sockaddr *ex_addr;
    u_char ex_addrlen;
    struct sockaddr *ex_mask;
    u_char ex_masklen;
    char *ex_indexfile;
    int ex_numsecflavors;
    int ex_secflavors[5];
};

// export_args: 0x60 (96) bytes
struct export_args
{
    unsigned long long ex_flags;
    uid_t ex_root;
    uid_t ex_uid;
    int ex_ngroups;
    gid_t *ex_groups;
    struct sockaddr *ex_addr;
    u_char ex_addrlen;
    struct sockaddr *ex_mask;
    u_char ex_masklen;
    char *ex_indexfile;
    int ex_numsecflavors;
    int ex_secflavors[5];
};

#pragma neverd_header()
// Declared only: its members are unknown.
struct mount;

#pragma neverd_header("sys/mount.h")
// nfs_public: 0x30 (48) bytes
struct nfs_public
{
    int np_valid;
    fhandle_t np_handle;
    struct mount *np_mount;
    char *np_index;
};

#pragma neverd_header()
// Declared only: its members are unknown.
struct vfsops;

// Declared only: its members are unknown.
struct vfsoptdecl;

#pragma neverd_header("sys/mount.h")
// vfsconf: 0x50 (80) bytes
struct vfsconf
{
    u_int vfc_version;
    char vfc_name[16];
    struct vfsops *vfc_vfsops;
    struct vfsops *vfc_vfsops_sd;
    int vfc_typenum;
    int vfc_refcount;
    int vfc_flags;
    int vfc_prison_flag;
    struct vfsoptdecl *vfc_opts;
    struct
    {
        struct vfsconf *tqe_next;
        struct vfsconf **tqe_prev;
    } vfc_list;
};

// xvfsconf: 0x30 (48) bytes
struct xvfsconf
{
    struct vfsops *vfc_vfsops;
    char vfc_name[16];
    int vfc_typenum;
    int vfc_refcount;
    int vfc_flags;
    struct vfsconf *vfc_next;
};

// ovfsconf: 0x38 (56) bytes
struct ovfsconf
{
    void *vfc_vfsops;
    char vfc_name[32];
    int vfc_index;
    int vfc_refcount;
    int vfc_flags;
};

typedef unsigned int fsctlop_t;

// vfsidctl: 0x60 (96) bytes
struct vfsidctl
{
    int vc_vers;
    fsid_t vc_fsid;
    char vc_fstypename[16];
    fsctlop_t vc_op;
    void *vc_ptr;
    size_t vc_len;
    u_int32_t vc_spare[12];
};

// vfsquery: 0x80 (128) bytes
struct vfsquery
{
    u_int32_t vq_flags;
    u_int32_t vq_spare[31];
};

#pragma neverd_header()
// Declared only: its members are unknown.
struct uio;

#pragma neverd_header("sys/linker.h")
// kld_file_stat: 0x820 (2080) bytes
struct kld_file_stat
{
    int version;
    char name[1024];
    int refs;
    int id;
    caddr_t address;
    size_t size;
    char pathname[1024];
};

// kld_sym_lookup: 0x20 (32) bytes
struct kld_sym_lookup
{
    int version;
    char *symname;
    u_long symvalue;
    size_t symsize;
};

#pragma neverd_header("sys/module.h")
typedef enum modeventtype modeventtype_t;

#pragma neverd_header()
// Declared only: its members are unknown.
struct module;

#pragma neverd_header("sys/module.h")
typedef struct module *module_t;

typedef int (*modeventhand_t)(module_t, int, void *);

// moduledata: 0x18 (24) bytes
struct moduledata
{
    const char *name;
    modeventhand_t evhand;
    void *priv;
};

typedef struct moduledata moduledata_t;

// modspecific: 0x8 (8) bytes
union modspecific
{
    int intval;
    u_int uintval;
    long longval;
    u_long ulongval;
};

typedef union modspecific modspecific_t;

// mod_depend: 0xC (12) bytes
struct mod_depend
{
    int md_ver_minimum;
    int md_ver_preferred;
    int md_ver_maximum;
};

// mod_version: 0x4 (4) bytes
struct mod_version
{
    int mv_version;
};

// mod_metadata: 0x18 (24) bytes
struct mod_metadata
{
    int md_version;
    int md_type;
    const void *md_data;
    const char *md_cval;
};

// mod_pnp_match_info: 0x20 (32) bytes
struct mod_pnp_match_info
{
    const char *descr;
    const char *bus;
    const void *table;
    int entry_len;
    int num_entry;
};

// module_stat: 0x418 (1048) bytes
struct module_stat
{
    int version;
    char name[1024];
    int refs;
    int id;
    modspecific_t data;
};

#pragma neverd_header("netinet/in.h")
// in_addr: 0x4 (4) bytes
struct in_addr
{
    in_addr_t s_addr;
};

// in6_addr: 0x10 (16) bytes
struct in6_addr
{
    union
    {
        unsigned char __u6_addr8[16];
        unsigned short __u6_addr16[8];
        unsigned int __u6_addr32[4];
    } __u6_addr;
};

#pragma neverd_header("sys/jail.h")
// jail: 0x38 (56) bytes
struct jail
{
    unsigned int version;
    char *path;
    char *hostname;
    char *jailname;
    unsigned int ip4s;
    unsigned int ip6s;
    struct in_addr *ip4;
    struct in6_addr *ip6;
};

// xprison_v1: 0x50C (1292) bytes
struct xprison_v1
{
    int pr_version;
    int pr_id;
    char pr_path[1024];
    char pr_host[256];
    u_int32_t pr_ip;
};

// xprison: 0x618 (1560) bytes
struct xprison
{
    int pr_version;
    int pr_id;
    int pr_state;
    cpusetid_t pr_cpusetid;
    char pr_path[1024];
    char pr_host[256];
    char pr_name[256];
    unsigned int pr_ip4s;
    unsigned int pr_ip6s;
};

#pragma neverd_header("netinet/in.h")
// sockaddr_in: 0x10 (16) bytes
struct sockaddr_in
{
    unsigned char sin_len;
    sa_family_t sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    char sin_zero[8];
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
    struct in_addr imr_sourceaddr;
    struct in_addr imr_interface;
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

// __msfilterreq: 0x98 (152) bytes
struct __msfilterreq
{
    unsigned int msfr_ifindex;
    unsigned int msfr_fmode;
    unsigned int msfr_nsrcs;
    struct sockaddr_storage msfr_group;
    struct sockaddr_storage *msfr_srcs;
};

// sockaddr_in6: 0x1C (28) bytes
struct sockaddr_in6
{
    unsigned char sin6_len;
    sa_family_t sin6_family;
    in_port_t sin6_port;
    unsigned int sin6_flowinfo;
    struct in6_addr sin6_addr;
    unsigned int sin6_scope_id;
};

#pragma neverd_header()
// Declared only: its members are unknown.
struct nhop_object;

// Declared only: its members are unknown.
struct llentry;

#pragma neverd_header("netinet/in.h")
// route_in6: 0x40 (64) bytes
struct route_in6
{
    struct nhop_object *ro_nh;
    struct llentry *ro_lle;
    char *ro_prepend;
    unsigned short ro_plen;
    unsigned short ro_flags;
    unsigned short ro_mtu;
    unsigned short spare;
    struct sockaddr_in6 ro_dst;
};

// ipv6_mreq: 0x14 (20) bytes
struct ipv6_mreq
{
    struct in6_addr ipv6mr_multiaddr;
    unsigned int ipv6mr_interface;
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

#pragma neverd_header("netinet/tcp.h")
typedef u_int32_t tcp_seq;

// tcphdr: 0x14 (20) bytes, packed to 1
#pragma pack(push, 1)
struct NEVERD_GNU_STRUCT tcphdr
{
    u_short th_sport;
    u_short th_dport;
    tcp_seq th_seq;
    tcp_seq th_ack;
    u_char th_x2 : 4;
    u_char th_off : 4;
    u_char th_flags;
    u_short th_win;
    u_short th_sum;
    u_short th_urp;
};
#pragma pack(pop)

// tcp_perf_info: 0xE0 (224) bytes
struct tcp_perf_info
{
    unsigned long long tcp_cnt_counters[13];
    unsigned long long tcp_proc_time[13];
    unsigned long long timebase;
    unsigned char tb_is_stable;
};

// tcp_info: 0x100 (256) bytes
struct NEVERD_GNU_STRUCT tcp_info
{
    u_int8_t tcpi_state;
    u_int8_t __tcpi_ca_state;
    u_int8_t __tcpi_retransmits;
    u_int8_t __tcpi_probes;
    u_int8_t __tcpi_backoff;
    u_int8_t tcpi_options;
    u_int8_t tcpi_snd_wscale : 4;
    u_int8_t tcpi_rcv_wscale : 4;
    u_int32_t tcpi_rto;
    u_int32_t __tcpi_ato;
    u_int32_t tcpi_snd_mss;
    u_int32_t tcpi_rcv_mss;
    u_int32_t __tcpi_unacked;
    u_int32_t __tcpi_sacked;
    u_int32_t __tcpi_lost;
    u_int32_t __tcpi_retrans;
    u_int32_t __tcpi_fackets;
    u_int32_t __tcpi_last_data_sent;
    u_int32_t __tcpi_last_ack_sent;
    u_int32_t tcpi_last_data_recv;
    u_int32_t __tcpi_last_ack_recv;
    u_int32_t __tcpi_pmtu;
    u_int32_t __tcpi_rcv_ssthresh;
    u_int32_t tcpi_rtt;
    u_int32_t tcpi_rttvar;
    u_int32_t tcpi_snd_ssthresh;
    u_int32_t tcpi_snd_cwnd;
    u_int32_t __tcpi_advmss;
    u_int32_t __tcpi_reordering;
    u_int32_t __tcpi_rcv_rtt;
    u_int32_t tcpi_rcv_space;
    u_int32_t tcpi_snd_wnd;
    u_int32_t tcpi_snd_bwnd;
    u_int32_t tcpi_snd_nxt;
    u_int32_t tcpi_rcv_nxt;
    u_int32_t tcpi_toe_tid;
    u_int32_t tcpi_snd_rexmitpack;
    u_int32_t tcpi_rcv_ooopack;
    u_int32_t tcpi_snd_zerowin;
    u_int32_t tcpi_delivered_ce;
    u_int32_t tcpi_received_ce;
    u_int32_t __tcpi_delivered_e1_bytes;
    u_int32_t __tcpi_delivered_e0_bytes;
    u_int32_t __tcpi_delivered_ce_bytes;
    u_int32_t __tcpi_received_e1_bytes;
    u_int32_t __tcpi_received_e0_bytes;
    u_int32_t __tcpi_received_ce_bytes;
    u_int32_t tcpi_total_tlp;
    u_int64_t tcpi_total_tlp_bytes;
    u_int32_t tcpi_snd_una;
    u_int32_t tcpi_snd_max;
    u_int32_t tcpi_rcv_numsacks;
    u_int32_t tcpi_rcv_adv;
    u_int32_t tcpi_dupacks;
    u_int32_t tcpi_rttmin;
    u_int32_t __tcpi_pad[14];
};

// tcp_fastopen: 0x14 (20) bytes
struct tcp_fastopen
{
    int enable;
    unsigned char psk[16];
};

// stack_specific_info: 0x90 (144) bytes
struct NEVERD_GNU_STRUCT stack_specific_info
{
    char stack_name[32];
    unsigned long long policer_last_bw;
    unsigned long long bytes_transmitted;
    unsigned long long bytes_retransmitted;
    unsigned int policer_detection_enabled : 1;
    unsigned int policer_detected : 1;
    unsigned int highly_buffered : 1;
    unsigned int spare : 29;
    unsigned int policer_bucket_size;
    unsigned int current_round;
    unsigned int _rack_i_pad[18];
};

// tcp_function_set: 0x24 (36) bytes
struct tcp_function_set
{
    char function_set_name[32];
    unsigned int pcbcnt;
};

// tcp_snd_req: 0x20 (32) bytes
struct tcp_snd_req
{
    unsigned long long timestamp;
    unsigned long long start;
    unsigned long long end;
    unsigned int flags;
    unsigned int playout_ms;
};

// tcp_log_userdata: 0x20 (32) bytes
union tcp_log_userdata
{
    struct tcp_snd_req tcp_req;
};

// tcp_log_user: 0x28 (40) bytes
struct tcp_log_user
{
    unsigned int type;
    unsigned int subtype;
    union tcp_log_userdata data;
};

// tcp_hybrid_req: 0x30 (48) bytes
struct tcp_hybrid_req
{
    struct tcp_snd_req req;
    unsigned long long cspr;
    unsigned int hint_maxseg;
    unsigned int hybrid_flags;
};

#pragma neverd_header("net/if.h")
// if_clonereq: 0x10 (16) bytes
struct if_clonereq
{
    int ifcr_total;
    int ifcr_count;
    char *ifcr_buffer;
};

// if_data: 0x98 (152) bytes
struct if_data
{
    unsigned char ifi_type;
    unsigned char ifi_physical;
    unsigned char ifi_addrlen;
    unsigned char ifi_hdrlen;
    unsigned char ifi_link_state;
    unsigned char ifi_vhid;
    unsigned short ifi_datalen;
    unsigned int ifi_mtu;
    unsigned int ifi_metric;
    unsigned long long ifi_baudrate;
    unsigned long long ifi_ipackets;
    unsigned long long ifi_ierrors;
    unsigned long long ifi_opackets;
    unsigned long long ifi_oerrors;
    unsigned long long ifi_collisions;
    unsigned long long ifi_ibytes;
    unsigned long long ifi_obytes;
    unsigned long long ifi_imcasts;
    unsigned long long ifi_omcasts;
    unsigned long long ifi_iqdrops;
    unsigned long long ifi_oqdrops;
    unsigned long long ifi_noproto;
    unsigned long long ifi_hwassist;
    union
    {
        time_t tt;
        unsigned long long ph;
    } __ifi_epoch;
    union
    {
        struct timeval tv;
        struct
        {
            unsigned long long ph1;
            unsigned long long ph2;
        } ph;
    } __ifi_lastchange;
};

// if_msghdr: 0xA8 (168) bytes
struct if_msghdr
{
    u_short ifm_msglen;
    u_char ifm_version;
    u_char ifm_type;
    int ifm_addrs;
    int ifm_flags;
    u_short ifm_index;
    u_short _ifm_spare1;
    struct if_data ifm_data;
};

// if_msghdrl: 0xB0 (176) bytes
struct if_msghdrl
{
    u_short ifm_msglen;
    u_char ifm_version;
    u_char ifm_type;
    int ifm_addrs;
    int ifm_flags;
    u_short ifm_index;
    u_short _ifm_spare1;
    u_short ifm_len;
    u_short ifm_data_off;
    int _ifm_spare2;
    struct if_data ifm_data;
};

// ifa_msghdr: 0x14 (20) bytes
struct ifa_msghdr
{
    u_short ifam_msglen;
    u_char ifam_version;
    u_char ifam_type;
    int ifam_addrs;
    int ifam_flags;
    u_short ifam_index;
    u_short _ifam_spare1;
    int ifam_metric;
};

// ifa_msghdrl: 0xB0 (176) bytes
struct ifa_msghdrl
{
    u_short ifam_msglen;
    u_char ifam_version;
    u_char ifam_type;
    int ifam_addrs;
    int ifam_flags;
    u_short ifam_index;
    u_short _ifam_spare1;
    u_short ifam_len;
    u_short ifam_data_off;
    int ifam_metric;
    struct if_data ifam_data;
};

// ifma_msghdr: 0x10 (16) bytes
struct ifma_msghdr
{
    u_short ifmam_msglen;
    u_char ifmam_version;
    u_char ifmam_type;
    int ifmam_addrs;
    int ifmam_flags;
    u_short ifmam_index;
    u_short _ifmam_spare1;
};

// if_announcemsghdr: 0x18 (24) bytes
struct if_announcemsghdr
{
    u_short ifan_msglen;
    u_char ifan_version;
    u_char ifan_type;
    u_short ifan_index;
    char ifan_name[16];
    u_short ifan_what;
};

// ifreq_buffer: 0x10 (16) bytes
struct ifreq_buffer
{
    size_t length;
    void *buffer;
};

// ifreq_nv_req: 0x10 (16) bytes
struct ifreq_nv_req
{
    u_int buf_length;
    u_int length;
    void *buffer;
};

// ifreq: 0x20 (32) bytes
struct ifreq
{
    char ifr_name[16];
    union
    {
        struct sockaddr ifru_addr;
        struct sockaddr ifru_dstaddr;
        struct sockaddr ifru_broadaddr;
        struct ifreq_buffer ifru_buffer;
        short ifru_flags[2];
        short ifru_index;
        int ifru_jid;
        int ifru_metric;
        int ifru_mtu;
        int ifru_phys;
        int ifru_media;
        caddr_t ifru_data;
        int ifru_cap[2];
        u_int ifru_fib;
        u_char ifru_vlan_pcp;
        struct ifreq_nv_req ifru_nv;
    } ifr_ifru;
};

// ifaliasreq: 0x44 (68) bytes
struct ifaliasreq
{
    char ifra_name[16];
    struct sockaddr ifra_addr;
    struct sockaddr ifra_broadaddr;
    struct sockaddr ifra_mask;
    int ifra_vhid;
};

// oifaliasreq: 0x40 (64) bytes
struct oifaliasreq
{
    char ifra_name[16];
    struct sockaddr ifra_addr;
    struct sockaddr ifra_broadaddr;
    struct sockaddr ifra_mask;
};

// ifmediareq: 0x30 (48) bytes
struct ifmediareq
{
    char ifm_name[16];
    int ifm_current;
    int ifm_mask;
    int ifm_status;
    int ifm_active;
    int ifm_count;
    int *ifm_ulist;
};

// ifdrv: 0x28 (40) bytes
struct ifdrv
{
    char ifd_name[16];
    unsigned long ifd_cmd;
    size_t ifd_len;
    void *ifd_data;
};

// ifstat: 0x331 (817) bytes
struct ifstat
{
    char ifs_name[16];
    char ascii[801];
};

// ifconf: 0x10 (16) bytes
struct ifconf
{
    int ifc_len;
    union
    {
        caddr_t ifcu_buf;
        struct ifreq *ifcu_req;
    } ifc_ifcu;
};

// ifg_req: 0x10 (16) bytes
struct ifg_req
{
    union
    {
        char ifgrqu_group[16];
        char ifgrqu_member[16];
    } ifgrq_ifgrqu;
};

// ifgroupreq: 0x28 (40) bytes
struct ifgroupreq
{
    char ifgr_name[16];
    u_int ifgr_len;
    union
    {
        char ifgru_group[16];
        struct ifg_req *ifgru_groups;
    } ifgr_ifgru;
};

// ifi2creq: 0x10 (16) bytes
struct ifi2creq
{
    unsigned char dev_addr;
    unsigned char offset;
    unsigned char len;
    unsigned char spare0;
    unsigned int spare1;
    unsigned char data[8];
};

// ifrsskey: 0x94 (148) bytes
struct ifrsskey
{
    char ifrk_name[16];
    unsigned char ifrk_func;
    unsigned char ifrk_spare0;
    unsigned short ifrk_keylen;
    unsigned char ifrk_key[128];
};

// ifrsshash: 0x18 (24) bytes
struct ifrsshash
{
    char ifrh_name[16];
    unsigned char ifrh_func;
    unsigned char ifrh_spare0;
    unsigned short ifrh_spare1;
    unsigned int ifrh_types;
};

// ifdownreason: 0x58 (88) bytes
struct ifdownreason
{
    char ifdr_name[16];
    unsigned int ifdr_reason;
    unsigned int ifdr_vendor;
    char ifdr_msg[64];
};

#pragma neverd_header()
// Declared only: its members are unknown.
struct ifnet;

#pragma neverd_header("net/if.h")
typedef struct ifnet *if_t;

// if_nameindex: 0x10 (16) bytes
struct if_nameindex
{
    unsigned int if_index;
    char *if_name;
};

#pragma neverd_header("aio.h")
// __aiocb_private: 0x18 (24) bytes
struct __aiocb_private
{
    long status;
    long error;
    void *spare;
};

// aiocb: 0xA0 (160) bytes
struct aiocb
{
    int aio_fildes;
    off_t aio_offset;
    volatile void *aio_buf;
    size_t aio_nbytes;
    int __spare__[2];
    void *__spare2__;
    int aio_lio_opcode;
    int aio_reqprio;
    struct __aiocb_private _aiocb_private;
    struct sigevent aio_sigevent;
};

typedef struct aiocb aiocb_t;

#pragma neverd_header("mqueue.h")
// mq_attr: 0x40 (64) bytes
struct mq_attr
{
    long mq_flags;
    long mq_maxmsg;
    long mq_msgsize;
    long mq_curmsgs;
    long __reserved[4];
};

#pragma neverd_header("malloc_np.h")
typedef struct extent_hooks_s extent_hooks_t;

typedef void *extent_alloc_t(extent_hooks_t *, void *, size_t, size_t, bool *, bool *, unsigned int);

typedef bool extent_dalloc_t(extent_hooks_t *, void *, size_t, bool, unsigned int);

typedef void extent_destroy_t(extent_hooks_t *, void *, size_t, bool, unsigned int);

typedef bool extent_commit_t(extent_hooks_t *, void *, size_t, size_t, size_t, unsigned int);

typedef bool extent_decommit_t(extent_hooks_t *, void *, size_t, size_t, size_t, unsigned int);

typedef bool extent_purge_t(extent_hooks_t *, void *, size_t, size_t, size_t, unsigned int);

typedef bool extent_split_t(extent_hooks_t *, void *, size_t, size_t, size_t, bool, unsigned int);

typedef bool extent_merge_t(extent_hooks_t *, void *, size_t, void *, size_t, bool, unsigned int);

// extent_hooks_s: 0x48 (72) bytes
struct extent_hooks_s
{
    extent_alloc_t *alloc;
    extent_dalloc_t *dalloc;
    extent_destroy_t *destroy;
    extent_commit_t *commit;
    extent_decommit_t *decommit;
    extent_purge_t *purge_lazy;
    extent_purge_t *purge_forced;
    extent_split_t *split;
    extent_merge_t *merge;
};

#pragma neverd_header("fts.h")
// _ftsent: 0x98 (152) bytes
struct _ftsent
{
    struct _ftsent *fts_cycle;
    struct _ftsent *fts_parent;
    struct _ftsent *fts_link;
    long long fts_number;
    void *fts_pointer;
    char *fts_accpath;
    char *fts_path;
    int fts_errno;
    int fts_symfd;
    __size_t fts_pathlen;
    __size_t fts_namelen;
    __ino_t fts_ino;
    __dev_t fts_dev;
    __nlink_t fts_nlink;
    long fts_level;
    int fts_info;
    unsigned int fts_flags;
    int fts_instr;
    struct stat *fts_statp;
    char *fts_name;
    FTS *fts_fts;
};

typedef struct _ftsent FTSENT;

// FTS: 0x58 (88) bytes
struct FTS
{
    FTSENT *fts_cur;
    FTSENT *fts_child;
    FTSENT **fts_array;
    __dev_t fts_dev;
    char *fts_path;
    int fts_rfd;
    __size_t fts_pathlen;
    __size_t fts_nitems;
    union
    {
        int (*fts_compar)(const FTSENT *const *, const FTSENT *const *);
        void *fts_compar_b;
    };
    int fts_options;
    void *fts_clientptr;
};

#pragma neverd_header("ftw.h")
// FTW: 0x8 (8) bytes
struct FTW
{
    int base;
    int level;
};

#pragma neverd_header("wordexp.h")
// wordexp_t: 0x28 (40) bytes
struct wordexp_t
{
    __size_t we_wordc;
    char **we_wordv;
    __size_t we_offs;
    char *we_strings;
    __size_t we_nbytes;
};

#pragma neverd_header("libutil.h")
// _property: 0x18 (24) bytes
struct _property
{
    struct _property *next;
    char *name;
    char *value;
};

typedef struct _property *properties;

#pragma neverd_header()
// Declared only: its members are unknown.
struct pidfh;

// Declared only: its members are unknown.
struct kinfo_file;

// Declared only: its members are unknown.
struct kinfo_vmentry;

// Declared only: its members are unknown.
struct kinfo_vmobject;

// Declared only: its members are unknown.
struct kinfo_proc;

#pragma neverd_header("kvm.h")
// nlist: 0x18 (24) bytes
struct nlist
{
    const char *n_name;
    unsigned char n_type;
    char n_other;
    short n_desc;
    unsigned long n_value;
};

typedef u_char vm_prot_t;

// kvm_nlist: 0x18 (24) bytes
struct kvm_nlist
{
    const char *n_name;
    unsigned char n_type;
    kvaddr_t n_value;
};

#pragma neverd_header()
// Declared only: its members are unknown.
struct __kvm;

#pragma neverd_header("kvm.h")
typedef struct __kvm kvm_t;

#pragma neverd_header()
// Declared only: its members are unknown.
struct proc;

#pragma neverd_header("kvm.h")
// kvm_swap: 0x34 (52) bytes
struct kvm_swap
{
    char ksw_devname[32];
    u_int ksw_used;
    u_int ksw_total;
    int ksw_flags;
    u_int ksw_reserved1;
    u_int ksw_reserved2;
};

// kvm_page: 0x38 (56) bytes
struct kvm_page
{
    u_int kp_version;
    kpaddr_t kp_paddr;
    kvaddr_t kp_kmap_vaddr;
    kvaddr_t kp_dmap_vaddr;
    vm_prot_t kp_prot;
    off_t kp_offset;
    size_t kp_len;
};

typedef int kvm_walk_pages_cb_t(struct kvm_page *, void *);

#pragma neverd_header("md5.h")
// MD5Context: 0x58 (88) bytes
struct MD5Context
{
    u_int32_t state[4];
    u_int32_t count[2];
    unsigned char buffer[64];
};

typedef struct MD5Context MD5_CTX;

#pragma neverd_header("sha256.h")
// SHA256Context: 0x68 (104) bytes
struct SHA256Context
{
    unsigned int state[8];
    unsigned long long count;
    unsigned char buf[64];
};

typedef struct SHA256Context SHA256_CTX;


_Noreturn void _Exit(int);
pid_t _Fork(void);
void __FreeBSD_libc_enter_restricted_mode(void);
int ___mb_cur_max(void);
int ___mb_cur_max_l(locale_t);
unsigned long ___runetype(__ct_rune_t);
unsigned long ___runetype_l(__ct_rune_t, locale_t);
__ct_rune_t ___tolower(__ct_rune_t);
__ct_rune_t ___tolower_l(__ct_rune_t, locale_t);
__ct_rune_t ___toupper(__ct_rune_t);
__ct_rune_t ___toupper_l(__ct_rune_t, locale_t);
void *__aligned_alloc(size_t alignment, size_t size);
_Noreturn void __assert(const char *, const char *, int, const char *);
__uint16_t __bitcount16(__uint16_t _x);
__uint32_t __bitcount32(__uint32_t _x);
__uint64_t __bitcount64(__uint64_t _x);
void *__calloc(size_t number, size_t size);
cap_rights_t *__cap_rights_clear(cap_rights_t *rights, ...);
int __cap_rights_get(int version, int fd, cap_rights_t *rights);
cap_rights_t *__cap_rights_init(int version, cap_rights_t *rights, ...);
bool __cap_rights_is_set(const cap_rights_t *rights, ...);
cap_rights_t *__cap_rights_set(cap_rights_t *rights, ...);
cpuset_t *__cpuset_alloc(size_t set_size);
void __cpuset_free(cpuset_t *ptr);
void __dallocx(void *ptr, int flags);
int *__error(void);
int __feclearexcept_int(int __excepts);
int __fedisableexcept_int(int __mask);
int __feenableexcept_int(int __mask);
int __fegetenv_int(fenv_t *__envp);
int __fegetexcept_int(void);
int __fegetexceptflag_int(fexcept_t *__flagp, int __excepts);
int __fegetround_int(void);
int __feholdexcept_int(fenv_t *__envp);
int __feraiseexcept_int(int __excepts);
int __fesetenv_int(const fenv_t *__envp);
int __fesetexceptflag_int(const fexcept_t *__flagp, int __excepts);
int __fesetround_int(int __round);
int __fetestexcept_int(int __excepts);
int __feupdateenv_int(const fenv_t *__envp);
int __fillcontextx(char *ctx);
int __fillcontextx2(char *ctx);
int __fpclassifyd(double);
int __fpclassifyf(float);
int __fpclassifyl(long double);
void __free(void *ptr);
const _RuneLocale *__getCurrentRuneLocale(void);
int __getcontextx_size(void);
int *__h_errno(void);
size_t __iconv(iconv_t, char **, size_t *, char **, size_t *, __uint32_t, size_t *);
void __iconv_free_list(char **, size_t);
int __iconv_get_list(char ***, size_t *, __iconv_bool);
in_addr_t __inet_addr(const char *);
int __inet_aton(const char *, struct in_addr *);
char *__inet_cidr_ntop(int, const void *, int, char *, size_t);
int __inet_cidr_pton(int, const char *, void *, int *);
in_addr_t __inet_lnaof(struct in_addr);
struct in_addr __inet_makeaddr(in_addr_t, in_addr_t);
char *__inet_net_ntop(int, const void *, int, char *, size_t);
int __inet_net_pton(int, const char *, void *, size_t);
char *__inet_neta(in_addr_t, char *, size_t);
in_addr_t __inet_netof(struct in_addr);
in_addr_t __inet_network(const char *);
unsigned int __inet_nsap_addr(const char *, unsigned char *, int);
char *__inet_nsap_ntoa(int, const unsigned char *, char *);
char *__inet_ntoa(struct in_addr);
char *__inet_ntoa_r(struct in_addr, char *buf, socklen_t size);
const char *__inet_ntop(int, const void *, char *, socklen_t);
int __inet_pton(int, const char *, void *);
int __inline_isnan(const double __x);
int __inline_isnanf(const float __x);
int __inline_isnanl(const long double __x);
int __isctype(__ct_rune_t _c, unsigned long _f);
int __isfinite(double);
int __isfinitef(float);
int __isfinitel(long double);
int __isinf(double);
int __isinff(float);
int __isinfl(long double);
int __isnormal(double);
int __isnormalf(float);
int __isnormall(long double);
int __istype(__ct_rune_t _c, unsigned long _f);
int __istype_l(__ct_rune_t __c, unsigned long __f, locale_t __loc);
int __major(dev_t _d);
dev_t __makedev(int _Major, int _Minor);
int __mallctl(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
int __mallctlbymib(const size_t *mib, size_t miblen, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
int __mallctlnametomib(const char *name, size_t *mibp, size_t *miblenp);
void *__malloc(size_t size);
void __malloc_stats_print(void (*write_cb)(void *, const char *), void *cbopaque, const char *opts);
size_t __malloc_usable_size(const void *ptr);
void *__mallocx(size_t size, int flags);
int __maskrune(__ct_rune_t _c, unsigned long _f);
int __maskrune_l(__ct_rune_t __c, unsigned long __f, locale_t __loc);
int __minor(dev_t _d);
size_t __nallocx(size_t size, int flags);
char *__old_basename(char *);
char *__old_dirname(char *);
DIR *__opendir2(const char *, int);
int __posix_memalign(void **ptr, size_t alignment, size_t size);
void __pthread_cleanup_pop_imp(int);
void __pthread_cleanup_push_imp(void (*)(void *), void *, struct _pthread_cleanup_info *);
void __qsort_r_compat(void *, size_t, size_t, void *, int (*)(void *, const void *, const void *));
void *__rallocx(void *ptr, size_t size, int flags);
void *__realloc(void *ptr, size_t size);
_RuneLocale *__runes_for_locale(locale_t, int *);
size_t __sallocx(const void *ptr, int flags);
int __sbistype(__ct_rune_t _c, unsigned long _f);
int __sbistype_l(__ct_rune_t __c, unsigned long __f, locale_t __loc);
int __sbmaskrune(__ct_rune_t _c, unsigned long _f);
int __sbmaskrune_l(__ct_rune_t __c, unsigned long __f, locale_t __loc);
__ct_rune_t __sbtolower(__ct_rune_t _c);
__ct_rune_t __sbtoupper(__ct_rune_t _c);
void __sdallocx(void *ptr, size_t size, int flags);
int __signbit(double);
int __signbitf(float);
int __signbitl(long double);
int __sputc(int _c, FILE *_p);
int __srget(FILE *);
long long __stime64_scale32_ceil(long long x, int factor, int divisor);
long long __stime64_scale32_floor(long long x, int factor, int divisor);
long long __stime64_scale64_ceil(long long x, long long factor, long long divisor);
long long __stime64_scale64_floor(long long x, long long factor, long long divisor);
int __swbuf(int, FILE *);
off_t __syscall(quad_t, ...);
unsigned short __tcp_get_flags(const struct tcphdr *th);
void __tcp_set_flags(struct tcphdr *th, unsigned short flags);
__ct_rune_t __tolower(__ct_rune_t _c);
__ct_rune_t __toupper(__ct_rune_t _c);
unsigned long long __utime64_scale32_ceil(unsigned long long x, unsigned int factor, unsigned int divisor);
unsigned long long __utime64_scale32_floor(unsigned long long x, unsigned int factor, unsigned int divisor);
unsigned long long __utime64_scale64_ceil(unsigned long long x, unsigned long long factor, unsigned long long divisor);
unsigned long long __utime64_scale64_floor(unsigned long long x, unsigned long long factor, unsigned long long divisor);
int __wcwidth(__ct_rune_t _c);
int __wcwidth_l(__ct_rune_t _c, locale_t __l);
size_t __xallocx(void *ptr, size_t size, size_t extra, int flags);
int __xuname(int, void *);
_Noreturn void _exit(int);
char *_libmd_MD5Data(const void *, unsigned int, char *);
char *_libmd_MD5End(MD5_CTX *, char *);
char *_libmd_MD5Fd(int, char *);
char *_libmd_MD5FdChunk(int, char *, off_t, off_t);
char *_libmd_MD5File(const char *, char *);
char *_libmd_MD5FileChunk(const char *, char *, off_t, off_t);
void _libmd_MD5Final(unsigned char *, MD5_CTX *);
void _libmd_MD5Init(MD5_CTX *);
void _libmd_MD5Update(MD5_CTX *, const void *, unsigned int);
char *_libmd_SHA256_Data(const void *, unsigned int, char *);
char *_libmd_SHA256_End(SHA256_CTX *, char *);
char *_libmd_SHA256_Fd(int, char *);
char *_libmd_SHA256_FdChunk(int, char *, off_t, off_t);
char *_libmd_SHA256_File(const char *, char *);
char *_libmd_SHA256_FileChunk(const char *, char *, off_t, off_t);
void _libmd_SHA256_Final(unsigned char *, SHA256_CTX *);
void _libmd_SHA256_Init(SHA256_CTX *);
void _libmd_SHA256_Update(SHA256_CTX *, const void *, size_t);
_Noreturn void _longjmp(jmp_buf, int);
int _rtld_addr_phdr(const void *, struct dl_phdr_info *);
int _rtld_get_stack_prot(void);
int _rtld_is_dlopened(void *);
int _secure_path(const char *_path, uid_t _uid, gid_t _gid);
int _setjmp(jmp_buf);
long a64l(const char *);
_Noreturn void abort(void);
_Noreturn void abort2(const char *, int, void **);
_Noreturn void abort_handler_s(const char *, void *, errno_t);
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
int aio_fsync(int op, struct aiocb *aiocbp);
int aio_mlock(struct aiocb *);
int aio_read(struct aiocb *);
int aio_read2(struct aiocb *, int);
int aio_readv(struct aiocb *);
ssize_t aio_return(struct aiocb *);
int aio_suspend(const struct aiocb *const *, int, const struct timespec *);
ssize_t aio_waitcomplete(struct aiocb **, struct timespec *);
int aio_write(struct aiocb *);
int aio_write2(struct aiocb *, int);
int aio_writev(struct aiocb *);
unsigned int alarm(unsigned int);
void *aligned_alloc(size_t, size_t);
int alphasort(const struct dirent **, const struct dirent **);
__uint32_t arc4random(void);
void arc4random_buf(void *, size_t);
__uint32_t arc4random_uniform(__uint32_t);
char *asctime(const struct tm *);
char *asctime_r(const struct tm *, char *);
double asin(double);
float asinf(float);
double asinh(double);
float asinhf(float);
long double asinhl(long double);
long double asinl(long double);
int asprintf(char **, const char *, ...);
int asprintf_l(char **, locale_t, const char *, ...);
int async_daemon(void);
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
double atof_l(const char *, locale_t);
int atoi(const char *);
int atoi_l(const char *, locale_t);
long atol(const char *);
long atol_l(const char *, locale_t);
long long atoll(const char *);
long long atoll_l(const char *, locale_t);
int audit(const void *, int);
int auditctl(const char *);
int auditon(int, void *, int);
size_t backtrace(void **, size_t);
char **backtrace_symbols(void *const *, size_t);
int backtrace_symbols_fd(void *const *, size_t, int);
int backtrace_symbols_fd_fmt(void *const *, size_t, int, const char *);
char **backtrace_symbols_fmt(void *const *, size_t, const char *);
char *basename(char *);
int bcmp(const void *, const void *, size_t);
void bcopy(const void *, void *, size_t);
unsigned short be16dec(const void *pp);
void be16enc(void *pp, unsigned short u);
unsigned int be32dec(const void *pp);
void be32enc(void *pp, unsigned int u);
unsigned long long be64dec(const void *pp);
void be64enc(void *pp, unsigned long long u);
int bind(int, const struct sockaddr *, socklen_t);
int bindat(int, int, const struct sockaddr *, socklen_t);
unsigned long long bintime2ns(const struct bintime *_bt);
void bintime2timespec(const struct bintime *_bt, struct timespec *_ts);
void bintime2timeval(const struct bintime *_bt, struct timeval *_tv);
void bintime_add(struct bintime *_bt, const struct bintime *_bt2);
void bintime_addx(struct bintime *_bt, unsigned long long _x);
void bintime_mul(struct bintime *_bt, u_int _x);
void bintime_shift(struct bintime *_bt, int _exp);
void bintime_sub(struct bintime *_bt, const struct bintime *_bt2);
int brk(const void *);
void *bsearch(const void *, const void *, size_t, size_t, int (*)(const void *, const void *));
wint_t btowc(int);
wint_t btowc_l(int, locale_t);
sbintime_t bttosbt(const struct bintime _bt);
void bzero(void *, size_t);
void *calloc(size_t, size_t);
int cap_enter(void);
int cap_fcntls_get(int fd, unsigned int *fcntlrightsp);
int cap_fcntls_limit(int fd, unsigned int fcntlrights);
int cap_getmode(u_int *modep);
ssize_t cap_ioctls_get(int fd, cap_ioctl_t *cmds, size_t maxcmds);
int cap_ioctls_limit(int fd, const cap_ioctl_t *cmds, size_t ncmds);
bool cap_rights_contains(const cap_rights_t *big, const cap_rights_t *little);
bool cap_rights_is_empty(const cap_rights_t *rights);
bool cap_rights_is_valid(const cap_rights_t *rights);
int cap_rights_limit(int fd, const cap_rights_t *rights);
cap_rights_t *cap_rights_merge(cap_rights_t *dst, const cap_rights_t *src);
cap_rights_t *cap_rights_remove(cap_rights_t *dst, const cap_rights_t *src);
bool cap_sandboxed(void);
double cbrt(double);
float cbrtf(float);
long double cbrtl(long double);
double ceil(double);
float ceilf(float);
long double ceill(long double);
speed_t cfgetispeed(const struct termios *);
speed_t cfgetospeed(const struct termios *);
void cfmakeraw(struct termios *);
void cfmakesane(struct termios *);
int cfsetispeed(struct termios *, speed_t);
int cfsetospeed(struct termios *, speed_t);
int cfsetspeed(struct termios *, speed_t);
char *cgetcap(char *, const char *, int);
int cgetclose(void);
int cgetent(char **, char **, const char *);
int cgetfirst(char **, char **);
int cgetmatch(const char *, const char *);
int cgetnext(char **, char **);
int cgetnum(char *, const char *, long *);
int cgetset(const char *);
int cgetstr(char *, const char *, char **);
int cgetustr(char *, const char *, char **);
int chdir(const char *);
int check_utility_compat(const char *);
int chflags(const char *, unsigned long);
int chflagsat(int, const char *, unsigned long, int);
int chmod(const char *, mode_t);
int chown(const char *, uid_t, gid_t);
int chroot(const char *);
void clean_environment(const char *const *_white, const char *const *_more_white);
int clearenv(void);
void clearerr(FILE *);
void clearerr_unlocked(FILE *);
clock_t clock(void);
int clock_getcpuclockid(pid_t, clockid_t *);
int clock_getcpuclockid2(id_t, int, clockid_t *);
int clock_getres(clockid_t, struct timespec *);
int clock_gettime(clockid_t, struct timespec *);
int clock_nanosleep(clockid_t, int, const struct timespec *, struct timespec *);
int clock_settime(clockid_t, const struct timespec *);
int close(int);
int close_range(unsigned int, unsigned int, int);
int closedir(DIR *);
void closefrom(int);
void closelog(void);
size_t confstr(int, char *, size_t);
int connect(int, const struct sockaddr *, socklen_t);
int connectat(int, int, const struct sockaddr *, socklen_t);
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
double cospi(double);
float cospif(float);
long double cospil(long double);
int cpuset(cpusetid_t *);
int cpuset_getaffinity(cpulevel_t, cpuwhich_t, id_t, size_t, cpuset_t *);
int cpuset_getid(cpulevel_t, cpuwhich_t, id_t, cpusetid_t *);
int cpuset_parselist(const char *list, cpuset_t *mask);
int cpuset_setaffinity(cpulevel_t, cpuwhich_t, id_t, size_t, const cpuset_t *);
int cpuset_setid(cpuwhich_t, id_t, cpusetid_t);
int creat(const char *, mode_t);
char *crypt(const char *, const char *);
const char *crypt_get_format(void);
char *crypt_r(const char *, const char *, struct crypt_data *);
int crypt_set_format(const char *);
char *ctermid(char *);
char *ctermid_r(char *);
char *ctime(const time_t *);
char *ctime_r(const time_t *, char *);
int daemon(int, int);
int daemonfd(int, int);
void dallocx(void *ptr, int flags);
char *devname(__dev_t, __mode_t);
char *devname_r(__dev_t, __mode_t, char *, int);
double difftime(time_t, time_t);
int digittoint(int);
int digittoint_l(int __c, locale_t __l);
int dirfd(DIR *);
char *dirname(char *);
div_t div(int, int);
int dl_iterate_phdr(__dl_iterate_hdr_callback, void *);
int dladdr(const void *, Dl_info *);
int dlclose(void *);
char *dlerror(void);
dlfunc_t dlfunc(void *, const char *);
int dlinfo(void *, int, void *);
void dllockinit(void *_context, void *(*_lock_create)(void *_context), void (*_rlock_acquire)(void *_lock), void (*_wlock_acquire)(void *_lock), void (*_lock_release)(void *_lock), void (*_lock_destroy)(void *_lock), void (*_context_destroy)(void *_context));
void *dlopen(const char *, int);
void *dlsym(void *, const char *);
void *dlvsym(void *, const char *, const char *);
int dprintf(int, const char *, ...);
int dprintf_l(int, locale_t, const char *, ...);
double drand48(void);
double drem(double, double);
float dremf(float, float);
int dup(int);
int dup2(int, int);
int dup3(int, int, int);
locale_t duplocale(locale_t base);
int eaccess(const char *, int);
int elf_aux_info(int aux, void *buf, int buflen);
void endgrent(void);
void endhostent(void);
void endnetent(void);
void endnetgrent(void);
void endprotoent(void);
void endpwent(void);
void endservent(void);
void endusershell(void);
void endutxent(void);
double erand48(unsigned short *);
double erf(double);
double erfc(double);
float erfcf(float);
long double erfcl(long double);
float erff(float);
long double erfl(long double);
_Noreturn void err(int, const char *, ...);
void err_set_exit(void (*)(int));
void err_set_file(void *);
_Noreturn void errc(int, int, const char *, ...);
_Noreturn void errx(int, const char *, ...);
int eventfd(unsigned int initval, int flags);
int eventfd_read(int fd, eventfd_t *value);
int eventfd_write(int fd, eventfd_t value);
int execl(const char *, const char *, ...);
int execle(const char *, const char *, ...);
int execlp(const char *, const char *, ...);
int exect(const char *, char *const *, char *const *);
int execv(const char *, char *const *);
int execvP(const char *, const char *, char *const *);
int execve(const char *, char *const *, char *const *);
int execvp(const char *, char *const *);
int execvpe(const char *, char *const *, char *const *);
_Noreturn void exit(int);
double exp(double);
double exp2(double);
float exp2f(float);
long double exp2l(long double);
int expand_number(const char *_buf, long long *_num);
int expand_unsigned(const char *_buf, unsigned long long *_num);
float expf(float);
long double expl(long double);
void explicit_bzero(void *, size_t);
double expm1(double);
float expm1f(float);
long double expm1l(long double);
int extattr_delete_fd(int _fd, int _attrnamespace, const char *_attrname);
int extattr_delete_file(const char *_path, int _attrnamespace, const char *_attrname);
int extattr_delete_link(const char *_path, int _attrnamespace, const char *_attrname);
ssize_t extattr_get_fd(int _fd, int _attrnamespace, const char *_attrname, void *_data, size_t _nbytes);
ssize_t extattr_get_file(const char *_path, int _attrnamespace, const char *_attrname, void *_data, size_t _nbytes);
ssize_t extattr_get_link(const char *_path, int _attrnamespace, const char *_attrname, void *_data, size_t _nbytes);
ssize_t extattr_list_fd(int _fd, int _attrnamespace, void *_data, size_t _nbytes);
ssize_t extattr_list_file(const char *_path, int _attrnamespace, void *_data, size_t _nbytes);
ssize_t extattr_list_link(const char *_path, int _attrnamespace, void *_data, size_t _nbytes);
int extattr_namespace_to_string(int _attrnamespace, char **_string);
ssize_t extattr_set_fd(int _fd, int _attrnamespace, const char *_attrname, const void *_data, size_t _nbytes);
ssize_t extattr_set_file(const char *_path, int _attrnamespace, const char *_attrname, const void *_data, size_t _nbytes);
ssize_t extattr_set_link(const char *_path, int _attrnamespace, const char *_attrname, const void *_data, size_t _nbytes);
int extattr_string_to_namespace(const char *_string, int *_attrnamespace);
int extattrctl(const char *_path, int _cmd, const char *_filename, int _attrnamespace, const char *_attrname);
double fabs(double);
float fabsf(float);
long double fabsl(long double);
int faccessat(int, const char *, int, int);
int fchdir(int);
int fchflags(int, unsigned long);
int fchmod(int, mode_t);
int fchmodat(int, const char *, mode_t, int);
int fchown(int, uid_t, gid_t);
int fchownat(int, const char *, uid_t, gid_t, int);
int fchroot(int);
int fclose(FILE *);
void fcloseall(void);
int fcntl(int, int, ...);
int fdatasync(int);
int fdclose(FILE *, int *);
int fdclosedir(DIR *);
char *fdevname(int);
char *fdevname_r(int, char *, int);
double fdim(double, double);
float fdimf(float, float);
long double fdiml(long double, long double);
void *fdlopen(int, int);
FILE *fdopen(int, const char *);
DIR *fdopendir(int);
int fdscandir(int, struct dirent ***, int (*)(const struct dirent *), int (*)(const struct dirent **, const struct dirent **));
int feature_present(const char *);
int feclearexcept(int);
int fedisableexcept(int);
int feenableexcept(int);
int fegetenv(fenv_t *);
int fegetexcept(void);
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
char *fflagstostr(u_long);
int fflush(FILE *);
int fflush_unlocked(FILE *);
int ffs(int);
int ffsl(long);
int ffsll(long long);
int fgetc(FILE *);
char *fgetln(FILE *, size_t *);
int fgetpos(FILE *, fpos_t *);
char *fgets(char *, int, FILE *);
wint_t fgetwc(FILE *);
wint_t fgetwc_l(FILE *, locale_t);
unsigned int *fgetwln(FILE *, size_t *);
unsigned int *fgetws(unsigned int *, int, FILE *);
unsigned int *fgetws_l(unsigned int *, int, FILE *, locale_t);
int fhlink(struct fhandle *, const char *);
int fhlinkat(struct fhandle *, int, const char *);
int fhopen(const struct fhandle *, int);
int fhreadlink(struct fhandle *, char *, size_t);
int fhstat(const struct fhandle *, struct stat *);
int fhstatfs(const struct fhandle *, struct statfs *);
int fileno(FILE *);
int fileno_unlocked(FILE *);
int finite(double);
int finitef(float);
int flock(int, int);
void flockfile(FILE *);
double floor(double);
float floorf(float);
long double floorl(long double);
int flopen(const char *_path, int _flags, ...);
int flopenat(int _dirfd, const char *_path, int _flags, ...);
int fls(int);
int flsl(long);
int flsll(long long);
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
const char *fmtcheck(const char *, const char *);
int fnmatch(const char *, const char *, int);
FILE *fopen(const char *, const char *);
FILE *fopencookie(void *, const char *, cookie_io_functions_t);
pid_t fork(void);
int forkpty(int *_amaster, char *_name, struct termios *_termp, struct winsize *_winp);
char *fparseln(FILE *_fp, size_t *_len, size_t *_lineno, const char *_delim, int _flags);
long fpathconf(int, int);
int fprintf(FILE *, const char *, ...);
int fprintf_l(FILE *, locale_t, const char *, ...);
int fpurge(FILE *);
int fputc(int, FILE *);
int fputc_unlocked(int, FILE *);
int fputs(const char *, FILE *);
int fputs_unlocked(const char *, FILE *);
wint_t fputwc(unsigned int, FILE *);
wint_t fputwc_l(unsigned int, FILE *, locale_t);
int fputws(const unsigned int *, FILE *);
int fputws_l(const unsigned int *, FILE *, locale_t);
size_t fread(void *, size_t, size_t, FILE *);
size_t fread_unlocked(void *, size_t, size_t, FILE *);
void free(void *);
void freeaddrinfo(struct addrinfo *);
void freehostent(struct hostent *);
void freeifaddrs(struct ifaddrs *);
void freeifmaddrs(struct ifmaddrs *);
void freelocale(locale_t loc);
FILE *freopen(const char *, const char *, FILE *);
double frexp(double, int *);
float frexpf(float, int *);
long double frexpl(long double, int *);
int fscanf(FILE *, const char *, ...);
int fscanf_l(FILE *, locale_t, const char *, ...);
int fseek(FILE *, long, int);
int fseeko(FILE *, __off_t, int);
int fsetpos(FILE *, const fpos_t *);
int fsidcmp(const fsid_t *a, const fsid_t *b);
int fspacectl(int, int, const struct spacectl_range *, int, struct spacectl_range *);
int fstat(int, struct stat *);
int fstatat(int, const char *, struct stat *, int);
int fstatfs(int, struct statfs *);
int fstatvfs(int, struct statvfs *);
int fsync(int);
long ftell(FILE *);
__off_t ftello(FILE *);
key_t ftok(const char *, int);
int ftruncate(int, off_t);
int ftrylockfile(FILE *);
FTSENT *fts_children(FTS *, int);
int fts_close(FTS *);
void *fts_get_clientptr(FTS *);
FTS *fts_get_stream(FTSENT *);
FTS *fts_open(char *const *, int, int (*)(const FTSENT *const *, const FTSENT *const *));
FTSENT *fts_read(FTS *);
int fts_set(FTS *, FTSENT *, int);
void fts_set_clientptr(FTS *, void *);
int ftw(const char *, int (*)(const char *, const struct stat *, int), int);
int funlinkat(int, const char *, int, int);
void funlockfile(FILE *);
FILE *funopen(const void *, int (*)(void *, char *, int), int (*)(void *, const char *, int), fpos_t (*)(void *, fpos_t, int), int (*)(void *));
int futimens(int fd, const struct timespec *times);
int futimes(int, const struct timeval *);
int futimesat(int, const char *, const struct timeval *);
int fwide(FILE *, int);
int fwprintf(FILE *, const unsigned int *, ...);
int fwprintf_l(FILE *, locale_t, const unsigned int *, ...);
size_t fwrite(const void *, size_t, size_t, FILE *);
size_t fwrite_unlocked(const void *, size_t, size_t, FILE *);
int fwscanf(FILE *, const unsigned int *, ...);
int fwscanf_l(FILE *, locale_t, const unsigned int *, ...);
const char *gai_strerror(int);
double gamma(double);
double gamma_r(double, int *);
float gammaf(float);
float gammaf_r(float, int *);
int getaddrinfo(const char *, const char *, const struct addrinfo *, struct addrinfo **);
int getaudit(struct auditinfo *);
int getaudit_addr(struct auditinfo_addr *, int);
int getauid(au_id_t *);
const char *getbootfile(void);
char *getbsize(int *, long *);
int getc(FILE *);
int getc_unlocked(FILE *);
int getchar(void);
int getchar_unlocked(void);
int getcontext(ucontext_t *);
ucontext_t *getcontextx(void);
char *getcwd(char *, size_t);
ssize_t getdelim(char **, size_t *, int, FILE *);
ssize_t getdents(int, char *, size_t);
ssize_t getdirentries(int, char *, size_t, off_t *);
int getdomainname(char *, int);
int getdtablesize(void);
gid_t getegid(void);
int getentropy(void *, size_t);
char *getenv(const char *);
int getenv_r(const char *, char *, size_t);
uid_t geteuid(void);
int getfh(const char *, fhandle_t *);
int getfhat(int, char *, struct fhandle *, int);
int getfsstat(struct statfs *, long, int);
gid_t getgid(void);
struct group *getgrent(void);
int getgrent_r(struct group *, char *, size_t, struct group **);
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
int gethostent_r(struct hostent *, char *, size_t, struct hostent **, int *);
long gethostid(void);
int gethostname(char *, size_t);
int getifaddrs(struct ifaddrs **);
int getifmaddrs(struct ifmaddrs **);
struct hostent *getipnodebyaddr(const void *, size_t, int, int *);
struct hostent *getipnodebyname(const char *, int, int, int *);
int getipv4sourcefilter(int, struct in_addr, struct in_addr, unsigned int *, unsigned int *, struct in_addr *);
int getitimer(int, struct itimerval *);
ssize_t getline(char **, size_t *, FILE *);
int getloadavg(double *, int);
const char *getlocalbase(void);
char *getlogin(void);
int getlogin_r(char *, size_t);
int getloginclass(char *, size_t);
int getmntinfo(struct statfs **, int);
mode_t getmode(const void *, mode_t);
int getnameinfo(const struct sockaddr *, socklen_t, char *, size_t, char *, size_t, int);
struct netent *getnetbyaddr(unsigned int, int);
int getnetbyaddr_r(unsigned int, int, struct netent *, char *, size_t, struct netent **, int *);
struct netent *getnetbyname(const char *);
int getnetbyname_r(const char *, struct netent *, char *, size_t, struct netent **, int *);
struct netent *getnetent(void);
int getnetent_r(struct netent *, char *, size_t, struct netent **, int *);
int getnetgrent(char **, char **, char **);
int getnetgrent_r(char **, char **, char **, char *, size_t);
int getopt(int, char *const *, const char *);
int getopt_long(int, char *const *, const char *, const struct option *, int *);
int getopt_long_only(int, char *const *, const char *, const struct option *, int *);
int getosreldate(void);
int getpagesize(void);
int getpagesizes(size_t *, int);
char *getpass(const char *);
int getpeereid(int, uid_t *, gid_t *);
int getpeername(int, struct sockaddr *, socklen_t *);
int getpgid(pid_t _pid);
pid_t getpgrp(void);
pid_t getpid(void);
pid_t getppid(void);
int getpriority(int, int);
const char *getprogname(void);
struct protoent *getprotobyname(const char *);
int getprotobyname_r(const char *, struct protoent *, char *, size_t, struct protoent **);
struct protoent *getprotobynumber(int);
int getprotobynumber_r(int, struct protoent *, char *, size_t, struct protoent **);
struct protoent *getprotoent(void);
int getprotoent_r(struct protoent *, char *, size_t, struct protoent **);
struct passwd *getpwent(void);
int getpwent_r(struct passwd *, char *, size_t, struct passwd **);
struct passwd *getpwnam(const char *);
int getpwnam_r(const char *, struct passwd *, char *, size_t, struct passwd **);
struct passwd *getpwuid(uid_t);
int getpwuid_r(uid_t, struct passwd *, char *, size_t, struct passwd **);
ssize_t getrandom(void *buf, size_t buflen, unsigned int flags);
int getresgid(gid_t *, gid_t *, gid_t *);
int getresuid(uid_t *, uid_t *, uid_t *);
int getrlimit(int, struct rlimit *);
int getrlimitusage(unsigned int which, int flags, rlim_t *res);
int getrusage(int, struct rusage *);
char *gets_s(char *, rsize_t);
struct servent *getservbyname(const char *, const char *);
int getservbyname_r(const char *, const char *, struct servent *, char *, size_t, struct servent **);
struct servent *getservbyport(int, const char *);
int getservbyport_r(int, const char *, struct servent *, char *, size_t, struct servent **);
struct servent *getservent(void);
int getservent_r(struct servent *, char *, size_t, struct servent **);
int getsid(pid_t _pid);
int getsockname(int, struct sockaddr *, socklen_t *);
int getsockopt(int, int, int, void *, socklen_t *);
int getsourcefilter(int, unsigned int, struct sockaddr *, socklen_t, unsigned int *, unsigned int *, struct sockaddr_storage *);
int getsubopt(char **, char *const *, char **);
int gettimeofday(struct timeval *, struct timezone *);
uid_t getuid(void);
char *getusershell(void);
struct utmpx *getutxent(void);
struct utmpx *getutxid(const struct utmpx *);
struct utmpx *getutxline(const struct utmpx *);
struct utmpx *getutxuser(const char *);
int getvfsbyname(const char *, struct xvfsconf *);
int getw(FILE *);
wint_t getwc(FILE *);
wint_t getwc_l(FILE *, locale_t);
wint_t getwchar(void);
wint_t getwchar_l(locale_t);
char *getwd(char *);
int gid_from_group(const char *, gid_t *);
int glob(const char *, int, int (*)(const char *, int), glob_t *);
void globfree(glob_t *);
struct tm *gmtime(const time_t *);
struct tm *gmtime_r(const time_t *, struct tm *);
struct group *gr_add(const struct group *_gr, const char *_newmember);
int gr_copy(int __ffd, int _tfd, const struct group *_gr, struct group *_old_gr);
struct group *gr_dup(const struct group *_gr);
int gr_equal(const struct group *_gr1, const struct group *_gr2);
void gr_fini(void);
int gr_init(const char *_dir, const char *_master);
int gr_lock(void);
char *gr_make(const struct group *_gr);
int gr_mkdb(void);
struct group *gr_scan(const char *_line);
int gr_tmp(int _mdf);
int grantpt(int);
const char *group_from_gid(gid_t, int);
int hcreate(size_t);
int hcreate_r(size_t, struct hsearch_data *);
void hdestroy(void);
void hdestroy_r(struct hsearch_data *);
int heapsort(void *, size_t, size_t, int (*)(const void *, const void *));
void herror(const char *);
void hexdump(const void *_ptr, int _length, const char *_hdr, int _flags);
ENTRY *hsearch(ENTRY, ACTION);
int hsearch_r(ENTRY, ACTION, ENTRY **, struct hsearch_data *);
const char *hstrerror(int);
unsigned int htonl(unsigned int);
unsigned short htons(unsigned short);
int humanize_number(char *_buf, size_t _len, long long _number, const char *_suffix, int _scale, int _flags);
double hypot(double, double);
float hypotf(float, float);
long double hypotl(long double, long double);
size_t iconv(iconv_t, char **, size_t *, char **, size_t *);
const char *iconv_canonicalize(const char *);
int iconv_close(iconv_t);
iconv_t iconv_open(const char *, const char *);
int iconv_open_into(const char *, const char *, iconv_allocation_t *);
void iconv_set_relocation_prefix(const char *, const char *);
int iconvctl(iconv_t, int, void *);
void iconvlist(int (*do_one)(unsigned int, const char *const *, void *), void *);
void if_freenameindex(struct if_nameindex *);
char *if_indextoname(unsigned int, char *);
struct if_nameindex *if_nameindex(void);
unsigned int if_nametoindex(const char *);
void ignore_handler_s(const char *, void *, errno_t);
int ilogb(double);
int ilogbf(float);
int ilogbl(long double);
intmax_t imaxabs(intmax_t);
imaxdiv_t imaxdiv(intmax_t, intmax_t);
char *index(const char *, int);
int inet6_opt_append(void *, socklen_t, int, unsigned char, socklen_t, unsigned char, void **);
int inet6_opt_find(void *, socklen_t, int, unsigned char, socklen_t *, void **);
int inet6_opt_finish(void *, socklen_t, int);
int inet6_opt_get_val(void *, int, void *, socklen_t);
int inet6_opt_init(void *, socklen_t);
int inet6_opt_next(void *, socklen_t, int, unsigned char *, socklen_t *, void **);
int inet6_opt_set_val(void *, int, void *, socklen_t);
unsigned char *inet6_option_alloc(struct cmsghdr *, int, int, int);
int inet6_option_append(struct cmsghdr *, const unsigned char *, int, int);
int inet6_option_find(const struct cmsghdr *, unsigned char **, int);
int inet6_option_init(void *, struct cmsghdr **, int);
int inet6_option_next(const struct cmsghdr *, unsigned char **);
int inet6_option_space(int);
int inet6_rth_add(void *, const struct in6_addr *);
struct in6_addr *inet6_rth_getaddr(const void *, int);
void *inet6_rth_init(void *, socklen_t, int, int);
int inet6_rth_reverse(const void *, void *);
int inet6_rth_segments(const void *);
socklen_t inet6_rth_space(int, int);
int inet6_rthdr_add(struct cmsghdr *, const struct in6_addr *, unsigned int);
struct in6_addr *inet6_rthdr_getaddr(struct cmsghdr *, int);
int inet6_rthdr_getflags(const struct cmsghdr *, int);
struct cmsghdr *inet6_rthdr_init(void *, int);
int inet6_rthdr_lasthop(struct cmsghdr *, unsigned int);
int inet6_rthdr_segments(const struct cmsghdr *);
size_t inet6_rthdr_space(int, int);
int initgroups(const char *, gid_t);
char *initstate(unsigned int, char *, size_t);
int innetgr(const char *, const char *, const char *, const char *);
int inotify_add_watch(int fd, const char *pathname, __uint32_t mask);
int inotify_add_watch_at(int fd, int dfd, const char *pathname, __uint32_t mask);
int inotify_init(void);
int inotify_init1(int flags);
int inotify_rm_watch(int fd, int wd);
void insque(void *, void *);
int ioctl(int, unsigned long, ...);
int iruserok(unsigned long, int, const char *, const char *);
int iruserok_sa(const void *, int, int, const char *, const char *);
int isalnum(int);
int isalnum_l(int __c, locale_t __l);
int isalpha(int);
int isalpha_l(int __c, locale_t __l);
int isascii(int);
int isatty(int);
int isblank(int);
int isblank_l(int __c, locale_t __l);
int iscntrl(int);
int iscntrl_l(int __c, locale_t __l);
int isdigit(int);
int isdigit_l(int __c, locale_t __l);
int isgraph(int);
int isgraph_l(int __c, locale_t __l);
int ishexnumber(int);
int ishexnumber_l(int __c, locale_t __l);
int isideogram(int);
int isideogram_l(int __c, locale_t __l);
int islower(int);
int islower_l(int __c, locale_t __l);
int isnanf(float);
int isnumber(int);
int isnumber_l(int __c, locale_t __l);
int isphonogram(int);
int isphonogram_l(int __c, locale_t __l);
int isprint(int);
int isprint_l(int __c, locale_t __l);
int ispunct(int);
int ispunct_l(int __c, locale_t __l);
int isrune(int);
int isrune_l(int __c, locale_t __l);
int issetugid(void);
int isspace(int);
int isspace_l(int __c, locale_t __l);
int isspecial(int);
int isspecial_l(int __c, locale_t __l);
int isupper(int);
int isupper_l(int __c, locale_t __l);
int iswalnum(wint_t);
int iswalnum_l(int __c, locale_t __l);
int iswalpha(wint_t);
int iswalpha_l(int __c, locale_t __l);
wint_t iswascii(wint_t);
int iswblank(wint_t);
int iswblank_l(int __c, locale_t __l);
int iswcntrl(wint_t);
int iswcntrl_l(int __c, locale_t __l);
int iswctype(wint_t, wctype_t);
int iswctype_l(wint_t __wc, wctype_t __charclass, locale_t __l);
int iswdigit(wint_t);
int iswdigit_l(int __c, locale_t __l);
int iswgraph(wint_t);
int iswgraph_l(int __c, locale_t __l);
wint_t iswhexnumber(wint_t);
int iswhexnumber_l(int __c, locale_t __l);
wint_t iswideogram(wint_t);
int iswideogram_l(int __c, locale_t __l);
int iswlower(wint_t);
int iswlower_l(int __c, locale_t __l);
wint_t iswnumber(wint_t);
int iswnumber_l(int __c, locale_t __l);
wint_t iswphonogram(wint_t);
int iswphonogram_l(int __c, locale_t __l);
int iswprint(wint_t);
int iswprint_l(int __c, locale_t __l);
int iswpunct(wint_t);
int iswpunct_l(int __c, locale_t __l);
wint_t iswrune(wint_t);
int iswrune_l(int __c, locale_t __l);
int iswspace(wint_t);
int iswspace_l(int __c, locale_t __l);
wint_t iswspecial(wint_t);
int iswspecial_l(int __c, locale_t __l);
int iswupper(wint_t);
int iswupper_l(int __c, locale_t __l);
int iswxdigit(wint_t);
int iswxdigit_l(int __c, locale_t __l);
int isxdigit(int);
int isxdigit_l(int __c, locale_t __l);
double j0(double);
float j0f(float);
double j1(double);
float j1f(float);
int jail(struct jail *);
int jail_attach(int);
int jail_attach_jd(int);
int jail_get(struct iovec *, unsigned int, int);
int jail_remove(int);
int jail_remove_jd(int);
int jail_set(struct iovec *, unsigned int, int);
double jn(int, double);
float jnf(int, float);
long jrand48(unsigned short *);
int kcmp(pid_t pid1, pid_t pid2, int type, uintptr_t idx1, uintptr_t idx2);
int kevent(int kq, const struct kevent *changelist, int nchanges, struct kevent *eventlist, int nevents, const struct timespec *timeout);
int kill(__pid_t, int);
int killpg(__pid_t, int);
struct kinfo_proc *kinfo_getallproc(int *_cntp);
struct kinfo_file *kinfo_getfile(pid_t _pid, int *_cntp);
struct kinfo_proc *kinfo_getproc(pid_t _pid);
struct kinfo_vmobject *kinfo_getswapvmobject(int *_cntp);
struct kinfo_vmentry *kinfo_getvmmap(pid_t _pid, int *_cntp);
struct kinfo_vmobject *kinfo_getvmobject(int *_cntp);
int kld_isloaded(const char *_name);
int kld_load(const char *_name);
int kldfind(const char *_file);
int kldfirstmod(int _fileid);
int kldload(const char *_file);
int kldnext(int _fileid);
int kldstat(int _fileid, struct kld_file_stat *_stat);
int kldsym(int _fileid, int _cmd, void *_data);
int kldunload(int _fileid);
int kldunloadf(int _fileid, int flags);
int kqueue(void);
int kqueue1(int flags);
int kqueuex(unsigned int flags);
int kvm_close(kvm_t *);
unsigned long long kvm_counter_u64_fetch(kvm_t *, u_long);
int kvm_dpcpu_setcpu(kvm_t *, unsigned int);
char **kvm_getargv(kvm_t *, const struct kinfo_proc *, int);
int kvm_getcptime(kvm_t *, long *);
char **kvm_getenvv(kvm_t *, const struct kinfo_proc *, int);
char *kvm_geterr(kvm_t *);
int kvm_getloadavg(kvm_t *, double *, int);
int kvm_getmaxcpu(kvm_t *);
int kvm_getncpus(kvm_t *);
void *kvm_getpcpu(kvm_t *, int);
struct kinfo_proc *kvm_getprocs(kvm_t *, int, int, int *);
int kvm_getswapinfo(kvm_t *, struct kvm_swap *, int, int);
kssize_t kvm_kerndisp(kvm_t *);
int kvm_native(kvm_t *);
int kvm_nlist(kvm_t *, struct nlist *);
int kvm_nlist2(kvm_t *, struct kvm_nlist *);
kvm_t *kvm_open(const char *, const char *, const char *, int, const char *);
kvm_t *kvm_open2(const char *, const char *, int, char *, int (*)(const char *, kvaddr_t *));
kvm_t *kvm_openfiles(const char *, const char *, const char *, int, char *);
ssize_t kvm_read(kvm_t *, unsigned long, void *, size_t);
ssize_t kvm_read2(kvm_t *, kvaddr_t, void *, size_t);
ssize_t kvm_read_zpcpu(kvm_t *, unsigned long, void *, size_t, int);
int kvm_walk_pages(kvm_t *, kvm_walk_pages_cb_t *, void *);
ssize_t kvm_write(kvm_t *, unsigned long, const void *, size_t);
char *l64a(long);
int l64a_r(long, char *, int);
long labs(long);
int lchflags(const char *, unsigned long);
int lchmod(const char *, mode_t);
int lchown(const char *, uid_t, gid_t);
void lcong48(unsigned short *);
double ldexp(double, int);
float ldexpf(float, int);
long double ldexpl(long double, int);
ldiv_t ldiv(long, long);
unsigned short le16dec(const void *pp);
void le16enc(void *pp, unsigned short u);
unsigned int le32dec(const void *pp);
void le32enc(void *pp, unsigned int u);
unsigned long long le64dec(const void *pp);
void le64enc(void *pp, unsigned long long u);
void *lfind(const void *, const void *, size_t *, size_t, int (*)(const void *, const void *));
double lgamma(double);
double lgamma_r(double, int *);
float lgammaf(float);
float lgammaf_r(float, int *);
long double lgammal(long double);
long double lgammal_r(long double, int *);
int lgetfh(const char *, fhandle_t *);
int link(const char *, const char *);
int linkat(int, const char *, int, const char *, int);
int lio_listio(int, struct aiocb *const *, int, struct sigevent *);
int listen(int, int);
long long llabs(long long);
lldiv_t lldiv(long long, long long);
long long llrint(double);
long long llrintf(float);
long long llrintl(long double);
long long llround(double);
long long llroundf(float);
long long llroundl(long double);
struct lconv *localeconv(void);
struct lconv *localeconv_l(locale_t);
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
int login_tty(int _fd);
long double logl(long double);
_Noreturn void longjmp(jmp_buf, int);
void longjmperror(void);
long lpathconf(const char *, int);
long lrand48(void);
long lrint(double);
long lrintf(float);
long lrintl(long double);
long lround(double);
long lroundf(float);
long lroundl(long double);
void *lsearch(const void *, void *, size_t *, size_t, int (*)(const void *, const void *));
off_t lseek(int, off_t, int);
int lstat(const char *, struct stat *);
int lutimes(const char *, const struct timeval *);
int madvise(void *, size_t, int);
void makecontext(ucontext_t *, void (*)(void), int, ...);
int mallctl(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
int mallctlbymib(const size_t *mib, size_t miblen, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
int mallctlnametomib(const char *name, size_t *mibp, size_t *miblenp);
void *malloc(size_t);
void malloc_stats_print(void (*write_cb)(void *, const char *), void *cbopaque, const char *opts);
size_t malloc_usable_size(const void *ptr);
void *mallocx(size_t size, int flags);
int mblen(const char *, size_t);
int mblen_l(const char *, size_t, locale_t);
size_t mbrlen(const char *, size_t, mbstate_t *);
size_t mbrlen_l(const char *, size_t, mbstate_t *, locale_t);
size_t mbrtowc(unsigned int *, const char *, size_t, mbstate_t *);
size_t mbrtowc_l(unsigned int *, const char *, size_t, mbstate_t *, locale_t);
int mbsinit(const mbstate_t *);
int mbsinit_l(const mbstate_t *, locale_t);
size_t mbsnrtowcs(unsigned int *, const char **, size_t, size_t, mbstate_t *);
size_t mbsnrtowcs_l(unsigned int *, const char **, size_t, size_t, mbstate_t *, locale_t);
size_t mbsrtowcs(unsigned int *, const char **, size_t, mbstate_t *);
size_t mbsrtowcs_l(unsigned int *, const char **, size_t, mbstate_t *, locale_t);
size_t mbstowcs(unsigned int *, const char *, size_t);
size_t mbstowcs_l(unsigned int *, const char *, size_t, locale_t);
int mbtowc(unsigned int *, const char *, size_t);
int mbtowc_l(unsigned int *, const char *, size_t, locale_t);
void *memalign(size_t, size_t);
size_t memalignment(const void *);
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
void *memset_explicit(void *, int, size_t);
errno_t memset_s(void *, rsize_t, int, rsize_t);
int mergesort(void *, size_t, size_t, int (*)(const void *, const void *));
int mincore(const void *, size_t, char *);
int minherit(void *, size_t, int);
int mkdir(const char *, mode_t);
int mkdirat(int, const char *, mode_t);
char *mkdtemp(char *);
int mkfifo(const char *, mode_t);
int mkfifoat(int, const char *, mode_t);
int mknod(const char *, mode_t, dev_t);
int mknodat(int, const char *, mode_t, dev_t);
int mkostemp(char *, int);
int mkostemps(char *, int, int);
int mkostempsat(int, char *, int, int);
int mkstemp(char *);
int mkstemps(char *, int);
char *mktemp(char *);
time_t mktime(struct tm *);
int mlock(const void *, size_t);
int mlockall(int);
void *mmap(void *, size_t, int, int, int, off_t);
double modf(double, double *);
float modff(float, float *);
int modfind(const char *_name);
long double modfl(long double, long double *);
int modfnext(int _modid);
int modnext(int _modid);
int modstat(int _modid, struct module_stat *_stat);
int mount(const char *, const char *, int, void *);
int mprotect(void *, size_t, int);
int mq_close(mqd_t);
int mq_getattr(mqd_t, struct mq_attr *);
int mq_getfd_np(mqd_t mqd);
int mq_notify(mqd_t, const struct sigevent *);
mqd_t mq_open(const char *, int, ...);
ssize_t mq_receive(mqd_t, char *, size_t, unsigned int *);
int mq_send(mqd_t, const char *, size_t, unsigned int);
int mq_setattr(mqd_t, const struct mq_attr *, struct mq_attr *);
ssize_t mq_timedreceive(mqd_t, char *, size_t, unsigned int *, const struct timespec *);
int mq_timedsend(mqd_t, const char *, size_t, unsigned int, const struct timespec *);
int mq_unlink(const char *);
long mrand48(void);
int msgctl(int, int, struct msqid_ds *);
int msgget(key_t, int);
ssize_t msgrcv(int, void *, size_t, long, int);
int msgsnd(int, const void *, size_t, int);
sbintime_t mstosbt(long long ms);
int msync(void *, size_t, int);
int munlock(const void *, size_t);
int munlockall(void);
int munmap(void *, size_t);
size_t nallocx(size_t size, int flags);
double nan(const char *);
float nanf(const char *);
long double nanl(const char *);
int nanosleep(const struct timespec *, struct timespec *);
double nearbyint(double);
float nearbyintf(float);
long double nearbyintl(long double);
locale_t newlocale(int mask, const char *locale, locale_t base);
double nextafter(double, double);
float nextafterf(float, float);
long double nextafterl(long double, long double);
double nexttoward(double, long double);
float nexttowardf(float, long double);
long double nexttowardl(long double, long double);
wint_t nextwctype(wint_t, wctype_t);
wint_t nextwctype_l(wint_t __wc, wctype_t wct, locale_t __l);
int nfssvc(int, void *);
int nftw(const char *, int (*)(const char *, const struct stat *, int, struct FTW *), int, int);
int nice(int);
char *nl_langinfo(nl_item);
char *nl_langinfo_l(nl_item, locale_t);
int nlist(const char *, struct nlist *);
int nlm_syscall(int, int, int, char **);
int nmount(struct iovec *, unsigned int, int);
long nrand48(unsigned short *);
sbintime_t nstosbt(long long ns);
unsigned int ntohl(unsigned int);
unsigned short ntohs(unsigned short);
char *nvis(char *, size_t, int, int, int);
struct tm *offtime(const time_t *, long);
struct tm *offtime_r(const time_t *, long, struct tm *);
int open(const char *, int, ...);
FILE *open_memstream(char **, size_t *);
FILE *open_wmemstream(unsigned int **, size_t *);
int openat(int, const char *, int, ...);
DIR *opendir(const char *);
void openlog(const char *, int, int);
int openpty(int *_amaster, int *_aslave, char *_name, struct termios *_termp, struct winsize *_winp);
long pathconf(const char *, int);
int pause(void);
int pclose(FILE *);
void perror(const char *);
int pidfile_close(struct pidfh *_pfh);
int pidfile_fileno(const struct pidfh *_pfh);
struct pidfh *pidfile_open(const char *_path, mode_t _mode, pid_t *_pidptr);
int pidfile_remove(struct pidfh *_pfh);
int pidfile_signal(const char *pathp, int sig, pid_t *pidptr);
int pidfile_write(struct pidfh *_pfh);
int pipe(int *);
int pipe2(int *, int);
int poll(struct pollfd *_pfd, nfds_t _nfds, int _timeout);
FILE *popen(const char *, const char *);
time_t posix2time(time_t t);
int posix_fadvise(int, off_t, off_t, int);
int posix_fallocate(int, off_t, off_t);
int posix_madvise(void *, size_t, int);
int posix_memalign(void **, size_t, size_t);
int posix_openpt(int);
int posix_spawn(pid_t *, const char *, const posix_spawn_file_actions_t *, const posix_spawnattr_t *, char *const *, char *const *);
int posix_spawn_file_actions_addchdir(posix_spawn_file_actions_t *, const char *);
int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t *, const char *);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *, int);
int posix_spawn_file_actions_addclosefrom_np(posix_spawn_file_actions_t *, int);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *, int, int);
int posix_spawn_file_actions_addfchdir(posix_spawn_file_actions_t *, int);
int posix_spawn_file_actions_addfchdir_np(posix_spawn_file_actions_t *, int);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *, int, const char *, int, mode_t);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *);
int posix_spawn_file_actions_init(posix_spawn_file_actions_t *);
int posix_spawnattr_destroy(posix_spawnattr_t *);
int posix_spawnattr_getexecfd_np(const posix_spawnattr_t *, int *);
int posix_spawnattr_getflags(const posix_spawnattr_t *, short *);
int posix_spawnattr_getpgroup(const posix_spawnattr_t *, pid_t *);
int posix_spawnattr_getprocdescp_np(const posix_spawnattr_t *, int **, int *);
int posix_spawnattr_getschedparam(const posix_spawnattr_t *, struct sched_param *);
int posix_spawnattr_getschedpolicy(const posix_spawnattr_t *, int *);
int posix_spawnattr_getsigdefault(const posix_spawnattr_t *, sigset_t *);
int posix_spawnattr_getsigmask(const posix_spawnattr_t *, sigset_t *sigmask);
int posix_spawnattr_init(posix_spawnattr_t *);
int posix_spawnattr_setexecfd_np(posix_spawnattr_t *, int);
int posix_spawnattr_setflags(posix_spawnattr_t *, short);
int posix_spawnattr_setpgroup(posix_spawnattr_t *, pid_t);
int posix_spawnattr_setprocdescp_np(const posix_spawnattr_t *, int *, int);
int posix_spawnattr_setschedparam(posix_spawnattr_t *, const struct sched_param *);
int posix_spawnattr_setschedpolicy(posix_spawnattr_t *, int);
int posix_spawnattr_setsigdefault(posix_spawnattr_t *, const sigset_t *);
int posix_spawnattr_setsigmask(posix_spawnattr_t *, const sigset_t *);
int posix_spawnp(pid_t *, const char *, const posix_spawn_file_actions_t *, const posix_spawnattr_t *, char *const *, char *const *);
double pow(double, double);
float powf(float, float);
long double powl(long double, long double);
int ppoll(struct pollfd *_pfd, nfds_t _nfds, const struct timespec *_timeout, const sigset_t *_newsigmask);
ssize_t pread(int, void *, size_t, off_t);
ssize_t preadv(int, const struct iovec *, int, off_t);
int printf(const char *, ...);
int printf_l(locale_t, const char *, ...);
int procctl(idtype_t, id_t, int, void *);
int profil(char *, size_t, vm_offset_t, int);
void properties_free(properties _list);
properties properties_read(int _fd);
char *property_find(properties _list, const char *_name);
int pselect(int, fd_set *, fd_set *, fd_set *, const struct timespec *, const sigset_t *);
void psiginfo(const siginfo_t *, const char *);
void psignal(int, const char *);
int pthread_atfork(void (*)(void), void (*)(void), void (*)(void));
int pthread_attr_destroy(pthread_attr_t *);
int pthread_attr_get_np(pthread_t, pthread_attr_t *);
int pthread_attr_getaffinity_np(const pthread_attr_t *, size_t, cpuset_t *);
int pthread_attr_getdetachstate(const pthread_attr_t *, int *);
int pthread_attr_getguardsize(const pthread_attr_t *, size_t *);
int pthread_attr_getinheritsched(const pthread_attr_t *, int *);
int pthread_attr_getschedparam(const pthread_attr_t *, struct sched_param *);
int pthread_attr_getschedpolicy(const pthread_attr_t *, int *);
int pthread_attr_getscope(const pthread_attr_t *, int *);
int pthread_attr_getstack(const pthread_attr_t *, void **, size_t *);
int pthread_attr_getstackaddr(const pthread_attr_t *, void **);
int pthread_attr_getstacksize(const pthread_attr_t *, size_t *);
int pthread_attr_init(pthread_attr_t *);
int pthread_attr_setaffinity_np(pthread_attr_t *, size_t, const cpuset_t *);
int pthread_attr_setcreatesuspend_np(pthread_attr_t *);
int pthread_attr_setdetachstate(pthread_attr_t *, int);
int pthread_attr_setguardsize(pthread_attr_t *, size_t);
int pthread_attr_setinheritsched(pthread_attr_t *, int);
int pthread_attr_setschedparam(pthread_attr_t *, const struct sched_param *);
int pthread_attr_setschedpolicy(pthread_attr_t *, int);
int pthread_attr_setscope(pthread_attr_t *, int);
int pthread_attr_setstack(pthread_attr_t *, void *, size_t);
int pthread_attr_setstackaddr(pthread_attr_t *, void *);
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
int pthread_cond_timedwait(pthread_cond_t *, pthread_mutex_t *__mutex, const struct timespec *);
int pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *__mutex);
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
void pthread_get_name_np(pthread_t, char *, size_t);
int pthread_getaffinity_np(pthread_t, size_t, cpuset_t *);
int pthread_getconcurrency(void);
int pthread_getcpuclockid(pthread_t, clockid_t *);
int pthread_getname_np(pthread_t, char *, size_t);
int pthread_getprio(pthread_t);
int pthread_getschedparam(pthread_t pthread, int *, struct sched_param *);
void *pthread_getspecific(pthread_key_t);
int pthread_getthreadid_np(void);
int pthread_join(pthread_t, void **);
int pthread_key_create(pthread_key_t *, void (*)(void *));
int pthread_key_delete(pthread_key_t);
int pthread_kill(__pthread_t, int);
int pthread_main_np(void);
int pthread_multi_np(void);
int pthread_mutex_consistent(pthread_mutex_t *__mutex);
int pthread_mutex_destroy(pthread_mutex_t *__mutex);
int pthread_mutex_getprioceiling(const pthread_mutex_t *, int *);
int pthread_mutex_getspinloops_np(pthread_mutex_t *mutex, int *count);
int pthread_mutex_getyieldloops_np(pthread_mutex_t *mutex, int *count);
int pthread_mutex_init(pthread_mutex_t *__mutex, const pthread_mutexattr_t *);
int pthread_mutex_isowned_np(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *__mutex);
int pthread_mutex_setprioceiling(pthread_mutex_t *, int, int *);
int pthread_mutex_setspinloops_np(pthread_mutex_t *mutex, int count);
int pthread_mutex_setyieldloops_np(pthread_mutex_t *mutex, int count);
int pthread_mutex_timedlock(pthread_mutex_t *__mutex, const struct timespec *);
int pthread_mutex_trylock(pthread_mutex_t *__mutex);
int pthread_mutex_unlock(pthread_mutex_t *__mutex);
int pthread_mutexattr_destroy(pthread_mutexattr_t *);
int pthread_mutexattr_getkind_np(pthread_mutexattr_t);
int pthread_mutexattr_getprioceiling(const pthread_mutexattr_t *, int *);
int pthread_mutexattr_getprotocol(const pthread_mutexattr_t *, int *);
int pthread_mutexattr_getpshared(const pthread_mutexattr_t *, int *);
int pthread_mutexattr_getrobust(pthread_mutexattr_t *, int *);
int pthread_mutexattr_gettype(const pthread_mutexattr_t *, int *);
int pthread_mutexattr_init(pthread_mutexattr_t *);
int pthread_mutexattr_setkind_np(pthread_mutexattr_t *, int);
int pthread_mutexattr_setprioceiling(pthread_mutexattr_t *, int);
int pthread_mutexattr_setprotocol(pthread_mutexattr_t *, int);
int pthread_mutexattr_setpshared(pthread_mutexattr_t *, int);
int pthread_mutexattr_setrobust(pthread_mutexattr_t *, int);
int pthread_mutexattr_settype(pthread_mutexattr_t *, int);
int pthread_once(pthread_once_t *, void (*)(void));
int pthread_peekjoin_np(pthread_t, void **);
void pthread_resume_all_np(void);
int pthread_resume_np(pthread_t);
int pthread_rwlock_destroy(pthread_rwlock_t *__rwlock);
int pthread_rwlock_init(pthread_rwlock_t *__rwlock, const pthread_rwlockattr_t *);
int pthread_rwlock_rdlock(pthread_rwlock_t *__rwlock);
int pthread_rwlock_timedrdlock(pthread_rwlock_t *__rwlock, const struct timespec *);
int pthread_rwlock_timedwrlock(pthread_rwlock_t *__rwlock, const struct timespec *);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *__rwlock);
int pthread_rwlock_trywrlock(pthread_rwlock_t *__rwlock);
int pthread_rwlock_unlock(pthread_rwlock_t *__rwlock);
int pthread_rwlock_wrlock(pthread_rwlock_t *__rwlock);
int pthread_rwlockattr_destroy(pthread_rwlockattr_t *);
int pthread_rwlockattr_getkind_np(const pthread_rwlockattr_t *, int *);
int pthread_rwlockattr_getpshared(const pthread_rwlockattr_t *, int *);
int pthread_rwlockattr_init(pthread_rwlockattr_t *);
int pthread_rwlockattr_setkind_np(pthread_rwlockattr_t *, int);
int pthread_rwlockattr_setpshared(pthread_rwlockattr_t *, int);
pthread_t pthread_self(void);
void pthread_set_name_np(pthread_t, const char *);
int pthread_setaffinity_np(pthread_t, size_t, const cpuset_t *);
int pthread_setcancelstate(int, int *);
int pthread_setcanceltype(int, int *);
int pthread_setconcurrency(int);
int pthread_setname_np(pthread_t, const char *);
int pthread_setprio(pthread_t, int);
int pthread_setschedparam(pthread_t, int, const struct sched_param *);
int pthread_setspecific(pthread_key_t, const void *);
int pthread_sigmask(int, const __sigset_t *, __sigset_t *);
void pthread_signals_block_np(void);
void pthread_signals_unblock_np(void);
int pthread_sigqueue(pthread_t thread, int sig, const union sigval value);
int pthread_single_np(void);
int pthread_spin_destroy(pthread_spinlock_t *__spin);
int pthread_spin_init(pthread_spinlock_t *__spin, int);
int pthread_spin_lock(pthread_spinlock_t *__spin);
int pthread_spin_trylock(pthread_spinlock_t *__spin);
int pthread_spin_unlock(pthread_spinlock_t *__spin);
void pthread_suspend_all_np(void);
int pthread_suspend_np(pthread_t);
void pthread_testcancel(void);
int pthread_timedjoin_np(pthread_t, void **, const struct timespec *);
int pthread_tryjoin_np(pthread_t, void **);
void pthread_yield(void);
int ptrace(int _request, pid_t _pid, caddr_t _addr, int _data);
char *ptsname(int);
int ptsname_r(int, char *, size_t);
int putc(int, FILE *);
int putc_unlocked(int, FILE *);
int putchar(int);
int putchar_unlocked(int);
int putenv(char *);
int puts(const char *);
struct utmpx *pututxline(const struct utmpx *);
int putw(int, FILE *);
wint_t putwc(unsigned int, FILE *);
wint_t putwc_l(unsigned int, FILE *, locale_t);
wint_t putwchar(unsigned int);
wint_t putwchar_l(unsigned int, locale_t);
int pw_copy(int _ffd, int _tfd, const struct passwd *_pw, struct passwd *_old_pw);
struct passwd *pw_dup(const struct passwd *_pw);
int pw_edit(int _notsetuid);
int pw_equal(const struct passwd *_pw1, const struct passwd *_pw2);
void pw_fini(void);
int pw_init(const char *_dir, const char *_master);
void pw_initpwd(struct passwd *_pw);
int pw_lock(void);
char *pw_make(const struct passwd *_pw);
char *pw_make_v7(const struct passwd *_pw);
int pw_mkdb(const char *_user);
struct passwd *pw_scan(const char *_line, int _flags);
const char *pw_tempname(void);
int pw_tmp(int _mfd);
int pwcache_groupdb(int (*)(int), void (*)(void), struct group *(*)(const char *), struct group *(*)(gid_t));
int pwcache_userdb(int (*)(int), void (*)(void), struct passwd *(*)(const char *), struct passwd *(*)(uid_t));
ssize_t pwrite(int, const void *, size_t, off_t);
ssize_t pwritev(int, const struct iovec *, int, off_t);
void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
void qsort_r(void *, size_t, size_t, int (*)(const void *, const void *, void *), void *);
errno_t qsort_s(void *, rsize_t, rsize_t, int (*)(const void *, const void *, void *), void *);
const char *querylocale(int mask, locale_t loc);
_Noreturn void quick_exit(int);
int radixsort(const unsigned char **, int, const unsigned char *, unsigned int);
int raise(int);
void *rallocx(void *ptr, size_t size, int flags);
int rand(void);
int rand_r(unsigned int *);
long random(void);
int rcmd(char **, int, const char *, const char *, const char *, int *);
int rcmd_af(char **, int, const char *, const char *, const char *, int *, int);
int rcmdsh(char **, int, const char *, const char *, const char *, const char *);
char *re_comp(const char *);
int re_exec(const char *);
ssize_t read(int, void *, size_t);
struct dirent *readdir(DIR *);
int readdir_r(DIR *, struct dirent *, struct dirent **);
ssize_t readlink(const char *, char *, size_t);
ssize_t readlinkat(int, const char *, char *, size_t);
ssize_t readv(int, const struct iovec *, int);
int realhostname(char *_host, size_t _hsize, const struct in_addr *_ip);
int realhostname_sa(char *_host, size_t _hsize, struct sockaddr *_addr, int _addrlen);
void *realloc(void *, size_t);
void *reallocarray(void *, size_t, size_t);
void *reallocf(void *, size_t);
char *realpath(const char *, char *);
int reboot(int);
void *recallocarray(void *, size_t, size_t, size_t);
ssize_t recv(int, void *, size_t, int);
ssize_t recvfrom(int, void *, size_t, int, struct sockaddr *, socklen_t *);
ssize_t recvmmsg(int, struct mmsghdr *, size_t, int, const struct timespec *);
ssize_t recvmsg(int, struct msghdr *, int);
int regcomp(regex_t *, const char *, int);
size_t regerror(int, const regex_t *, char *, size_t);
int regexec(const regex_t *, const char *, size_t, regmatch_t *, int);
void regfree(regex_t *);
double remainder(double, double);
float remainderf(float, float);
long double remainderl(long double, long double);
int remove(const char *);
void remque(void *);
double remquo(double, double, int *);
float remquof(float, float, int *);
long double remquol(long double, long double, int *);
int rename(const char *, const char *);
int renameat(int, const char *, int, const char *);
int renameat2(int, const char *, int, const char *, unsigned int);
int revoke(const char *);
void rewind(FILE *);
void rewinddir(DIR *);
pid_t rfork(int);
pid_t rfork_thread(int, void *, int (*)(void *), void *);
char *rindex(const char *, int);
double rint(double);
float rintf(float);
long double rintl(long double);
int rmdir(const char *);
double round(double);
float roundf(float);
long double roundl(long double);
int rpmatch(const char *);
int rresvport(int *);
int rresvport_af(int *, int);
const char *rtld_get_var(const char *name);
int rtld_set_var(const char *name, const char *val);
int ruserok(const char *, int, const char *, const char *);
size_t sallocx(const void *ptr, int flags);
int sbintime_getsec(sbintime_t _sbt);
void *sbrk(intptr_t);
struct bintime sbttobt(sbintime_t _sbt);
long long sbttoms(sbintime_t sbt);
long long sbttons(sbintime_t sbt);
struct timespec sbttots(sbintime_t _sbt);
struct timeval sbttotv(sbintime_t _sbt);
long long sbttous(sbintime_t sbt);
double scalb(double, double);
float scalbf(float, float);
double scalbln(double, long);
float scalblnf(float, long);
long double scalblnl(long double, long);
double scalbn(double, int);
float scalbnf(float, int);
long double scalbnl(long double, int);
int scandir(const char *, struct dirent ***, int (*)(const struct dirent *), int (*)(const struct dirent **, const struct dirent **));
int scandirat(int, const char *, struct dirent ***, int (*)(const struct dirent *), int (*)(const struct dirent **, const struct dirent **));
int scanf(const char *, ...);
int scanf_l(locale_t, const char *, ...);
int sched_get_priority_max(int);
int sched_get_priority_min(int);
int sched_getaffinity(pid_t pid, size_t cpusetsz, cpuset_t *cpuset);
int sched_getcpu(void);
int sched_getparam(pid_t, struct sched_param *);
int sched_getscheduler(pid_t);
int sched_rr_get_interval(pid_t, struct timespec *);
int sched_setaffinity(pid_t pid, size_t cpusetsz, const cpuset_t *cpuset);
int sched_setparam(pid_t, const struct sched_param *);
int sched_setscheduler(pid_t, int, const struct sched_param *);
int sched_yield(void);
void sdallocx(void *ptr, size_t size, int flags);
char *secure_getenv(const char *);
unsigned short *seed48(unsigned short *);
void seekdir(DIR *, long);
int select(int, fd_set *, fd_set *, fd_set *, struct timeval *);
int sem_clockwait_np(sem_t *, __clockid_t, int, const struct timespec *, struct timespec *);
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
ssize_t send(int, const void *, size_t, int);
int sendfile(int, int, off_t, size_t, struct sf_hdtr *, off_t *, int);
ssize_t sendmmsg(int, struct mmsghdr *, size_t, int);
ssize_t sendmsg(int, const struct msghdr *, int);
ssize_t sendto(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
constraint_handler_t set_constraint_handler_s(constraint_handler_t handler);
int setaudit(const struct auditinfo *);
int setaudit_addr(const struct auditinfo_addr *, int);
int setauid(const au_id_t *);
void setbuf(FILE *, char *);
void setbuffer(FILE *, char *, int);
int setcontext(const ucontext_t *);
int setcred(u_int flags, const struct setcred *wcred, size_t size);
int setdomainname(const char *, int);
int setegid(gid_t);
int setenv(const char *, const char *, int);
int seteuid(uid_t);
int setfib(int);
int setgid(gid_t);
void setgrent(void);
int setgroupent(int);
int setgroups(int, const gid_t *);
void sethostent(int);
void sethostid(long);
int sethostname(const char *, int);
int setipv4sourcefilter(int, struct in_addr, struct in_addr, unsigned int, unsigned int, struct in_addr *);
int setitimer(int, const struct itimerval *, struct itimerval *);
int setjmp(jmp_buf);
int setlinebuf(FILE *);
char *setlocale(int, const char *);
int setlogin(const char *);
int setloginclass(const char *);
int setlogmask(int);
void *setmode(const char *);
void setnetent(int);
void setnetgrent(const char *);
int setpassent(int);
int setpgid(pid_t, pid_t);
int setpgrp(pid_t, pid_t);
int setpriority(int, int, int);
void setproctitle(const char *_fmt, ...);
void setproctitle_fast(const char *_fmt, ...);
void setprogname(const char *);
void setprotoent(int);
void setpwent(void);
int setregid(gid_t, gid_t);
int setresgid(gid_t, gid_t, gid_t);
int setresuid(uid_t, uid_t, uid_t);
int setreuid(uid_t, uid_t);
int setrgid(gid_t);
int setrlimit(int, const struct rlimit *);
int setruid(uid_t);
void setservent(int);
pid_t setsid(void);
int setsockopt(int, int, int, const void *, socklen_t);
int setsourcefilter(int, unsigned int, struct sockaddr *, socklen_t, unsigned int, unsigned int, struct sockaddr_storage *);
char *setstate(char *);
int settimeofday(const struct timeval *, const struct timezone *);
int setuid(uid_t);
void setusershell(void);
int setutxdb(int, const char *);
void setutxent(void);
int setvbuf(FILE *, char *, int, size_t);
int shm_create_largepage(const char *, int, int, int, mode_t);
int shm_open(const char *, int, mode_t);
int shm_rename(const char *, const char *, int);
int shm_unlink(const char *);
void *shmat(int, const void *, int);
int shmctl(int, int, struct shmid_ds *);
int shmdt(const void *);
int shmget(key_t, size_t, int);
int shutdown(int, int);
int sig2str(int, char *);
int sigaction(int, const struct sigaction *, struct sigaction *);
int sigaddset(sigset_t *, int);
int sigaltstack(const stack_t *, stack_t *);
int sigandset(sigset_t *dest, const sigset_t *left, const sigset_t *right);
int sigblock(int);
int sigdelset(sigset_t *, int);
int sigemptyset(sigset_t *);
int sigfillset(sigset_t *);
int sighold(int);
int sigignore(int);
int siginterrupt(int, int);
int sigisemptyset(const sigset_t *set);
int sigismember(const sigset_t *, int);
_Noreturn void siglongjmp(sigjmp_buf, int);
__sighandler_t *signal(int, __sighandler_t *);
int signalcontext(ucontext_t *, int, __sighandler_t *);
double significand(double);
float significandf(float);
int sigorset(sigset_t *dest, const sigset_t *left, const sigset_t *right);
int sigpause(int);
int sigpending(sigset_t *);
int sigprocmask(int, const sigset_t *, sigset_t *);
int sigqueue(__pid_t, int, const union sigval);
int sigrelse(int);
int sigreturn(const struct __ucontext *);
void (*sigset(int, void (*)(int)))(int);
int sigsetjmp(sigjmp_buf, int);
int sigsetmask(int);
int sigstack(const struct sigstack *, struct sigstack *);
int sigsuspend(const sigset_t *);
int sigtimedwait(const sigset_t *, siginfo_t *, const struct timespec *);
int sigvec(int, struct sigvec *, struct sigvec *);
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
double sinpi(double);
float sinpif(float);
long double sinpil(long double);
unsigned int sleep(unsigned int);
int snprintf(char *, size_t, const char *, ...);
int snprintf_l(char *, size_t, locale_t, const char *, ...);
char *snvis(char *, size_t, int, int, int, const char *);
int sockatmark(int);
int socket(int, int, int);
int socketpair(int, int, int, int *);
int sprintf(char *, const char *, ...);
int sprintf_l(char *, locale_t, const char *, ...);
double sqrt(double);
float sqrtf(float);
long double sqrtl(long double);
int sradixsort(const unsigned char **, int, const unsigned char *, unsigned int);
void srand(unsigned int);
void srand48(long);
void srandom(unsigned int);
void srandomdev(void);
int sscanf(const char *, const char *, ...);
int sscanf_l(const char *, locale_t, const char *, ...);
int stat(const char *, struct stat *);
int statfs(const char *, struct statfs *);
int statvfs(const char *, struct statvfs *);
char *stpcpy(char *, const char *);
char *stpncpy(char *, const char *, size_t);
int str2sig(const char *, int *);
int stravis(char **, const char *, int);
int strcasecmp(const char *, const char *);
int strcasecmp_l(const char *, const char *, locale_t);
char *strcasestr(const char *, const char *);
char *strcasestr_l(const char *, const char *, locale_t);
char *strcat(char *, const char *);
char *strchr(const char *, int);
char *strchrnul(const char *, int);
int strcmp(const char *, const char *);
int strcoll(const char *, const char *);
int strcoll_l(const char *, const char *, locale_t);
char *strcpy(char *, const char *);
size_t strcspn(const char *, const char *);
char *strdup(const char *);
int strenvisx(char *, size_t, const char *, size_t, int, int *);
char *strerror(int);
char *strerror_l(int num, locale_t);
int strerror_r(int, char *, size_t);
size_t strftime(char *, size_t, const char *, const struct tm *);
size_t strftime_l(char *, size_t, const char *, const struct tm *, locale_t);
size_t strlcat(char *, const char *, size_t);
size_t strlcpy(char *, const char *, size_t);
size_t strlen(const char *);
void strmode(mode_t, char *);
int strncasecmp(const char *, const char *, size_t);
int strncasecmp_l(const char *, const char *, size_t, locale_t);
char *strncat(char *, const char *, size_t);
int strncmp(const char *, const char *, size_t);
char *strncpy(char *, const char *, size_t);
char *strndup(const char *, size_t);
size_t strnlen(const char *, size_t);
char *strnstr(const char *, const char *, size_t);
int strnunvis(char *, size_t, const char *);
int strnunvisx(char *, size_t, const char *, int);
int strnvis(char *, size_t, const char *, int);
int strnvisx(char *, size_t, const char *, size_t, int);
char *strpbrk(const char *, const char *);
char *strptime(const char *, const char *, struct tm *);
char *strptime_l(const char *, const char *, struct tm *, locale_t);
char *strrchr(const char *, int);
int strsenvisx(char *, size_t, const char *, size_t, int, const char *, int *);
char *strsep(char **, const char *);
char *strsignal(int);
int strsnvis(char *, size_t, const char *, int, const char *);
int strsnvisx(char *, size_t, const char *, size_t, int, const char *);
size_t strspn(const char *, const char *);
char *strstr(const char *, const char *);
int strsvis(char *, const char *, int, const char *);
int strsvisx(char *, const char *, size_t, int, const char *);
double strtod(const char *, char **);
double strtod_l(const char *, char **, locale_t);
float strtof(const char *, char **);
float strtof_l(const char *, char **, locale_t);
int strtofflags(char **, u_long *, u_long *);
intmax_t strtoimax(const char *, char **, int);
intmax_t strtoimax_l(const char *, char **, int, locale_t);
char *strtok(char *, const char *);
char *strtok_r(char *, const char *, char **);
long strtol(const char *, char **, int);
long strtol_l(const char *, char **, int, locale_t);
long double strtold(const char *, char **);
long double strtold_l(const char *, char **, locale_t);
long long strtoll(const char *, char **, int);
long long strtoll_l(const char *, char **, int, locale_t);
long long strtonum(const char *, long long, long long, const char **);
__int64_t strtoq(const char *, char **, int);
unsigned long strtoul(const char *, char **, int);
unsigned long strtoul_l(const char *, char **, int, locale_t);
unsigned long long strtoull(const char *, char **, int);
unsigned long long strtoull_l(const char *, char **, int, locale_t);
uintmax_t strtoumax(const char *, char **, int);
uintmax_t strtoumax_l(const char *, char **, int, locale_t);
__uint64_t strtouq(const char *, char **, int);
int strunvis(char *, const char *);
int strunvisx(char *, const char *, int);
int strverscmp(const char *, const char *);
int strvis(char *, const char *, int);
int strvisx(char *, const char *, size_t, int);
size_t strxfrm(char *, const char *, size_t);
size_t strxfrm_l(char *, const char *, size_t, locale_t);
char *svis(char *, int, int, int, const char *);
void swab(const void *, void *, ssize_t);
int swapcontext(ucontext_t *, const ucontext_t *);
int swapoff(const char *, u_int);
int swapon(const char *);
int swprintf(unsigned int *, size_t n, const unsigned int *, ...);
int swprintf_l(unsigned int *, size_t n, locale_t, const unsigned int *, ...);
int swscanf(const unsigned int *, const unsigned int *, ...);
int swscanf_l(const unsigned int *, locale_t, const unsigned int *, ...);
int symlink(const char *, const char *);
int symlinkat(const char *, int, const char *);
void sync(void);
int syscall(int, ...);
long sysconf(int);
int sysctl(const int *, unsigned int, void *, size_t *, const void *, size_t);
int sysctlbyname(const char *, void *, size_t *, const void *, size_t);
int sysctlnametomib(const char *, int *, size_t *);
void syslog(int, const char *, ...);
int system(const char *);
double tan(double);
float tanf(float);
double tanh(double);
float tanhf(float);
long double tanhl(long double);
long double tanl(long double);
double tanpi(double);
float tanpif(float);
long double tanpil(long double);
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
int tcsetsid(int, pid_t);
int tcsetwinsize(int, const struct winsize *);
void *tdelete(const void *, posix_tnode **, int (*)(const void *, const void *));
void tdestroy(void *, void (*)(void *));
long telldir(DIR *);
char *tempnam(const char *, const char *);
posix_tnode *tfind(const void *, posix_tnode *const *, int (*)(const void *, const void *));
double tgamma(double);
float tgammaf(float);
long double tgammal(long double);
time_t time(time_t *);
time_t time2posix(time_t t);
time_t timegm(struct tm *const);
time_t timelocal(struct tm *const);
int timer_create(clockid_t, struct sigevent *, timer_t *);
int timer_delete(timer_t);
int timer_getoverrun(timer_t);
int timer_gettime(timer_t, struct itimerspec *);
int timer_oshandle_np(timer_t timerid);
int timer_settime(timer_t, int, const struct itimerspec *, struct itimerspec *);
int timerfd_create(int clockid, int flags);
int timerfd_gettime(int fd, struct itimerspec *curr_value);
int timerfd_settime(int fd, int flags, const struct itimerspec *new_value, struct itimerspec *old_value);
clock_t times(struct tms *);
void timespec2bintime(const struct timespec *_ts, struct bintime *_bt);
int timespec_get(struct timespec *ts, int base);
int timespec_getres(struct timespec *, int);
void timeval2bintime(const struct timeval *_tv, struct bintime *_bt);
int timingsafe_bcmp(const void *, const void *, size_t);
int timingsafe_memcmp(const void *, const void *, size_t);
FILE *tmpfile(void);
char *tmpnam(char *);
int toascii(int);
int tolower(int);
int tolower_l(int __c, locale_t __l);
int toupper(int);
int toupper_l(int __c, locale_t __l);
wint_t towctrans(wint_t, wctrans_t);
wint_t towctrans_l(wint_t __wc, wctrans_t desc, locale_t __l);
wint_t towlower(wint_t);
int towlower_l(int __c, locale_t __l);
wint_t towupper(wint_t);
int towupper_l(int __c, locale_t __l);
void trimdomain(char *_fullhost, size_t _hostsize);
double trunc(double);
int truncate(const char *, off_t);
float truncf(float);
long double truncl(long double);
posix_tnode *tsearch(const void *, posix_tnode **, int (*)(const void *, const void *));
sbintime_t tstosbt(struct timespec _ts);
sbintime_t tstosbt_sat(struct timespec _ts);
char *ttyname(int);
int ttyname_r(int, char *, size_t);
sbintime_t tvtosbt(struct timeval _tv);
sbintime_t tvtosbt_sat(struct timeval _tv);
void twalk(const posix_tnode *, void (*)(const posix_tnode *, VISIT, int));
void tzset(void);
useconds_t ualarm(useconds_t, useconds_t);
int uid_from_user(const char *, uid_t *);
mode_t umask(mode_t);
int uname(struct utsname *name);
int undelete(const char *);
int ungetc(int, FILE *);
wint_t ungetwc(wint_t, FILE *);
wint_t ungetwc_l(wint_t, FILE *, locale_t);
int unlink(const char *);
int unlinkat(int, const char *, int);
int unlockpt(int);
int unmount(const char *, int);
int unsetenv(const char *);
int unvis(char *, int, int *, int);
int unwhiteout(const char *);
locale_t uselocale(locale_t loc);
const char *user_from_uid(uid_t, int);
int usleep(useconds_t);
sbintime_t ustosbt(long long us);
int utime(const char *, const struct utimbuf *);
int utimensat(int fd, const char *path, const struct timespec *times, int flag);
int utimes(const char *, const struct timeval *);
int uu_lock(const char *_ttyname);
int uu_lock_txfr(const char *_ttyname, pid_t _pid);
const char *uu_lockerr(int _uu_lockresult);
int uu_unlock(const char *_ttyname);
void *valloc(size_t);
int vasprintf(char **, const char *, char *);
int vasprintf_l(char **, locale_t, const char *, char *);
int vdprintf(int, const char *, char *);
int vdprintf_l(int, locale_t, const char *, char *);
_Noreturn void verr(int, const char *, char *);
_Noreturn void verrc(int, int, const char *, char *);
_Noreturn void verrx(int, const char *, char *);
int versionsort(const struct dirent **, const struct dirent **);
pid_t vfork(void);
int vfprintf(FILE *, const char *, char *);
int vfprintf_l(FILE *, locale_t, const char *, char *);
int vfscanf(FILE *, const char *, char *);
int vfscanf_l(FILE *, locale_t, const char *, char *);
int vfwprintf(FILE *, const unsigned int *, char *);
int vfwprintf_l(FILE *, locale_t, const unsigned int *, char *);
int vfwscanf(FILE *, const unsigned int *, char *);
int vfwscanf_l(FILE *, locale_t, const unsigned int *, char *);
char *vis(char *, int, int, int);
int vprintf(const char *, char *);
int vprintf_l(locale_t, const char *, char *);
int vscanf(const char *, char *);
int vscanf_l(locale_t, const char *, char *);
int vsnprintf(char *, size_t, const char *, char *);
int vsnprintf_l(char *, size_t, locale_t, const char *, char *);
int vsprintf(char *, const char *, char *);
int vsprintf_l(char *, locale_t, const char *, char *);
int vsscanf(const char *, const char *, char *);
int vsscanf_l(const char *, locale_t, const char *, char *);
int vswprintf(unsigned int *, size_t n, const unsigned int *, char *);
int vswprintf_l(unsigned int *, size_t n, locale_t, const unsigned int *, char *);
int vswscanf(const unsigned int *, const unsigned int *, char *);
int vswscanf_l(const unsigned int *, locale_t, const unsigned int *, char *);
void vsyslog(int, const char *, char *);
void vwarn(const char *, char *);
void vwarnc(int, const char *, char *);
void vwarnx(const char *, char *);
int vwprintf(const unsigned int *, char *);
int vwprintf_l(locale_t, const unsigned int *, char *);
int vwscanf(const unsigned int *, char *);
int vwscanf_l(locale_t, const unsigned int *, char *);
pid_t wait(int *);
pid_t wait3(int *, int, struct rusage *);
pid_t wait4(pid_t, int *, int, struct rusage *);
pid_t wait6(idtype_t, id_t, int *, int, struct __wrusage *, struct __siginfo *);
int waitid(idtype_t, id_t, struct __siginfo *, int);
pid_t waitpid(pid_t, int *, int);
void warn(const char *, ...);
void warnc(int, const char *, ...);
void warnx(const char *, ...);
unsigned int *wcpcpy(unsigned int *, const unsigned int *);
unsigned int *wcpncpy(unsigned int *, const unsigned int *, size_t);
size_t wcrtomb(char *, unsigned int, mbstate_t *);
size_t wcrtomb_l(char *, unsigned int, mbstate_t *, locale_t);
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
size_t wcslcat(unsigned int *, const unsigned int *, size_t);
size_t wcslcpy(unsigned int *, const unsigned int *, size_t);
size_t wcslen(const unsigned int *);
int wcsncasecmp(const unsigned int *, const unsigned int *, size_t n);
int wcsncasecmp_l(const unsigned int *, const unsigned int *, size_t, locale_t);
unsigned int *wcsncat(unsigned int *, const unsigned int *, size_t);
int wcsncmp(const unsigned int *, const unsigned int *, size_t);
unsigned int *wcsncpy(unsigned int *, const unsigned int *, size_t);
size_t wcsnlen(const unsigned int *, size_t);
size_t wcsnrtombs(char *, const unsigned int **, size_t, size_t, mbstate_t *);
size_t wcsnrtombs_l(char *, const unsigned int **, size_t, size_t, mbstate_t *, locale_t);
unsigned int *wcspbrk(const unsigned int *, const unsigned int *);
unsigned int *wcsrchr(const unsigned int *, unsigned int);
size_t wcsrtombs(char *, const unsigned int **, size_t, mbstate_t *);
size_t wcsrtombs_l(char *, const unsigned int **, size_t, mbstate_t *, locale_t);
size_t wcsspn(const unsigned int *, const unsigned int *);
unsigned int *wcsstr(const unsigned int *, const unsigned int *);
double wcstod(const unsigned int *, unsigned int **);
double wcstod_l(const unsigned int *, unsigned int **, locale_t);
float wcstof(const unsigned int *, unsigned int **);
float wcstof_l(const unsigned int *, unsigned int **, locale_t);
intmax_t wcstoimax(const unsigned int *, unsigned int **, int);
intmax_t wcstoimax_l(const unsigned int *, unsigned int **, int, locale_t);
unsigned int *wcstok(unsigned int *, const unsigned int *, unsigned int **);
long wcstol(const unsigned int *, unsigned int **, int);
long wcstol_l(const unsigned int *, unsigned int **, int, locale_t);
long double wcstold(const unsigned int *, unsigned int **);
long double wcstold_l(const unsigned int *, unsigned int **, locale_t);
long long wcstoll(const unsigned int *, unsigned int **, int);
long long wcstoll_l(const unsigned int *, unsigned int **, int, locale_t);
size_t wcstombs(char *, const unsigned int *, size_t);
size_t wcstombs_l(char *, const unsigned int *, size_t, locale_t);
unsigned long wcstoul(const unsigned int *, unsigned int **, int);
unsigned long wcstoul_l(const unsigned int *, unsigned int **, int, locale_t);
unsigned long long wcstoull(const unsigned int *, unsigned int **, int);
unsigned long long wcstoull_l(const unsigned int *, unsigned int **, int, locale_t);
uintmax_t wcstoumax(const unsigned int *, unsigned int **, int);
uintmax_t wcstoumax_l(const unsigned int *, unsigned int **, int, locale_t);
int wcswidth(const unsigned int *, size_t);
int wcswidth_l(const unsigned int *, size_t, locale_t);
size_t wcsxfrm(unsigned int *, const unsigned int *, size_t);
size_t wcsxfrm_l(unsigned int *, const unsigned int *, size_t, locale_t);
int wctob(wint_t);
int wctob_l(wint_t, locale_t);
int wctomb(char *, unsigned int);
int wctomb_l(char *, unsigned int, locale_t);
wctrans_t wctrans(const char *);
wctrans_t wctrans_l(const char *__charclass, locale_t __l);
wctype_t wctype(const char *);
wctype_t wctype_l(const char *property, locale_t __l);
int wcwidth(unsigned int);
int wcwidth_l(unsigned int, locale_t);
unsigned int *wmemchr(const unsigned int *, unsigned int, size_t);
int wmemcmp(const unsigned int *, const unsigned int *, size_t);
unsigned int *wmemcpy(unsigned int *, const unsigned int *, size_t);
unsigned int *wmemmove(unsigned int *, const unsigned int *, size_t);
unsigned int *wmempcpy(unsigned int *, const unsigned int *, size_t);
unsigned int *wmemset(unsigned int *, unsigned int, size_t);
int wordexp(const char *, wordexp_t *, int);
void wordfree(wordexp_t *);
int wprintf(const unsigned int *, ...);
int wprintf_l(locale_t, const unsigned int *, ...);
ssize_t write(int, const void *, size_t);
ssize_t writev(int, const struct iovec *, int);
int wscanf(const unsigned int *, ...);
int wscanf_l(locale_t, const unsigned int *, ...);
size_t xallocx(void *ptr, size_t size, size_t extra, int flags);
int xsi_sigpause(int);
double y0(double);
float y0f(float);
double y1(double);
float y1f(float);
double yn(int, double);
float ynf(int, float);
