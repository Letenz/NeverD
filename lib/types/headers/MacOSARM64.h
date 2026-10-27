// The Darwin types and prototypes for macOS on ARM64, as a program declares
// them with libSystem's headers.  Generated on 2026-10-18 by
// scripts/generate_darwin_types.py --target macos-arm64 from the headers of
// AvailabilityVersions-157.2, Libc-1752.120.2, Libinfo-600, dyld-1378,
// libmalloc-812.100.31, libplatform-375.120.2, libpthread-539, xnu-12377.121.6
// (https://github.com/apple-oss-distributions): changed from them, as NeverD
// reads and renders their declarations again.
//
// This file contains Original Code and/or Modifications of Original Code as
// defined in and that are subject to the Apple Public Source License Version
// 2.0 (the 'License'). You may not use this file except in compliance with the
// License. Please obtain a copy of the License at
// http://www.opensource.apple.com/apsl/ and read it before using this file.
// The Original Code and all software distributed under the License are
// distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESS
// OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES, INCLUDING WITHOUT
// LIMITATION, ANY WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
// PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT. Please see the License for the
// specific language governing rights and limitations under the License.  The
// License and the notices of the headers it was read from are in
// LICENSES/darwin-headers.txt.  Do not edit; generate it again.
//
// 1283 types, 1605 prototypes, rendered by NeverD.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef DECLSPEC_ALIGN
#if defined(_MSC_VER)
#define DECLSPEC_ALIGN(x) __declspec(align(x))
#else
#define DECLSPEC_ALIGN(x) __attribute__((aligned(x)))
#endif
#endif

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

typedef union __mbstate_t __mbstate_t;
struct __darwin_pthread_handler_rec;
struct _opaque_pthread_attr_t;
struct _opaque_pthread_cond_t;
struct _opaque_pthread_condattr_t;
struct _opaque_pthread_mutex_t;
struct _opaque_pthread_mutexattr_t;
struct _opaque_pthread_once_t;
struct _opaque_pthread_rwlock_t;
struct _opaque_pthread_rwlockattr_t;
struct _opaque_pthread_t;
struct __sbuf;
struct __sFILEX;
struct __sFILE;
struct __darwin_arm_exception_state;
struct __darwin_arm_exception_state64;
struct __darwin_arm_exception_state64_v2;
struct __darwin_arm_thread_state;
struct __darwin_arm_thread_state64;
struct __darwin_arm_vfp_state;
struct __darwin_arm_neon_state64;
struct __darwin_arm_neon_state;
struct __arm_pagein_state;
struct __darwin_arm_sme_state;
struct __darwin_arm_sve_z_state;
struct __darwin_arm_sve_p_state;
struct __darwin_arm_sme_za_state;
struct __darwin_arm_sme2_state;
struct __arm_legacy_debug_state;
struct __darwin_arm_debug_state32;
struct __darwin_arm_debug_state64;
struct __darwin_arm_cpmu_state64;
struct __darwin_mcontext32;
struct __darwin_mcontext64;
struct __darwin_sigaltstack;
struct __darwin_ucontext;
union sigval;
struct sigevent;
struct __siginfo;
union __sigaction_u;
struct __sigaction;
struct sigaction;
struct sigvec;
struct sigstack;
struct timeval;
struct rusage;
struct rusage_info_v0;
struct rusage_info_v1;
struct rusage_info_v2;
struct rusage_info_v3;
struct rusage_info_v4;
struct rusage_info_v5;
struct rusage_info_v6;
struct rlimit;
struct proc_rlimit_control_wakeupmon;
union wait;
typedef struct div_t div_t;
typedef struct ldiv_t ldiv_t;
typedef struct lldiv_t lldiv_t;
struct _malloc_zone_t;
struct accessx_descriptor;
typedef struct fd_set fd_set;
struct timespec;
struct fssearchblock;
struct searchstate;
struct flock;
struct flocktimeout;
struct radvisory;
struct fsignatures;
struct fsupplement;
struct fchecklv;
struct fgetsigsinfo;
struct fstore;
struct fpunchhole;
struct ftrimactivefile;
struct fspecread;
struct fattributiontag;
struct log2phys;
struct _filesec;
typedef struct _RuneEntry _RuneEntry;
typedef struct _RuneRange _RuneRange;
typedef struct _RuneCharClass _RuneCharClass;
typedef struct _RuneLocale _RuneLocale;
struct tm;
struct _xlocale;
struct lconv;
typedef struct imaxdiv_t imaxdiv_t;
struct dirent;
struct _telldir;
typedef struct DIR DIR;
struct dl_info;
struct sched_param;
struct pthread_override_s;
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
struct mach_port_status;
struct mach_port_limits;
struct mach_port_info_ext;
struct mach_port_guard_info;
struct mach_port_qos;
struct mach_service_port_info;
struct mach_port_options;
typedef struct mach_msg_type_descriptor_t mach_msg_type_descriptor_t;
typedef struct mach_msg_port_descriptor_t mach_msg_port_descriptor_t;
typedef struct mach_msg_ool_descriptor32_t mach_msg_ool_descriptor32_t;
typedef struct mach_msg_ool_descriptor64_t mach_msg_ool_descriptor64_t;
typedef struct mach_msg_ool_descriptor_t mach_msg_ool_descriptor_t;
typedef struct mach_msg_ool_ports_descriptor32_t mach_msg_ool_ports_descriptor32_t;
typedef struct mach_msg_ool_ports_descriptor64_t mach_msg_ool_ports_descriptor64_t;
typedef struct mach_msg_ool_ports_descriptor_t mach_msg_ool_ports_descriptor_t;
typedef struct mach_msg_guarded_port_descriptor32_t mach_msg_guarded_port_descriptor32_t;
typedef struct mach_msg_guarded_port_descriptor64_t mach_msg_guarded_port_descriptor64_t;
typedef struct mach_msg_guarded_port_descriptor_t mach_msg_guarded_port_descriptor_t;
typedef union mach_msg_descriptor_t mach_msg_descriptor_t;
typedef struct mach_msg_body_t mach_msg_body_t;
typedef struct mach_msg_header_t mach_msg_header_t;
typedef struct mach_msg_base_t mach_msg_base_t;
typedef struct mach_msg_trailer_t mach_msg_trailer_t;
typedef struct mach_msg_seqno_trailer_t mach_msg_seqno_trailer_t;
typedef struct security_token_t security_token_t;
typedef struct mach_msg_security_trailer_t mach_msg_security_trailer_t;
typedef struct audit_token_t audit_token_t;
typedef struct mach_msg_audit_trailer_t mach_msg_audit_trailer_t;
typedef struct mach_msg_context_trailer_t mach_msg_context_trailer_t;
typedef struct msg_labels_t msg_labels_t;
typedef struct mach_msg_mac_trailer_t mach_msg_mac_trailer_t;
typedef struct mach_msg_empty_send_t mach_msg_empty_send_t;
typedef struct mach_msg_empty_rcv_t mach_msg_empty_rcv_t;
typedef union mach_msg_empty_t mach_msg_empty_t;
struct mach_vm_range;
typedef struct mach_vm_range_recipe_v1_t mach_vm_range_recipe_v1_t;
struct arm_state_hdr;
struct arm_unified_thread_state;
struct ipc_info_space;
struct ipc_info_space_basic;
struct ipc_info_name;
struct ipc_info_tree_name;
struct ipc_info_port;
struct iovec;
struct sockaddr;
struct sa_endpoints;
struct linger;
struct so_np_extensions;
struct __sockaddr_header;
struct sockproto;
struct sockaddr_storage;
struct msghdr;
struct cmsghdr;
struct sf_hdtr;
struct in_addr;
struct sockaddr_in;
struct ip_opts;
struct ip_mreq;
struct ip_mreqn;
struct ip_mreq_source;
struct group_req;
struct group_source_req;
struct __msfilterreq;
struct in_pktinfo;
struct in6_addr;
struct sockaddr_in6;
struct ipv6_mreq;
struct in6_pktinfo;
struct ip6_mtuinfo;
struct hostent;
struct netent;
struct servent;
struct protoent;
struct addrinfo;
struct rpcent;
struct ifaddrs;
struct ifmaddrs;
struct entry;
struct utimbuf;
struct timeval64;
struct itimerval;
struct timezone;
struct clockinfo;
struct utmpx;
struct lastlogx;
struct utmp;
struct ostat;
struct statvfs;
struct tms;
struct ttysize;
struct net_event_data;
struct timeval32;
struct if_data;
struct if_data64;
struct ifnet_interface_advisory;
struct ifqueue;
struct if_clonereq;
struct if_msghdr;
struct ifa_msghdr;
struct ifma_msghdr;
struct if_msghdr2;
struct ifma_msghdr2;
struct ifdevmtu;
struct ifkpi;
struct ifreq;
struct ifaliasreq;
struct rslvmulti_req;
struct ifmediareq;
struct ifdrv;
struct ifstat;
struct ifconf;
struct kev_dl_proto_data;
struct if_nameindex;
struct sockaddr_un;
struct kevent;
struct kevent64_s;
struct knote;
struct klist;
struct utsname;
struct ucred;
struct posix_cred;
struct au_tid;
struct au_tid_addr;
struct au_mask;
struct auditinfo;
struct auditinfo_addr;
struct auditpinfo;
struct auditpinfo_addr;
struct au_session;
struct au_expire_after;
struct au_token;
struct au_qctrl;
struct audit_stat;
struct audit_fstat;
struct au_evclass_map;
struct label;
struct xucred;
struct session;
struct pgrp;
struct proc;
struct proc_ident;
struct vmspace;
struct sigacts;
struct vnode;
struct user;
struct extern_proc;
struct ctlname;
struct _pcred;
struct _ucred;
struct eproc;
struct kinfo_proc;
struct xsw_usage;
struct loadavg;
struct ipc_perm;
struct __semid_ds_new;
struct sem;
struct sembuf;
union semun;
struct __shmid_ds_new;
struct __msqid_ds_new;
struct msg;
struct mymsg;
struct msginfo;
struct fsobj_id;
struct attrlist;
struct attribute_set;
struct attrreference;
struct diskextent;
struct vol_capabilities_attr;
struct vol_attributes_attr;
struct fsid;
struct secure_boot_cryptex_args;
union graft_args;
struct mount;
struct statfs;
struct vfsstatfs;
struct vfsconf;
struct vfsidctl;
struct vfsquery;
struct vfs_server;
struct netfs_status;
struct fhandle;
struct tcphdr;
struct tcp_connection_info;
struct aiocb;
struct vm_statistics;
struct vm_statistics64;
struct vm_extmod_statistics;
struct vm_purgeable_stat;
struct vm_purgeable_info;
struct time_value;
struct host_can_has_debugger_info;
struct host_basic_info;
struct host_sched_info;
struct kernel_resource_sizes;
struct host_priority_info;
struct host_load_info;
struct host_cpu_load_info;
struct host_preferred_user_arch;
struct memory_object_perf_info;
struct memory_object_attr_info;
struct memory_object_behave_info;
struct mach_voucher_attr_recipe_data;
struct processor_cpu_stat;
struct processor_cpu_stat64;
struct processor_basic_info;
struct processor_cpu_load_info;
struct processor_set_basic_info;
struct processor_set_load_info;
struct policy_timeshare_base;
struct policy_timeshare_limit;
struct policy_timeshare_info;
struct policy_rr_base;
struct policy_rr_limit;
struct policy_rr_info;
struct policy_fifo_base;
struct policy_fifo_limit;
struct policy_fifo_info;
struct policy_bases;
struct policy_limits;
struct policy_infos;
struct task_basic_info_32;
struct task_basic_info_64;
struct task_basic_info;
struct task_events_info;
struct task_thread_times_info;
struct task_absolutetime_info;
struct task_kernelmemory_info;
struct task_affinity_tag_info;
struct task_dyld_info;
struct task_basic_info_64_2;
struct task_extmod_info;
struct mach_task_basic_info;
struct task_power_info;
struct task_vm_info;
struct task_trace_memory_info;
struct task_wait_state_info;
typedef struct gpu_energy_data gpu_energy_data;
struct task_power_info_v2;
struct task_flags_info;
struct task_security_config_info;
struct task_ipc_space_policy_info;
struct task_inspect_basic_counts;
struct task_category_policy;
struct task_qos_policy;
struct thread_basic_info;
struct thread_identifier_info;
struct thread_extended_info;
struct io_stat_entry;
struct io_stat_info;
struct thread_standard_policy;
struct thread_extended_policy;
struct thread_time_constraint_policy;
struct thread_precedence_policy;
struct thread_affinity_policy;
struct thread_background_policy;
struct thread_latency_qos_policy;
struct thread_throughput_qos_policy;
struct mach_timespec;
struct vm_region_basic_info_64;
struct vm_region_basic_info;
struct vm_region_extended_info;
struct vm_region_top_info;
struct vm_region_submap_info;
struct vm_region_submap_info_64;
struct vm_region_submap_short_info_64;
struct mach_vm_read_entry;
struct vm_read_entry;
struct vm_page_info_basic;
struct kmod_info;
struct kmod_reference;
struct kmod_info_32_v1;
struct kmod_info_64_v1;
struct dyld_kernel_image_info;
struct dyld_kernel_process_info;
typedef struct malloc_introspection_t malloc_introspection_t;
typedef struct malloc_type_layout_semantics_v0_t malloc_type_layout_semantics_v0_t;
typedef struct malloc_type_summary_v0_t malloc_type_summary_v0_t;
typedef union malloc_type_descriptor_v0_t malloc_type_descriptor_v0_t;
typedef struct vm_range_t vm_range_t;
typedef struct malloc_statistics_t malloc_statistics_t;
struct mstats;
struct image_offset;
struct _ftsent;
typedef struct FTS FTS;
struct FTW;
typedef struct wordexp_t wordexp_t;
struct ctl_event_data;
struct ctl_info;
struct sockaddr_ctl;
struct rt_metrics;
struct rtstat;
struct rt_msghdr;
struct rt_msghdr2;
struct rt_msghdr_prelude;
struct rt_addrinfo;
struct proc_bsdinfo;
struct proc_bsdshortinfo;
struct proc_taskinfo;
struct proc_taskallinfo;
struct proc_threadinfo;
struct proc_regioninfo;
struct proc_workqueueinfo;
struct proc_fileinfo;
struct proc_exitreasonbasicinfo;
struct proc_exitreasoninfo;
struct vinfo_stat;
struct vnode_info;
struct vnode_info_path;
struct vnode_fdinfo;
struct vnode_fdinfowithpath;
struct proc_regionwithpathinfo;
struct proc_regionpath;
struct proc_vnodepathinfo;
struct proc_threadwithpathinfo;
struct proc_archinfo;
struct in4in6_addr;
struct in_sockinfo;
struct tcp_sockinfo;
struct un_sockinfo;
struct ndrv_info;
struct kern_event_info;
struct kern_ctl_info;
struct vsock_sockinfo;
struct sockbuf_info;
struct socket_info;
struct socket_fdinfo;
struct psem_info;
struct psem_fdinfo;
struct pshm_info;
struct pshm_fdinfo;
struct pipe_info;
struct pipe_fdinfo;
struct kqueue_info;
struct kqueue_dyninfo;
struct kqueue_fdinfo;
struct appletalk_info;
struct appletalk_fdinfo;
struct proc_fdinfo;
struct proc_fileportinfo;
struct proc_channel_info;
struct channel_fdinfo;
struct _OSUnalignedU16;
struct _OSUnalignedU32;
struct _OSUnalignedU64;
struct mach_header;
struct mach_header_64;
struct load_command;
union lc_str;
struct segment_command;
struct segment_command_64;
struct section;
struct section_64;
struct fvmlib;
struct fvmlib_command;
struct dylib;
struct dylib_command;
struct sub_framework_command;
struct sub_client_command;
struct sub_umbrella_command;
struct sub_library_command;
struct prebound_dylib_command;
struct dylinker_command;
struct thread_command;
struct routines_command;
struct routines_command_64;
struct symtab_command;
struct dysymtab_command;
struct dylib_table_of_contents;
struct dylib_module;
struct dylib_module_64;
struct dylib_reference;
struct twolevel_hints_command;
struct twolevel_hint;
struct prebind_cksum_command;
struct uuid_command;
struct rpath_command;
struct linkedit_data_command;
struct fileset_entry_command;
struct encryption_info_command;
struct encryption_info_command_64;
struct version_min_command;
struct build_version_command;
struct build_tool_version;
struct dyld_info_command;
struct linker_option_command;
struct symseg_command;
struct ident_command;
struct fvmfile_command;
struct entry_point_command;
struct source_version_command;
struct data_in_code_entry;
struct tlv_descriptor;
struct note_command;
struct __NSObjectFileImage;
struct __NSModule;
struct __NSSymbol;
typedef struct NSLinkEditErrorHandlers NSLinkEditErrorHandlers;
struct os_unfair_lock_s;
typedef struct OSFifoQueueHead OSFifoQueueHead;
typedef struct OSQueueHead OSQueueHead;
struct mach_timebase_info;

#pragma neverd_header("stdlib.h")
// idtype_t: 4 bytes
enum idtype_t
{
    P_ALL = 0,
    P_PID = 1,
    P_PGID = 2,
};
typedef enum idtype_t idtype_t;

#pragma neverd_header("fcntl.h")
// filesec_property_t: 4 bytes
enum filesec_property_t
{
    FILESEC_OWNER = 1,
    FILESEC_GROUP = 2,
    FILESEC_UUID = 3,
    FILESEC_MODE = 4,
    FILESEC_ACL = 5,
    FILESEC_GRPUUID = 6,
    FILESEC_ACL_RAW = 100,
    FILESEC_ACL_ALLOCSIZE = 101,
};
typedef enum filesec_property_t filesec_property_t;

#pragma neverd_header("wchar.h")
// clockid_t: 4 bytes
enum clockid_t
{
    _CLOCK_REALTIME = 0,
    _CLOCK_MONOTONIC = 6,
    _CLOCK_MONOTONIC_RAW = 4,
    _CLOCK_MONOTONIC_RAW_APPROX = 5,
    _CLOCK_UPTIME_RAW = 8,
    _CLOCK_UPTIME_RAW_APPROX = 9,
    _CLOCK_PROCESS_CPUTIME_ID = 12,
    _CLOCK_THREAD_CPUTIME_ID = 16,
};
typedef enum clockid_t clockid_t;

#pragma neverd_header("spawn.h")
// mpo_flags_t: 4 bytes
enum mpo_flags_t : unsigned int
{
    MPO_PORT = 0x0,
    MPO_SERVICE_PORT = 0x400,
    MPO_CONNECTION_PORT = 0x800,
    MPO_REPLY_PORT = 0x1000,
    MPO_WEAK_REPLY_PORT = 0x4000,
    MPO_NOTIFICATION_PORT = 0x4400,
    MPO_EXCEPTION_PORT = 0x8000,
    MPO_CONNECTION_PORT_WITH_PORT_ARRAY = 0x10000,
};
typedef enum mpo_flags_t mpo_flags_t;

// mach_port_guard_exception_codes: 4 bytes
enum mach_port_guard_exception_codes
{
    kGUARD_EXC_NONE = 0x0,
    kGUARD_EXC_DESTROY = 0x1,
    kGUARD_EXC_MOD_REFS = 0x2,
    kGUARD_EXC_INVALID_OPTIONS = 0x3,
    kGUARD_EXC_SET_CONTEXT = 0x4,
    kGUARD_EXC_THREAD_SET_STATE = 0x5,
    kGUARD_EXC_EXCEPTION_BEHAVIOR_ENFORCE = 0x6,
    kGUARD_EXC_SERVICE_PORT_VIOLATION_FATAL = 0x7,
    kGUARD_EXC_UNGUARDED = 0x8,
    kGUARD_EXC_KOBJECT_REPLY_PORT_SEMANTICS = 0x9,
    kGUARD_EXC_REQUIRE_REPLY_PORT_SEMANTICS = 0xA,
    kGUARD_EXC_INCORRECT_GUARD = 0x10,
    kGUARD_EXC_IMMOVABLE = 0x20,
    kGUARD_EXC_STRICT_REPLY = 0x40,
    kGUARD_EXC_INVALID_NOTIFICATION_REQ = 0x41,
    kGUARD_EXC_INVALID_MPO_ENTITLEMENT = 0x42,
    kGUARD_EXC_DESCRIPTOR_VIOLATION = 0x43,
    kGUARD_EXC_MSG_FILTERED = 0x80,
    kGUARD_EXC_INVALID_RIGHT = 0x100,
    kGUARD_EXC_INVALID_NAME = 0x200,
    kGUARD_EXC_INVALID_VALUE = 0x400,
    kGUARD_EXC_INVALID_ARGUMENT = 0x800,
    kGUARD_EXC_RIGHT_EXISTS = 0x1000,
    kGUARD_EXC_KERN_NO_SPACE = 0x2000,
    kGUARD_EXC_KERN_FAILURE = 0x4000,
    kGUARD_EXC_KERN_RESOURCE = 0x8000,
    kGUARD_EXC_SEND_INVALID_REPLY = 0x10000,
    kGUARD_EXC_SEND_INVALID_VOUCHER = 0x20000,
    kGUARD_EXC_SEND_INVALID_RIGHT = 0x40000,
    kGUARD_EXC_RCV_INVALID_NAME = 0x80000,
    kGUARD_EXC_RCV_GUARDED_DESC = 0x100000,
    kGUARD_EXC_SERVICE_PORT_VIOLATION_NON_FATAL = 0x100001,
    kGUARD_EXC_INVALID_NOTIFICATION_PORT = 0x100006,
    kGUARD_EXC_MACH_EXC_THREAD_SET_STATE = 0x100007,
    kGUARD_EXC_CV_NOTIFICATION_PORT_REQ = 0x100008,
    kGUARD_EXC_WEAK_REPLY_PORT = 0x100002,
    kGUARD_EXC_OOL_PORT_ARRAY_CREATION = 0x100003,
    kGUARD_EXC_MOVE_WEAK_REPLY_PORT = 0x100004,
    kGUARD_EXC_REPLY_PORT_SINGLE_SO_RIGHT = 0x100005,
    kGUARD_EXC_MOD_REFS_NON_FATAL = 0x200000,
    kGUARD_EXC_IMMOVABLE_NON_FATAL = 0x400000,
};

// mach_vm_range_flavor_t: 4 bytes
enum mach_vm_range_flavor_t : unsigned int
{
    MACH_VM_RANGE_FLAVOR_INVALID = 0,
    MACH_VM_RANGE_FLAVOR_V1 = 1,
};
typedef enum mach_vm_range_flavor_t mach_vm_range_flavor_t;

// mach_vm_range_flags_t: 8 bytes
enum mach_vm_range_flags_t : unsigned long long
{
    MACH_VM_RANGE_NONE = 0,
};
typedef enum mach_vm_range_flags_t mach_vm_range_flags_t;

// mach_vm_range_tag_t: 2 bytes
enum mach_vm_range_tag_t : unsigned short
{
    MACH_VM_RANGE_DEFAULT = 0,
    MACH_VM_RANGE_DATA = 1,
    MACH_VM_RANGE_FIXED = 2,
};
typedef enum mach_vm_range_tag_t mach_vm_range_tag_t;

// ipc_info_object_type_t: 4 bytes
enum ipc_info_object_type_t : unsigned int
{
    IPC_OTYPE_NONE = 0x0,
    IPC_OTYPE_THREAD_CONTROL = 0x1,
    IPC_OTYPE_TASK_CONTROL = 0x2,
    IPC_OTYPE_HOST = 0x3,
    IPC_OTYPE_HOST_PRIV = 0x4,
    IPC_OTYPE_PROCESSOR = 0x5,
    IPC_OTYPE_PROCESSOR_SET = 0x6,
    IPC_OTYPE_PROCESSOR_SET_NAME = 0x7,
    IPC_OTYPE_TIMER = 0x8,
    IPC_OTYPE_PORT_SUBST_ONCE = 0x9,
    IPC_OTYPE_MIG = 0xA,
    IPC_OTYPE_MEMORY_OBJECT = 0xB,
    IPC_OTYPE_XMM_PAGER = 0xC,
    IPC_OTYPE_XMM_KERNEL = 0xD,
    IPC_OTYPE_XMM_REPLY = 0xE,
    IPC_OTYPE_UND_REPLY = 0xF,
    IPC_OTYPE_HOST_NOTIFY = 0x10,
    IPC_OTYPE_HOST_SECURITY = 0x11,
    IPC_OTYPE_LEDGER = 0x12,
    IPC_OTYPE_MAIN_DEVICE = 0x13,
    IPC_OTYPE_TASK_NAME = 0x14,
    IPC_OTYPE_SUBSYSTEM = 0x15,
    IPC_OTYPE_IO_DONE_QUEUE = 0x16,
    IPC_OTYPE_SEMAPHORE = 0x17,
    IPC_OTYPE_LOCK_SET = 0x18,
    IPC_OTYPE_CLOCK = 0x19,
    IPC_OTYPE_CLOCK_CTRL = 0x1A,
    IPC_OTYPE_IOKIT_IDENT = 0x1B,
    IPC_OTYPE_NAMED_ENTRY = 0x1C,
    IPC_OTYPE_IOKIT_CONNECT = 0x1D,
    IPC_OTYPE_IOKIT_OBJECT = 0x1E,
    IPC_OTYPE_UPL = 0x1F,
    IPC_OTYPE_MEM_OBJ_CONTROL = 0x20,
    IPC_OTYPE_AU_SESSIONPORT = 0x21,
    IPC_OTYPE_FILEPORT = 0x22,
    IPC_OTYPE_LABELH = 0x23,
    IPC_OTYPE_TASK_RESUME = 0x24,
    IPC_OTYPE_VOUCHER = 0x25,
    IPC_OTYPE_VOUCHER_ATTR_CONTROL = 0x26,
    IPC_OTYPE_WORK_INTERVAL = 0x27,
    IPC_OTYPE_UX_HANDLER = 0x28,
    IPC_OTYPE_UEXT_OBJECT = 0x29,
    IPC_OTYPE_ARCADE_REG = 0x2A,
    IPC_OTYPE_EVENTLINK = 0x2B,
    IPC_OTYPE_TASK_INSPECT = 0x2C,
    IPC_OTYPE_TASK_READ = 0x2D,
    IPC_OTYPE_THREAD_INSPECT = 0x2E,
    IPC_OTYPE_THREAD_READ = 0x2F,
    IPC_OTYPE_SUID_CRED = 0x30,
    IPC_OTYPE_HYPERVISOR = 0x31,
    IPC_OTYPE_TASK_ID_TOKEN = 0x32,
    IPC_OTYPE_TASK_FATAL = 0x33,
    IPC_OTYPE_KCDATA = 0x34,
    IPC_OTYPE_EXCLAVES_RESOURCE = 0x35,
    IPC_OTYPE_THREAD_RESUME = 0x36,
    IPC_OTYPE_UNKNOWN = 0xFFFFFFFF,
};
typedef enum ipc_info_object_type_t ipc_info_object_type_t;

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

#pragma neverd_header("sys/uio.h")
// uio_rw: 4 bytes
enum uio_rw
{
    UIO_READ = 0,
    UIO_WRITE = 1,
};

#pragma neverd_header("sys/sysctl.h")
// audit_session_flags: 4 bytes
enum audit_session_flags
{
    AU_SESSION_FLAG_IS_INITIAL = 0x1,
    AU_SESSION_FLAG_HAS_GRAPHIC_ACCESS = 0x10,
    AU_SESSION_FLAG_HAS_TTY = 0x20,
    AU_SESSION_FLAG_IS_REMOTE = 0x1000,
    AU_SESSION_FLAG_HAS_CONSOLE_ACCESS = 0x2000,
    AU_SESSION_FLAG_HAS_AUTHENTICATED = 0x4000,
};

#pragma neverd_header("sys/mount.h")
// graftdmg_type_t: 4 bytes
enum graftdmg_type_t : unsigned int
{
    GRAFTDMG_CRYPTEX_BOOT = 1,
    GRAFTDMG_CRYPTEX_PREBOOT = 2,
    GRAFTDMG_CRYPTEX_DOWNLEVEL = 3,
    GRAFTDMG_CRYPTEX_AUTH_ENV_GENERIC = 4,
    GRAFTDMG_CRYPTEX_PDI_NONCE = 6,
    GRAFTDMG_CRYPTEX_EFFECTIVE_AP = 7,
    GRAFTDMG_CRYPTEX_MOBILE_ASSET = 8,
    GRAFTDMG_CRYPTEX_MOBILE_ASSET_WITH_CODE = 9,
    GRAFTDMG_CRYPTEX_MAX = 9,
};
typedef enum graftdmg_type_t graftdmg_type_t;

// cryptex_auth_type_t: 4 bytes
enum cryptex_auth_type_t : unsigned int
{
    CRYPTEX1_AUTH_ENV_GENERIC = 4,
    CRYPTEX1_AUTH_ENV_GENERIC_SUPPLEMENTAL = 5,
    CRYPTEX_AUTH_PDI_NONCE = 6,
    CRYPTEX_AUTH_MOBILE_ASSET = 8,
    CRYPTEX_AUTH_MOBILE_ASSET_WITH_CODE = 9,
    CRYPTEX_AUTH_MAX = 9,
};
typedef enum cryptex_auth_type_t cryptex_auth_type_t;

#pragma neverd_header("malloc/malloc.h")
// virtual_memory_guard_exception_code_t: 4 bytes
enum virtual_memory_guard_exception_code_t : unsigned int
{
    kGUARD_EXC_DEALLOC_GAP = 1,
    kGUARD_EXC_RECLAIM_COPYIO_FAILURE = 2,
    kGUARD_EXC_RECLAIM_INDEX_FAILURE = 4,
    kGUARD_EXC_RECLAIM_DEALLOCATE_FAILURE = 8,
    kGUARD_EXC_RECLAIM_ACCOUNTING_FAILURE = 9,
    kGUARD_EXC_SEC_IOPL_ON_EXEC_PAGE = 10,
    kGUARD_EXC_SEC_EXEC_ON_IOPL_PAGE = 11,
    kGUARD_EXC_SEC_UPL_WRITE_ON_EXEC_REGION = 12,
    kGUARD_EXC_LARGE_ALLOCATION_TELEMETRY = 13,
    kGUARD_EXC_SEC_ACCESS_FAULT = 98,
    kGUARD_EXC_SEC_ASYNC_ACCESS_FAULT = 99,
    kGUARD_EXC_SEC_COPY_DENIED = 100,
    kGUARD_EXC_SEC_SHARING_DENIED = 101,
    kGUARD_EXC_MTE_SYNC_FAULT = 200,
    kGUARD_EXC_MTE_ASYNC_USER_FAULT = 201,
    kGUARD_EXC_MTE_ASYNC_KERN_FAULT = 202,
    kGUARD_EXC_GUARD_OBJECT_ASYNC_USER_FAULT = 203,
    kGUARD_EXC_GUARD_OBJECT_ASYNC_KERN_FAULT = 204,
};
typedef enum virtual_memory_guard_exception_code_t virtual_memory_guard_exception_code_t;

// task_inspect_flavor: 4 bytes
enum task_inspect_flavor
{
    TASK_INSPECT_BASIC_COUNTS = 1,
};

// task_role: 4 bytes
enum task_role
{
    TASK_RENICED = -1,
    TASK_UNSPECIFIED = 0,
    TASK_FOREGROUND_APPLICATION = 1,
    TASK_BACKGROUND_APPLICATION = 2,
    TASK_CONTROL_APPLICATION = 3,
    TASK_GRAPHICS_SERVER = 4,
    TASK_THROTTLE_APPLICATION = 5,
    TASK_NONUI_APPLICATION = 6,
    TASK_DEFAULT_APPLICATION = 7,
    TASK_DARWINBG_APPLICATION = 8,
    TASK_USER_INIT_APPLICATION = 9,
};

// task_latency_qos: 4 bytes
enum task_latency_qos
{
    LATENCY_QOS_TIER_UNSPECIFIED = 0x0,
    LATENCY_QOS_TIER_0 = 0xFF0001,
    LATENCY_QOS_TIER_1 = 0xFF0002,
    LATENCY_QOS_TIER_2 = 0xFF0003,
    LATENCY_QOS_TIER_3 = 0xFF0004,
    LATENCY_QOS_TIER_4 = 0xFF0005,
    LATENCY_QOS_TIER_5 = 0xFF0006,
};

// task_throughput_qos: 4 bytes
enum task_throughput_qos
{
    THROUGHPUT_QOS_TIER_UNSPECIFIED = 0x0,
    THROUGHPUT_QOS_TIER_0 = 0xFE0001,
    THROUGHPUT_QOS_TIER_1 = 0xFE0002,
    THROUGHPUT_QOS_TIER_2 = 0xFE0003,
    THROUGHPUT_QOS_TIER_3 = 0xFE0004,
    THROUGHPUT_QOS_TIER_4 = 0xFE0005,
    THROUGHPUT_QOS_TIER_5 = 0xFE0006,
};

// malloc_zone_malloc_options_t: 8 bytes
enum malloc_zone_malloc_options_t : unsigned long long
{
    MALLOC_ZONE_MALLOC_OPTION_NONE = 0,
    MALLOC_ZONE_MALLOC_OPTION_CLEAR = 1,
    MALLOC_ZONE_MALLOC_OPTION_CANONICAL_TAG = 2,
};
typedef enum malloc_zone_malloc_options_t malloc_zone_malloc_options_t;

// malloc_type_callsite_flags_v0_t: 4 bytes
enum malloc_type_callsite_flags_v0_t
{
    MALLOC_TYPE_CALLSITE_FLAGS_V0_NONE = 0,
    MALLOC_TYPE_CALLSITE_FLAGS_V0_FIXED_SIZE = 1,
    MALLOC_TYPE_CALLSITE_FLAGS_V0_ARRAY = 2,
};
typedef enum malloc_type_callsite_flags_v0_t malloc_type_callsite_flags_v0_t;

// malloc_type_kind_v0_t: 4 bytes
enum malloc_type_kind_v0_t
{
    MALLOC_TYPE_KIND_V0_OTHER = 0,
    MALLOC_TYPE_KIND_V0_OBJC = 1,
    MALLOC_TYPE_KIND_V0_SWIFT = 2,
    MALLOC_TYPE_KIND_V0_CXX = 3,
};
typedef enum malloc_type_kind_v0_t malloc_type_kind_v0_t;

#pragma neverd_header("mach-o/dyld.h")
// NXByteOrder: 4 bytes
enum NXByteOrder
{
    NX_UnknownByteOrder = 0,
    NX_LittleEndian = 1,
    NX_BigEndian = 2,
};

// DYLD_BOOL: 4 bytes
enum DYLD_BOOL
{
    FALSE = 0,
    TRUE = 1,
};

// NSObjectFileImageReturnCode: 4 bytes
enum NSObjectFileImageReturnCode
{
    NSObjectFileImageFailure = 0,
    NSObjectFileImageSuccess = 1,
    NSObjectFileImageInappropriateFile = 2,
    NSObjectFileImageArch = 3,
    NSObjectFileImageFormat = 4,
    NSObjectFileImageAccess = 5,
};
typedef enum NSObjectFileImageReturnCode NSObjectFileImageReturnCode;

// NSLinkEditErrors: 4 bytes
enum NSLinkEditErrors
{
    NSLinkEditFileAccessError = 0,
    NSLinkEditFileFormatError = 1,
    NSLinkEditMachResourceError = 2,
    NSLinkEditUnixResourceError = 3,
    NSLinkEditOtherError = 4,
    NSLinkEditWarningError = 5,
    NSLinkEditMultiplyDefinedError = 6,
    NSLinkEditUndefinedError = 7,
};
typedef enum NSLinkEditErrors NSLinkEditErrors;

// NSOtherErrorNumbers: 4 bytes
enum NSOtherErrorNumbers
{
    NSOtherErrorRelocation = 0,
    NSOtherErrorLazyBind = 1,
    NSOtherErrorIndrLoop = 2,
    NSOtherErrorLazyInit = 3,
    NSOtherErrorInvalidArgs = 4,
};
typedef enum NSOtherErrorNumbers NSOtherErrorNumbers;

#pragma neverd_header("os/lock.h")
// os_unfair_lock_flags_t: 4 bytes
enum os_unfair_lock_flags_t : unsigned int
{
    OS_UNFAIR_LOCK_FLAG_NONE = 0x0,
    OS_UNFAIR_LOCK_FLAG_ADAPTIVE_SPIN = 0x40000,
};
typedef enum os_unfair_lock_flags_t os_unfair_lock_flags_t;

#pragma neverd_header("stdio.h")
typedef unsigned long __darwin_size_t;

typedef __darwin_size_t size_t;

#pragma neverd_header()
typedef long ptrdiff_t;

typedef __int128 __int128_t;

typedef unsigned __int128 __uint128_t;

#pragma neverd_header("stdio.h")
typedef signed char __int8_t;

typedef unsigned char __uint8_t;

typedef short __int16_t;

typedef unsigned short __uint16_t;

typedef int __int32_t;

typedef unsigned int __uint32_t;

typedef long long __int64_t;

typedef unsigned long long __uint64_t;

typedef long __darwin_intptr_t;

typedef unsigned int __darwin_natural_t;

typedef int __darwin_ct_rune_t;

// __mbstate_t: 0x80 (128) bytes
union __mbstate_t
{
    char __mbstate8[128];
    long long _mbstateL;
};

typedef __mbstate_t __darwin_mbstate_t;

typedef long __darwin_ptrdiff_t;

typedef int __darwin_wchar_t;

typedef __darwin_wchar_t __darwin_rune_t;

typedef int __darwin_wint_t;

typedef unsigned long __darwin_clock_t;

typedef __uint32_t __darwin_socklen_t;

typedef long __darwin_ssize_t;

typedef long __darwin_time_t;

typedef __int64_t __darwin_blkcnt_t;

typedef __int32_t __darwin_blksize_t;

typedef __int32_t __darwin_dev_t;

typedef unsigned int __darwin_fsblkcnt_t;

typedef unsigned int __darwin_fsfilcnt_t;

typedef __uint32_t __darwin_gid_t;

typedef __uint32_t __darwin_id_t;

typedef __uint64_t __darwin_ino64_t;

typedef __darwin_ino64_t __darwin_ino_t;

typedef __darwin_natural_t __darwin_mach_port_name_t;

typedef __darwin_mach_port_name_t __darwin_mach_port_t;

typedef __uint16_t __darwin_mode_t;

typedef __int64_t __darwin_off_t;

typedef __int32_t __darwin_pid_t;

typedef __uint32_t __darwin_sigset_t;

typedef __int32_t __darwin_suseconds_t;

typedef __uint32_t __darwin_uid_t;

typedef __uint32_t __darwin_useconds_t;

typedef unsigned char __darwin_uuid_t[16];

typedef char __darwin_uuid_string_t[37];

// __darwin_pthread_handler_rec: 0x18 (24) bytes
struct __darwin_pthread_handler_rec
{
    void (*__routine)(void *);
    void *__arg;
    struct __darwin_pthread_handler_rec *__next;
};

// _opaque_pthread_attr_t: 0x40 (64) bytes
struct _opaque_pthread_attr_t
{
    long __sig;
    char __opaque[56];
};

// _opaque_pthread_cond_t: 0x30 (48) bytes
struct _opaque_pthread_cond_t
{
    long __sig;
    char __opaque[40];
};

// _opaque_pthread_condattr_t: 0x10 (16) bytes
struct _opaque_pthread_condattr_t
{
    long __sig;
    char __opaque[8];
};

// _opaque_pthread_mutex_t: 0x40 (64) bytes
struct _opaque_pthread_mutex_t
{
    long __sig;
    char __opaque[56];
};

// _opaque_pthread_mutexattr_t: 0x10 (16) bytes
struct _opaque_pthread_mutexattr_t
{
    long __sig;
    char __opaque[8];
};

// _opaque_pthread_once_t: 0x10 (16) bytes
struct _opaque_pthread_once_t
{
    long __sig;
    char __opaque[8];
};

// _opaque_pthread_rwlock_t: 0xC8 (200) bytes
struct _opaque_pthread_rwlock_t
{
    long __sig;
    char __opaque[192];
};

// _opaque_pthread_rwlockattr_t: 0x18 (24) bytes
struct _opaque_pthread_rwlockattr_t
{
    long __sig;
    char __opaque[16];
};

// _opaque_pthread_t: 0x2000 (8192) bytes
struct _opaque_pthread_t
{
    long __sig;
    struct __darwin_pthread_handler_rec *__cleanup_stack;
    char __opaque[8176];
};

typedef struct _opaque_pthread_attr_t __darwin_pthread_attr_t;

typedef struct _opaque_pthread_cond_t __darwin_pthread_cond_t;

typedef struct _opaque_pthread_condattr_t __darwin_pthread_condattr_t;

typedef unsigned long __darwin_pthread_key_t;

typedef struct _opaque_pthread_mutex_t __darwin_pthread_mutex_t;

typedef struct _opaque_pthread_mutexattr_t __darwin_pthread_mutexattr_t;

typedef struct _opaque_pthread_once_t __darwin_pthread_once_t;

typedef struct _opaque_pthread_rwlock_t __darwin_pthread_rwlock_t;

typedef struct _opaque_pthread_rwlockattr_t __darwin_pthread_rwlockattr_t;

typedef struct _opaque_pthread_t *__darwin_pthread_t;

typedef int __darwin_nl_item;

typedef int __darwin_wctrans_t;

typedef __uint32_t __darwin_wctype_t;

typedef unsigned char u_int8_t;

typedef unsigned short u_int16_t;

typedef unsigned int u_int32_t;

typedef unsigned long long u_int64_t;

typedef long long register_t;

typedef __darwin_intptr_t intptr_t;

typedef unsigned long uintptr_t;

typedef u_int64_t user_addr_t;

typedef u_int64_t user_size_t;

typedef long long user_ssize_t;

typedef long long user_long_t;

typedef u_int64_t user_ulong_t;

typedef long long user_time_t;

typedef long long user_off_t;

typedef u_int64_t syscall_arg_t;

typedef __darwin_off_t fpos_t;

// __sbuf: 0x10 (16) bytes
struct __sbuf
{
    unsigned char *_base;
    int _size;
};

#pragma neverd_header()
// Declared only: its members are unknown.
struct __sFILEX;

#pragma neverd_header("stdio.h")
// __sFILE: 0x98 (152) bytes
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
    int (*_read)(void *, char *, int __n);
    fpos_t (*_seek)(void *, fpos_t, int);
    int (*_write)(void *, const char *, int __n);
    struct __sbuf _ub;
    struct __sFILEX *_extra;
    int _ur;
    unsigned char _ubuf[3];
    unsigned char _nbuf[1];
    struct __sbuf _lb;
    int _blksize;
    fpos_t _offset;
};

typedef struct __sFILE FILE;

typedef __darwin_off_t off_t;

typedef __darwin_ssize_t ssize_t;

#pragma neverd_header("stdlib.h")
typedef __darwin_pid_t pid_t;

typedef __darwin_id_t id_t;

typedef int sig_atomic_t;

// __darwin_arm_exception_state: 0xC (12) bytes
struct __darwin_arm_exception_state
{
    __uint32_t __exception;
    __uint32_t __fsr;
    __uint32_t __far;
};

// __darwin_arm_exception_state64: 0x10 (16) bytes
struct __darwin_arm_exception_state64
{
    __uint64_t __far;
    __uint32_t __esr;
    __uint32_t __exception;
};

// __darwin_arm_exception_state64_v2: 0x10 (16) bytes
struct __darwin_arm_exception_state64_v2
{
    __uint64_t __far;
    __uint64_t __esr;
};

// __darwin_arm_thread_state: 0x44 (68) bytes
struct __darwin_arm_thread_state
{
    __uint32_t __r[13];
    __uint32_t __sp;
    __uint32_t __lr;
    __uint32_t __pc;
    __uint32_t __cpsr;
};

// __darwin_arm_thread_state64: 0x110 (272) bytes
struct __darwin_arm_thread_state64
{
    __uint64_t __x[29];
    __uint64_t __fp;
    __uint64_t __lr;
    __uint64_t __sp;
    __uint64_t __pc;
    __uint32_t __cpsr;
    __uint32_t __pad;
};

// __darwin_arm_vfp_state: 0x104 (260) bytes
struct __darwin_arm_vfp_state
{
    __uint32_t __r[64];
    __uint32_t __fpscr;
};

// __darwin_arm_neon_state64: 0x210 (528) bytes
struct __darwin_arm_neon_state64
{
    __uint128_t __v[32];
    __uint32_t __fpsr;
    __uint32_t __fpcr;
};

// __darwin_arm_neon_state: 0x110 (272) bytes
struct __darwin_arm_neon_state
{
    __uint128_t __v[16];
    __uint32_t __fpsr;
    __uint32_t __fpcr;
};

// __arm_pagein_state: 0x4 (4) bytes
struct __arm_pagein_state
{
    int __pagein_error;
};

// __darwin_arm_sme_state: 0x18 (24) bytes
struct __darwin_arm_sme_state
{
    __uint64_t __svcr;
    __uint64_t __tpidr2_el0;
    __uint16_t __svl_b;
};

// __darwin_arm_sve_z_state: 0x1000 (4096) bytes
struct DECLSPEC_ALIGN(4) __darwin_arm_sve_z_state
{
    char __z[16][256];
};

// __darwin_arm_sve_p_state: 0x200 (512) bytes
struct DECLSPEC_ALIGN(4) __darwin_arm_sve_p_state
{
    char __p[16][32];
};

// __darwin_arm_sme_za_state: 0x1000 (4096) bytes
struct DECLSPEC_ALIGN(4) __darwin_arm_sme_za_state
{
    char __za[4096];
};

// __darwin_arm_sme2_state: 0x40 (64) bytes
struct DECLSPEC_ALIGN(4) __darwin_arm_sme2_state
{
    char __zt0[64];
};

// __arm_legacy_debug_state: 0x100 (256) bytes
struct __arm_legacy_debug_state
{
    __uint32_t __bvr[16];
    __uint32_t __bcr[16];
    __uint32_t __wvr[16];
    __uint32_t __wcr[16];
};

// __darwin_arm_debug_state32: 0x108 (264) bytes
struct __darwin_arm_debug_state32
{
    __uint32_t __bvr[16];
    __uint32_t __bcr[16];
    __uint32_t __wvr[16];
    __uint32_t __wcr[16];
    __uint64_t __mdscr_el1;
};

// __darwin_arm_debug_state64: 0x208 (520) bytes
struct __darwin_arm_debug_state64
{
    __uint64_t __bvr[16];
    __uint64_t __bcr[16];
    __uint64_t __wvr[16];
    __uint64_t __wcr[16];
    __uint64_t __mdscr_el1;
};

// __darwin_arm_cpmu_state64: 0x80 (128) bytes
struct __darwin_arm_cpmu_state64
{
    __uint64_t __ctrs[16];
};

// __darwin_mcontext32: 0x154 (340) bytes
struct __darwin_mcontext32
{
    struct __darwin_arm_exception_state __es;
    struct __darwin_arm_thread_state __ss;
    struct __darwin_arm_vfp_state __fs;
};

// __darwin_mcontext64: 0x330 (816) bytes
struct __darwin_mcontext64
{
    struct __darwin_arm_exception_state64 __es;
    struct __darwin_arm_thread_state64 __ss;
    struct __darwin_arm_neon_state64 __ns;
};

typedef struct __darwin_mcontext64 *mcontext_t;

typedef __darwin_pthread_attr_t pthread_attr_t;

// __darwin_sigaltstack: 0x18 (24) bytes
struct __darwin_sigaltstack
{
    void *ss_sp;
    __darwin_size_t ss_size;
    int ss_flags;
};

typedef struct __darwin_sigaltstack stack_t;

// __darwin_ucontext: 0x38 (56) bytes
struct __darwin_ucontext
{
    int uc_onstack;
    __darwin_sigset_t uc_sigmask;
    struct __darwin_sigaltstack uc_stack;
    struct __darwin_ucontext *uc_link;
    __darwin_size_t uc_mcsize;
    struct __darwin_mcontext64 *uc_mcontext;
};

typedef struct __darwin_ucontext ucontext_t;

typedef __darwin_sigset_t sigset_t;

typedef __darwin_uid_t uid_t;

// sigval: 0x8 (8) bytes
union sigval
{
    int sival_int;
    void *sival_ptr;
};

// sigevent: 0x20 (32) bytes
struct sigevent
{
    int sigev_notify;
    int sigev_signo;
    union sigval sigev_value;
    void (*sigev_notify_function)(union sigval);
    pthread_attr_t *sigev_notify_attributes;
};

// __siginfo: 0x68 (104) bytes
struct __siginfo
{
    int si_signo;
    int si_errno;
    int si_code;
    pid_t si_pid;
    uid_t si_uid;
    int si_status;
    void *si_addr;
    union sigval si_value;
    long si_band;
    unsigned long __pad[7];
};

typedef struct __siginfo siginfo_t;

// __sigaction_u: 0x8 (8) bytes
union __sigaction_u
{
    void (*__sa_handler)(int);
    void (*__sa_sigaction)(int, struct __siginfo *, void *);
};

// __sigaction: 0x18 (24) bytes
struct __sigaction
{
    union __sigaction_u __sigaction_u;
    void (*sa_tramp)(void *, int, int, siginfo_t *, void *);
    sigset_t sa_mask;
    int sa_flags;
};

// sigaction: 0x10 (16) bytes
struct sigaction
{
    union __sigaction_u __sigaction_u;
    sigset_t sa_mask;
    int sa_flags;
};

typedef void (*sig_t)(int);

// sigvec: 0x10 (16) bytes
struct sigvec
{
    void (*sv_handler)(int);
    int sv_mask;
    int sv_flags;
};

// sigstack: 0x10 (16) bytes
struct sigstack
{
    char *ss_sp;
    int ss_onstack;
};

typedef signed char int_least8_t;

typedef short int_least16_t;

typedef int int_least32_t;

typedef long long int_least64_t;

typedef unsigned char uint_least8_t;

typedef unsigned short uint_least16_t;

typedef unsigned int uint_least32_t;

typedef unsigned long long uint_least64_t;

typedef signed char int_fast8_t;

typedef short int_fast16_t;

typedef int int_fast32_t;

typedef long long int_fast64_t;

typedef unsigned char uint_fast8_t;

typedef unsigned short uint_fast16_t;

typedef unsigned int uint_fast32_t;

typedef unsigned long long uint_fast64_t;

typedef long intmax_t;

typedef unsigned long uintmax_t;

// timeval: 0x10 (16) bytes
struct timeval
{
    __darwin_time_t tv_sec;
    __darwin_suseconds_t tv_usec;
};

typedef __uint64_t rlim_t;

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

typedef void *rusage_info_t;

// rusage_info_v0: 0x60 (96) bytes
struct rusage_info_v0
{
    unsigned char ri_uuid[16];
    unsigned long long ri_user_time;
    unsigned long long ri_system_time;
    unsigned long long ri_pkg_idle_wkups;
    unsigned long long ri_interrupt_wkups;
    unsigned long long ri_pageins;
    unsigned long long ri_wired_size;
    unsigned long long ri_resident_size;
    unsigned long long ri_phys_footprint;
    unsigned long long ri_proc_start_abstime;
    unsigned long long ri_proc_exit_abstime;
};

// rusage_info_v1: 0x90 (144) bytes
struct rusage_info_v1
{
    unsigned char ri_uuid[16];
    unsigned long long ri_user_time;
    unsigned long long ri_system_time;
    unsigned long long ri_pkg_idle_wkups;
    unsigned long long ri_interrupt_wkups;
    unsigned long long ri_pageins;
    unsigned long long ri_wired_size;
    unsigned long long ri_resident_size;
    unsigned long long ri_phys_footprint;
    unsigned long long ri_proc_start_abstime;
    unsigned long long ri_proc_exit_abstime;
    unsigned long long ri_child_user_time;
    unsigned long long ri_child_system_time;
    unsigned long long ri_child_pkg_idle_wkups;
    unsigned long long ri_child_interrupt_wkups;
    unsigned long long ri_child_pageins;
    unsigned long long ri_child_elapsed_abstime;
};

// rusage_info_v2: 0xA0 (160) bytes
struct rusage_info_v2
{
    unsigned char ri_uuid[16];
    unsigned long long ri_user_time;
    unsigned long long ri_system_time;
    unsigned long long ri_pkg_idle_wkups;
    unsigned long long ri_interrupt_wkups;
    unsigned long long ri_pageins;
    unsigned long long ri_wired_size;
    unsigned long long ri_resident_size;
    unsigned long long ri_phys_footprint;
    unsigned long long ri_proc_start_abstime;
    unsigned long long ri_proc_exit_abstime;
    unsigned long long ri_child_user_time;
    unsigned long long ri_child_system_time;
    unsigned long long ri_child_pkg_idle_wkups;
    unsigned long long ri_child_interrupt_wkups;
    unsigned long long ri_child_pageins;
    unsigned long long ri_child_elapsed_abstime;
    unsigned long long ri_diskio_bytesread;
    unsigned long long ri_diskio_byteswritten;
};

// rusage_info_v3: 0xE8 (232) bytes
struct rusage_info_v3
{
    unsigned char ri_uuid[16];
    unsigned long long ri_user_time;
    unsigned long long ri_system_time;
    unsigned long long ri_pkg_idle_wkups;
    unsigned long long ri_interrupt_wkups;
    unsigned long long ri_pageins;
    unsigned long long ri_wired_size;
    unsigned long long ri_resident_size;
    unsigned long long ri_phys_footprint;
    unsigned long long ri_proc_start_abstime;
    unsigned long long ri_proc_exit_abstime;
    unsigned long long ri_child_user_time;
    unsigned long long ri_child_system_time;
    unsigned long long ri_child_pkg_idle_wkups;
    unsigned long long ri_child_interrupt_wkups;
    unsigned long long ri_child_pageins;
    unsigned long long ri_child_elapsed_abstime;
    unsigned long long ri_diskio_bytesread;
    unsigned long long ri_diskio_byteswritten;
    unsigned long long ri_cpu_time_qos_default;
    unsigned long long ri_cpu_time_qos_maintenance;
    unsigned long long ri_cpu_time_qos_background;
    unsigned long long ri_cpu_time_qos_utility;
    unsigned long long ri_cpu_time_qos_legacy;
    unsigned long long ri_cpu_time_qos_user_initiated;
    unsigned long long ri_cpu_time_qos_user_interactive;
    unsigned long long ri_billed_system_time;
    unsigned long long ri_serviced_system_time;
};

// rusage_info_v4: 0x128 (296) bytes
struct rusage_info_v4
{
    unsigned char ri_uuid[16];
    unsigned long long ri_user_time;
    unsigned long long ri_system_time;
    unsigned long long ri_pkg_idle_wkups;
    unsigned long long ri_interrupt_wkups;
    unsigned long long ri_pageins;
    unsigned long long ri_wired_size;
    unsigned long long ri_resident_size;
    unsigned long long ri_phys_footprint;
    unsigned long long ri_proc_start_abstime;
    unsigned long long ri_proc_exit_abstime;
    unsigned long long ri_child_user_time;
    unsigned long long ri_child_system_time;
    unsigned long long ri_child_pkg_idle_wkups;
    unsigned long long ri_child_interrupt_wkups;
    unsigned long long ri_child_pageins;
    unsigned long long ri_child_elapsed_abstime;
    unsigned long long ri_diskio_bytesread;
    unsigned long long ri_diskio_byteswritten;
    unsigned long long ri_cpu_time_qos_default;
    unsigned long long ri_cpu_time_qos_maintenance;
    unsigned long long ri_cpu_time_qos_background;
    unsigned long long ri_cpu_time_qos_utility;
    unsigned long long ri_cpu_time_qos_legacy;
    unsigned long long ri_cpu_time_qos_user_initiated;
    unsigned long long ri_cpu_time_qos_user_interactive;
    unsigned long long ri_billed_system_time;
    unsigned long long ri_serviced_system_time;
    unsigned long long ri_logical_writes;
    unsigned long long ri_lifetime_max_phys_footprint;
    unsigned long long ri_instructions;
    unsigned long long ri_cycles;
    unsigned long long ri_billed_energy;
    unsigned long long ri_serviced_energy;
    unsigned long long ri_interval_max_phys_footprint;
    unsigned long long ri_runnable_time;
};

// rusage_info_v5: 0x130 (304) bytes
struct rusage_info_v5
{
    unsigned char ri_uuid[16];
    unsigned long long ri_user_time;
    unsigned long long ri_system_time;
    unsigned long long ri_pkg_idle_wkups;
    unsigned long long ri_interrupt_wkups;
    unsigned long long ri_pageins;
    unsigned long long ri_wired_size;
    unsigned long long ri_resident_size;
    unsigned long long ri_phys_footprint;
    unsigned long long ri_proc_start_abstime;
    unsigned long long ri_proc_exit_abstime;
    unsigned long long ri_child_user_time;
    unsigned long long ri_child_system_time;
    unsigned long long ri_child_pkg_idle_wkups;
    unsigned long long ri_child_interrupt_wkups;
    unsigned long long ri_child_pageins;
    unsigned long long ri_child_elapsed_abstime;
    unsigned long long ri_diskio_bytesread;
    unsigned long long ri_diskio_byteswritten;
    unsigned long long ri_cpu_time_qos_default;
    unsigned long long ri_cpu_time_qos_maintenance;
    unsigned long long ri_cpu_time_qos_background;
    unsigned long long ri_cpu_time_qos_utility;
    unsigned long long ri_cpu_time_qos_legacy;
    unsigned long long ri_cpu_time_qos_user_initiated;
    unsigned long long ri_cpu_time_qos_user_interactive;
    unsigned long long ri_billed_system_time;
    unsigned long long ri_serviced_system_time;
    unsigned long long ri_logical_writes;
    unsigned long long ri_lifetime_max_phys_footprint;
    unsigned long long ri_instructions;
    unsigned long long ri_cycles;
    unsigned long long ri_billed_energy;
    unsigned long long ri_serviced_energy;
    unsigned long long ri_interval_max_phys_footprint;
    unsigned long long ri_runnable_time;
    unsigned long long ri_flags;
};

// rusage_info_v6: 0x1D0 (464) bytes
struct rusage_info_v6
{
    unsigned char ri_uuid[16];
    unsigned long long ri_user_time;
    unsigned long long ri_system_time;
    unsigned long long ri_pkg_idle_wkups;
    unsigned long long ri_interrupt_wkups;
    unsigned long long ri_pageins;
    unsigned long long ri_wired_size;
    unsigned long long ri_resident_size;
    unsigned long long ri_phys_footprint;
    unsigned long long ri_proc_start_abstime;
    unsigned long long ri_proc_exit_abstime;
    unsigned long long ri_child_user_time;
    unsigned long long ri_child_system_time;
    unsigned long long ri_child_pkg_idle_wkups;
    unsigned long long ri_child_interrupt_wkups;
    unsigned long long ri_child_pageins;
    unsigned long long ri_child_elapsed_abstime;
    unsigned long long ri_diskio_bytesread;
    unsigned long long ri_diskio_byteswritten;
    unsigned long long ri_cpu_time_qos_default;
    unsigned long long ri_cpu_time_qos_maintenance;
    unsigned long long ri_cpu_time_qos_background;
    unsigned long long ri_cpu_time_qos_utility;
    unsigned long long ri_cpu_time_qos_legacy;
    unsigned long long ri_cpu_time_qos_user_initiated;
    unsigned long long ri_cpu_time_qos_user_interactive;
    unsigned long long ri_billed_system_time;
    unsigned long long ri_serviced_system_time;
    unsigned long long ri_logical_writes;
    unsigned long long ri_lifetime_max_phys_footprint;
    unsigned long long ri_instructions;
    unsigned long long ri_cycles;
    unsigned long long ri_billed_energy;
    unsigned long long ri_serviced_energy;
    unsigned long long ri_interval_max_phys_footprint;
    unsigned long long ri_runnable_time;
    unsigned long long ri_flags;
    unsigned long long ri_user_ptime;
    unsigned long long ri_system_ptime;
    unsigned long long ri_pinstructions;
    unsigned long long ri_pcycles;
    unsigned long long ri_energy_nj;
    unsigned long long ri_penergy_nj;
    unsigned long long ri_secure_time_in_system;
    unsigned long long ri_secure_ptime_in_system;
    unsigned long long ri_neural_footprint;
    unsigned long long ri_lifetime_max_neural_footprint;
    unsigned long long ri_interval_max_neural_footprint;
    unsigned long long ri_reserved[9];
};

typedef struct rusage_info_v6 rusage_info_current;

// rlimit: 0x10 (16) bytes
struct rlimit
{
    rlim_t rlim_cur;
    rlim_t rlim_max;
};

// proc_rlimit_control_wakeupmon: 0x8 (8) bytes
struct proc_rlimit_control_wakeupmon
{
    unsigned int wm_flags;
    int wm_rate;
};

// wait: 0x4 (4) bytes
union wait
{
    int w_status;
    struct NEVERD_GNU_STRUCT
    {
        unsigned int w_Termsig : 7;
        unsigned int w_Coredump : 1;
        unsigned int w_Retcode : 8;
        unsigned int w_Filler : 16;
    } w_T;
    struct NEVERD_GNU_STRUCT
    {
        unsigned int w_Stopval : 8;
        unsigned int w_Stopsig : 8;
        unsigned int w_Filler : 16;
    } w_S;
};

typedef __darwin_ct_rune_t ct_rune_t;

typedef __darwin_rune_t rune_t;

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

typedef unsigned long long malloc_type_id_t;

#pragma neverd_header("spawn.h")
typedef int boolean_t;

#pragma neverd_header("malloc/malloc.h")
// _malloc_zone_t: 0xC8 (200) bytes
struct _malloc_zone_t
{
    void *reserved1;
    void *reserved2;
    size_t (*size)(struct _malloc_zone_t *zone, const void *ptr);
    void *(*malloc)(struct _malloc_zone_t *zone, size_t size);
    void *(*calloc)(struct _malloc_zone_t *zone, size_t num_items, size_t size);
    void *(*valloc)(struct _malloc_zone_t *zone, size_t size);
    void (*free)(struct _malloc_zone_t *zone, void *ptr);
    void *(*realloc)(struct _malloc_zone_t *zone, void *ptr, size_t size);
    void (*destroy)(struct _malloc_zone_t *zone);
    const char *zone_name;
    unsigned int (*batch_malloc)(struct _malloc_zone_t *zone, size_t size, void **results, unsigned int num_requested);
    void (*batch_free)(struct _malloc_zone_t *zone, void **to_be_freed, unsigned int num_to_be_freed);
    malloc_introspection_t *introspect;
    unsigned int version;
    void *(*memalign)(struct _malloc_zone_t *zone, size_t alignment, size_t size);
    void (*free_definite_size)(struct _malloc_zone_t *zone, void *ptr, size_t size);
    size_t (*pressure_relief)(struct _malloc_zone_t *zone, size_t goal);
    boolean_t (*claimed_address)(struct _malloc_zone_t *zone, void *ptr);
    void (*try_free_default)(struct _malloc_zone_t *zone, void *ptr);
    void *(*malloc_with_options)(struct _malloc_zone_t *zone, size_t align, size_t size, unsigned long long options);
    void *(*malloc_type_malloc)(struct _malloc_zone_t *zone, size_t size, malloc_type_id_t type_id);
    void *(*malloc_type_calloc)(struct _malloc_zone_t *zone, size_t count, size_t size, malloc_type_id_t type_id);
    void *(*malloc_type_realloc)(struct _malloc_zone_t *zone, void *ptr, size_t size, malloc_type_id_t type_id);
    void *(*malloc_type_memalign)(struct _malloc_zone_t *zone, size_t alignment, size_t size, malloc_type_id_t type_id);
    void *(*malloc_type_malloc_with_options)(struct _malloc_zone_t *zone, size_t align, size_t size, unsigned long long options, malloc_type_id_t type_id);
};

typedef struct _malloc_zone_t malloc_zone_t;

#pragma neverd_header("stdlib.h")
typedef __darwin_dev_t dev_t;

typedef __darwin_mode_t mode_t;

#pragma neverd_header("string.h")
typedef __darwin_size_t rsize_t;

typedef int errno_t;

#pragma neverd_header("unistd.h")
// accessx_descriptor: 0x10 (16) bytes
struct accessx_descriptor
{
    unsigned int ad_name_offset;
    int ad_flags;
    int ad_pad[2];
};

typedef __darwin_gid_t gid_t;

typedef __darwin_useconds_t useconds_t;

// fd_set: 0x80 (128) bytes
struct fd_set
{
    __int32_t fds_bits[32];
};

// timespec: 0x10 (16) bytes
struct timespec
{
    __darwin_time_t tv_sec;
    long tv_nsec;
};

typedef __darwin_time_t time_t;

typedef __darwin_suseconds_t suseconds_t;

typedef __darwin_uuid_t uuid_t;

#pragma neverd_header("semaphore.h")
typedef unsigned long u_long;

typedef unsigned short u_short;

#pragma neverd_header("sys/mount.h")
typedef u_int32_t attrgroup_t;

// attrlist: 0x18 (24) bytes
struct attrlist
{
    u_short bitmapcount;
    u_int16_t reserved;
    attrgroup_t commonattr;
    attrgroup_t volattr;
    attrgroup_t dirattr;
    attrgroup_t fileattr;
    attrgroup_t forkattr;
};

// fssearchblock: 0x68 (104) bytes
struct fssearchblock
{
    struct attrlist *returnattrs;
    void *returnbuffer;
    size_t returnbuffersize;
    u_long maxmatches;
    struct timeval timelimit;
    void *searchparams1;
    size_t sizeofsearchparams1;
    void *searchparams2;
    size_t sizeofsearchparams2;
    struct attrlist searchattrs;
};

#pragma neverd_header("semaphore.h")
typedef unsigned char u_char;

#pragma neverd_header("sys/mount.h")
// searchstate: 0x22C (556) bytes, packed to 1
#pragma pack(push, 1)
struct searchstate
{
    unsigned int ss_union_flags;
    unsigned int ss_union_layer;
    u_char ss_fsstate[548];
};
#pragma pack(pop)

#pragma neverd_header("fcntl.h")
// flock: 0x18 (24) bytes
struct flock
{
    off_t l_start;
    off_t l_len;
    pid_t l_pid;
    short l_type;
    short l_whence;
};

// flocktimeout: 0x28 (40) bytes
struct flocktimeout
{
    struct flock fl;
    struct timespec timeout;
};

// radvisory: 0x10 (16) bytes
struct radvisory
{
    off_t ra_offset;
    int ra_count;
};

// fsignatures: 0x38 (56) bytes
struct fsignatures
{
    off_t fs_file_start;
    void *fs_blob_start;
    size_t fs_blob_size;
    size_t fs_fsignatures_size;
    char fs_cdhash[20];
    int fs_hash_type;
};

typedef struct fsignatures fsignatures_t;

// fsupplement: 0x20 (32) bytes
struct fsupplement
{
    off_t fs_file_start;
    off_t fs_blob_start;
    size_t fs_blob_size;
    int fs_orig_fd;
};

typedef struct fsupplement fsupplement_t;

// fchecklv: 0x18 (24) bytes
struct fchecklv
{
    off_t lv_file_start;
    size_t lv_error_message_size;
    void *lv_error_message;
};

typedef struct fchecklv fchecklv_t;

// fgetsigsinfo: 0x10 (16) bytes
struct fgetsigsinfo
{
    off_t fg_file_start;
    int fg_info_request;
    int fg_sig_is_platform;
};

typedef struct fgetsigsinfo fgetsigsinfo_t;

// fstore: 0x20 (32) bytes
struct fstore
{
    unsigned int fst_flags;
    int fst_posmode;
    off_t fst_offset;
    off_t fst_length;
    off_t fst_bytesalloc;
};

typedef struct fstore fstore_t;

// fpunchhole: 0x18 (24) bytes
struct fpunchhole
{
    unsigned int fp_flags;
    unsigned int reserved;
    off_t fp_offset;
    off_t fp_length;
};

typedef struct fpunchhole fpunchhole_t;

// ftrimactivefile: 0x10 (16) bytes
struct ftrimactivefile
{
    off_t fta_offset;
    off_t fta_length;
};

typedef struct ftrimactivefile ftrimactivefile_t;

// fspecread: 0x18 (24) bytes
struct fspecread
{
    unsigned int fsr_flags;
    unsigned int reserved;
    off_t fsr_offset;
    off_t fsr_length;
};

typedef struct fspecread fspecread_t;

// fattributiontag: 0x110 (272) bytes
struct fattributiontag
{
    unsigned int ft_flags;
    unsigned long long ft_hash;
    char ft_attribution_name[255];
};

typedef struct fattributiontag fattributiontag_t;

// log2phys: 0x14 (20) bytes, packed to 4
#pragma pack(push, 4)
struct log2phys
{
    unsigned int l2p_flags;
    off_t l2p_contigbytes;
    off_t l2p_devoffset;
};
#pragma pack(pop)

#pragma neverd_header()
// Declared only: its members are unknown.
struct _filesec;

#pragma neverd_header("fcntl.h")
typedef struct _filesec *filesec_t;

#pragma neverd_header("ctype.h")
typedef __darwin_wint_t wint_t;

// _RuneEntry: 0x18 (24) bytes
struct _RuneEntry
{
    __darwin_rune_t __min;
    __darwin_rune_t __max;
    __darwin_rune_t __map;
    __uint32_t *__types;
};

// _RuneRange: 0x10 (16) bytes
struct _RuneRange
{
    int __nranges;
    _RuneEntry *__ranges;
};

// _RuneCharClass: 0x14 (20) bytes
struct _RuneCharClass
{
    char __name[14];
    __uint32_t __mask;
};

// _RuneLocale: 0xC88 (3208) bytes
struct _RuneLocale
{
    char __magic[8];
    char __encoding[32];
    __darwin_rune_t (*__sgetrune)(const char *__string, __darwin_size_t __n, const char **);
    int (*__sputrune)(__darwin_rune_t, char *__string, __darwin_size_t __n, char **);
    __darwin_rune_t __invalid_rune;
    __uint32_t __runetype[256];
    __darwin_rune_t __maplower[256];
    __darwin_rune_t __mapupper[256];
    _RuneRange __runetype_ext;
    _RuneRange __maplower_ext;
    _RuneRange __mapupper_ext;
    void *__variable;
    int __variable_len;
    int __ncharclasses;
    _RuneCharClass *__charclasses;
};

#pragma neverd_header("wchar.h")
typedef __darwin_mbstate_t mbstate_t;

typedef __darwin_clock_t clock_t;

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

typedef __darwin_wctype_t wctype_t;

#pragma neverd_header("wctype.h")
typedef __darwin_wctrans_t wctrans_t;

#pragma neverd_header()
// Declared only: its members are unknown.
struct _xlocale;

#pragma neverd_header("locale.h")
typedef struct _xlocale *locale_t;

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
typedef __darwin_pthread_t pthread_t;

#pragma neverd_header("setjmp.h")
typedef int jmp_buf[48];

typedef int sigjmp_buf[49];

#pragma neverd_header("inttypes.h")
// imaxdiv_t: 0x10 (16) bytes
struct imaxdiv_t
{
    intmax_t quot;
    intmax_t rem;
};

#pragma neverd_header("dirent.h")
typedef __darwin_ino_t ino_t;

// dirent: 0x418 (1048) bytes
struct dirent
{
    __uint64_t d_ino;
    __uint64_t d_seekoff;
    __uint16_t d_reclen;
    __uint16_t d_namlen;
    __uint8_t d_type;
    char d_name[1024];
};

#pragma neverd_header()
// Declared only: its members are unknown.
struct _telldir;

#pragma neverd_header("dirent.h")
// DIR: 0x88 (136) bytes
struct DIR
{
    int __dd_fd;
    size_t __dd_loc;
    size_t __dd_size;
    char *__dd_buf;
    int __dd_len;
    long __dd_seek;
    long __padding;
    int __dd_flags;
    __darwin_pthread_mutex_t __dd_lock;
    struct _telldir *__dd_td;
};

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

#pragma neverd_header("pthread.h")
// sched_param: 0x8 (8) bytes
struct sched_param
{
    int sched_priority;
    char __opaque[4];
};

typedef __darwin_pthread_cond_t pthread_cond_t;

typedef __darwin_pthread_condattr_t pthread_condattr_t;

typedef __darwin_pthread_key_t pthread_key_t;

typedef __darwin_pthread_mutex_t pthread_mutex_t;

typedef __darwin_pthread_mutexattr_t pthread_mutexattr_t;

typedef __darwin_pthread_once_t pthread_once_t;

typedef __darwin_pthread_rwlock_t pthread_rwlock_t;

typedef __darwin_pthread_rwlockattr_t pthread_rwlockattr_t;

typedef unsigned int qos_class_t;

#pragma neverd_header()
// Declared only: its members are unknown.
struct pthread_override_s;

#pragma neverd_header("pthread.h")
typedef struct pthread_override_s *pthread_override_t;

typedef __darwin_mach_port_t mach_port_t;

typedef int (*pthread_jit_write_callback_t)(void *ctx);

#pragma neverd_header("semaphore.h")
typedef unsigned int u_int;

typedef unsigned short ushort;

typedef unsigned int uint;

typedef u_int64_t u_quad_t;

typedef long long quad_t;

typedef quad_t *qaddr_t;

typedef char *caddr_t;

typedef int daddr_t;

typedef u_int32_t fixpt_t;

typedef __darwin_blkcnt_t blkcnt_t;

typedef __darwin_blksize_t blksize_t;

typedef __uint32_t in_addr_t;

typedef __uint16_t in_port_t;

typedef __darwin_ino64_t ino64_t;

typedef __int32_t key_t;

typedef __uint16_t nlink_t;

typedef int segsz_t;

typedef int swblk_t;

typedef __int32_t fd_mask;

typedef __darwin_fsblkcnt_t fsblkcnt_t;

typedef __darwin_fsfilcnt_t fsfilcnt_t;

typedef int sem_t;

#pragma neverd_header("poll.h")
// pollfd: 0x8 (8) bytes
struct pollfd
{
    int fd;
    short events;
    short revents;
};

typedef unsigned int nfds_t;

#pragma neverd_header("termios.h")
typedef unsigned long tcflag_t;

typedef unsigned char cc_t;

typedef unsigned long speed_t;

// termios: 0x48 (72) bytes
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
// passwd: 0x48 (72) bytes
struct passwd
{
    char *pw_name;
    char *pw_passwd;
    uid_t pw_uid;
    gid_t pw_gid;
    __darwin_time_t pw_change;
    char *pw_class;
    char *pw_gecos;
    char *pw_dir;
    char *pw_shell;
    __darwin_time_t pw_expire;
};

typedef __darwin_uuid_string_t uuid_string_t;

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
// stat: 0x90 (144) bytes
struct stat
{
    dev_t st_dev;
    mode_t st_mode;
    nlink_t st_nlink;
    __darwin_ino64_t st_ino;
    uid_t st_uid;
    gid_t st_gid;
    dev_t st_rdev;
    struct timespec st_atimespec;
    struct timespec st_mtimespec;
    struct timespec st_ctimespec;
    struct timespec st_birthtimespec;
    off_t st_size;
    blkcnt_t st_blocks;
    blksize_t st_blksize;
    __uint32_t st_flags;
    __uint32_t st_gen;
    __int32_t st_lspare;
    __int64_t st_qspare[2];
};

#pragma neverd_header("glob.h")
// glob_t: 0x58 (88) bytes
struct glob_t
{
    size_t gl_pathc;
    int gl_matchc;
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
typedef __darwin_off_t regoff_t;

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

#pragma neverd_header("spawn.h")
typedef void *posix_spawnattr_t;

typedef void *posix_spawn_file_actions_t;

typedef __darwin_natural_t natural_t;

typedef int integer_t;

typedef uintptr_t vm_offset_t;

typedef uintptr_t vm_size_t;

typedef unsigned long long mach_vm_address_t;

typedef unsigned long long mach_vm_offset_t;

typedef unsigned long long mach_vm_size_t;

typedef unsigned long long vm_map_offset_t;

typedef unsigned long long vm_map_address_t;

typedef unsigned long long vm_map_size_t;

typedef unsigned int vm32_offset_t;

typedef unsigned int vm32_address_t;

typedef unsigned int vm32_size_t;

typedef vm_offset_t mach_port_context_t;

typedef natural_t mach_port_name_t;

typedef mach_port_name_t *mach_port_name_array_t;

typedef mach_port_t *mach_port_array_t;

typedef natural_t mach_port_right_t;

typedef natural_t mach_port_type_t;

typedef mach_port_type_t *mach_port_type_array_t;

typedef natural_t mach_port_urefs_t;

typedef integer_t mach_port_delta_t;

typedef natural_t mach_port_seqno_t;

typedef natural_t mach_port_mscount_t;

typedef natural_t mach_port_msgcount_t;

typedef natural_t mach_port_rights_t;

typedef unsigned int mach_port_srights_t;

// mach_port_status: 0x28 (40) bytes
struct mach_port_status
{
    mach_port_rights_t mps_pset;
    mach_port_seqno_t mps_seqno;
    mach_port_mscount_t mps_mscount;
    mach_port_msgcount_t mps_qlimit;
    mach_port_msgcount_t mps_msgcount;
    mach_port_rights_t mps_sorights;
    boolean_t mps_srights;
    boolean_t mps_pdrequest;
    boolean_t mps_nsrequest;
    natural_t mps_flags;
};

typedef struct mach_port_status mach_port_status_t;

// mach_port_limits: 0x4 (4) bytes
struct mach_port_limits
{
    mach_port_msgcount_t mpl_qlimit;
};

typedef struct mach_port_limits mach_port_limits_t;

// mach_port_info_ext: 0x44 (68) bytes
struct mach_port_info_ext
{
    mach_port_status_t mpie_status;
    mach_port_msgcount_t mpie_boost_cnt;
    unsigned int reserved[6];
};

typedef struct mach_port_info_ext mach_port_info_ext_t;

// mach_port_guard_info: 0x8 (8) bytes
struct mach_port_guard_info
{
    unsigned long long mpgi_guard;
};

typedef struct mach_port_guard_info mach_port_guard_info_t;

typedef integer_t *mach_port_info_t;

typedef int mach_port_flavor_t;

// mach_port_qos: 0x8 (8) bytes
struct NEVERD_GNU_STRUCT mach_port_qos
{
    unsigned int name : 1;
    unsigned int prealloc : 1;
    boolean_t pad1 : 30;
    natural_t len;
};

typedef struct mach_port_qos mach_port_qos_t;

// mach_service_port_info: 0x100 (256) bytes
struct mach_service_port_info
{
    char mspi_string_name[255];
    unsigned char mspi_domain_type;
};

typedef struct mach_service_port_info mach_service_port_info_data_t;

typedef struct mach_service_port_info *mach_service_port_info_t;

// mach_port_options: 0x18 (24) bytes
struct mach_port_options
{
    unsigned int flags;
    mach_port_limits_t mpl;
    union
    {
        unsigned long long reserved[2];
        mach_port_name_t work_interval_port;
        mach_service_port_info_t service_port_info;
        mach_port_name_t service_port_name;
    };
};

typedef struct mach_port_options mach_port_options_t;

typedef mach_port_options_t *mach_port_options_ptr_t;

typedef int kern_return_t;

typedef natural_t mach_msg_timeout_t;

typedef unsigned int mach_msg_bits_t;

typedef natural_t mach_msg_size_t;

typedef integer_t mach_msg_id_t;

typedef unsigned int mach_msg_priority_t;

typedef unsigned int mach_msg_type_name_t;

typedef unsigned int mach_msg_copy_options_t;

typedef unsigned int mach_msg_guard_flags_t;

typedef unsigned int mach_msg_descriptor_type_t;

// mach_msg_type_descriptor_t: 0xC (12) bytes, packed to 4
#pragma pack(push, 4)
struct NEVERD_GNU_STRUCT mach_msg_type_descriptor_t
{
    natural_t pad1;
    mach_msg_size_t pad2;
    unsigned int pad3 : 24;
    mach_msg_descriptor_type_t type : 8;
};
#pragma pack(pop)

// mach_msg_port_descriptor_t: 0xC (12) bytes, packed to 4
#pragma pack(push, 4)
struct NEVERD_GNU_STRUCT mach_msg_port_descriptor_t
{
    mach_port_t name;
    mach_msg_size_t pad1;
    unsigned int pad2 : 16;
    mach_msg_type_name_t disposition : 8;
    mach_msg_descriptor_type_t type : 8;
};
#pragma pack(pop)

// mach_msg_ool_descriptor32_t: 0xC (12) bytes, packed to 4
#pragma pack(push, 4)
struct NEVERD_GNU_STRUCT mach_msg_ool_descriptor32_t
{
    unsigned int address;
    mach_msg_size_t size;
    boolean_t deallocate : 8;
    mach_msg_copy_options_t copy : 8;
    unsigned int pad1 : 8;
    mach_msg_descriptor_type_t type : 8;
};
#pragma pack(pop)

// mach_msg_ool_descriptor64_t: 0x10 (16) bytes, packed to 4
#pragma pack(push, 4)
struct NEVERD_GNU_STRUCT mach_msg_ool_descriptor64_t
{
    unsigned long long address;
    boolean_t deallocate : 8;
    mach_msg_copy_options_t copy : 8;
    unsigned int pad1 : 8;
    mach_msg_descriptor_type_t type : 8;
    mach_msg_size_t size;
};
#pragma pack(pop)

// mach_msg_ool_descriptor_t: 0x10 (16) bytes, packed to 4
#pragma pack(push, 4)
struct NEVERD_GNU_STRUCT mach_msg_ool_descriptor_t
{
    void *address;
    boolean_t deallocate : 8;
    mach_msg_copy_options_t copy : 8;
    unsigned int pad1 : 8;
    mach_msg_descriptor_type_t type : 8;
    mach_msg_size_t size;
};
#pragma pack(pop)

// mach_msg_ool_ports_descriptor32_t: 0xC (12) bytes, packed to 4
#pragma pack(push, 4)
struct NEVERD_GNU_STRUCT mach_msg_ool_ports_descriptor32_t
{
    unsigned int address;
    mach_msg_size_t count;
    boolean_t deallocate : 8;
    mach_msg_copy_options_t copy : 8;
    mach_msg_type_name_t disposition : 8;
    mach_msg_descriptor_type_t type : 8;
};
#pragma pack(pop)

// mach_msg_ool_ports_descriptor64_t: 0x10 (16) bytes, packed to 4
#pragma pack(push, 4)
struct NEVERD_GNU_STRUCT mach_msg_ool_ports_descriptor64_t
{
    unsigned long long address;
    boolean_t deallocate : 8;
    mach_msg_copy_options_t copy : 8;
    mach_msg_type_name_t disposition : 8;
    mach_msg_descriptor_type_t type : 8;
    mach_msg_size_t count;
};
#pragma pack(pop)

// mach_msg_ool_ports_descriptor_t: 0x10 (16) bytes, packed to 4
#pragma pack(push, 4)
struct NEVERD_GNU_STRUCT mach_msg_ool_ports_descriptor_t
{
    void *address;
    boolean_t deallocate : 8;
    mach_msg_copy_options_t copy : 8;
    mach_msg_type_name_t disposition : 8;
    mach_msg_descriptor_type_t type : 8;
    mach_msg_size_t count;
};
#pragma pack(pop)

// mach_msg_guarded_port_descriptor32_t: 0xC (12) bytes, packed to 4
#pragma pack(push, 4)
struct NEVERD_GNU_STRUCT mach_msg_guarded_port_descriptor32_t
{
    unsigned int context;
    mach_port_name_t name;
    mach_msg_guard_flags_t flags : 16;
    mach_msg_type_name_t disposition : 8;
    mach_msg_descriptor_type_t type : 8;
};
#pragma pack(pop)

// mach_msg_guarded_port_descriptor64_t: 0x10 (16) bytes, packed to 4
#pragma pack(push, 4)
struct NEVERD_GNU_STRUCT mach_msg_guarded_port_descriptor64_t
{
    unsigned long long context;
    mach_msg_guard_flags_t flags : 16;
    mach_msg_type_name_t disposition : 8;
    mach_msg_descriptor_type_t type : 8;
    mach_port_name_t name;
};
#pragma pack(pop)

// mach_msg_guarded_port_descriptor_t: 0x10 (16) bytes, packed to 4
#pragma pack(push, 4)
struct NEVERD_GNU_STRUCT mach_msg_guarded_port_descriptor_t
{
    mach_port_context_t context;
    mach_msg_guard_flags_t flags : 16;
    mach_msg_type_name_t disposition : 8;
    mach_msg_descriptor_type_t type : 8;
    mach_port_name_t name;
};
#pragma pack(pop)

// mach_msg_descriptor_t: 0x10 (16) bytes, packed to 4
#pragma pack(push, 4)
union mach_msg_descriptor_t
{
    mach_msg_port_descriptor_t port;
    mach_msg_ool_descriptor_t out_of_line;
    mach_msg_ool_ports_descriptor_t ool_ports;
    mach_msg_type_descriptor_t type;
    mach_msg_guarded_port_descriptor_t guarded_port;
};
#pragma pack(pop)

// mach_msg_body_t: 0x4 (4) bytes, packed to 4
#pragma pack(push, 4)
struct mach_msg_body_t
{
    mach_msg_size_t msgh_descriptor_count;
};
#pragma pack(pop)

// mach_msg_header_t: 0x18 (24) bytes, packed to 4
#pragma pack(push, 4)
struct mach_msg_header_t
{
    mach_msg_bits_t msgh_bits;
    mach_msg_size_t msgh_size;
    mach_port_t msgh_remote_port;
    mach_port_t msgh_local_port;
    mach_port_name_t msgh_voucher_port;
    mach_msg_id_t msgh_id;
};
#pragma pack(pop)

// mach_msg_base_t: 0x1C (28) bytes, packed to 4
#pragma pack(push, 4)
struct mach_msg_base_t
{
    mach_msg_header_t header;
    mach_msg_body_t body;
};
#pragma pack(pop)

typedef unsigned int mach_msg_trailer_type_t;

typedef unsigned int mach_msg_trailer_size_t;

typedef char *mach_msg_trailer_info_t;

// mach_msg_trailer_t: 0x8 (8) bytes, packed to 4
#pragma pack(push, 4)
struct mach_msg_trailer_t
{
    mach_msg_trailer_type_t msgh_trailer_type;
    mach_msg_trailer_size_t msgh_trailer_size;
};
#pragma pack(pop)

// mach_msg_seqno_trailer_t: 0xC (12) bytes, packed to 4
#pragma pack(push, 4)
struct mach_msg_seqno_trailer_t
{
    mach_msg_trailer_type_t msgh_trailer_type;
    mach_msg_trailer_size_t msgh_trailer_size;
    mach_port_seqno_t msgh_seqno;
};
#pragma pack(pop)

// security_token_t: 0x8 (8) bytes, packed to 4
#pragma pack(push, 4)
struct security_token_t
{
    unsigned int val[2];
};
#pragma pack(pop)

// mach_msg_security_trailer_t: 0x14 (20) bytes, packed to 4
#pragma pack(push, 4)
struct mach_msg_security_trailer_t
{
    mach_msg_trailer_type_t msgh_trailer_type;
    mach_msg_trailer_size_t msgh_trailer_size;
    mach_port_seqno_t msgh_seqno;
    security_token_t msgh_sender;
};
#pragma pack(pop)

// audit_token_t: 0x20 (32) bytes, packed to 4
#pragma pack(push, 4)
struct audit_token_t
{
    unsigned int val[8];
};
#pragma pack(pop)

// mach_msg_audit_trailer_t: 0x34 (52) bytes, packed to 4
#pragma pack(push, 4)
struct mach_msg_audit_trailer_t
{
    mach_msg_trailer_type_t msgh_trailer_type;
    mach_msg_trailer_size_t msgh_trailer_size;
    mach_port_seqno_t msgh_seqno;
    security_token_t msgh_sender;
    audit_token_t msgh_audit;
};
#pragma pack(pop)

// mach_msg_context_trailer_t: 0x3C (60) bytes, packed to 4
#pragma pack(push, 4)
struct mach_msg_context_trailer_t
{
    mach_msg_trailer_type_t msgh_trailer_type;
    mach_msg_trailer_size_t msgh_trailer_size;
    mach_port_seqno_t msgh_seqno;
    security_token_t msgh_sender;
    audit_token_t msgh_audit;
    mach_port_context_t msgh_context;
};
#pragma pack(pop)

// msg_labels_t: 0x4 (4) bytes, packed to 4
#pragma pack(push, 4)
struct msg_labels_t
{
    mach_port_name_t sender;
};
#pragma pack(pop)

typedef int mach_msg_filter_id;

// mach_msg_mac_trailer_t: 0x44 (68) bytes, packed to 4
#pragma pack(push, 4)
struct mach_msg_mac_trailer_t
{
    mach_msg_trailer_type_t msgh_trailer_type;
    mach_msg_trailer_size_t msgh_trailer_size;
    mach_port_seqno_t msgh_seqno;
    security_token_t msgh_sender;
    audit_token_t msgh_audit;
    mach_port_context_t msgh_context;
    mach_msg_filter_id msgh_ad;
    msg_labels_t msgh_labels;
};
#pragma pack(pop)

typedef mach_msg_mac_trailer_t mach_msg_max_trailer_t;

typedef mach_msg_security_trailer_t mach_msg_format_0_trailer_t;

typedef integer_t mach_msg_options_t;

// mach_msg_empty_send_t: 0x18 (24) bytes, packed to 4
#pragma pack(push, 4)
struct mach_msg_empty_send_t
{
    mach_msg_header_t header;
};
#pragma pack(pop)

// mach_msg_empty_rcv_t: 0x20 (32) bytes, packed to 4
#pragma pack(push, 4)
struct mach_msg_empty_rcv_t
{
    mach_msg_header_t header;
    mach_msg_trailer_t trailer;
};
#pragma pack(pop)

// mach_msg_empty_t: 0x20 (32) bytes, packed to 4
#pragma pack(push, 4)
union mach_msg_empty_t
{
    mach_msg_empty_send_t send;
    mach_msg_empty_rcv_t rcv;
};
#pragma pack(pop)

typedef natural_t mach_msg_type_size_t;

typedef natural_t mach_msg_type_number_t;

typedef integer_t mach_msg_option_t;

typedef kern_return_t mach_msg_return_t;

typedef kern_return_t mach_error_t;

typedef mach_error_t (*mach_error_fn_t)(void);

typedef vm_offset_t pointer_t;

typedef vm_offset_t vm_address_t;

typedef unsigned long long addr64_t;

typedef unsigned int reg64_t;

typedef unsigned int ppnum_t;

typedef mach_port_t vm_map_t;

typedef mach_port_t vm_map_read_t;

typedef mach_port_t vm_map_inspect_t;

typedef mach_port_t upl_t;

typedef mach_port_t vm_named_entry_t;

typedef mach_vm_offset_t *mach_vm_offset_list_t;

typedef unsigned long long vm_object_offset_t;

typedef unsigned long long vm_object_size_t;

// mach_vm_range: 0x10 (16) bytes
struct mach_vm_range
{
    mach_vm_offset_t min_address;
    mach_vm_offset_t max_address;
};

typedef struct mach_vm_range *mach_vm_range_t;

// mach_vm_range_recipe_v1_t: 0x18 (24) bytes, packed to 1
#pragma pack(push, 1)
struct NEVERD_GNU_STRUCT mach_vm_range_recipe_v1_t
{
    mach_vm_range_flags_t flags : 48;
    mach_vm_range_tag_t range_tag : 8;
    unsigned char vm_tag : 8;
    struct mach_vm_range range;
};
#pragma pack(pop)

typedef mach_vm_range_recipe_v1_t mach_vm_range_recipe_t;

typedef unsigned char *mach_vm_range_recipes_raw_t;

// arm_state_hdr: 0x8 (8) bytes
struct arm_state_hdr
{
    unsigned int flavor;
    unsigned int count;
};

typedef struct arm_state_hdr arm_state_hdr_t;

typedef struct __darwin_arm_thread_state arm_thread_state_t;

typedef struct __darwin_arm_thread_state arm_thread_state32_t;

typedef struct __darwin_arm_thread_state64 arm_thread_state64_t;

// arm_unified_thread_state: 0x118 (280) bytes
struct arm_unified_thread_state
{
    arm_state_hdr_t ash;
    union
    {
        arm_thread_state32_t ts_32;
        arm_thread_state64_t ts_64;
    } uts;
};

typedef struct arm_unified_thread_state arm_unified_thread_state_t;

typedef struct __darwin_arm_vfp_state arm_vfp_state_t;

typedef struct __darwin_arm_neon_state arm_neon_state_t;

typedef struct __darwin_arm_neon_state arm_neon_state32_t;

typedef struct __darwin_arm_neon_state64 arm_neon_state64_t;

typedef struct __darwin_arm_exception_state arm_exception_state_t;

typedef struct __darwin_arm_exception_state arm_exception_state32_t;

typedef struct __darwin_arm_exception_state64 arm_exception_state64_t;

typedef struct __darwin_arm_exception_state64_v2 arm_exception_state64_v2_t;

typedef struct __darwin_arm_debug_state32 arm_debug_state32_t;

typedef struct __darwin_arm_debug_state64 arm_debug_state64_t;

typedef struct __arm_pagein_state arm_pagein_state_t;

typedef struct __darwin_arm_sme_state arm_sme_state_t;

typedef struct __darwin_arm_sve_z_state arm_sve_z_state_t;

typedef struct __darwin_arm_sve_p_state arm_sve_p_state_t;

typedef struct __darwin_arm_sme_za_state arm_sme_za_state_t;

typedef struct __darwin_arm_sme2_state arm_sme2_state_t;

typedef struct __arm_legacy_debug_state arm_debug_state_t;

typedef natural_t *thread_state_t;

typedef natural_t thread_state_data_t[1296];

typedef int thread_state_flavor_t;

typedef thread_state_flavor_t *thread_state_flavor_array_t;

// ipc_info_space: 0x18 (24) bytes
struct ipc_info_space
{
    natural_t iis_genno_mask;
    natural_t iis_table_size;
    natural_t iis_table_next;
    natural_t iis_tree_size;
    natural_t iis_tree_small;
    natural_t iis_tree_hash;
};

typedef struct ipc_info_space ipc_info_space_t;

// ipc_info_space_basic: 0x18 (24) bytes
struct ipc_info_space_basic
{
    natural_t iisb_genno_mask;
    natural_t iisb_table_size;
    natural_t iisb_table_next;
    natural_t iisb_table_inuse;
    natural_t iisb_reserved[2];
};

typedef struct ipc_info_space_basic ipc_info_space_basic_t;

// ipc_info_name: 0x1C (28) bytes
struct ipc_info_name
{
    mach_port_name_t iin_name;
    integer_t iin_collision;
    mach_port_type_t iin_type;
    mach_port_urefs_t iin_urefs;
    natural_t iin_object;
    natural_t iin_next;
    natural_t iin_hash;
};

typedef struct ipc_info_name ipc_info_name_t;

typedef ipc_info_name_t *ipc_info_name_array_t;

// ipc_info_tree_name: 0x24 (36) bytes
struct ipc_info_tree_name
{
    ipc_info_name_t iitn_name;
    mach_port_name_t iitn_lchild;
    mach_port_name_t iitn_rchild;
};

typedef struct ipc_info_tree_name ipc_info_tree_name_t;

typedef ipc_info_tree_name_t *ipc_info_tree_name_array_t;

// ipc_info_port: 0x8 (8) bytes
struct ipc_info_port
{
    natural_t iip_port_object;
    natural_t iip_receiver_object;
};

typedef struct ipc_info_port ipc_info_port_t;

typedef ipc_info_port_t *exception_handler_info_array_t;

typedef int exception_type_t;

typedef integer_t exception_data_type_t;

typedef long long mach_exception_data_type_t;

typedef int exception_behavior_t;

typedef exception_data_type_t *exception_data_t;

typedef mach_exception_data_type_t *mach_exception_data_t;

typedef unsigned int exception_mask_t;

typedef exception_mask_t *exception_mask_array_t;

typedef exception_behavior_t *exception_behavior_array_t;

typedef thread_state_flavor_t *exception_flavor_array_t;

typedef mach_port_t *exception_port_array_t;

typedef ipc_info_port_t *exception_port_info_array_t;

typedef mach_exception_data_type_t mach_exception_code_t;

typedef mach_exception_data_type_t mach_exception_subcode_t;

typedef integer_t cpu_type_t;

typedef integer_t cpu_subtype_t;

typedef integer_t cpu_threadtype_t;

#pragma neverd_header("langinfo.h")
typedef __darwin_nl_item nl_item;

#pragma neverd_header("netdb.h")
typedef __darwin_socklen_t socklen_t;

typedef __uint8_t sa_family_t;

// iovec: 0x10 (16) bytes
struct iovec
{
    void *iov_base;
    size_t iov_len;
};

typedef __uint32_t sae_associd_t;

typedef __uint32_t sae_connid_t;

// sockaddr: 0x10 (16) bytes
struct sockaddr
{
    __uint8_t sa_len;
    sa_family_t sa_family;
    char sa_data[14];
};

// sa_endpoints: 0x28 (40) bytes
struct sa_endpoints
{
    unsigned int sae_srcif;
    const struct sockaddr *sae_srcaddr;
    socklen_t sae_srcaddrlen;
    const struct sockaddr *sae_dstaddr;
    socklen_t sae_dstaddrlen;
};

typedef struct sa_endpoints sa_endpoints_t;

// linger: 0x8 (8) bytes
struct linger
{
    int l_onoff;
    int l_linger;
};

// so_np_extensions: 0x8 (8) bytes
struct so_np_extensions
{
    u_int32_t npx_flags;
    u_int32_t npx_mask;
};

// __sockaddr_header: 0x2 (2) bytes
struct __sockaddr_header
{
    __uint8_t sa_len;
    sa_family_t sa_family;
};

// sockproto: 0x4 (4) bytes
struct sockproto
{
    __uint16_t sp_family;
    __uint16_t sp_protocol;
};

// sockaddr_storage: 0x80 (128) bytes
struct sockaddr_storage
{
    __uint8_t ss_len;
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

// sf_hdtr: 0x20 (32) bytes
struct sf_hdtr
{
    struct iovec *headers;
    int hdr_cnt;
    struct iovec *trailers;
    int trl_cnt;
};

// in_addr: 0x4 (4) bytes
struct in_addr
{
    in_addr_t s_addr;
};

// sockaddr_in: 0x10 (16) bytes
struct sockaddr_in
{
    __uint8_t sin_len;
    sa_family_t sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    char sin_zero[8];
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

// ip_mreq_source: 0xC (12) bytes, packed to 4
#pragma pack(push, 4)
struct ip_mreq_source
{
    struct in_addr imr_multiaddr;
    struct in_addr imr_sourceaddr;
    struct in_addr imr_interface;
};
#pragma pack(pop)

// group_req: 0x84 (132) bytes, packed to 4
#pragma pack(push, 4)
struct group_req
{
    unsigned int gr_interface;
    struct sockaddr_storage gr_group;
};
#pragma pack(pop)

// group_source_req: 0x104 (260) bytes, packed to 4
#pragma pack(push, 4)
struct group_source_req
{
    unsigned int gsr_interface;
    struct sockaddr_storage gsr_group;
    struct sockaddr_storage gsr_source;
};
#pragma pack(pop)

// __msfilterreq: 0x98 (152) bytes, packed to 4
#pragma pack(push, 4)
struct __msfilterreq
{
    unsigned int msfr_ifindex;
    unsigned int msfr_fmode;
    unsigned int msfr_nsrcs;
    unsigned int __msfr_align;
    struct sockaddr_storage msfr_group;
    struct sockaddr_storage *msfr_srcs;
};
#pragma pack(pop)

// in_pktinfo: 0xC (12) bytes
struct in_pktinfo
{
    unsigned int ipi_ifindex;
    struct in_addr ipi_spec_dst;
    struct in_addr ipi_addr;
};

// in6_addr: 0x10 (16) bytes
struct in6_addr
{
    union
    {
        __uint8_t __u6_addr8[16];
        __uint16_t __u6_addr16[8];
        __uint32_t __u6_addr32[4];
    } __u6_addr;
};

typedef struct in6_addr in6_addr_t;

// sockaddr_in6: 0x1C (28) bytes
struct sockaddr_in6
{
    __uint8_t sin6_len;
    sa_family_t sin6_family;
    in_port_t sin6_port;
    __uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    __uint32_t sin6_scope_id;
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

// rpcent: 0x18 (24) bytes
struct rpcent
{
    char *r_name;
    char **r_aliases;
    int r_number;
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

#pragma neverd_header("utime.h")
// utimbuf: 0x10 (16) bytes
struct utimbuf
{
    time_t actime;
    time_t modtime;
};

#pragma neverd_header("utmpx.h")
// timeval64: 0x10 (16) bytes
struct timeval64
{
    __int64_t tv_sec;
    __int64_t tv_usec;
};

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

// clockinfo: 0x14 (20) bytes
struct clockinfo
{
    int hz;
    int tick;
    int tickadj;
    int stathz;
    int profhz;
};

// utmpx: 0x280 (640) bytes
struct utmpx
{
    char ut_user[256];
    char ut_id[4];
    char ut_line[32];
    pid_t ut_pid;
    short ut_type;
    struct timeval ut_tv;
    char ut_host[256];
    __uint32_t ut_pad[16];
};

// lastlogx: 0x130 (304) bytes
struct lastlogx
{
    struct timeval ll_tv;
    char ll_line[32];
    char ll_host[256];
};

#pragma neverd_header()
// Declared only: its members are unknown.
struct utmp;

#pragma neverd_header("sys/stat.h")
// ostat: 0x60 (96) bytes
struct ostat
{
    __uint16_t st_dev;
    ino_t st_ino;
    mode_t st_mode;
    nlink_t st_nlink;
    __uint16_t st_uid;
    __uint16_t st_gid;
    __uint16_t st_rdev;
    __int32_t st_size;
    struct timespec st_atimespec;
    struct timespec st_mtimespec;
    struct timespec st_ctimespec;
    __int32_t st_blksize;
    __int32_t st_blocks;
    __uint32_t st_flags;
    __uint32_t st_gen;
};

#pragma neverd_header("sys/statvfs.h")
// statvfs: 0x40 (64) bytes
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

#pragma neverd_header("sys/ioctl.h")
// ttysize: 0x8 (8) bytes
struct ttysize
{
    unsigned short ts_lines;
    unsigned short ts_cols;
    unsigned short ts_xxx;
    unsigned short ts_yyy;
};

// net_event_data: 0x18 (24) bytes
struct net_event_data
{
    u_int32_t if_family;
    u_int32_t if_unit;
    char if_name[16];
};

// timeval32: 0x8 (8) bytes
struct timeval32
{
    __int32_t tv_sec;
    __int32_t tv_usec;
};

// if_data: 0x60 (96) bytes, packed to 4
#pragma pack(push, 4)
struct if_data
{
    u_char ifi_type;
    u_char ifi_typelen;
    u_char ifi_physical;
    u_char ifi_addrlen;
    u_char ifi_hdrlen;
    u_char ifi_recvquota;
    u_char ifi_xmitquota;
    u_char ifi_unused1;
    u_int32_t ifi_mtu;
    u_int32_t ifi_metric;
    u_int32_t ifi_baudrate;
    u_int32_t ifi_ipackets;
    u_int32_t ifi_ierrors;
    u_int32_t ifi_opackets;
    u_int32_t ifi_oerrors;
    u_int32_t ifi_collisions;
    u_int32_t ifi_ibytes;
    u_int32_t ifi_obytes;
    u_int32_t ifi_imcasts;
    u_int32_t ifi_omcasts;
    u_int32_t ifi_iqdrops;
    u_int32_t ifi_noproto;
    u_int32_t ifi_recvtiming;
    u_int32_t ifi_xmittiming;
    struct timeval32 ifi_lastchange;
    u_int32_t ifi_unused2;
    u_int32_t ifi_hwassist;
    u_int32_t ifi_reserved1;
    u_int32_t ifi_reserved2;
};
#pragma pack(pop)

// if_data64: 0x80 (128) bytes, packed to 4
#pragma pack(push, 4)
struct if_data64
{
    u_char ifi_type;
    u_char ifi_typelen;
    u_char ifi_physical;
    u_char ifi_addrlen;
    u_char ifi_hdrlen;
    u_char ifi_recvquota;
    u_char ifi_xmitquota;
    u_char ifi_unused1;
    u_int32_t ifi_mtu;
    u_int32_t ifi_metric;
    u_int64_t ifi_baudrate;
    u_int64_t ifi_ipackets;
    u_int64_t ifi_ierrors;
    u_int64_t ifi_opackets;
    u_int64_t ifi_oerrors;
    u_int64_t ifi_collisions;
    u_int64_t ifi_ibytes;
    u_int64_t ifi_obytes;
    u_int64_t ifi_imcasts;
    u_int64_t ifi_omcasts;
    u_int64_t ifi_iqdrops;
    u_int64_t ifi_noproto;
    u_int32_t ifi_recvtiming;
    u_int32_t ifi_xmittiming;
    struct timeval32 ifi_lastchange;
};
#pragma pack(pop)

#pragma neverd_header()
// Declared only: its members are unknown.
struct ifnet_interface_advisory;

#pragma neverd_header("sys/ioctl.h")
// ifqueue: 0x20 (32) bytes
struct ifqueue
{
    void *ifq_head;
    void *ifq_tail;
    int ifq_len;
    int ifq_maxlen;
    int ifq_drops;
};

// if_clonereq: 0x10 (16) bytes
struct if_clonereq
{
    int ifcr_total;
    int ifcr_count;
    char *ifcr_buffer;
};

// if_msghdr: 0x70 (112) bytes
struct if_msghdr
{
    unsigned short ifm_msglen;
    unsigned char ifm_version;
    unsigned char ifm_type;
    int ifm_addrs;
    int ifm_flags;
    unsigned short ifm_index;
    struct if_data ifm_data;
};

// ifa_msghdr: 0x14 (20) bytes
struct ifa_msghdr
{
    unsigned short ifam_msglen;
    unsigned char ifam_version;
    unsigned char ifam_type;
    int ifam_addrs;
    int ifam_flags;
    unsigned short ifam_index;
    int ifam_metric;
};

// ifma_msghdr: 0x10 (16) bytes
struct ifma_msghdr
{
    unsigned short ifmam_msglen;
    unsigned char ifmam_version;
    unsigned char ifmam_type;
    int ifmam_addrs;
    int ifmam_flags;
    unsigned short ifmam_index;
};

// if_msghdr2: 0xA0 (160) bytes
struct if_msghdr2
{
    u_short ifm_msglen;
    u_char ifm_version;
    u_char ifm_type;
    int ifm_addrs;
    int ifm_flags;
    u_short ifm_index;
    int ifm_snd_len;
    int ifm_snd_maxlen;
    int ifm_snd_drops;
    int ifm_timer;
    struct if_data64 ifm_data;
};

// ifma_msghdr2: 0x14 (20) bytes
struct ifma_msghdr2
{
    u_short ifmam_msglen;
    u_char ifmam_version;
    u_char ifmam_type;
    int ifmam_addrs;
    int ifmam_flags;
    u_short ifmam_index;
    int ifmam_refcount;
};

// ifdevmtu: 0xC (12) bytes
struct ifdevmtu
{
    int ifdm_current;
    int ifdm_min;
    int ifdm_max;
};

// ifkpi: 0x10 (16) bytes, packed to 4
#pragma pack(push, 4)
struct ifkpi
{
    unsigned int ifk_module_id;
    unsigned int ifk_type;
    union
    {
        void *ifk_ptr;
        int ifk_value;
    } ifk_data;
};
#pragma pack(pop)

// ifreq: 0x20 (32) bytes
struct ifreq
{
    char ifr_name[16];
    union
    {
        struct sockaddr ifru_addr;
        struct sockaddr ifru_dstaddr;
        struct sockaddr ifru_broadaddr;
        short ifru_flags;
        int ifru_metric;
        int ifru_mtu;
        int ifru_phys;
        int ifru_media;
        int ifru_intval;
        caddr_t ifru_data;
        struct ifdevmtu ifru_devmtu;
        struct ifkpi ifru_kpi;
        u_int32_t ifru_wake_flags;
        u_int32_t ifru_route_refcnt;
        int ifru_cap[2];
        u_int32_t ifru_functional_type;
        u_int32_t ifru_peer_egress_functional_type;
        u_int8_t ifru_is_directlink;
        u_int8_t ifru_is_vpn;
        u_int8_t ifru_is_companionlink;
    } ifr_ifru;
};

// ifaliasreq: 0x40 (64) bytes
struct ifaliasreq
{
    char ifra_name[16];
    struct sockaddr ifra_addr;
    struct sockaddr ifra_broadaddr;
    struct sockaddr ifra_mask;
};

// rslvmulti_req: 0x10 (16) bytes
struct rslvmulti_req
{
    struct sockaddr *sa;
    struct sockaddr **llsa;
};

// ifmediareq: 0x2C (44) bytes, packed to 4
#pragma pack(push, 4)
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
#pragma pack(pop)

// ifdrv: 0x28 (40) bytes, packed to 4
#pragma pack(push, 4)
struct ifdrv
{
    char ifd_name[16];
    unsigned long ifd_cmd;
    size_t ifd_len;
    void *ifd_data;
};
#pragma pack(pop)

// ifstat: 0x331 (817) bytes
struct ifstat
{
    char ifs_name[16];
    char ascii[801];
};

// ifconf: 0xC (12) bytes, packed to 4
#pragma pack(push, 4)
struct ifconf
{
    int ifc_len;
    union
    {
        caddr_t ifcu_buf;
        struct ifreq *ifcu_req;
    } ifc_ifcu;
};
#pragma pack(pop)

// kev_dl_proto_data: 0x20 (32) bytes
struct kev_dl_proto_data
{
    struct net_event_data link_data;
    u_int32_t proto_family;
    u_int32_t proto_remaining_count;
};

// if_nameindex: 0x10 (16) bytes
struct if_nameindex
{
    unsigned int if_index;
    char *if_name;
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
// kevent: 0x20 (32) bytes, packed to 4
#pragma pack(push, 4)
struct kevent
{
    uintptr_t ident;
    short filter;
    unsigned short flags;
    unsigned int fflags;
    intptr_t data;
    void *udata;
};
#pragma pack(pop)

// kevent64_s: 0x30 (48) bytes
struct kevent64_s
{
    unsigned long long ident;
    short filter;
    unsigned short flags;
    unsigned int fflags;
    long long data;
    unsigned long long udata;
    unsigned long long ext[2];
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

#pragma neverd_header()
// Declared only: its members are unknown.
struct ucred;

#pragma neverd_header("sys/file.h")
typedef struct ucred *kauth_cred_t;

#pragma neverd_header()
// Declared only: its members are unknown.
struct posix_cred;

#pragma neverd_header("sys/file.h")
typedef struct posix_cred *posix_cred_t;

#pragma neverd_header("sys/sysctl.h")
typedef uid_t au_id_t;

typedef pid_t au_asid_t;

typedef u_int16_t au_event_t;

typedef u_int16_t au_emod_t;

typedef u_int32_t au_class_t;

typedef u_int64_t au_asflgs_t;

typedef unsigned char au_ctlmode_t;

// au_tid: 0x8 (8) bytes
struct au_tid
{
    dev_t port;
    u_int32_t machine;
};

typedef struct au_tid au_tid_t;

// au_tid_addr: 0x18 (24) bytes
struct au_tid_addr
{
    dev_t at_port;
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

// au_expire_after: 0x18 (24) bytes
struct au_expire_after
{
    time_t age;
    size_t size;
    unsigned char op_type;
};

typedef struct au_expire_after au_expire_after_t;

#pragma neverd_header()
// Declared only: its members are unknown.
struct au_token;

#pragma neverd_header("sys/sysctl.h")
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

#pragma neverd_header()
// Declared only: its members are unknown.
struct label;

#pragma neverd_header("sys/sysctl.h")
// xucred: 0x4C (76) bytes
struct xucred
{
    u_int cr_version;
    uid_t cr_uid;
    short cr_ngroups;
    gid_t cr_groups[16];
};

#pragma neverd_header()
// Declared only: its members are unknown.
struct session;

// Declared only: its members are unknown.
struct pgrp;

// Declared only: its members are unknown.
struct proc;

// Declared only: its members are unknown.
struct proc_ident;

#pragma neverd_header("sys/sysctl.h")
// vmspace: 0x40 (64) bytes
struct vmspace
{
    int dummy;
    caddr_t dummy2;
    int dummy3[5];
    caddr_t dummy4[3];
};

#pragma neverd_header()
// Declared only: its members are unknown.
struct sigacts;

// Declared only: its members are unknown.
struct vnode;

// Declared only: its members are unknown.
struct user;

#pragma neverd_header("sys/sysctl.h")
// extern_proc: 0x128 (296) bytes
struct extern_proc
{
    union
    {
        struct
        {
            struct proc *__p_forw;
            struct proc *__p_back;
        } p_st1;
        struct timeval __p_starttime;
    } p_un;
    struct vmspace *p_vmspace;
    struct sigacts *p_sigacts;
    int p_flag;
    char p_stat;
    pid_t p_pid;
    pid_t p_oppid;
    int p_dupfd;
    caddr_t user_stack;
    void *exit_thread;
    int p_debugger;
    boolean_t sigwait;
    u_int p_estcpu;
    int p_cpticks;
    fixpt_t p_pctcpu;
    void *p_wchan;
    char *p_wmesg;
    u_int p_swtime;
    u_int p_slptime;
    struct itimerval p_realtimer;
    struct timeval p_rtime;
    u_quad_t p_uticks;
    u_quad_t p_sticks;
    u_quad_t p_iticks;
    int p_traceflag;
    struct vnode *p_tracep;
    int p_siglist;
    struct vnode *p_textvp;
    int p_holdcnt;
    sigset_t p_sigmask;
    sigset_t p_sigignore;
    sigset_t p_sigcatch;
    u_char p_priority;
    u_char p_usrpri;
    char p_nice;
    char p_comm[17];
    struct pgrp *p_pgrp;
    struct user *p_addr;
    u_short p_xstat;
    u_short p_acflag;
    struct rusage *p_ru;
};

// ctlname: 0x10 (16) bytes
struct ctlname
{
    char *ctl_name;
    int ctl_type;
};

// _pcred: 0x68 (104) bytes
struct _pcred
{
    char pc_lock[72];
    struct ucred *pc_ucred;
    uid_t p_ruid;
    uid_t p_svuid;
    gid_t p_rgid;
    gid_t p_svgid;
    int p_refcnt;
};

// _ucred: 0x4C (76) bytes
struct _ucred
{
    int cr_ref;
    uid_t cr_uid;
    short cr_ngroups;
    gid_t cr_groups[16];
};

// eproc: 0x160 (352) bytes
struct eproc
{
    struct proc *e_paddr;
    struct session *e_sess;
    struct _pcred e_pcred;
    struct _ucred e_ucred;
    struct vmspace e_vm;
    pid_t e_ppid;
    pid_t e_pgid;
    short e_jobc;
    dev_t e_tdev;
    pid_t e_tpgid;
    struct session *e_tsess;
    char e_wmesg[8];
    segsz_t e_xsize;
    short e_xrssize;
    short e_xccount;
    short e_xswrss;
    int e_flag;
    char e_login[12];
    int e_spare[4];
};

// kinfo_proc: 0x288 (648) bytes
struct kinfo_proc
{
    struct extern_proc kp_proc;
    struct eproc kp_eproc;
};

// xsw_usage: 0x20 (32) bytes
struct xsw_usage
{
    u_int64_t xsu_total;
    u_int64_t xsu_avail;
    u_int64_t xsu_used;
    u_int32_t xsu_pagesize;
    boolean_t xsu_encrypted;
};

// loadavg: 0x18 (24) bytes
struct loadavg
{
    fixpt_t ldavg[3];
    long fscale;
};

#pragma neverd_header("sys/sem.h")
// ipc_perm: 0x18 (24) bytes, packed to 4
#pragma pack(push, 4)
struct ipc_perm
{
    uid_t uid;
    gid_t gid;
    uid_t cuid;
    gid_t cgid;
    mode_t mode;
    unsigned short _seq;
    key_t _key;
};
#pragma pack(pop)

// __semid_ds_new: 0x48 (72) bytes, packed to 4
#pragma pack(push, 4)
struct __semid_ds_new
{
    struct ipc_perm sem_perm;
    __int32_t sem_base;
    unsigned short sem_nsems;
    time_t sem_otime;
    __int32_t sem_pad1;
    time_t sem_ctime;
    __int32_t sem_pad2;
    __int32_t sem_pad3[4];
};
#pragma pack(pop)

// sem: 0xC (12) bytes
struct sem
{
    unsigned short semval;
    pid_t sempid;
    unsigned short semncnt;
    unsigned short semzcnt;
};

// sembuf: 0x6 (6) bytes
struct sembuf
{
    unsigned short sem_num;
    short sem_op;
    short sem_flg;
};

// semun: 0x8 (8) bytes
union semun
{
    int val;
    struct __semid_ds_new *buf;
    unsigned short *array;
};

typedef union semun semun_t;

#pragma neverd_header("sys/shm.h")
typedef unsigned short shmatt_t;

// __shmid_ds_new: 0x4C (76) bytes, packed to 4
#pragma pack(push, 4)
struct __shmid_ds_new
{
    struct ipc_perm shm_perm;
    size_t shm_segsz;
    pid_t shm_lpid;
    pid_t shm_cpid;
    shmatt_t shm_nattch;
    time_t shm_atime;
    time_t shm_dtime;
    time_t shm_ctime;
    void *shm_internal;
};
#pragma pack(pop)

#pragma neverd_header("sys/msg.h")
typedef unsigned long msgqnum_t;

typedef unsigned long msglen_t;

// __msqid_ds_new: 0x74 (116) bytes, packed to 4
#pragma pack(push, 4)
struct __msqid_ds_new
{
    struct ipc_perm msg_perm;
    __int32_t msg_first;
    __int32_t msg_last;
    msglen_t msg_cbytes;
    msgqnum_t msg_qnum;
    msglen_t msg_qbytes;
    pid_t msg_lspid;
    pid_t msg_lrpid;
    time_t msg_stime;
    __int32_t msg_pad1;
    time_t msg_rtime;
    __int32_t msg_pad2;
    time_t msg_ctime;
    __int32_t msg_pad3;
    __int32_t msg_pad4[4];
};
#pragma pack(pop)

// msg: 0x20 (32) bytes
struct msg
{
    struct msg *msg_next;
    long msg_type;
    unsigned short msg_ts;
    short msg_spot;
    struct label *label;
};

// mymsg: 0x10 (16) bytes
struct mymsg
{
    long mtype;
    char mtext[1];
};

// msginfo: 0x18 (24) bytes
struct msginfo
{
    int msgmax;
    int msgmni;
    int msgmnb;
    int msgtql;
    int msgssz;
    int msgseg;
};

#pragma neverd_header("sys/mount.h")
typedef u_int32_t text_encoding_t;

typedef u_int32_t fsobj_type_t;

typedef u_int32_t fsobj_tag_t;

typedef u_int32_t fsfile_type_t;

typedef u_int32_t fsvolid_t;

// fsobj_id: 0x8 (8) bytes
struct fsobj_id
{
    u_int32_t fid_objno;
    u_int32_t fid_generation;
};

typedef struct fsobj_id fsobj_id_t;

// attribute_set: 0x14 (20) bytes
struct attribute_set
{
    attrgroup_t commonattr;
    attrgroup_t volattr;
    attrgroup_t dirattr;
    attrgroup_t fileattr;
    attrgroup_t forkattr;
};

typedef struct attribute_set attribute_set_t;

// attrreference: 0x8 (8) bytes
struct attrreference
{
    int attr_dataoffset;
    u_int32_t attr_length;
};

typedef struct attrreference attrreference_t;

// diskextent: 0x8 (8) bytes
struct diskextent
{
    u_int32_t startblock;
    u_int32_t blockcount;
};

typedef struct diskextent extentrecord[8];

typedef u_int32_t vol_capabilities_set_t[4];

// vol_capabilities_attr: 0x20 (32) bytes
struct vol_capabilities_attr
{
    vol_capabilities_set_t capabilities;
    vol_capabilities_set_t valid;
};

typedef struct vol_capabilities_attr vol_capabilities_attr_t;

// vol_attributes_attr: 0x28 (40) bytes
struct vol_attributes_attr
{
    attribute_set_t validattr;
    attribute_set_t nativeattr;
};

typedef struct vol_attributes_attr vol_attributes_attr_t;

typedef void (*os_function_t)(void *);

// fsid: 0x8 (8) bytes
struct fsid
{
    int val[2];
};

typedef struct fsid fsid_t;

// secure_boot_cryptex_args: 0x1C (28) bytes, packed to 1
#pragma pack(push, 1)
struct DECLSPEC_ALIGN(4) secure_boot_cryptex_args
{
    u_int32_t sbc_version;
    u_int32_t sbc_4cc;
    int sbc_authentic_manifest_fd;
    int sbc_user_manifest_fd;
    int sbc_payload_fd;
    u_int64_t sbc_flags;
};
#pragma pack(pop)

typedef struct secure_boot_cryptex_args secure_boot_cryptex_args_t;

// graft_args: 0x200 (512) bytes
union graft_args
{
    u_int8_t max_size[512];
    secure_boot_cryptex_args_t sbc_args;
};

typedef union graft_args graftdmg_args_un;

#pragma neverd_header()
// Declared only: its members are unknown.
struct mount;

#pragma neverd_header("sys/mount.h")
typedef struct mount *mount_t;

typedef struct vnode *vnode_t;

// statfs: 0x878 (2168) bytes
struct statfs
{
    unsigned int f_bsize;
    int f_iosize;
    unsigned long long f_blocks;
    unsigned long long f_bfree;
    unsigned long long f_bavail;
    unsigned long long f_files;
    unsigned long long f_ffree;
    fsid_t f_fsid;
    uid_t f_owner;
    unsigned int f_type;
    unsigned int f_flags;
    unsigned int f_fssubtype;
    char f_fstypename[16];
    char f_mntonname[1024];
    char f_mntfromname[1024];
    unsigned int f_flags_ext;
    unsigned int f_reserved[7];
};

// vfsstatfs: 0x874 (2164) bytes, packed to 4
#pragma pack(push, 4)
struct vfsstatfs
{
    unsigned int f_bsize;
    size_t f_iosize;
    unsigned long long f_blocks;
    unsigned long long f_bfree;
    unsigned long long f_bavail;
    unsigned long long f_bused;
    unsigned long long f_files;
    unsigned long long f_ffree;
    fsid_t f_fsid;
    uid_t f_owner;
    unsigned long long f_flags;
    char f_fstypename[16];
    char f_mntonname[1024];
    char f_mntfromname[1024];
    unsigned int f_fssubtype;
    void *f_reserved[2];
};
#pragma pack(pop)

// vfsconf: 0x28 (40) bytes
struct vfsconf
{
    unsigned int vfc_reserved1;
    char vfc_name[15];
    int vfc_typenum;
    int vfc_refcount;
    int vfc_flags;
    unsigned int vfc_reserved2;
    unsigned int vfc_reserved3;
};

// vfsidctl: 0x50 (80) bytes
struct vfsidctl
{
    int vc_vers;
    fsid_t vc_fsid;
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

// vfs_server: 0x304 (772) bytes
struct vfs_server
{
    int vs_minutes;
    u_int8_t vs_server_name[768];
};

// netfs_status: 0x210 (528) bytes
struct netfs_status
{
    u_int32_t ns_status;
    char ns_mountopts[512];
    unsigned int ns_waittime;
    unsigned int ns_threadcount;
    unsigned long long ns_threadids[0];
};

// fhandle: 0x84 (132) bytes
struct fhandle
{
    unsigned int fh_len;
    unsigned char fh_data[128];
};

typedef struct fhandle fhandle_t;

#pragma neverd_header("netinet/tcp.h")
typedef __uint32_t tcp_seq;

typedef __uint32_t tcp_cc;

// tcphdr: 0x14 (20) bytes
struct NEVERD_GNU_STRUCT tcphdr
{
    unsigned short th_sport;
    unsigned short th_dport;
    tcp_seq th_seq;
    tcp_seq th_ack;
    unsigned int th_x2 : 4;
    unsigned int th_off : 4;
    unsigned char th_flags;
    unsigned short th_win;
    unsigned short th_sum;
    unsigned short th_urp;
};

// tcp_connection_info: 0x70 (112) bytes
struct NEVERD_GNU_STRUCT tcp_connection_info
{
    u_int8_t tcpi_state;
    u_int8_t tcpi_snd_wscale;
    u_int8_t tcpi_rcv_wscale;
    u_int8_t __pad1;
    u_int32_t tcpi_options;
    u_int32_t tcpi_flags;
    u_int32_t tcpi_rto;
    u_int32_t tcpi_maxseg;
    u_int32_t tcpi_snd_ssthresh;
    u_int32_t tcpi_snd_cwnd;
    u_int32_t tcpi_snd_wnd;
    u_int32_t tcpi_snd_sbbytes;
    u_int32_t tcpi_rcv_wnd;
    u_int32_t tcpi_rttcur;
    u_int32_t tcpi_srtt;
    u_int32_t tcpi_rttvar;
    u_int32_t tcpi_tfo_cookie_req : 1;
    u_int32_t tcpi_tfo_cookie_rcv : 1;
    u_int32_t tcpi_tfo_syn_loss : 1;
    u_int32_t tcpi_tfo_syn_data_sent : 1;
    u_int32_t tcpi_tfo_syn_data_acked : 1;
    u_int32_t tcpi_tfo_syn_data_rcv : 1;
    u_int32_t tcpi_tfo_cookie_req_rcv : 1;
    u_int32_t tcpi_tfo_cookie_sent : 1;
    u_int32_t tcpi_tfo_cookie_invalid : 1;
    u_int32_t tcpi_tfo_cookie_wrong : 1;
    u_int32_t tcpi_tfo_no_cookie_rcv : 1;
    u_int32_t tcpi_tfo_heuristics_disable : 1;
    u_int32_t tcpi_tfo_send_blackhole : 1;
    u_int32_t tcpi_tfo_recv_blackhole : 1;
    u_int32_t tcpi_tfo_onebyte_proxy : 1;
    u_int32_t __pad2 : 17;
    u_int64_t tcpi_txpackets;
    u_int64_t tcpi_txbytes;
    u_int64_t tcpi_txretransmitbytes;
    u_int64_t tcpi_rxpackets;
    u_int64_t tcpi_rxbytes;
    u_int64_t tcpi_rxoutoforderbytes;
    u_int64_t tcpi_txretransmitpackets;
};

#pragma neverd_header("aio.h")
// aiocb: 0x50 (80) bytes
struct aiocb
{
    int aio_fildes;
    off_t aio_offset;
    volatile void *aio_buf;
    size_t aio_nbytes;
    int aio_reqprio;
    struct sigevent aio_sigevent;
    int aio_lio_opcode;
};

#pragma neverd_header("malloc/malloc.h")
// vm_statistics: 0x3C (60) bytes
struct vm_statistics
{
    natural_t free_count;
    natural_t active_count;
    natural_t inactive_count;
    natural_t wire_count;
    natural_t zero_fill_count;
    natural_t reactivations;
    natural_t pageins;
    natural_t pageouts;
    natural_t faults;
    natural_t cow_faults;
    natural_t lookups;
    natural_t hits;
    natural_t purgeable_count;
    natural_t purges;
    natural_t speculative_count;
};

typedef struct vm_statistics *vm_statistics_t;

typedef struct vm_statistics vm_statistics_data_t;

// vm_statistics64: 0xF8 (248) bytes
struct vm_statistics64
{
    natural_t free_count;
    natural_t active_count;
    natural_t inactive_count;
    natural_t wire_count;
    unsigned long long zero_fill_count;
    unsigned long long reactivations;
    unsigned long long pageins;
    unsigned long long pageouts;
    unsigned long long faults;
    unsigned long long cow_faults;
    unsigned long long lookups;
    unsigned long long hits;
    unsigned long long purges;
    natural_t purgeable_count;
    natural_t speculative_count;
    unsigned long long decompressions;
    unsigned long long compressions;
    unsigned long long swapins;
    unsigned long long swapouts;
    natural_t compressor_page_count;
    natural_t throttled_count;
    natural_t external_page_count;
    natural_t internal_page_count;
    unsigned long long total_uncompressed_pages_in_compressor;
    unsigned long long swapped_count;
    unsigned long long total_tag_storage_pages;
    unsigned long long nontag_pageable_tag_storage_pages;
    unsigned long long nontag_wired_tag_storage_pages;
    unsigned long long free_tag_storage_pages;
    unsigned long long tag_storing_tag_storage_pages;
    unsigned long long total_tagged_pages;
    unsigned long long resident_tagged_pages;
    unsigned long long compressed_tagged_pages;
    unsigned long long tagged_compressions;
    unsigned long long tagged_decompressions;
    unsigned long long compressed_tag_storage_bytes;
};

typedef struct vm_statistics64 *vm_statistics64_t;

typedef struct vm_statistics64 vm_statistics64_data_t;

// vm_extmod_statistics: 0x30 (48) bytes
struct vm_extmod_statistics
{
    long long task_for_pid_count;
    long long task_for_pid_caller_count;
    long long thread_creation_count;
    long long thread_creation_caller_count;
    long long thread_set_state_count;
    long long thread_set_state_caller_count;
};

typedef struct vm_extmod_statistics *vm_extmod_statistics_t;

typedef struct vm_extmod_statistics vm_extmod_statistics_data_t;

// vm_purgeable_stat: 0x10 (16) bytes
struct vm_purgeable_stat
{
    unsigned long long count;
    unsigned long long size;
};

typedef struct vm_purgeable_stat vm_purgeable_stat_t;

// vm_purgeable_info: 0x110 (272) bytes
struct vm_purgeable_info
{
    vm_purgeable_stat_t fifo_data[8];
    vm_purgeable_stat_t obsolete_data;
    vm_purgeable_stat_t lifo_data[8];
};

typedef struct vm_purgeable_info *vm_purgeable_info_t;

typedef int vm_page_disposition_t;

// time_value: 0x8 (8) bytes
struct time_value
{
    integer_t seconds;
    integer_t microseconds;
};

typedef struct time_value time_value_t;

typedef integer_t *host_info_t;

typedef integer_t *host_info64_t;

typedef integer_t host_info_data_t[1024];

typedef char kernel_version_t[512];

typedef char kernel_boot_info_t[4096];

typedef integer_t host_flavor_t;

// host_can_has_debugger_info: 0x4 (4) bytes
struct host_can_has_debugger_info
{
    boolean_t can_has_debugger;
};

typedef struct host_can_has_debugger_info host_can_has_debugger_info_data_t;

typedef struct host_can_has_debugger_info *host_can_has_debugger_info_t;

// host_basic_info: 0x30 (48) bytes, packed to 4
#pragma pack(push, 4)
struct host_basic_info
{
    integer_t max_cpus;
    integer_t avail_cpus;
    natural_t memory_size;
    cpu_type_t cpu_type;
    cpu_subtype_t cpu_subtype;
    cpu_threadtype_t cpu_threadtype;
    integer_t physical_cpu;
    integer_t physical_cpu_max;
    integer_t logical_cpu;
    integer_t logical_cpu_max;
    unsigned long long max_mem;
};
#pragma pack(pop)

typedef struct host_basic_info host_basic_info_data_t;

typedef struct host_basic_info *host_basic_info_t;

// host_sched_info: 0x8 (8) bytes
struct host_sched_info
{
    integer_t min_timeout;
    integer_t min_quantum;
};

typedef struct host_sched_info host_sched_info_data_t;

typedef struct host_sched_info *host_sched_info_t;

// kernel_resource_sizes: 0x14 (20) bytes
struct kernel_resource_sizes
{
    natural_t task;
    natural_t thread;
    natural_t port;
    natural_t memory_region;
    natural_t memory_object;
};

typedef struct kernel_resource_sizes kernel_resource_sizes_data_t;

typedef struct kernel_resource_sizes *kernel_resource_sizes_t;

// host_priority_info: 0x20 (32) bytes
struct host_priority_info
{
    integer_t kernel_priority;
    integer_t system_priority;
    integer_t server_priority;
    integer_t user_priority;
    integer_t depress_priority;
    integer_t idle_priority;
    integer_t minimum_priority;
    integer_t maximum_priority;
};

typedef struct host_priority_info host_priority_info_data_t;

typedef struct host_priority_info *host_priority_info_t;

// host_load_info: 0x18 (24) bytes
struct host_load_info
{
    integer_t avenrun[3];
    integer_t mach_factor[3];
};

typedef struct host_load_info host_load_info_data_t;

typedef struct host_load_info *host_load_info_t;

typedef struct vm_purgeable_info host_purgable_info_data_t;

typedef struct vm_purgeable_info *host_purgable_info_t;

// host_cpu_load_info: 0x10 (16) bytes
struct host_cpu_load_info
{
    natural_t cpu_ticks[4];
};

typedef struct host_cpu_load_info host_cpu_load_info_data_t;

typedef struct host_cpu_load_info *host_cpu_load_info_t;

// host_preferred_user_arch: 0x8 (8) bytes
struct host_preferred_user_arch
{
    cpu_type_t cpu_type;
    cpu_subtype_t cpu_subtype;
};

typedef struct host_preferred_user_arch host_preferred_user_arch_data_t;

typedef struct host_preferred_user_arch *host_preferred_user_arch_t;

typedef int vm_prot_t;

typedef unsigned int vm_sync_t;

typedef unsigned long long memory_object_offset_t;

typedef unsigned long long memory_object_size_t;

typedef natural_t memory_object_cluster_size_t;

typedef natural_t *memory_object_fault_info_t;

typedef unsigned long long vm_object_id_t;

typedef mach_port_t memory_object_t;

typedef mach_port_t memory_object_control_t;

typedef memory_object_t *memory_object_array_t;

typedef mach_port_t memory_object_name_t;

typedef mach_port_t memory_object_default_t;

typedef int memory_object_copy_strategy_t;

typedef int memory_object_return_t;

typedef int *memory_object_info_t;

typedef int memory_object_flavor_t;

typedef int memory_object_info_data_t[1024];

// memory_object_perf_info: 0x8 (8) bytes
struct memory_object_perf_info
{
    memory_object_cluster_size_t cluster_size;
    boolean_t may_cache;
};

// memory_object_attr_info: 0x10 (16) bytes
struct memory_object_attr_info
{
    memory_object_copy_strategy_t copy_strategy;
    memory_object_cluster_size_t cluster_size;
    boolean_t may_cache_object;
    boolean_t temporary;
};

// memory_object_behave_info: 0x14 (20) bytes
struct memory_object_behave_info
{
    memory_object_copy_strategy_t copy_strategy;
    boolean_t temporary;
    boolean_t invalidate;
    boolean_t silent_overwrite;
    boolean_t advisory_pageout;
};

typedef struct memory_object_behave_info *memory_object_behave_info_t;

typedef struct memory_object_behave_info memory_object_behave_info_data_t;

typedef struct memory_object_perf_info *memory_object_perf_info_t;

typedef struct memory_object_perf_info memory_object_perf_info_data_t;

typedef struct memory_object_attr_info *memory_object_attr_info_t;

typedef struct memory_object_attr_info memory_object_attr_info_data_t;

typedef mach_port_t mach_voucher_t;

typedef mach_port_name_t mach_voucher_name_t;

typedef mach_voucher_name_t *mach_voucher_name_array_t;

typedef mach_voucher_t ipc_voucher_t;

typedef unsigned int mach_voucher_selector_t;

typedef unsigned int mach_voucher_attr_key_t;

typedef mach_voucher_attr_key_t *mach_voucher_attr_key_array_t;

typedef unsigned char *mach_voucher_attr_content_t;

typedef unsigned int mach_voucher_attr_content_size_t;

typedef unsigned int mach_voucher_attr_command_t;

typedef unsigned int mach_voucher_attr_recipe_command_t;

typedef mach_voucher_attr_recipe_command_t *mach_voucher_attr_recipe_command_array_t;

// mach_voucher_attr_recipe_data: 0x10 (16) bytes, packed to 1
#pragma pack(push, 1)
struct mach_voucher_attr_recipe_data
{
    mach_voucher_attr_key_t key;
    mach_voucher_attr_recipe_command_t command;
    mach_voucher_name_t previous_voucher;
    mach_voucher_attr_content_size_t content_size;
    unsigned char content[];
};
#pragma pack(pop)

typedef struct mach_voucher_attr_recipe_data mach_voucher_attr_recipe_data_t;

typedef mach_voucher_attr_recipe_data_t *mach_voucher_attr_recipe_t;

typedef mach_msg_type_number_t mach_voucher_attr_recipe_size_t;

typedef unsigned char *mach_voucher_attr_raw_recipe_t;

typedef mach_voucher_attr_raw_recipe_t mach_voucher_attr_raw_recipe_array_t;

typedef mach_msg_type_number_t mach_voucher_attr_raw_recipe_size_t;

typedef mach_msg_type_number_t mach_voucher_attr_raw_recipe_array_size_t;

typedef mach_port_t mach_voucher_attr_manager_t;

typedef mach_port_t mach_voucher_attr_control_t;

typedef mach_port_t ipc_voucher_attr_manager_t;

typedef mach_port_t ipc_voucher_attr_control_t;

typedef unsigned long long mach_voucher_attr_value_handle_t;

typedef mach_voucher_attr_value_handle_t *mach_voucher_attr_value_handle_array_t;

typedef mach_msg_type_number_t mach_voucher_attr_value_handle_array_size_t;

typedef unsigned int mach_voucher_attr_value_reference_t;

typedef unsigned int mach_voucher_attr_value_flags_t;

typedef unsigned int mach_voucher_attr_control_flags_t;

typedef unsigned int mach_voucher_attr_importance_refs;

// processor_cpu_stat: 0x24 (36) bytes
struct processor_cpu_stat
{
    unsigned int irq_ex_cnt;
    unsigned int ipi_cnt;
    unsigned int timer_cnt;
    unsigned int undef_ex_cnt;
    unsigned int unaligned_cnt;
    unsigned int vfp_cnt;
    unsigned int vfp_shortv_cnt;
    unsigned int data_ex_cnt;
    unsigned int instr_ex_cnt;
};

typedef struct processor_cpu_stat processor_cpu_stat_data_t;

typedef struct processor_cpu_stat *processor_cpu_stat_t;

// processor_cpu_stat64: 0x50 (80) bytes, packed to 1
#pragma pack(push, 1)
struct DECLSPEC_ALIGN(4) processor_cpu_stat64
{
    unsigned long long irq_ex_cnt;
    unsigned long long ipi_cnt;
    unsigned long long timer_cnt;
    unsigned long long undef_ex_cnt;
    unsigned long long unaligned_cnt;
    unsigned long long vfp_cnt;
    unsigned long long vfp_shortv_cnt;
    unsigned long long data_ex_cnt;
    unsigned long long instr_ex_cnt;
    unsigned long long pmi_cnt;
};
#pragma pack(pop)

typedef struct processor_cpu_stat64 processor_cpu_stat64_data_t;

typedef struct processor_cpu_stat64 *processor_cpu_stat64_t;

typedef integer_t *processor_info_t;

typedef integer_t *processor_info_array_t;

typedef integer_t processor_info_data_t[1024];

typedef integer_t *processor_set_info_t;

typedef integer_t processor_set_info_data_t[1024];

typedef int processor_flavor_t;

// processor_basic_info: 0x14 (20) bytes
struct processor_basic_info
{
    cpu_type_t cpu_type;
    cpu_subtype_t cpu_subtype;
    boolean_t running;
    int slot_num;
    union
    {
        boolean_t is_master;
        boolean_t is_main;
    };
};

typedef struct processor_basic_info processor_basic_info_data_t;

typedef struct processor_basic_info *processor_basic_info_t;

// processor_cpu_load_info: 0x10 (16) bytes
struct processor_cpu_load_info
{
    unsigned int cpu_ticks[4];
};

typedef struct processor_cpu_load_info processor_cpu_load_info_data_t;

typedef struct processor_cpu_load_info *processor_cpu_load_info_t;

typedef int processor_set_flavor_t;

// processor_set_basic_info: 0x8 (8) bytes
struct processor_set_basic_info
{
    int processor_count;
    int default_policy;
};

typedef struct processor_set_basic_info processor_set_basic_info_data_t;

typedef struct processor_set_basic_info *processor_set_basic_info_t;

// processor_set_load_info: 0x10 (16) bytes
struct processor_set_load_info
{
    int task_count;
    int thread_count;
    integer_t load_average;
    integer_t mach_factor;
};

typedef struct processor_set_load_info processor_set_load_info_data_t;

typedef struct processor_set_load_info *processor_set_load_info_t;

typedef int policy_t;

typedef integer_t *policy_info_t;

typedef integer_t *policy_base_t;

typedef integer_t *policy_limit_t;

// policy_timeshare_base: 0x4 (4) bytes
struct policy_timeshare_base
{
    integer_t base_priority;
};

// policy_timeshare_limit: 0x4 (4) bytes
struct policy_timeshare_limit
{
    integer_t max_priority;
};

// policy_timeshare_info: 0x14 (20) bytes
struct policy_timeshare_info
{
    integer_t max_priority;
    integer_t base_priority;
    integer_t cur_priority;
    boolean_t depressed;
    integer_t depress_priority;
};

typedef struct policy_timeshare_base *policy_timeshare_base_t;

typedef struct policy_timeshare_limit *policy_timeshare_limit_t;

typedef struct policy_timeshare_info *policy_timeshare_info_t;

typedef struct policy_timeshare_base policy_timeshare_base_data_t;

typedef struct policy_timeshare_limit policy_timeshare_limit_data_t;

typedef struct policy_timeshare_info policy_timeshare_info_data_t;

// policy_rr_base: 0x8 (8) bytes
struct policy_rr_base
{
    integer_t base_priority;
    integer_t quantum;
};

// policy_rr_limit: 0x4 (4) bytes
struct policy_rr_limit
{
    integer_t max_priority;
};

// policy_rr_info: 0x14 (20) bytes
struct policy_rr_info
{
    integer_t max_priority;
    integer_t base_priority;
    integer_t quantum;
    boolean_t depressed;
    integer_t depress_priority;
};

typedef struct policy_rr_base *policy_rr_base_t;

typedef struct policy_rr_limit *policy_rr_limit_t;

typedef struct policy_rr_info *policy_rr_info_t;

typedef struct policy_rr_base policy_rr_base_data_t;

typedef struct policy_rr_limit policy_rr_limit_data_t;

typedef struct policy_rr_info policy_rr_info_data_t;

// policy_fifo_base: 0x4 (4) bytes
struct policy_fifo_base
{
    integer_t base_priority;
};

// policy_fifo_limit: 0x4 (4) bytes
struct policy_fifo_limit
{
    integer_t max_priority;
};

// policy_fifo_info: 0x10 (16) bytes
struct policy_fifo_info
{
    integer_t max_priority;
    integer_t base_priority;
    boolean_t depressed;
    integer_t depress_priority;
};

typedef struct policy_fifo_base *policy_fifo_base_t;

typedef struct policy_fifo_limit *policy_fifo_limit_t;

typedef struct policy_fifo_info *policy_fifo_info_t;

typedef struct policy_fifo_base policy_fifo_base_data_t;

typedef struct policy_fifo_limit policy_fifo_limit_data_t;

typedef struct policy_fifo_info policy_fifo_info_data_t;

// policy_bases: 0x10 (16) bytes
struct policy_bases
{
    policy_timeshare_base_data_t ts;
    policy_rr_base_data_t rr;
    policy_fifo_base_data_t fifo;
};

// policy_limits: 0xC (12) bytes
struct policy_limits
{
    policy_timeshare_limit_data_t ts;
    policy_rr_limit_data_t rr;
    policy_fifo_limit_data_t fifo;
};

// policy_infos: 0x38 (56) bytes
struct policy_infos
{
    policy_timeshare_info_data_t ts;
    policy_rr_info_data_t rr;
    policy_fifo_info_data_t fifo;
};

typedef struct policy_bases policy_base_data_t;

typedef struct policy_limits policy_limit_data_t;

typedef struct policy_infos policy_info_data_t;

typedef natural_t task_flavor_t;

typedef integer_t *task_info_t;

typedef integer_t task_info_data_t[1024];

// task_basic_info_32: 0x20 (32) bytes, packed to 4
#pragma pack(push, 4)
struct task_basic_info_32
{
    integer_t suspend_count;
    natural_t virtual_size;
    natural_t resident_size;
    time_value_t user_time;
    time_value_t system_time;
    policy_t policy;
};
#pragma pack(pop)

typedef struct task_basic_info_32 task_basic_info_32_data_t;

typedef struct task_basic_info_32 *task_basic_info_32_t;

// task_basic_info_64: 0x28 (40) bytes, packed to 4
#pragma pack(push, 4)
struct task_basic_info_64
{
    integer_t suspend_count;
    mach_vm_size_t virtual_size;
    mach_vm_size_t resident_size;
    time_value_t user_time;
    time_value_t system_time;
    policy_t policy;
};
#pragma pack(pop)

typedef struct task_basic_info_64 task_basic_info_64_data_t;

typedef struct task_basic_info_64 *task_basic_info_64_t;

// task_basic_info: 0x28 (40) bytes, packed to 4
#pragma pack(push, 4)
struct task_basic_info
{
    integer_t suspend_count;
    vm_size_t virtual_size;
    vm_size_t resident_size;
    time_value_t user_time;
    time_value_t system_time;
    policy_t policy;
};
#pragma pack(pop)

typedef struct task_basic_info task_basic_info_data_t;

typedef struct task_basic_info *task_basic_info_t;

// task_events_info: 0x20 (32) bytes, packed to 4
#pragma pack(push, 4)
struct task_events_info
{
    integer_t faults;
    integer_t pageins;
    integer_t cow_faults;
    integer_t messages_sent;
    integer_t messages_received;
    integer_t syscalls_mach;
    integer_t syscalls_unix;
    integer_t csw;
};
#pragma pack(pop)

typedef struct task_events_info task_events_info_data_t;

typedef struct task_events_info *task_events_info_t;

// task_thread_times_info: 0x10 (16) bytes, packed to 4
#pragma pack(push, 4)
struct task_thread_times_info
{
    time_value_t user_time;
    time_value_t system_time;
};
#pragma pack(pop)

typedef struct task_thread_times_info task_thread_times_info_data_t;

typedef struct task_thread_times_info *task_thread_times_info_t;

// task_absolutetime_info: 0x20 (32) bytes, packed to 4
#pragma pack(push, 4)
struct task_absolutetime_info
{
    unsigned long long total_user;
    unsigned long long total_system;
    unsigned long long threads_user;
    unsigned long long threads_system;
};
#pragma pack(pop)

typedef struct task_absolutetime_info task_absolutetime_info_data_t;

typedef struct task_absolutetime_info *task_absolutetime_info_t;

// task_kernelmemory_info: 0x20 (32) bytes, packed to 4
#pragma pack(push, 4)
struct task_kernelmemory_info
{
    unsigned long long total_palloc;
    unsigned long long total_pfree;
    unsigned long long total_salloc;
    unsigned long long total_sfree;
};
#pragma pack(pop)

typedef struct task_kernelmemory_info task_kernelmemory_info_data_t;

typedef struct task_kernelmemory_info *task_kernelmemory_info_t;

// task_affinity_tag_info: 0x10 (16) bytes, packed to 4
#pragma pack(push, 4)
struct task_affinity_tag_info
{
    integer_t set_count;
    integer_t min;
    integer_t max;
    integer_t task_count;
};
#pragma pack(pop)

typedef struct task_affinity_tag_info task_affinity_tag_info_data_t;

typedef struct task_affinity_tag_info *task_affinity_tag_info_t;

// task_dyld_info: 0x14 (20) bytes, packed to 4
#pragma pack(push, 4)
struct task_dyld_info
{
    mach_vm_address_t all_image_info_addr;
    mach_vm_size_t all_image_info_size;
    integer_t all_image_info_format;
};
#pragma pack(pop)

typedef struct task_dyld_info task_dyld_info_data_t;

typedef struct task_dyld_info *task_dyld_info_t;

// task_basic_info_64_2: 0x28 (40) bytes, packed to 4
#pragma pack(push, 4)
struct task_basic_info_64_2
{
    integer_t suspend_count;
    mach_vm_size_t virtual_size;
    mach_vm_size_t resident_size;
    time_value_t user_time;
    time_value_t system_time;
    policy_t policy;
};
#pragma pack(pop)

typedef struct task_basic_info_64_2 task_basic_info_64_2_data_t;

typedef struct task_basic_info_64_2 *task_basic_info_64_2_t;

// task_extmod_info: 0x40 (64) bytes, packed to 4
#pragma pack(push, 4)
struct task_extmod_info
{
    unsigned char task_uuid[16];
    vm_extmod_statistics_data_t extmod_statistics;
};
#pragma pack(pop)

typedef struct task_extmod_info task_extmod_info_data_t;

typedef struct task_extmod_info *task_extmod_info_t;

// mach_task_basic_info: 0x30 (48) bytes, packed to 4
#pragma pack(push, 4)
struct mach_task_basic_info
{
    mach_vm_size_t virtual_size;
    mach_vm_size_t resident_size;
    mach_vm_size_t resident_size_max;
    time_value_t user_time;
    time_value_t system_time;
    policy_t policy;
    integer_t suspend_count;
};
#pragma pack(pop)

typedef struct mach_task_basic_info mach_task_basic_info_data_t;

typedef struct mach_task_basic_info *mach_task_basic_info_t;

// task_power_info: 0x30 (48) bytes, packed to 4
#pragma pack(push, 4)
struct task_power_info
{
    unsigned long long total_user;
    unsigned long long total_system;
    unsigned long long task_interrupt_wakeups;
    unsigned long long task_platform_idle_wakeups;
    unsigned long long task_timer_wakeups_bin_1;
    unsigned long long task_timer_wakeups_bin_2;
};
#pragma pack(pop)

typedef struct task_power_info task_power_info_data_t;

typedef struct task_power_info *task_power_info_t;

// task_vm_info: 0x174 (372) bytes, packed to 4
#pragma pack(push, 4)
struct task_vm_info
{
    mach_vm_size_t virtual_size;
    integer_t region_count;
    integer_t page_size;
    mach_vm_size_t resident_size;
    mach_vm_size_t resident_size_peak;
    mach_vm_size_t device;
    mach_vm_size_t device_peak;
    mach_vm_size_t internal;
    mach_vm_size_t internal_peak;
    mach_vm_size_t external;
    mach_vm_size_t external_peak;
    mach_vm_size_t reusable;
    mach_vm_size_t reusable_peak;
    mach_vm_size_t purgeable_volatile_pmap;
    mach_vm_size_t purgeable_volatile_resident;
    mach_vm_size_t purgeable_volatile_virtual;
    mach_vm_size_t compressed;
    mach_vm_size_t compressed_peak;
    mach_vm_size_t compressed_lifetime;
    mach_vm_size_t phys_footprint;
    mach_vm_address_t min_address;
    mach_vm_address_t max_address;
    long long ledger_phys_footprint_peak;
    long long ledger_purgeable_nonvolatile;
    long long ledger_purgeable_novolatile_compressed;
    long long ledger_purgeable_volatile;
    long long ledger_purgeable_volatile_compressed;
    long long ledger_tag_network_nonvolatile;
    long long ledger_tag_network_nonvolatile_compressed;
    long long ledger_tag_network_volatile;
    long long ledger_tag_network_volatile_compressed;
    long long ledger_tag_media_footprint;
    long long ledger_tag_media_footprint_compressed;
    long long ledger_tag_media_nofootprint;
    long long ledger_tag_media_nofootprint_compressed;
    long long ledger_tag_graphics_footprint;
    long long ledger_tag_graphics_footprint_compressed;
    long long ledger_tag_graphics_nofootprint;
    long long ledger_tag_graphics_nofootprint_compressed;
    long long ledger_tag_neural_footprint;
    long long ledger_tag_neural_footprint_compressed;
    long long ledger_tag_neural_nofootprint;
    long long ledger_tag_neural_nofootprint_compressed;
    unsigned long long limit_bytes_remaining;
    integer_t decompressions;
    long long ledger_swapins;
    long long ledger_tag_neural_nofootprint_total;
    long long ledger_tag_neural_nofootprint_peak;
};
#pragma pack(pop)

typedef struct task_vm_info task_vm_info_data_t;

typedef struct task_vm_info *task_vm_info_t;

typedef struct vm_purgeable_info task_purgable_info_t;

// task_trace_memory_info: 0x18 (24) bytes, packed to 4
#pragma pack(push, 4)
struct task_trace_memory_info
{
    unsigned long long user_memory_address;
    unsigned long long buffer_size;
    unsigned long long mailbox_array_size;
};
#pragma pack(pop)

typedef struct task_trace_memory_info task_trace_memory_info_data_t;

typedef struct task_trace_memory_info *task_trace_memory_info_t;

// task_wait_state_info: 0x20 (32) bytes, packed to 4
#pragma pack(push, 4)
struct task_wait_state_info
{
    unsigned long long total_wait_state_time;
    unsigned long long total_wait_sfi_state_time;
    unsigned int _reserved[4];
};
#pragma pack(pop)

typedef struct task_wait_state_info task_wait_state_info_data_t;

typedef struct task_wait_state_info *task_wait_state_info_t;

// gpu_energy_data: 0x20 (32) bytes, packed to 4
#pragma pack(push, 4)
struct gpu_energy_data
{
    unsigned long long task_gpu_utilisation;
    unsigned long long task_gpu_stat_reserved0;
    unsigned long long task_gpu_stat_reserved1;
    unsigned long long task_gpu_stat_reserved2;
};
#pragma pack(pop)

typedef gpu_energy_data *gpu_energy_data_t;

// task_power_info_v2: 0x68 (104) bytes, packed to 4
#pragma pack(push, 4)
struct task_power_info_v2
{
    task_power_info_data_t cpu_energy;
    gpu_energy_data gpu_energy;
    unsigned long long task_energy;
    unsigned long long task_ptime;
    unsigned long long task_pset_switches;
};
#pragma pack(pop)

typedef struct task_power_info_v2 task_power_info_v2_data_t;

typedef struct task_power_info_v2 *task_power_info_v2_t;

// task_flags_info: 0x4 (4) bytes, packed to 4
#pragma pack(push, 4)
struct task_flags_info
{
    unsigned int flags;
};
#pragma pack(pop)

typedef struct task_flags_info task_flags_info_data_t;

typedef struct task_flags_info *task_flags_info_t;

// task_security_config_info: 0x4 (4) bytes, packed to 4
#pragma pack(push, 4)
struct task_security_config_info
{
    unsigned int config;
};
#pragma pack(pop)

typedef struct task_security_config_info *task_security_config_info_t;

// task_ipc_space_policy_info: 0x4 (4) bytes, packed to 4
#pragma pack(push, 4)
struct task_ipc_space_policy_info
{
    unsigned int space_policy;
};
#pragma pack(pop)

typedef struct task_ipc_space_policy_info *task_ipc_space_policy_info_t;

typedef unsigned int task_exc_guard_behavior_t;

typedef unsigned int task_corpse_forking_behavior_t;

typedef natural_t task_inspect_flavor_t;

// task_inspect_basic_counts: 0x10 (16) bytes
struct task_inspect_basic_counts
{
    unsigned long long instructions;
    unsigned long long cycles;
};

typedef struct task_inspect_basic_counts task_inspect_basic_counts_data_t;

typedef struct task_inspect_basic_counts *task_inspect_basic_counts_t;

typedef integer_t *task_inspect_info_t;

typedef natural_t task_policy_flavor_t;

typedef integer_t *task_policy_t;

typedef enum task_role task_role_t;

// task_category_policy: 0x4 (4) bytes
struct task_category_policy
{
    task_role_t role;
};

typedef struct task_category_policy task_category_policy_data_t;

typedef struct task_category_policy *task_category_policy_t;

typedef integer_t task_latency_qos_t;

typedef integer_t task_throughput_qos_t;

// task_qos_policy: 0x8 (8) bytes
struct task_qos_policy
{
    task_latency_qos_t task_latency_qos_tier;
    task_throughput_qos_t task_throughput_qos_tier;
};

typedef struct task_qos_policy *task_qos_policy_t;

typedef int task_special_port_t;

typedef natural_t thread_flavor_t;

typedef integer_t *thread_info_t;

typedef integer_t thread_info_data_t[32];

// thread_basic_info: 0x28 (40) bytes
struct thread_basic_info
{
    time_value_t user_time;
    time_value_t system_time;
    integer_t cpu_usage;
    policy_t policy;
    integer_t run_state;
    integer_t flags;
    integer_t suspend_count;
    integer_t sleep_time;
};

typedef struct thread_basic_info thread_basic_info_data_t;

typedef struct thread_basic_info *thread_basic_info_t;

// thread_identifier_info: 0x18 (24) bytes
struct thread_identifier_info
{
    unsigned long long thread_id;
    unsigned long long thread_handle;
    unsigned long long dispatch_qaddr;
};

typedef struct thread_identifier_info thread_identifier_info_data_t;

typedef struct thread_identifier_info *thread_identifier_info_t;

// thread_extended_info: 0x70 (112) bytes
struct thread_extended_info
{
    unsigned long long pth_user_time;
    unsigned long long pth_system_time;
    int pth_cpu_usage;
    int pth_policy;
    int pth_run_state;
    int pth_flags;
    int pth_sleep_time;
    int pth_curpri;
    int pth_priority;
    int pth_maxpriority;
    char pth_name[64];
};

typedef struct thread_extended_info thread_extended_info_data_t;

typedef struct thread_extended_info *thread_extended_info_t;

// io_stat_entry: 0x10 (16) bytes
struct io_stat_entry
{
    unsigned long long count;
    unsigned long long size;
};

// io_stat_info: 0x80 (128) bytes
struct io_stat_info
{
    struct io_stat_entry disk_reads;
    struct io_stat_entry io_priority[4];
    struct io_stat_entry paging;
    struct io_stat_entry metadata;
    struct io_stat_entry total_io;
};

typedef struct io_stat_info *io_stat_info_t;

typedef natural_t thread_policy_flavor_t;

typedef integer_t *thread_policy_t;

// thread_standard_policy: 0x4 (4) bytes
struct thread_standard_policy
{
    natural_t no_data;
};

typedef struct thread_standard_policy thread_standard_policy_data_t;

typedef struct thread_standard_policy *thread_standard_policy_t;

// thread_extended_policy: 0x4 (4) bytes
struct thread_extended_policy
{
    boolean_t timeshare;
};

typedef struct thread_extended_policy thread_extended_policy_data_t;

typedef struct thread_extended_policy *thread_extended_policy_t;

// thread_time_constraint_policy: 0x10 (16) bytes
struct thread_time_constraint_policy
{
    unsigned int period;
    unsigned int computation;
    unsigned int constraint;
    boolean_t preemptible;
};

typedef struct thread_time_constraint_policy thread_time_constraint_policy_data_t;

typedef struct thread_time_constraint_policy *thread_time_constraint_policy_t;

// thread_precedence_policy: 0x4 (4) bytes
struct thread_precedence_policy
{
    integer_t importance;
};

typedef struct thread_precedence_policy thread_precedence_policy_data_t;

typedef struct thread_precedence_policy *thread_precedence_policy_t;

// thread_affinity_policy: 0x4 (4) bytes
struct thread_affinity_policy
{
    integer_t affinity_tag;
};

typedef struct thread_affinity_policy thread_affinity_policy_data_t;

typedef struct thread_affinity_policy *thread_affinity_policy_t;

// thread_background_policy: 0x4 (4) bytes
struct thread_background_policy
{
    integer_t priority;
};

typedef struct thread_background_policy thread_background_policy_data_t;

typedef struct thread_background_policy *thread_background_policy_t;

typedef integer_t thread_latency_qos_t;

// thread_latency_qos_policy: 0x4 (4) bytes
struct thread_latency_qos_policy
{
    thread_latency_qos_t thread_latency_qos_tier;
};

typedef struct thread_latency_qos_policy thread_latency_qos_policy_data_t;

typedef struct thread_latency_qos_policy *thread_latency_qos_policy_t;

typedef integer_t thread_throughput_qos_t;

// thread_throughput_qos_policy: 0x4 (4) bytes
struct thread_throughput_qos_policy
{
    thread_throughput_qos_t thread_throughput_qos_tier;
};

typedef struct thread_throughput_qos_policy thread_throughput_qos_policy_data_t;

typedef struct thread_throughput_qos_policy *thread_throughput_qos_policy_t;

typedef int alarm_type_t;

typedef int sleep_type_t;

typedef int clock_id_t;

typedef int clock_flavor_t;

typedef int *clock_attr_t;

typedef int clock_res_t;

// mach_timespec: 0x8 (8) bytes
struct mach_timespec
{
    unsigned int tv_sec;
    clock_res_t tv_nsec;
};

typedef struct mach_timespec mach_timespec_t;

typedef unsigned int vm_machine_attribute_t;

typedef int vm_machine_attribute_val_t;

typedef unsigned int vm_inherit_t;

typedef int vm_purgable_t;

typedef int vm_behavior_t;

typedef unsigned int vm32_object_id_t;

typedef int *vm_region_info_t;

typedef int *vm_region_info_64_t;

typedef int *vm_region_recurse_info_t;

typedef int *vm_region_recurse_info_64_t;

typedef int vm_region_flavor_t;

typedef int vm_region_info_data_t[1024];

// vm_region_basic_info_64: 0x24 (36) bytes, packed to 4
#pragma pack(push, 4)
struct vm_region_basic_info_64
{
    vm_prot_t protection;
    vm_prot_t max_protection;
    vm_inherit_t inheritance;
    boolean_t shared;
    boolean_t reserved;
    memory_object_offset_t offset;
    vm_behavior_t behavior;
    unsigned short user_wired_count;
};
#pragma pack(pop)

typedef struct vm_region_basic_info_64 *vm_region_basic_info_64_t;

typedef struct vm_region_basic_info_64 vm_region_basic_info_data_64_t;

// vm_region_basic_info: 0x20 (32) bytes, packed to 4
#pragma pack(push, 4)
struct vm_region_basic_info
{
    vm_prot_t protection;
    vm_prot_t max_protection;
    vm_inherit_t inheritance;
    boolean_t shared;
    boolean_t reserved;
    unsigned int offset;
    vm_behavior_t behavior;
    unsigned short user_wired_count;
};
#pragma pack(pop)

typedef struct vm_region_basic_info *vm_region_basic_info_t;

typedef struct vm_region_basic_info vm_region_basic_info_data_t;

// vm_region_extended_info: 0x24 (36) bytes, packed to 4
#pragma pack(push, 4)
struct vm_region_extended_info
{
    vm_prot_t protection;
    unsigned int user_tag;
    unsigned int pages_resident;
    unsigned int pages_shared_now_private;
    unsigned int pages_swapped_out;
    unsigned int pages_dirtied;
    unsigned int ref_count;
    unsigned short shadow_depth;
    unsigned char external_pager;
    unsigned char share_mode;
    unsigned int pages_reusable;
};
#pragma pack(pop)

typedef struct vm_region_extended_info *vm_region_extended_info_t;

typedef struct vm_region_extended_info vm_region_extended_info_data_t;

// vm_region_top_info: 0x14 (20) bytes, packed to 4
#pragma pack(push, 4)
struct vm_region_top_info
{
    unsigned int obj_id;
    unsigned int ref_count;
    unsigned int private_pages_resident;
    unsigned int shared_pages_resident;
    unsigned char share_mode;
};
#pragma pack(pop)

typedef struct vm_region_top_info *vm_region_top_info_t;

typedef struct vm_region_top_info vm_region_top_info_data_t;

// vm_region_submap_info: 0x3C (60) bytes, packed to 4
#pragma pack(push, 4)
struct vm_region_submap_info
{
    vm_prot_t protection;
    vm_prot_t max_protection;
    vm_inherit_t inheritance;
    unsigned int offset;
    unsigned int user_tag;
    unsigned int pages_resident;
    unsigned int pages_shared_now_private;
    unsigned int pages_swapped_out;
    unsigned int pages_dirtied;
    unsigned int ref_count;
    unsigned short shadow_depth;
    unsigned char external_pager;
    unsigned char share_mode;
    boolean_t is_submap;
    vm_behavior_t behavior;
    vm32_object_id_t object_id;
    unsigned short user_wired_count;
};
#pragma pack(pop)

typedef struct vm_region_submap_info *vm_region_submap_info_t;

typedef struct vm_region_submap_info vm_region_submap_info_data_t;

// vm_region_submap_info_64: 0x4C (76) bytes, packed to 4
#pragma pack(push, 4)
struct vm_region_submap_info_64
{
    vm_prot_t protection;
    vm_prot_t max_protection;
    vm_inherit_t inheritance;
    memory_object_offset_t offset;
    unsigned int user_tag;
    unsigned int pages_resident;
    unsigned int pages_shared_now_private;
    unsigned int pages_swapped_out;
    unsigned int pages_dirtied;
    unsigned int ref_count;
    unsigned short shadow_depth;
    unsigned char external_pager;
    unsigned char share_mode;
    boolean_t is_submap;
    vm_behavior_t behavior;
    vm32_object_id_t object_id;
    unsigned short user_wired_count;
    unsigned short flags;
    unsigned int pages_reusable;
    vm_object_id_t object_id_full;
};
#pragma pack(pop)

typedef struct vm_region_submap_info_64 *vm_region_submap_info_64_t;

typedef struct vm_region_submap_info_64 vm_region_submap_info_data_64_t;

// vm_region_submap_short_info_64: 0x30 (48) bytes, packed to 4
#pragma pack(push, 4)
struct vm_region_submap_short_info_64
{
    vm_prot_t protection;
    vm_prot_t max_protection;
    vm_inherit_t inheritance;
    memory_object_offset_t offset;
    unsigned int user_tag;
    unsigned int ref_count;
    unsigned short shadow_depth;
    unsigned char external_pager;
    unsigned char share_mode;
    boolean_t is_submap;
    vm_behavior_t behavior;
    vm32_object_id_t object_id;
    unsigned short user_wired_count;
    unsigned short flags;
};
#pragma pack(pop)

typedef struct vm_region_submap_short_info_64 *vm_region_submap_short_info_64_t;

typedef struct vm_region_submap_short_info_64 vm_region_submap_short_info_data_64_t;

// mach_vm_read_entry: 0x10 (16) bytes, packed to 4
#pragma pack(push, 4)
struct mach_vm_read_entry
{
    mach_vm_address_t address;
    mach_vm_size_t size;
};
#pragma pack(pop)

// vm_read_entry: 0x10 (16) bytes, packed to 4
#pragma pack(push, 4)
struct vm_read_entry
{
    vm_address_t address;
    vm_size_t size;
};
#pragma pack(pop)

typedef struct mach_vm_read_entry mach_vm_read_entry_t[256];

typedef struct vm_read_entry vm_read_entry_t[256];

typedef int *vm_page_info_t;

typedef int vm_page_info_data_t[];

typedef int vm_page_info_flavor_t;

// vm_page_info_basic: 0x20 (32) bytes
struct vm_page_info_basic
{
    int disposition;
    int ref_count;
    vm_object_id_t object_id;
    memory_object_offset_t offset;
    int depth;
    int __pad;
};

typedef struct vm_page_info_basic *vm_page_info_basic_t;

typedef struct vm_page_info_basic vm_page_info_basic_data_t;

typedef int kmod_t;

typedef struct kmod_reference kmod_reference_t;

typedef kern_return_t kmod_start_func_t(struct kmod_info *ki, void *data);

typedef kern_return_t kmod_stop_func_t(struct kmod_info *ki, void *data);

// kmod_info: 0xC4 (196) bytes, packed to 4
#pragma pack(push, 4)
struct kmod_info
{
    struct kmod_info *next;
    int info_version;
    unsigned int id;
    char name[64];
    char version[64];
    int reference_count;
    kmod_reference_t *reference_list;
    vm_address_t address;
    vm_size_t size;
    vm_size_t hdr_size;
    kmod_start_func_t *start;
    kmod_stop_func_t *stop;
};
#pragma pack(pop)

// kmod_reference: 0x10 (16) bytes, packed to 4
#pragma pack(push, 4)
struct kmod_reference
{
    struct kmod_reference *next;
    struct kmod_info *info;
};
#pragma pack(pop)

typedef struct kmod_info kmod_info_t;

// kmod_info_32_v1: 0xA8 (168) bytes, packed to 4
#pragma pack(push, 4)
struct kmod_info_32_v1
{
    unsigned int next_addr;
    int info_version;
    unsigned int id;
    unsigned char name[64];
    unsigned char version[64];
    int reference_count;
    unsigned int reference_list_addr;
    unsigned int address;
    unsigned int size;
    unsigned int hdr_size;
    unsigned int start_addr;
    unsigned int stop_addr;
};
#pragma pack(pop)

typedef struct kmod_info_32_v1 kmod_info_32_v1_t;

// kmod_info_64_v1: 0xC4 (196) bytes, packed to 4
#pragma pack(push, 4)
struct kmod_info_64_v1
{
    unsigned long long next_addr;
    int info_version;
    unsigned int id;
    unsigned char name[64];
    unsigned char version[64];
    int reference_count;
    unsigned long long reference_list_addr;
    unsigned long long address;
    unsigned long long size;
    unsigned long long hdr_size;
    unsigned long long start_addr;
    unsigned long long stop_addr;
};
#pragma pack(pop)

typedef struct kmod_info_64_v1 kmod_info_64_v1_t;

typedef void *kmod_args_t;

typedef int kmod_control_flavor_t;

typedef kmod_info_t *kmod_info_array_t;

// dyld_kernel_image_info: 0x28 (40) bytes
struct dyld_kernel_image_info
{
    uuid_t uuid;
    fsobj_id_t fsobjid;
    fsid_t fsid;
    unsigned long long load_addr;
};

// dyld_kernel_process_info: 0x48 (72) bytes
struct dyld_kernel_process_info
{
    struct dyld_kernel_image_info cache_image_info;
    unsigned long long timestamp;
    unsigned int imageCount;
    unsigned int initialImageCount;
    unsigned char dyldState;
    boolean_t no_cache;
    boolean_t private_cache;
};

typedef struct dyld_kernel_image_info dyld_kernel_image_info_t;

typedef struct dyld_kernel_process_info dyld_kernel_process_info_t;

typedef dyld_kernel_image_info_t *dyld_kernel_image_info_array_t;

typedef mach_port_t task_t;

typedef mach_port_t task_name_t;

typedef mach_port_t task_policy_set_t;

typedef mach_port_t task_policy_get_t;

typedef mach_port_t task_inspect_t;

typedef mach_port_t task_read_t;

typedef mach_port_t task_suspension_token_t;

typedef mach_port_t thread_t;

typedef mach_port_t thread_act_t;

typedef mach_port_t thread_inspect_t;

typedef mach_port_t thread_read_t;

typedef mach_port_t thread_suspension_token_t;

typedef mach_port_t ipc_space_t;

typedef mach_port_t ipc_space_read_t;

typedef mach_port_t ipc_space_inspect_t;

typedef mach_port_t coalition_t;

typedef mach_port_t host_t;

typedef mach_port_t host_priv_t;

typedef mach_port_t host_security_t;

typedef mach_port_t processor_t;

typedef mach_port_t processor_set_t;

typedef mach_port_t processor_set_control_t;

typedef mach_port_t semaphore_t;

typedef mach_port_t lock_set_t;

typedef mach_port_t ledger_t;

typedef mach_port_t alarm_t;

typedef mach_port_t clock_serv_t;

typedef mach_port_t clock_ctrl_t;

typedef mach_port_t arcade_register_t;

typedef mach_port_t ipc_eventlink_t;

typedef mach_port_t eventlink_port_pair_t[2];

typedef mach_port_t task_id_token_t;

typedef mach_port_t kcdata_object_t;

typedef processor_set_t processor_set_name_t;

typedef mach_port_t clock_reply_t;

typedef mach_port_t bootstrap_t;

typedef mach_port_t mem_entry_name_port_t;

typedef mach_port_t exception_handler_t;

typedef exception_handler_t *exception_handler_array_t;

typedef mach_port_t vm_task_entry_t;

typedef mach_port_t io_main_t;

typedef mach_port_t UNDServerRef;

typedef mach_port_t mach_eventlink_t;

typedef ipc_info_port_t exception_handler_info_t;

typedef task_t *task_array_t;

typedef thread_t *thread_array_t;

typedef processor_set_t *processor_set_array_t;

typedef processor_set_t *processor_set_name_array_t;

typedef processor_t *processor_array_t;

typedef thread_act_t *thread_act_array_t;

typedef ledger_t *ledger_array_t;

typedef task_t task_port_t;

typedef task_array_t task_port_array_t;

typedef thread_t thread_port_t;

typedef thread_array_t thread_port_array_t;

typedef ipc_space_t ipc_space_port_t;

typedef host_t host_name_t;

typedef host_t host_name_port_t;

typedef processor_set_t processor_set_port_t;

typedef processor_set_t processor_set_name_port_t;

typedef processor_set_array_t processor_set_name_port_array_t;

typedef processor_set_t processor_set_control_port_t;

typedef processor_t processor_port_t;

typedef processor_array_t processor_port_array_t;

typedef thread_act_t thread_act_port_t;

typedef thread_act_array_t thread_act_port_array_t;

typedef semaphore_t semaphore_port_t;

typedef lock_set_t lock_set_port_t;

typedef ledger_t ledger_port_t;

typedef ledger_array_t ledger_port_array_t;

typedef alarm_t alarm_port_t;

typedef clock_serv_t clock_serv_port_t;

typedef clock_ctrl_t clock_ctrl_port_t;

typedef exception_handler_t exception_port_t;

typedef exception_handler_array_t exception_port_arrary_t;

typedef char vfs_path_t[4096];

typedef char nspace_path_t[8192];

typedef char nspace_name_t[8192];

typedef unsigned int mach_task_flavor_t;

typedef unsigned int mach_thread_flavor_t;

typedef natural_t ledger_item_t;

typedef unsigned int ledger_entry_id_t;

typedef long long ledger_amount_t;

typedef mach_vm_offset_t *emulation_vector_t;

typedef char *user_subsystem_t;

typedef char *labelstr_t;

typedef kern_return_t memory_reader_t(task_t remote_task, vm_address_t remote_address, vm_size_t size, void **local_memory);

typedef void vm_range_recorder_t(task_t, void *, unsigned int type, vm_range_t *, unsigned int);

typedef void print_task_printer_t(const char *fmt, ...);

// malloc_introspection_t: 0x88 (136) bytes
struct malloc_introspection_t
{
    kern_return_t (*enumerator)(task_t task, void *, unsigned int type_mask, vm_address_t zone_address, memory_reader_t reader, vm_range_recorder_t recorder);
    size_t (*good_size)(malloc_zone_t *zone, size_t size);
    boolean_t (*check)(malloc_zone_t *zone);
    void (*print)(malloc_zone_t *zone, boolean_t verbose);
    void (*log)(malloc_zone_t *zone, void *address);
    void (*force_lock)(malloc_zone_t *zone);
    void (*force_unlock)(malloc_zone_t *zone);
    void (*statistics)(malloc_zone_t *zone, malloc_statistics_t *stats);
    boolean_t (*zone_locked)(malloc_zone_t *zone);
    boolean_t (*enable_discharge_checking)(malloc_zone_t *zone);
    void (*disable_discharge_checking)(malloc_zone_t *zone);
    void (*discharge)(malloc_zone_t *zone, void *memory);
    void *enumerate_unavailable_without_blocks;
    void (*reinit_lock)(malloc_zone_t *zone);
    void (*print_task)(task_t task, unsigned int level, vm_address_t zone_address, memory_reader_t reader, print_task_printer_t printer);
    void (*task_statistics)(task_t task, vm_address_t zone_address, memory_reader_t reader, malloc_statistics_t *stats);
    unsigned int zone_type;
};

// malloc_type_layout_semantics_v0_t: 0x2 (2) bytes
struct NEVERD_GNU_STRUCT malloc_type_layout_semantics_v0_t
{
    bool contains_data_pointer : 1;
    bool contains_struct_pointer : 1;
    bool contains_immutable_pointer : 1;
    bool contains_anonymous_pointer : 1;
    bool is_reference_counted : 1;
    unsigned short reserved_0 : 3;
    bool contains_generic_data : 1;
    unsigned short reserved_1 : 7;
};

// malloc_type_summary_v0_t: 0x4 (4) bytes
struct NEVERD_GNU_STRUCT malloc_type_summary_v0_t
{
    unsigned int version : 4;
    unsigned int reserved_0 : 2;
    malloc_type_callsite_flags_v0_t callsite_flags : 4;
    malloc_type_kind_v0_t type_kind : 2;
    unsigned int reserved_1 : 4;
    malloc_type_layout_semantics_v0_t layout_semantics;
};

// malloc_type_descriptor_v0_t: 0x8 (8) bytes
union malloc_type_descriptor_v0_t
{
    struct
    {
        unsigned int hash;
        malloc_type_summary_v0_t summary;
    };
    malloc_type_id_t type_id;
};

// vm_range_t: 0x10 (16) bytes
struct vm_range_t
{
    vm_address_t address;
    vm_size_t size;
};

// malloc_statistics_t: 0x20 (32) bytes
struct malloc_statistics_t
{
    unsigned int blocks_in_use;
    size_t size_in_use;
    size_t max_size_in_use;
    size_t size_allocated;
};

// mstats: 0x28 (40) bytes
struct mstats
{
    size_t bytes_total;
    size_t chunks_used;
    size_t bytes_used;
    size_t chunks_free;
    size_t bytes_free;
};

#pragma neverd_header("execinfo.h")
// image_offset: 0x14 (20) bytes
struct image_offset
{
    uuid_t uuid;
    unsigned int offset;
};

#pragma neverd_header("fts.h")
// _ftsent: 0x70 (112) bytes
struct _ftsent
{
    struct _ftsent *fts_cycle;
    struct _ftsent *fts_parent;
    struct _ftsent *fts_link;
    long fts_number;
    void *fts_pointer;
    char *fts_accpath;
    char *fts_path;
    int fts_errno;
    int fts_symfd;
    unsigned short fts_pathlen;
    unsigned short fts_namelen;
    ino_t fts_ino;
    dev_t fts_dev;
    nlink_t fts_nlink;
    short fts_level;
    unsigned short fts_info;
    unsigned short fts_flags;
    unsigned short fts_instr;
    struct stat *fts_statp;
    char fts_name[1];
};

// FTS: 0x48 (72) bytes
struct FTS
{
    struct _ftsent *fts_cur;
    struct _ftsent *fts_child;
    struct _ftsent **fts_array;
    dev_t fts_dev;
    char *fts_path;
    int fts_rfd;
    int fts_pathlen;
    int fts_nitems;
    int (*fts_compar)(void);
    int fts_options;
};

typedef struct _ftsent FTSENT;

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

#pragma neverd_header("libproc.h")
// ctl_event_data: 0x8 (8) bytes
struct ctl_event_data
{
    u_int32_t ctl_id;
    u_int32_t ctl_unit;
};

// ctl_info: 0x64 (100) bytes
struct ctl_info
{
    u_int32_t ctl_id;
    char ctl_name[96];
};

// sockaddr_ctl: 0x20 (32) bytes
struct sockaddr_ctl
{
    u_char sc_len;
    u_char sc_family;
    u_int16_t ss_sysaddr;
    u_int32_t sc_id;
    u_int32_t sc_unit;
    u_int32_t sc_reserved[5];
};

// rt_metrics: 0x38 (56) bytes
struct rt_metrics
{
    u_int32_t rmx_locks;
    u_int32_t rmx_mtu;
    u_int32_t rmx_hopcount;
    int rmx_expire;
    u_int32_t rmx_recvpipe;
    u_int32_t rmx_sendpipe;
    u_int32_t rmx_ssthresh;
    u_int32_t rmx_rtt;
    u_int32_t rmx_rttvar;
    u_int32_t rmx_pksent;
    u_int32_t rmx_filler[4];
};

// rtstat: 0xC (12) bytes
struct rtstat
{
    short rts_badredirect;
    short rts_dynamic;
    short rts_newgateway;
    short rts_unreach;
    short rts_wildcard;
    short rts_badrtgwroute;
};

// rt_msghdr: 0x5C (92) bytes
struct rt_msghdr
{
    u_short rtm_msglen;
    u_char rtm_version;
    u_char rtm_type;
    u_short rtm_index;
    int rtm_flags;
    int rtm_addrs;
    pid_t rtm_pid;
    int rtm_seq;
    int rtm_errno;
    int rtm_use;
    u_int32_t rtm_inits;
    struct rt_metrics rtm_rmx;
};

// rt_msghdr2: 0x5C (92) bytes
struct rt_msghdr2
{
    u_short rtm_msglen;
    u_char rtm_version;
    u_char rtm_type;
    u_short rtm_index;
    int rtm_flags;
    int rtm_addrs;
    int rtm_refcnt;
    int rtm_parentflags;
    int rtm_reserved;
    int rtm_use;
    u_int32_t rtm_inits;
    struct rt_metrics rtm_rmx;
};

// rt_msghdr_prelude: 0x2 (2) bytes
struct rt_msghdr_prelude
{
    u_short rtm_msglen;
};

// rt_addrinfo: 0x48 (72) bytes
struct rt_addrinfo
{
    int rti_addrs;
    struct sockaddr *rti_info[8];
};

// proc_bsdinfo: 0x88 (136) bytes
struct proc_bsdinfo
{
    unsigned int pbi_flags;
    unsigned int pbi_status;
    unsigned int pbi_xstatus;
    unsigned int pbi_pid;
    unsigned int pbi_ppid;
    uid_t pbi_uid;
    gid_t pbi_gid;
    uid_t pbi_ruid;
    gid_t pbi_rgid;
    uid_t pbi_svuid;
    gid_t pbi_svgid;
    unsigned int rfu_1;
    char pbi_comm[16];
    char pbi_name[32];
    unsigned int pbi_nfiles;
    unsigned int pbi_pgid;
    unsigned int pbi_pjobc;
    unsigned int e_tdev;
    unsigned int e_tpgid;
    int pbi_nice;
    unsigned long long pbi_start_tvsec;
    unsigned long long pbi_start_tvusec;
};

// proc_bsdshortinfo: 0x40 (64) bytes
struct proc_bsdshortinfo
{
    unsigned int pbsi_pid;
    unsigned int pbsi_ppid;
    unsigned int pbsi_pgid;
    unsigned int pbsi_status;
    char pbsi_comm[16];
    unsigned int pbsi_flags;
    uid_t pbsi_uid;
    gid_t pbsi_gid;
    uid_t pbsi_ruid;
    gid_t pbsi_rgid;
    uid_t pbsi_svuid;
    gid_t pbsi_svgid;
    unsigned int pbsi_rfu;
};

// proc_taskinfo: 0x60 (96) bytes
struct proc_taskinfo
{
    unsigned long long pti_virtual_size;
    unsigned long long pti_resident_size;
    unsigned long long pti_total_user;
    unsigned long long pti_total_system;
    unsigned long long pti_threads_user;
    unsigned long long pti_threads_system;
    int pti_policy;
    int pti_faults;
    int pti_pageins;
    int pti_cow_faults;
    int pti_messages_sent;
    int pti_messages_received;
    int pti_syscalls_mach;
    int pti_syscalls_unix;
    int pti_csw;
    int pti_threadnum;
    int pti_numrunning;
    int pti_priority;
};

// proc_taskallinfo: 0xE8 (232) bytes
struct proc_taskallinfo
{
    struct proc_bsdinfo pbsd;
    struct proc_taskinfo ptinfo;
};

// proc_threadinfo: 0x70 (112) bytes
struct proc_threadinfo
{
    unsigned long long pth_user_time;
    unsigned long long pth_system_time;
    int pth_cpu_usage;
    int pth_policy;
    int pth_run_state;
    int pth_flags;
    int pth_sleep_time;
    int pth_curpri;
    int pth_priority;
    int pth_maxpriority;
    char pth_name[64];
};

// proc_regioninfo: 0x60 (96) bytes
struct proc_regioninfo
{
    unsigned int pri_protection;
    unsigned int pri_max_protection;
    unsigned int pri_inheritance;
    unsigned int pri_flags;
    unsigned long long pri_offset;
    unsigned int pri_behavior;
    unsigned int pri_user_wired_count;
    unsigned int pri_user_tag;
    unsigned int pri_pages_resident;
    unsigned int pri_pages_shared_now_private;
    unsigned int pri_pages_swapped_out;
    unsigned int pri_pages_dirtied;
    unsigned int pri_ref_count;
    unsigned int pri_shadow_depth;
    unsigned int pri_share_mode;
    unsigned int pri_private_pages_resident;
    unsigned int pri_shared_pages_resident;
    unsigned int pri_obj_id;
    unsigned int pri_depth;
    unsigned long long pri_address;
    unsigned long long pri_size;
};

// proc_workqueueinfo: 0x10 (16) bytes
struct proc_workqueueinfo
{
    unsigned int pwq_nthreads;
    unsigned int pwq_runthreads;
    unsigned int pwq_blockedthreads;
    unsigned int pwq_state;
};

// proc_fileinfo: 0x18 (24) bytes
struct proc_fileinfo
{
    unsigned int fi_openflags;
    unsigned int fi_status;
    off_t fi_offset;
    int fi_type;
    unsigned int fi_guardflags;
};

// proc_exitreasonbasicinfo: 0x18 (24) bytes, packed to 1
#pragma pack(push, 1)
struct proc_exitreasonbasicinfo
{
    unsigned int beri_namespace;
    unsigned long long beri_code;
    unsigned long long beri_flags;
    unsigned int beri_reason_buf_size;
};
#pragma pack(pop)

// proc_exitreasoninfo: 0x20 (32) bytes, packed to 1
#pragma pack(push, 1)
struct proc_exitreasoninfo
{
    unsigned int eri_namespace;
    unsigned long long eri_code;
    unsigned long long eri_flags;
    unsigned int eri_reason_buf_size;
    unsigned long long eri_kcd_buf;
};
#pragma pack(pop)

// vinfo_stat: 0x88 (136) bytes
struct vinfo_stat
{
    unsigned int vst_dev;
    unsigned short vst_mode;
    unsigned short vst_nlink;
    unsigned long long vst_ino;
    uid_t vst_uid;
    gid_t vst_gid;
    long long vst_atime;
    long long vst_atimensec;
    long long vst_mtime;
    long long vst_mtimensec;
    long long vst_ctime;
    long long vst_ctimensec;
    long long vst_birthtime;
    long long vst_birthtimensec;
    off_t vst_size;
    long long vst_blocks;
    int vst_blksize;
    unsigned int vst_flags;
    unsigned int vst_gen;
    unsigned int vst_rdev;
    long long vst_qspare[2];
};

// vnode_info: 0x98 (152) bytes
struct vnode_info
{
    struct vinfo_stat vi_stat;
    int vi_type;
    int vi_pad;
    fsid_t vi_fsid;
};

// vnode_info_path: 0x498 (1176) bytes
struct vnode_info_path
{
    struct vnode_info vip_vi;
    char vip_path[1024];
};

// vnode_fdinfo: 0xB0 (176) bytes
struct vnode_fdinfo
{
    struct proc_fileinfo pfi;
    struct vnode_info pvi;
};

// vnode_fdinfowithpath: 0x4B0 (1200) bytes
struct vnode_fdinfowithpath
{
    struct proc_fileinfo pfi;
    struct vnode_info_path pvip;
};

// proc_regionwithpathinfo: 0x4F8 (1272) bytes
struct proc_regionwithpathinfo
{
    struct proc_regioninfo prp_prinfo;
    struct vnode_info_path prp_vip;
};

// proc_regionpath: 0x410 (1040) bytes
struct proc_regionpath
{
    unsigned long long prpo_addr;
    unsigned long long prpo_regionlength;
    char prpo_path[1024];
};

// proc_vnodepathinfo: 0x930 (2352) bytes
struct proc_vnodepathinfo
{
    struct vnode_info_path pvi_cdir;
    struct vnode_info_path pvi_rdir;
};

// proc_threadwithpathinfo: 0x508 (1288) bytes
struct proc_threadwithpathinfo
{
    struct proc_threadinfo pt;
    struct vnode_info_path pvip;
};

// proc_archinfo: 0x8 (8) bytes
struct proc_archinfo
{
    cpu_type_t p_cputype;
    cpu_subtype_t p_cpusubtype;
};

// in4in6_addr: 0x10 (16) bytes
struct in4in6_addr
{
    u_int32_t i46a_pad32[3];
    struct in_addr i46a_addr4;
};

// in_sockinfo: 0x50 (80) bytes
struct in_sockinfo
{
    int insi_fport;
    int insi_lport;
    unsigned long long insi_gencnt;
    unsigned int insi_flags;
    unsigned int insi_flow;
    unsigned char insi_vflag;
    unsigned char insi_ip_ttl;
    unsigned int rfu_1;
    union
    {
        struct in4in6_addr ina_46;
        struct in6_addr ina_6;
    } insi_faddr;
    union
    {
        struct in4in6_addr ina_46;
        struct in6_addr ina_6;
    } insi_laddr;
    struct
    {
        u_char in4_tos;
    } insi_v4;
    struct
    {
        unsigned char in6_hlim;
        int in6_cksum;
        u_short in6_ifindex;
        short in6_hops;
    } insi_v6;
};

// tcp_sockinfo: 0x78 (120) bytes
struct tcp_sockinfo
{
    struct in_sockinfo tcpsi_ini;
    int tcpsi_state;
    int tcpsi_timer[4];
    int tcpsi_mss;
    unsigned int tcpsi_flags;
    unsigned int rfu_1;
    unsigned long long tcpsi_tp;
};

// un_sockinfo: 0x210 (528) bytes
struct un_sockinfo
{
    unsigned long long unsi_conn_so;
    unsigned long long unsi_conn_pcb;
    union
    {
        struct sockaddr_un ua_sun;
        char ua_dummy[255];
    } unsi_addr;
    union
    {
        struct sockaddr_un ua_sun;
        char ua_dummy[255];
    } unsi_caddr;
};

// ndrv_info: 0x18 (24) bytes
struct ndrv_info
{
    unsigned int ndrvsi_if_family;
    unsigned int ndrvsi_if_unit;
    char ndrvsi_if_name[16];
};

// kern_event_info: 0xC (12) bytes
struct kern_event_info
{
    unsigned int kesi_vendor_code_filter;
    unsigned int kesi_class_filter;
    unsigned int kesi_subclass_filter;
};

// kern_ctl_info: 0x78 (120) bytes
struct kern_ctl_info
{
    unsigned int kcsi_id;
    unsigned int kcsi_reg_unit;
    unsigned int kcsi_flags;
    unsigned int kcsi_recvbufsize;
    unsigned int kcsi_sendbufsize;
    unsigned int kcsi_unit;
    char kcsi_name[96];
};

// vsock_sockinfo: 0x10 (16) bytes
struct vsock_sockinfo
{
    unsigned int local_cid;
    unsigned int local_port;
    unsigned int remote_cid;
    unsigned int remote_port;
};

// sockbuf_info: 0x18 (24) bytes
struct sockbuf_info
{
    unsigned int sbi_cc;
    unsigned int sbi_hiwat;
    unsigned int sbi_mbcnt;
    unsigned int sbi_mbmax;
    unsigned int sbi_lowat;
    short sbi_flags;
    short sbi_timeo;
};

// socket_info: 0x300 (768) bytes
struct socket_info
{
    struct vinfo_stat soi_stat;
    unsigned long long soi_so;
    unsigned long long soi_pcb;
    int soi_type;
    int soi_protocol;
    int soi_family;
    short soi_options;
    short soi_linger;
    short soi_state;
    short soi_qlen;
    short soi_incqlen;
    short soi_qlimit;
    short soi_timeo;
    u_short soi_error;
    unsigned int soi_oobmark;
    struct sockbuf_info soi_rcv;
    struct sockbuf_info soi_snd;
    int soi_kind;
    unsigned int rfu_1;
    union
    {
        struct in_sockinfo pri_in;
        struct tcp_sockinfo pri_tcp;
        struct un_sockinfo pri_un;
        struct ndrv_info pri_ndrv;
        struct kern_event_info pri_kern_event;
        struct kern_ctl_info pri_kern_ctl;
        struct vsock_sockinfo pri_vsock;
    } soi_proto;
};

// socket_fdinfo: 0x318 (792) bytes
struct socket_fdinfo
{
    struct proc_fileinfo pfi;
    struct socket_info psi;
};

// psem_info: 0x488 (1160) bytes
struct psem_info
{
    struct vinfo_stat psem_stat;
    char psem_name[1024];
};

// psem_fdinfo: 0x4A0 (1184) bytes
struct psem_fdinfo
{
    struct proc_fileinfo pfi;
    struct psem_info pseminfo;
};

// pshm_info: 0x490 (1168) bytes
struct pshm_info
{
    struct vinfo_stat pshm_stat;
    unsigned long long pshm_mappaddr;
    char pshm_name[1024];
};

// pshm_fdinfo: 0x4A8 (1192) bytes
struct pshm_fdinfo
{
    struct proc_fileinfo pfi;
    struct pshm_info pshminfo;
};

// pipe_info: 0xA0 (160) bytes
struct pipe_info
{
    struct vinfo_stat pipe_stat;
    unsigned long long pipe_handle;
    unsigned long long pipe_peerhandle;
    int pipe_status;
    int rfu_1;
};

// pipe_fdinfo: 0xB8 (184) bytes
struct pipe_fdinfo
{
    struct proc_fileinfo pfi;
    struct pipe_info pipeinfo;
};

// kqueue_info: 0x90 (144) bytes
struct kqueue_info
{
    struct vinfo_stat kq_stat;
    unsigned int kq_state;
    unsigned int rfu_1;
};

// kqueue_dyninfo: 0xD0 (208) bytes
struct kqueue_dyninfo
{
    struct kqueue_info kqdi_info;
    unsigned long long kqdi_servicer;
    unsigned long long kqdi_owner;
    unsigned int kqdi_sync_waiters;
    unsigned char kqdi_sync_waiter_qos;
    unsigned char kqdi_async_qos;
    unsigned short kqdi_request_state;
    unsigned char kqdi_events_qos;
    unsigned char kqdi_pri;
    unsigned char kqdi_pol;
    unsigned char kqdi_cpupercent;
    unsigned char _kqdi_reserved0[4];
    unsigned long long _kqdi_reserved1[4];
};

// kqueue_fdinfo: 0xA8 (168) bytes
struct kqueue_fdinfo
{
    struct proc_fileinfo pfi;
    struct kqueue_info kqueueinfo;
};

// appletalk_info: 0x88 (136) bytes
struct appletalk_info
{
    struct vinfo_stat atalk_stat;
};

// appletalk_fdinfo: 0xA0 (160) bytes
struct appletalk_fdinfo
{
    struct proc_fileinfo pfi;
    struct appletalk_info appletalkinfo;
};

typedef unsigned long long proc_info_udata_t;

// proc_fdinfo: 0x8 (8) bytes
struct proc_fdinfo
{
    int proc_fd;
    unsigned int proc_fdtype;
};

// proc_fileportinfo: 0x8 (8) bytes
struct proc_fileportinfo
{
    unsigned int proc_fileport;
    unsigned int proc_fdtype;
};

// proc_channel_info: 0x20 (32) bytes
struct proc_channel_info
{
    uuid_t chi_instance;
    unsigned int chi_port;
    unsigned int chi_type;
    unsigned int chi_flags;
    unsigned int rfu_1;
};

// channel_fdinfo: 0x38 (56) bytes
struct channel_fdinfo
{
    struct proc_fileinfo pfi;
    struct proc_channel_info channelinfo;
};

#pragma neverd_header("mach-o/dyld.h")
// _OSUnalignedU16: 0x2 (2) bytes, packed to 1
#pragma pack(push, 1)
struct _OSUnalignedU16
{
    volatile unsigned short __val;
};
#pragma pack(pop)

// _OSUnalignedU32: 0x4 (4) bytes, packed to 1
#pragma pack(push, 1)
struct _OSUnalignedU32
{
    volatile unsigned int __val;
};
#pragma pack(pop)

// _OSUnalignedU64: 0x8 (8) bytes, packed to 1
#pragma pack(push, 1)
struct _OSUnalignedU64
{
    volatile unsigned long long __val;
};
#pragma pack(pop)

typedef unsigned long NXSwappedFloat;

typedef unsigned long long NXSwappedDouble;

// mach_header: 0x1C (28) bytes
struct mach_header
{
    unsigned int magic;
    cpu_type_t cputype;
    cpu_subtype_t cpusubtype;
    unsigned int filetype;
    unsigned int ncmds;
    unsigned int sizeofcmds;
    unsigned int flags;
};

// mach_header_64: 0x20 (32) bytes
struct mach_header_64
{
    unsigned int magic;
    cpu_type_t cputype;
    cpu_subtype_t cpusubtype;
    unsigned int filetype;
    unsigned int ncmds;
    unsigned int sizeofcmds;
    unsigned int flags;
    unsigned int reserved;
};

// load_command: 0x8 (8) bytes
struct load_command
{
    unsigned int cmd;
    unsigned int cmdsize;
};

// lc_str: 0x4 (4) bytes
union lc_str
{
    unsigned int offset;
};

// segment_command: 0x38 (56) bytes
struct segment_command
{
    unsigned int cmd;
    unsigned int cmdsize;
    char segname[16];
    unsigned int vmaddr;
    unsigned int vmsize;
    unsigned int fileoff;
    unsigned int filesize;
    vm_prot_t maxprot;
    vm_prot_t initprot;
    unsigned int nsects;
    unsigned int flags;
};

// segment_command_64: 0x48 (72) bytes
struct segment_command_64
{
    unsigned int cmd;
    unsigned int cmdsize;
    char segname[16];
    unsigned long long vmaddr;
    unsigned long long vmsize;
    unsigned long long fileoff;
    unsigned long long filesize;
    vm_prot_t maxprot;
    vm_prot_t initprot;
    unsigned int nsects;
    unsigned int flags;
};

// section: 0x44 (68) bytes
struct section
{
    char sectname[16];
    char segname[16];
    unsigned int addr;
    unsigned int size;
    unsigned int offset;
    unsigned int align;
    unsigned int reloff;
    unsigned int nreloc;
    unsigned int flags;
    unsigned int reserved1;
    unsigned int reserved2;
};

// section_64: 0x50 (80) bytes
struct section_64
{
    char sectname[16];
    char segname[16];
    unsigned long long addr;
    unsigned long long size;
    unsigned int offset;
    unsigned int align;
    unsigned int reloff;
    unsigned int nreloc;
    unsigned int flags;
    unsigned int reserved1;
    unsigned int reserved2;
    unsigned int reserved3;
};

// fvmlib: 0xC (12) bytes
struct fvmlib
{
    union lc_str name;
    unsigned int minor_version;
    unsigned int header_addr;
};

// fvmlib_command: 0x14 (20) bytes
struct fvmlib_command
{
    unsigned int cmd;
    unsigned int cmdsize;
    struct fvmlib fvmlib;
};

// dylib: 0x10 (16) bytes
struct dylib
{
    union lc_str name;
    unsigned int timestamp;
    unsigned int current_version;
    unsigned int compatibility_version;
};

// dylib_command: 0x18 (24) bytes
struct dylib_command
{
    unsigned int cmd;
    unsigned int cmdsize;
    struct dylib dylib;
};

// sub_framework_command: 0xC (12) bytes
struct sub_framework_command
{
    unsigned int cmd;
    unsigned int cmdsize;
    union lc_str umbrella;
};

// sub_client_command: 0xC (12) bytes
struct sub_client_command
{
    unsigned int cmd;
    unsigned int cmdsize;
    union lc_str client;
};

// sub_umbrella_command: 0xC (12) bytes
struct sub_umbrella_command
{
    unsigned int cmd;
    unsigned int cmdsize;
    union lc_str sub_umbrella;
};

// sub_library_command: 0xC (12) bytes
struct sub_library_command
{
    unsigned int cmd;
    unsigned int cmdsize;
    union lc_str sub_library;
};

// prebound_dylib_command: 0x14 (20) bytes
struct prebound_dylib_command
{
    unsigned int cmd;
    unsigned int cmdsize;
    union lc_str name;
    unsigned int nmodules;
    union lc_str linked_modules;
};

// dylinker_command: 0xC (12) bytes
struct dylinker_command
{
    unsigned int cmd;
    unsigned int cmdsize;
    union lc_str name;
};

// thread_command: 0x8 (8) bytes
struct thread_command
{
    unsigned int cmd;
    unsigned int cmdsize;
};

// routines_command: 0x28 (40) bytes
struct routines_command
{
    unsigned int cmd;
    unsigned int cmdsize;
    unsigned int init_address;
    unsigned int init_module;
    unsigned int reserved1;
    unsigned int reserved2;
    unsigned int reserved3;
    unsigned int reserved4;
    unsigned int reserved5;
    unsigned int reserved6;
};

// routines_command_64: 0x48 (72) bytes
struct routines_command_64
{
    unsigned int cmd;
    unsigned int cmdsize;
    unsigned long long init_address;
    unsigned long long init_module;
    unsigned long long reserved1;
    unsigned long long reserved2;
    unsigned long long reserved3;
    unsigned long long reserved4;
    unsigned long long reserved5;
    unsigned long long reserved6;
};

// symtab_command: 0x18 (24) bytes
struct symtab_command
{
    unsigned int cmd;
    unsigned int cmdsize;
    unsigned int symoff;
    unsigned int nsyms;
    unsigned int stroff;
    unsigned int strsize;
};

// dysymtab_command: 0x50 (80) bytes
struct dysymtab_command
{
    unsigned int cmd;
    unsigned int cmdsize;
    unsigned int ilocalsym;
    unsigned int nlocalsym;
    unsigned int iextdefsym;
    unsigned int nextdefsym;
    unsigned int iundefsym;
    unsigned int nundefsym;
    unsigned int tocoff;
    unsigned int ntoc;
    unsigned int modtaboff;
    unsigned int nmodtab;
    unsigned int extrefsymoff;
    unsigned int nextrefsyms;
    unsigned int indirectsymoff;
    unsigned int nindirectsyms;
    unsigned int extreloff;
    unsigned int nextrel;
    unsigned int locreloff;
    unsigned int nlocrel;
};

// dylib_table_of_contents: 0x8 (8) bytes
struct dylib_table_of_contents
{
    unsigned int symbol_index;
    unsigned int module_index;
};

// dylib_module: 0x34 (52) bytes
struct dylib_module
{
    unsigned int module_name;
    unsigned int iextdefsym;
    unsigned int nextdefsym;
    unsigned int irefsym;
    unsigned int nrefsym;
    unsigned int ilocalsym;
    unsigned int nlocalsym;
    unsigned int iextrel;
    unsigned int nextrel;
    unsigned int iinit_iterm;
    unsigned int ninit_nterm;
    unsigned int objc_module_info_addr;
    unsigned int objc_module_info_size;
};

// dylib_module_64: 0x38 (56) bytes
struct dylib_module_64
{
    unsigned int module_name;
    unsigned int iextdefsym;
    unsigned int nextdefsym;
    unsigned int irefsym;
    unsigned int nrefsym;
    unsigned int ilocalsym;
    unsigned int nlocalsym;
    unsigned int iextrel;
    unsigned int nextrel;
    unsigned int iinit_iterm;
    unsigned int ninit_nterm;
    unsigned int objc_module_info_size;
    unsigned long long objc_module_info_addr;
};

// dylib_reference: 0x4 (4) bytes
struct NEVERD_GNU_STRUCT dylib_reference
{
    unsigned int isym : 24;
    unsigned int flags : 8;
};

// twolevel_hints_command: 0x10 (16) bytes
struct twolevel_hints_command
{
    unsigned int cmd;
    unsigned int cmdsize;
    unsigned int offset;
    unsigned int nhints;
};

// twolevel_hint: 0x4 (4) bytes
struct NEVERD_GNU_STRUCT twolevel_hint
{
    unsigned int isub_image : 8;
    unsigned int itoc : 24;
};

// prebind_cksum_command: 0xC (12) bytes
struct prebind_cksum_command
{
    unsigned int cmd;
    unsigned int cmdsize;
    unsigned int cksum;
};

// uuid_command: 0x18 (24) bytes
struct uuid_command
{
    unsigned int cmd;
    unsigned int cmdsize;
    unsigned char uuid[16];
};

// rpath_command: 0xC (12) bytes
struct rpath_command
{
    unsigned int cmd;
    unsigned int cmdsize;
    union lc_str path;
};

// linkedit_data_command: 0x10 (16) bytes
struct linkedit_data_command
{
    unsigned int cmd;
    unsigned int cmdsize;
    unsigned int dataoff;
    unsigned int datasize;
};

// fileset_entry_command: 0x20 (32) bytes
struct fileset_entry_command
{
    unsigned int cmd;
    unsigned int cmdsize;
    unsigned long long vmaddr;
    unsigned long long fileoff;
    union lc_str entry_id;
    unsigned int reserved;
};

// encryption_info_command: 0x14 (20) bytes
struct encryption_info_command
{
    unsigned int cmd;
    unsigned int cmdsize;
    unsigned int cryptoff;
    unsigned int cryptsize;
    unsigned int cryptid;
};

// encryption_info_command_64: 0x18 (24) bytes
struct encryption_info_command_64
{
    unsigned int cmd;
    unsigned int cmdsize;
    unsigned int cryptoff;
    unsigned int cryptsize;
    unsigned int cryptid;
    unsigned int pad;
};

// version_min_command: 0x10 (16) bytes
struct version_min_command
{
    unsigned int cmd;
    unsigned int cmdsize;
    unsigned int version;
    unsigned int sdk;
};

// build_version_command: 0x18 (24) bytes
struct build_version_command
{
    unsigned int cmd;
    unsigned int cmdsize;
    unsigned int platform;
    unsigned int minos;
    unsigned int sdk;
    unsigned int ntools;
};

// build_tool_version: 0x8 (8) bytes
struct build_tool_version
{
    unsigned int tool;
    unsigned int version;
};

// dyld_info_command: 0x30 (48) bytes
struct dyld_info_command
{
    unsigned int cmd;
    unsigned int cmdsize;
    unsigned int rebase_off;
    unsigned int rebase_size;
    unsigned int bind_off;
    unsigned int bind_size;
    unsigned int weak_bind_off;
    unsigned int weak_bind_size;
    unsigned int lazy_bind_off;
    unsigned int lazy_bind_size;
    unsigned int export_off;
    unsigned int export_size;
};

// linker_option_command: 0xC (12) bytes
struct linker_option_command
{
    unsigned int cmd;
    unsigned int cmdsize;
    unsigned int count;
};

// symseg_command: 0x10 (16) bytes
struct symseg_command
{
    unsigned int cmd;
    unsigned int cmdsize;
    unsigned int offset;
    unsigned int size;
};

// ident_command: 0x8 (8) bytes
struct ident_command
{
    unsigned int cmd;
    unsigned int cmdsize;
};

// fvmfile_command: 0x10 (16) bytes
struct fvmfile_command
{
    unsigned int cmd;
    unsigned int cmdsize;
    union lc_str name;
    unsigned int header_addr;
};

// entry_point_command: 0x18 (24) bytes
struct entry_point_command
{
    unsigned int cmd;
    unsigned int cmdsize;
    unsigned long long entryoff;
    unsigned long long stacksize;
};

// source_version_command: 0x10 (16) bytes
struct source_version_command
{
    unsigned int cmd;
    unsigned int cmdsize;
    unsigned long long version;
};

// data_in_code_entry: 0x8 (8) bytes
struct data_in_code_entry
{
    unsigned int offset;
    unsigned short length;
    unsigned short kind;
};

// tlv_descriptor: 0x18 (24) bytes
struct tlv_descriptor
{
    void *(*thunk)(struct tlv_descriptor *);
    unsigned long key;
    unsigned long offset;
};

// note_command: 0x28 (40) bytes
struct note_command
{
    unsigned int cmd;
    unsigned int cmdsize;
    char data_owner[16];
    unsigned long long offset;
    unsigned long long size;
};

#pragma neverd_header()
// Declared only: its members are unknown.
struct __NSObjectFileImage;

#pragma neverd_header("mach-o/dyld.h")
typedef struct __NSObjectFileImage *NSObjectFileImage;

#pragma neverd_header()
// Declared only: its members are unknown.
struct __NSModule;

#pragma neverd_header("mach-o/dyld.h")
typedef struct __NSModule *NSModule;

#pragma neverd_header()
// Declared only: its members are unknown.
struct __NSSymbol;

#pragma neverd_header("mach-o/dyld.h")
typedef struct __NSSymbol *NSSymbol;

// NSLinkEditErrorHandlers: 0x18 (24) bytes
struct NSLinkEditErrorHandlers
{
    void (*undefined)(const char *symbolName);
    NSModule (*multiple)(NSSymbol s, NSModule oldModule, NSModule newModule);
    void (*linkEdit)(NSLinkEditErrors errorClass, int errorNumber, const char *fileName, const char *errorString);
};

#pragma neverd_header("os/lock.h")
// os_unfair_lock_s: 0x4 (4) bytes
struct os_unfair_lock_s
{
    unsigned int _os_unfair_lock_opaque;
};

typedef struct os_unfair_lock_s os_unfair_lock;

typedef struct os_unfair_lock_s *os_unfair_lock_t;

#pragma neverd_header("libkern/OSAtomic.h")
typedef long long OSAtomic_int64_aligned64_t;

// OSFifoQueueHead: 0x20 (32) bytes
struct DECLSPEC_ALIGN(16) OSFifoQueueHead
{
    void *opaque1;
    void *opaque2;
    int opaque3;
};

typedef int OSSpinLock;

// OSQueueHead: 0x10 (16) bytes
struct DECLSPEC_ALIGN(16) OSQueueHead
{
    void *opaque1;
    long opaque2;
};

#pragma neverd_header("mach/mach_time.h")
// mach_timebase_info: 0x8 (8) bytes
struct mach_timebase_info
{
    unsigned int numer;
    unsigned int denom;
};

typedef struct mach_timebase_info *mach_timebase_info_t;

typedef struct mach_timebase_info mach_timebase_info_data_t;


const struct mach_header *NSAddImage(const char *image_name, unsigned int options);
bool NSAddLibrary(const char *pathName);
bool NSAddLibraryWithSearching(const char *pathName);
void *NSAddressOfSymbol(NSSymbol symbol);
NSObjectFileImageReturnCode NSCreateObjectFileImageFromFile(const char *pathName, NSObjectFileImage *objectFileImage);
NSObjectFileImageReturnCode NSCreateObjectFileImageFromMemory(const void *address, size_t size, NSObjectFileImage *objectFileImage);
bool NSDestroyObjectFileImage(NSObjectFileImage objectFileImage);
void *NSGetSectionDataInObjectFileImage(NSObjectFileImage objectFileImage, const char *segmentName, const char *sectionName, size_t *size);
void NSInstallLinkEditErrorHandlers(const NSLinkEditErrorHandlers *handlers);
bool NSIsSymbolDefinedInObjectFileImage(NSObjectFileImage objectFileImage, const char *symbolName);
bool NSIsSymbolNameDefined(const char *symbolName);
bool NSIsSymbolNameDefinedInImage(const struct mach_header *image, const char *symbolName);
bool NSIsSymbolNameDefinedWithHint(const char *symbolName, const char *libraryNameHint);
const char *NSLibraryNameForModule(NSModule m);
void NSLinkEditError(NSLinkEditErrors *c, int *errorNumber, const char **fileName, const char **errorString);
NSModule NSLinkModule(NSObjectFileImage objectFileImage, const char *moduleName, unsigned int options);
NSSymbol NSLookupAndBindSymbol(const char *symbolName);
NSSymbol NSLookupAndBindSymbolWithHint(const char *symbolName, const char *libraryNameHint);
NSSymbol NSLookupSymbolInImage(const struct mach_header *image, const char *symbolName, unsigned int options);
NSSymbol NSLookupSymbolInModule(NSModule module, const char *symbolName);
NSModule NSModuleForSymbol(NSSymbol symbol);
const char *NSNameOfModule(NSModule m);
const char *NSNameOfSymbol(NSSymbol symbol);
unsigned int NSSymbolDefinitionCountInObjectFileImage(NSObjectFileImage objectFileImage);
const char *NSSymbolDefinitionNameInObjectFileImage(NSObjectFileImage objectFileImage, unsigned int ordinal);
unsigned int NSSymbolReferenceCountInObjectFileImage(NSObjectFileImage objectFileImage);
const char *NSSymbolReferenceNameInObjectFileImage(NSObjectFileImage objectFileImage, unsigned int ordinal, bool *tentative_definition);
bool NSUnLinkModule(NSModule module, unsigned int options);
int NSVersionOfLinkTimeLibrary(const char *libraryName);
int NSVersionOfRunTimeLibrary(const char *libraryName);
NXSwappedDouble NXConvertHostDoubleToSwapped(double x);
NXSwappedFloat NXConvertHostFloatToSwapped(float x);
double NXConvertSwappedDoubleToHost(NXSwappedDouble x);
float NXConvertSwappedFloatToHost(NXSwappedFloat x);
enum NXByteOrder NXHostByteOrder(void);
double NXSwapBigDoubleToHost(NXSwappedDouble x);
float NXSwapBigFloatToHost(NXSwappedFloat x);
unsigned int NXSwapBigIntToHost(unsigned int x);
unsigned long long NXSwapBigLongLongToHost(unsigned long long x);
unsigned long NXSwapBigLongToHost(unsigned long x);
unsigned short NXSwapBigShortToHost(unsigned short x);
NXSwappedDouble NXSwapDouble(NXSwappedDouble x);
NXSwappedFloat NXSwapFloat(NXSwappedFloat x);
NXSwappedDouble NXSwapHostDoubleToBig(double x);
NXSwappedDouble NXSwapHostDoubleToLittle(double x);
NXSwappedFloat NXSwapHostFloatToBig(float x);
NXSwappedFloat NXSwapHostFloatToLittle(float x);
unsigned int NXSwapHostIntToBig(unsigned int x);
unsigned int NXSwapHostIntToLittle(unsigned int x);
unsigned long long NXSwapHostLongLongToBig(unsigned long long x);
unsigned long long NXSwapHostLongLongToLittle(unsigned long long x);
unsigned long NXSwapHostLongToBig(unsigned long x);
unsigned long NXSwapHostLongToLittle(unsigned long x);
unsigned short NXSwapHostShortToBig(unsigned short x);
unsigned short NXSwapHostShortToLittle(unsigned short x);
unsigned int NXSwapInt(unsigned int inv);
double NXSwapLittleDoubleToHost(NXSwappedDouble x);
float NXSwapLittleFloatToHost(NXSwappedFloat x);
unsigned int NXSwapLittleIntToHost(unsigned int x);
unsigned long long NXSwapLittleLongLongToHost(unsigned long long x);
unsigned long NXSwapLittleLongToHost(unsigned long x);
unsigned short NXSwapLittleShortToHost(unsigned short x);
unsigned long NXSwapLong(unsigned long inv);
unsigned long long NXSwapLongLong(unsigned long long inv);
unsigned short NXSwapShort(unsigned short inv);
int OSAtomicAdd32(int __theAmount, volatile int *__theValue);
int OSAtomicAdd32Barrier(int __theAmount, volatile int *__theValue);
long long OSAtomicAdd64(long long __theAmount, volatile OSAtomic_int64_aligned64_t *__theValue);
long long OSAtomicAdd64Barrier(long long __theAmount, volatile OSAtomic_int64_aligned64_t *__theValue);
int OSAtomicAnd32(unsigned int __theMask, volatile unsigned int *__theValue);
int OSAtomicAnd32Barrier(unsigned int __theMask, volatile unsigned int *__theValue);
int OSAtomicAnd32Orig(unsigned int __theMask, volatile unsigned int *__theValue);
int OSAtomicAnd32OrigBarrier(unsigned int __theMask, volatile unsigned int *__theValue);
bool OSAtomicCompareAndSwap32(int __oldValue, int __newValue, volatile int *__theValue);
bool OSAtomicCompareAndSwap32Barrier(int __oldValue, int __newValue, volatile int *__theValue);
bool OSAtomicCompareAndSwap64(long long __oldValue, long long __newValue, volatile OSAtomic_int64_aligned64_t *__theValue);
bool OSAtomicCompareAndSwap64Barrier(long long __oldValue, long long __newValue, volatile OSAtomic_int64_aligned64_t *__theValue);
bool OSAtomicCompareAndSwapInt(int __oldValue, int __newValue, volatile int *__theValue);
bool OSAtomicCompareAndSwapIntBarrier(int __oldValue, int __newValue, volatile int *__theValue);
bool OSAtomicCompareAndSwapLong(long __oldValue, long __newValue, volatile long *__theValue);
bool OSAtomicCompareAndSwapLongBarrier(long __oldValue, long __newValue, volatile long *__theValue);
bool OSAtomicCompareAndSwapPtr(void *__oldValue, void *__newValue, void *volatile *__theValue);
bool OSAtomicCompareAndSwapPtrBarrier(void *__oldValue, void *__newValue, void *volatile *__theValue);
int OSAtomicDecrement32(volatile int *__theValue);
int OSAtomicDecrement32Barrier(volatile int *__theValue);
long long OSAtomicDecrement64(volatile OSAtomic_int64_aligned64_t *__theValue);
long long OSAtomicDecrement64Barrier(volatile OSAtomic_int64_aligned64_t *__theValue);
void *OSAtomicDequeue(OSQueueHead *__list, size_t __offset);
void OSAtomicEnqueue(OSQueueHead *__list, void *__new, size_t __offset);
void *OSAtomicFifoDequeue(OSFifoQueueHead *__list, size_t __offset);
void OSAtomicFifoEnqueue(OSFifoQueueHead *__list, void *__new, size_t __offset);
int OSAtomicIncrement32(volatile int *__theValue);
int OSAtomicIncrement32Barrier(volatile int *__theValue);
long long OSAtomicIncrement64(volatile OSAtomic_int64_aligned64_t *__theValue);
long long OSAtomicIncrement64Barrier(volatile OSAtomic_int64_aligned64_t *__theValue);
int OSAtomicOr32(unsigned int __theMask, volatile unsigned int *__theValue);
int OSAtomicOr32Barrier(unsigned int __theMask, volatile unsigned int *__theValue);
int OSAtomicOr32Orig(unsigned int __theMask, volatile unsigned int *__theValue);
int OSAtomicOr32OrigBarrier(unsigned int __theMask, volatile unsigned int *__theValue);
bool OSAtomicTestAndClear(unsigned int __n, volatile void *__theAddress);
bool OSAtomicTestAndClearBarrier(unsigned int __n, volatile void *__theAddress);
bool OSAtomicTestAndSet(unsigned int __n, volatile void *__theAddress);
bool OSAtomicTestAndSetBarrier(unsigned int __n, volatile void *__theAddress);
int OSAtomicXor32(unsigned int __theMask, volatile unsigned int *__theValue);
int OSAtomicXor32Barrier(unsigned int __theMask, volatile unsigned int *__theValue);
int OSAtomicXor32Orig(unsigned int __theMask, volatile unsigned int *__theValue);
int OSAtomicXor32OrigBarrier(unsigned int __theMask, volatile unsigned int *__theValue);
int OSHostByteOrder(void);
void OSMemoryBarrier(void);
unsigned short OSReadSwapInt16(const volatile void *_base, uintptr_t _offset);
unsigned int OSReadSwapInt32(const volatile void *_base, uintptr_t _offset);
unsigned long long OSReadSwapInt64(const volatile void *_base, uintptr_t _offset);
void OSSpinLockLock(volatile OSSpinLock *__lock);
bool OSSpinLockTry(volatile OSSpinLock *__lock);
void OSSpinLockUnlock(volatile OSSpinLock *__lock);
void OSWriteSwapInt16(volatile void *_base, uintptr_t _offset, unsigned short _data);
void OSWriteSwapInt32(volatile void *_base, uintptr_t _offset, unsigned int _data);
void OSWriteSwapInt64(volatile void *_base, uintptr_t _offset, unsigned long long _data);
_Noreturn void _Exit(int);
int *_NSGetArgc(void);
char ***_NSGetArgv(void);
char ***_NSGetEnviron(void);
int _NSGetExecutablePath(char *buf, unsigned int *bufsize);
struct mach_header_64 *_NSGetMachExecuteHeader(void);
char **_NSGetProgname(void);
unsigned short _OSReadInt16(const volatile void *base, uintptr_t byteOffset);
unsigned int _OSReadInt32(const volatile void *base, uintptr_t byteOffset);
unsigned long long _OSReadInt64(const volatile void *base, uintptr_t byteOffset);
__uint16_t _OSSwapInt16(__uint16_t _data);
__uint32_t _OSSwapInt32(__uint32_t _data);
__uint64_t _OSSwapInt64(__uint64_t _data);
void _OSWriteInt16(volatile void *base, uintptr_t byteOffset, unsigned short data);
void _OSWriteInt32(volatile void *base, uintptr_t byteOffset, unsigned int data);
void _OSWriteInt64(volatile void *base, uintptr_t byteOffset, unsigned long long data);
int ___mb_cur_max(void);
int ___mb_cur_max_l(locale_t);
unsigned long ___runetype(__darwin_ct_rune_t);
unsigned long ___runetype_l(__darwin_ct_rune_t, locale_t);
__darwin_ct_rune_t ___tolower(__darwin_ct_rune_t);
__darwin_ct_rune_t ___tolower_l(__darwin_ct_rune_t, locale_t);
__darwin_ct_rune_t ___toupper(__darwin_ct_rune_t);
__darwin_ct_rune_t ___toupper_l(__darwin_ct_rune_t, locale_t);
_Noreturn void __assert_rtn(const char *, const char *, int, const char *);
int __darwin_check_fd_set(int _a, const void *_b);
int __darwin_check_fd_set_overflow(int, const void *, int);
void __darwin_fd_clr(int _fd, fd_set *const _p);
int __darwin_fd_isset(int _fd, const fd_set *_p);
void __darwin_fd_set(int _fd, fd_set *const _p);
int *__error(void);
__darwin_ct_rune_t __isctype(__darwin_ct_rune_t _c, unsigned long _f);
int __istype(__darwin_ct_rune_t _c, unsigned long _f);
int __istype_l(__darwin_ct_rune_t _c, unsigned long _f, locale_t _l);
int __maskrune(__darwin_ct_rune_t, unsigned long);
int __maskrune_l(__darwin_ct_rune_t, unsigned long, locale_t);
DIR *__opendir2(const char *, int);
unsigned int __sigbits(int __signo);
int __snprintf_chk(char *, size_t __maxlen, int, size_t, const char *, ...);
int __sprintf_chk(char *, int, size_t, const char *, ...);
int __sputc(int _c, FILE *_p);
int __srget(FILE *);
int __svfscanf(FILE *, const char *, char *);
int __swbuf(int, FILE *);
__darwin_ct_rune_t __tolower(__darwin_ct_rune_t);
__darwin_ct_rune_t __tolower_l(__darwin_ct_rune_t _c, locale_t _l);
__darwin_ct_rune_t __toupper(__darwin_ct_rune_t);
__darwin_ct_rune_t __toupper_l(__darwin_ct_rune_t _c, locale_t _l);
int __vsnprintf_chk(char *, size_t __maxlen, int, size_t, const char *, char *);
int __vsprintf_chk(char *, int, size_t, const char *, char *);
int __wcwidth(__darwin_ct_rune_t _c);
int __wcwidth_l(__darwin_ct_rune_t _c, locale_t _l);
bool _dyld_all_twolevel_modules_prebound(void);
bool _dyld_bind_fully_image_containing_address(const void *address);
const struct mach_header *_dyld_get_image_header(unsigned int image_index);
const struct mach_header *_dyld_get_image_header_containing_address(const void *address);
const char *_dyld_get_image_name(unsigned int image_index);
intptr_t _dyld_get_image_vmaddr_slide(unsigned int image_index);
bool _dyld_image_containing_address(const void *address);
unsigned int _dyld_image_count(void);
bool _dyld_launched_prebound(void);
void _dyld_lookup_and_bind(const char *symbol_name, void **address, NSModule *module);
void _dyld_lookup_and_bind_fully(const char *symbol_name, void **address, NSModule *module);
void _dyld_lookup_and_bind_with_hint(const char *symbol_name, const char *library_name_hint, void **address, NSModule *module);
bool _dyld_present(void);
void _dyld_register_func_for_add_image(void (*func)(const struct mach_header *mh, intptr_t vmaddr_slide));
void _dyld_register_func_for_remove_image(void (*func)(const struct mach_header *mh, intptr_t vmaddr_slide));
bool _dyld_shared_cache_contains_path(const char *path);
_Noreturn void _exit(int);
int _getdirentries_is_not_available_when_64_bit_inodes_are_in_effect(int, char *, int __nbytes, long *);
_Noreturn void _longjmp(jmp_buf, int);
int _setjmp(jmp_buf);
void _tlv_atexit(void (*termFunc)(void *objAddr), void *objAddr);
void _tlv_bootstrap(void);
long a64l(const char *);
_Noreturn void abort(void);
int abs(int);
int accept(int, struct sockaddr *, socklen_t *);
int access(const char *, int);
int accessx_np(const struct accessx_descriptor *, size_t __sz, int *, uid_t);
int acct(const char *);
int add_profil(char *, size_t __bufsiz, unsigned long, unsigned int);
char *addr2ascii(int, const void *, int __size, char *);
int adjtime(const struct timeval *, struct timeval *);
int aio_cancel(int fd, struct aiocb *aiocbp);
int aio_error(const struct aiocb *aiocbp);
int aio_fsync(int op, struct aiocb *aiocbp);
int aio_read(struct aiocb *aiocbp);
ssize_t aio_return(struct aiocb *aiocbp);
int aio_suspend(const struct aiocb *const *aiocblist, int nent, const struct timespec *timeoutp);
int aio_write(struct aiocb *aiocbp);
unsigned int alarm(unsigned int);
void *aligned_alloc(size_t __alignment, size_t __size);
void *alloca(size_t __size);
int alphasort(const struct dirent **, const struct dirent **);
unsigned int arc4random(void);
void arc4random_addrandom(unsigned char *, int __datlen);
void arc4random_buf(void *__buf, size_t __nbytes);
void arc4random_stir(void);
unsigned int arc4random_uniform(unsigned int __upper_bound);
int ascii2addr(int, const char *, void *);
char *asctime(const struct tm *);
char *asctime_r(const struct tm *, char *);
int asprintf(char **, const char *, ...);
int asprintf_l(char **, locale_t, const char *, ...);
int at_quick_exit(void (*)(void));
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
au_asid_t audit_session_join(mach_port_name_t port);
int audit_session_port(au_asid_t asid, mach_port_name_t *portname);
mach_port_name_t audit_session_self(void);
int auditctl(const char *);
int auditon(int, void *, int);
int backtrace(void **, int __size);
size_t backtrace_async(void **array, size_t length, unsigned int *task_id);
int backtrace_from_fp(void *startfp, void **array, int size);
void backtrace_image_offsets(void *const *array, struct image_offset *image_offsets, int size);
char **backtrace_symbols(void *const *, int __size);
void backtrace_symbols_fd(void *const *, int __size, int);
char *basename(char *);
char *basename_r(const char *, char *);
int bcmp(const void *, const void *, size_t __n);
void bcopy(const void *, void *, size_t __n);
int bind(int, const struct sockaddr *, socklen_t);
int bindresvport(int, struct sockaddr_in *);
int bindresvport_sa(int, struct sockaddr *);
void *brk(const void *);
void (*bsd_signal(int, void (*)(int)))(int);
void *bsearch(const void *__key, const void *__base, size_t __nel, size_t __width, int (*__compar)(const void *, const void *));
wint_t btowc(int);
wint_t btowc_l(int, locale_t);
void bzero(void *, size_t __n);
void *calloc(size_t __count, size_t __size);
speed_t cfgetispeed(const struct termios *);
speed_t cfgetospeed(const struct termios *);
void cfmakeraw(struct termios *);
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
int chflags(const char *, __uint32_t);
int chmod(const char *, mode_t);
int chmodx_np(const char *, filesec_t);
int chown(const char *, uid_t, gid_t);
int chroot(const char *);
void clearerr(FILE *);
clock_t clock(void);
int clock_getres(clockid_t __clock_id, struct timespec *__res);
int clock_gettime(clockid_t __clock_id, struct timespec *__tp);
__uint64_t clock_gettime_nsec_np(clockid_t __clock_id);
int clock_settime(clockid_t __clock_id, const struct timespec *__tp);
int close(int);
int closedir(DIR *);
void closelog(void);
size_t confstr(int, char *, size_t __len);
int connect(int, const struct sockaddr *, socklen_t);
int connectx(int, const sa_endpoints_t *, sae_associd_t, unsigned int, const struct iovec *, unsigned int, size_t *, sae_connid_t *);
int creat(const char *, mode_t);
char *crypt(const char *, const char *);
char *ctermid(char *);
char *ctermid_r(char *);
char *ctime(const time_t *);
char *ctime_r(const time_t *, char *);
int daemon(int, int);
char *devname(dev_t, mode_t);
char *devname_r(dev_t, mode_t, char *buf, int len);
double difftime(time_t, time_t);
int digittoint(int _c);
int digittoint_l(int c, locale_t l);
int dirfd(DIR *dirp);
char *dirname(char *);
char *dirname_r(const char *, char *);
int disconnectx(int, sae_associd_t, sae_connid_t);
div_t div(int, int);
int dladdr(const void *, Dl_info *);
int dlclose(void *__handle);
char *dlerror(void);
void *dlopen(const char *__path, int __mode);
bool dlopen_preflight(const char *__path);
void *dlsym(void *__handle, const char *__symbol);
int dprintf(int, const char *, ...);
int dprintf_l(int, locale_t, const char *, ...);
double drand48(void);
int dup(int);
int dup2(int, int);
locale_t duplocale(locale_t);
char *ecvt(double, int, int *, int *);
void encrypt(char *, int);
void endgrent(void);
void endhostent(void);
void endnetent(void);
void endnetgrent(void);
void endprotoent(void);
void endpwent(void);
void endrpcent(void);
void endservent(void);
void endusershell(void);
void endutxent(void);
void endutxent_wtmp(void);
double erand48(unsigned short *);
_Noreturn void err(int, const char *, ...);
void err_set_exit(void (*)(int));
void err_set_file(void *);
_Noreturn void errc(int, int, const char *, ...);
_Noreturn void errx(int, const char *, ...);
int exchangedata(const char *, const char *, unsigned int);
int execl(const char *__path, const char *__arg0, ...);
int execle(const char *__path, const char *__arg0, ...);
int execlp(const char *__file, const char *__arg0, ...);
int execv(const char *__path, char *const *__argv);
int execvP(const char *__file, const char *__searchpath, char *const *__argv);
int execve(const char *__file, char *const *__argv, char *const *__envp);
int execvp(const char *__file, char *const *__argv);
_Noreturn void exit(int);
int faccessat(int, const char *, int, int);
int fchdir(int);
int fchflags(int, __uint32_t);
int fchmod(int, mode_t);
int fchmodat(int, const char *, mode_t, int);
int fchmodx_np(int, filesec_t);
int fchown(int, uid_t, gid_t);
int fchownat(int, const char *, uid_t, gid_t, int);
int fclose(FILE *);
int fcntl(int, int, ...);
char *fcvt(double, int, int *, int *);
int fdclosedir(DIR *);
FILE *fdopen(int, const char *);
DIR *fdopendir(int);
int fdscandir(int, struct dirent ***, int (*)(const struct dirent *), int (*)(const struct dirent **, const struct dirent **));
int feof(FILE *);
int ferror(FILE *);
char *fflagstostr(unsigned long);
int fflush(FILE *);
int ffs(int);
int ffsctl(int, unsigned long, void *, unsigned int);
int ffsl(long);
int ffsll(long long);
int fgetattrlist(int, void *, void *, size_t __attrBufSize, unsigned int);
int fgetc(FILE *);
char *fgetln(FILE *, size_t *__len);
int fgetpos(FILE *, fpos_t *);
char *fgets(char *, int __size, FILE *);
wint_t fgetwc(FILE *);
wint_t fgetwc_l(FILE *, locale_t);
int *fgetwln(FILE *, size_t *__len);
int *fgetwln_l(FILE *, size_t *, locale_t);
int *fgetws(int *, int __n, FILE *);
int *fgetws_l(int *, int __n, FILE *, locale_t);
ssize_t fgetxattr(int fd, const char *name, void *value, size_t size, u_int32_t position, int options);
int fhopen(const struct fhandle *, int);
int fileno(FILE *);
filesec_t filesec_dup(filesec_t);
void filesec_free(filesec_t);
int filesec_get_property(filesec_t, filesec_property_t, void *);
filesec_t filesec_init(void);
int filesec_query_property(filesec_t, filesec_property_t, int *);
int filesec_set_property(filesec_t, filesec_property_t, const void *);
int filesec_unset_property(filesec_t, filesec_property_t);
ssize_t flistxattr(int fd, char *namebuff, size_t size, int options);
int flock(int, int);
void flockfile(FILE *);
int fls(int);
int flsl(long);
int flsll(long long);
FILE *fmemopen(void *__buf, size_t __size, const char *__mode);
int fmount(const char *, int, int, void *);
const char *fmtcheck(const char *, const char *);
int fnmatch(const char *, const char *, int);
FILE *fopen(const char *__filename, const char *__mode);
pid_t fork(void);
long fpathconf(int, int);
int fprintf(FILE *, const char *, ...);
int fprintf_l(FILE *, locale_t, const char *, ...);
int fpurge(FILE *);
int fputc(int, FILE *);
int fputs(const char *, FILE *);
wint_t fputwc(int, FILE *);
wint_t fputwc_l(int, FILE *, locale_t);
int fputws(const int *, FILE *);
int fputws_l(const int *, FILE *, locale_t);
size_t fread(void *__ptr, size_t __size, size_t __nitems, FILE *__stream);
ssize_t freadlink(int, char *, size_t);
void free(void *);
void freeaddrinfo(struct addrinfo *);
void freehostent(struct hostent *);
void freeifaddrs(struct ifaddrs *);
void freeifmaddrs(struct ifmaddrs *);
int freelocale(locale_t);
int fremovexattr(int fd, const char *name, int options);
FILE *freopen(const char *, const char *, FILE *);
int fscanf(FILE *, const char *, ...);
int fscanf_l(FILE *, locale_t, const char *, ...);
int fsctl(const char *, unsigned long, void *, unsigned int);
int fseek(FILE *, long, int);
int fseeko(FILE *__stream, off_t __offset, int __whence);
int fsetattrlist(int, void *, void *, size_t __attrBufSize, unsigned int);
int fsetpos(FILE *, const fpos_t *);
int fsetxattr(int fd, const char *name, const void *value, size_t size, u_int32_t position, int options);
int fstat(int, struct stat *);
int fstatat(int, const char *, struct stat *, int);
int fstatfs(int, struct statfs *);
int fstatvfs(int, struct statvfs *);
int fstatx_np(int, struct stat *, filesec_t);
int fsync(int);
int fsync_volume_np(int, int);
long ftell(FILE *);
off_t ftello(FILE *__stream);
key_t ftok(const char *, int);
int ftruncate(int, off_t);
int ftrylockfile(FILE *);
FTSENT *fts_children(FTS *, int);
int fts_close(FTS *);
FTS *fts_open(char *const *, int, int (*)(const FTSENT **, const FTSENT **));
FTSENT *fts_read(FTS *);
int fts_set(FTS *, FTSENT *, int);
int ftw(const char *, int (*)(const char *, const struct stat *, int), int);
void funlockfile(FILE *);
int funmount(int, int);
FILE *funopen(const void *, int (*)(void *, char *, int __n), int (*)(void *, const char *, int __n), fpos_t (*)(void *, fpos_t, int), int (*)(void *));
int futimens(int __fd, const struct timespec *__times);
int futimes(int, const struct timeval *);
int fwide(FILE *, int);
int fwprintf(FILE *, const int *, ...);
int fwprintf_l(FILE *, locale_t, const int *, ...);
size_t fwrite(const void *__ptr, size_t __size, size_t __nitems, FILE *__stream);
int fwscanf(FILE *, const int *, ...);
int fwscanf_l(FILE *, locale_t, const int *, ...);
const char *gai_strerror(int);
char *gcvt(double, int, char *);
int getaddrinfo(const char *, const char *, const struct addrinfo *, struct addrinfo **);
int getattrlist(const char *, void *, void *, size_t __attrBufSize, unsigned int);
int getattrlistat(int, const char *, void *, void *, size_t, unsigned long);
int getattrlistbulk(int, void *, void *, size_t, unsigned long long);
int getaudit(struct auditinfo *);
int getaudit_addr(struct auditinfo_addr *, int);
int getauid(au_id_t *);
char *getbsize(int *, long *);
int getc(FILE *);
int getc_unlocked(FILE *);
int getchar(void);
int getchar_unlocked(void);
char *getcwd(char *, size_t __size);
struct tm *getdate(const char *);
ssize_t getdelim(char **__linep, size_t *__linecapp, int __delimiter, FILE *__stream);
int getdirentries(int, char *, int __nbytes, long *);
int getdirentriesattr(int, void *, void *, size_t __attrBufSize, unsigned int *, unsigned int *, unsigned int *, unsigned int);
int getdomainname(char *, int __namelen);
int getdtablesize(void);
gid_t getegid(void);
char *getenv(const char *);
uid_t geteuid(void);
int getfh(const char *, fhandle_t *);
int getfsstat(struct statfs *, int, int);
gid_t getgid(void);
struct group *getgrent(void);
struct group *getgrgid(gid_t);
int getgrgid_r(gid_t, struct group *, char *, size_t, struct group **);
struct group *getgrnam(const char *);
int getgrnam_r(const char *, struct group *, char *, size_t, struct group **);
int getgrouplist(const char *, int, int *, int *__ngroups);
int getgroups(int __gidsetsize, gid_t *);
struct group *getgruuid(uuid_t);
int getgruuid_r(uuid_t, struct group *, char *, size_t, struct group **);
struct hostent *gethostbyaddr(const void *, socklen_t, int);
struct hostent *gethostbyname(const char *);
struct hostent *gethostbyname2(const char *, int);
struct hostent *gethostent(void);
long gethostid(void);
int gethostname(char *, size_t __namelen);
int gethostuuid(uuid_t, const struct timespec *);
int getifaddrs(struct ifaddrs **);
int getifmaddrs(struct ifmaddrs **);
int getiopolicy_np(int, int);
struct hostent *getipnodebyaddr(const void *, size_t, int, int *);
struct hostent *getipnodebyname(const char *, int, int, int *);
int getipv4sourcefilter(int, struct in_addr, struct in_addr, unsigned int *, unsigned int *, struct in_addr *);
int getitimer(int, struct itimerval *);
struct lastlogx *getlastlogx(uid_t, struct lastlogx *);
struct lastlogx *getlastlogxbyname(const char *, struct lastlogx *);
ssize_t getline(char **__linep, size_t *__linecapp, FILE *__stream);
int getloadavg(double *, int __nelem);
char *getlogin(void);
int getlogin_r(char *, size_t __namelen);
int getmntinfo(struct statfs **, int);
int getmntinfo_r_np(struct statfs **, int);
mode_t getmode(const void *, mode_t);
int getnameinfo(const struct sockaddr *, socklen_t, char *, socklen_t, char *, socklen_t, int);
struct netent *getnetbyaddr(unsigned int, int);
struct netent *getnetbyname(const char *);
struct netent *getnetent(void);
int getnetgrent(char **, char **, char **);
int getopt(int __argc, char *const *, const char *);
int getopt_long(int __argc, char *const *, const char *, const struct option *, int *);
int getopt_long_only(int __argc, char *const *, const char *, const struct option *, int *);
int getpagesize(void);
char *getpass(const char *);
int getpeereid(int, uid_t *, gid_t *);
int getpeername(int, struct sockaddr *, socklen_t *);
pid_t getpgid(pid_t);
pid_t getpgrp(void);
pid_t getpid(void);
pid_t getppid(void);
int getpriority(int, id_t);
const char *getprogname(void);
struct protoent *getprotobyname(const char *);
struct protoent *getprotobynumber(int);
struct protoent *getprotoent(void);
struct passwd *getpwent(void);
struct passwd *getpwnam(const char *);
int getpwnam_r(const char *, struct passwd *, char *, size_t, struct passwd **);
struct passwd *getpwuid(uid_t);
int getpwuid_r(uid_t, struct passwd *, char *, size_t, struct passwd **);
struct passwd *getpwuuid(uuid_t);
int getpwuuid_r(uuid_t, struct passwd *, char *, size_t, struct passwd **);
int getrlimit(int, struct rlimit *);
struct rpcent *getrpcbyname(const char *name);
struct rpcent *getrpcbynumber(int number);
struct rpcent *getrpcent(void);
int getrusage(int, struct rusage *);
char *gets(char *);
struct servent *getservbyname(const char *, const char *);
struct servent *getservbyport(int, const char *);
struct servent *getservent(void);
int getsgroups_np(int *, uuid_t);
pid_t getsid(pid_t);
int getsockname(int, struct sockaddr *, socklen_t *);
int getsockopt(int, int, int, void *, socklen_t *);
int getsourcefilter(int, unsigned int, struct sockaddr *, socklen_t, unsigned int *, unsigned int *, struct sockaddr_storage *);
int getsubopt(char **, char *const *, char **);
int gettimeofday(struct timeval *, void *);
uid_t getuid(void);
char *getusershell(void);
void getutmp(const struct utmpx *, struct utmp *);
void getutmpx(const struct utmp *, struct utmpx *);
struct utmpx *getutxent(void);
struct utmpx *getutxent_wtmp(void);
struct utmpx *getutxid(const struct utmpx *);
struct utmpx *getutxline(const struct utmpx *);
int getvfsbyname(const char *, struct vfsconf *);
int getw(FILE *);
wint_t getwc(FILE *);
wint_t getwc_l(FILE *, locale_t);
wint_t getwchar(void);
wint_t getwchar_l(locale_t);
char *getwd(char *);
int getwgroups_np(int *, uuid_t);
ssize_t getxattr(const char *path, const char *name, void *value, size_t size, u_int32_t position, int options);
int glob(const char *, int, int (*)(const char *, int), glob_t *);
void globfree(glob_t *);
struct tm *gmtime(const time_t *);
struct tm *gmtime_r(const time_t *, struct tm *);
int grantpt(int);
char *group_from_gid(gid_t, int);
int hcreate(size_t);
void hdestroy(void);
int heapsort(void *__base, size_t __nel, size_t __width, int (*__compar)(const void *, const void *));
void herror(const char *);
ENTRY *hsearch(ENTRY, ACTION);
const char *hstrerror(int);
void if_freenameindex(struct if_nameindex *);
char *if_indextoname(unsigned int, char *);
struct if_nameindex *if_nameindex(void);
unsigned int if_nametoindex(const char *);
intmax_t imaxabs(intmax_t j);
imaxdiv_t imaxdiv(intmax_t __numer, intmax_t __denom);
char *index(const char *, int);
int inet6_opt_append(void *, socklen_t, int, __uint8_t, socklen_t, __uint8_t, void **);
int inet6_opt_find(void *, socklen_t, int, __uint8_t, socklen_t *, void **);
int inet6_opt_finish(void *, socklen_t, int);
int inet6_opt_get_val(void *, int, void *, socklen_t);
int inet6_opt_init(void *, socklen_t);
int inet6_opt_next(void *, socklen_t, int, __uint8_t *, socklen_t *, void **);
int inet6_opt_set_val(void *, int, void *, socklen_t);
__uint8_t *inet6_option_alloc(struct cmsghdr *, int, int, int);
int inet6_option_append(struct cmsghdr *, const __uint8_t *, int, int);
int inet6_option_find(const struct cmsghdr *, __uint8_t **, int);
int inet6_option_init(void *, struct cmsghdr **, int);
int inet6_option_next(const struct cmsghdr *, __uint8_t **);
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
in_addr_t inet_addr(const char *);
int inet_aton(const char *, struct in_addr *);
in_addr_t inet_lnaof(struct in_addr);
struct in_addr inet_makeaddr(in_addr_t, in_addr_t);
char *inet_net_ntop(int, const void *, int, char *, __darwin_size_t __size);
int inet_net_pton(int, const char *, void *, __darwin_size_t __size);
char *inet_neta(in_addr_t, char *, __darwin_size_t __size);
in_addr_t inet_netof(struct in_addr);
in_addr_t inet_network(const char *);
unsigned int inet_nsap_addr(const char *, unsigned char *, int __maxlen);
char *inet_nsap_ntoa(int __binlen, const unsigned char *, char *);
char *inet_ntoa(struct in_addr);
const char *inet_ntop(int, const void *, char *, socklen_t __size);
int inet_pton(int, const char *, void *);
int initgroups(const char *, int);
char *initstate(unsigned int, char *, size_t __size);
int innetgr(const char *, const char *, const char *, const char *);
void insque(void *, void *);
int ioctl(int, unsigned long, ...);
int iruserok(unsigned long, int, const char *, const char *);
int iruserok_sa(const void *, int, int, const char *, const char *);
int isalnum(int _c);
int isalnum_l(int c, locale_t l);
int isalpha(int _c);
int isalpha_l(int c, locale_t l);
int isascii(int _c);
int isatty(int);
int isblank(int _c);
int isblank_l(int c, locale_t l);
int iscntrl(int _c);
int iscntrl_l(int c, locale_t l);
int isdigit(int _c);
int isdigit_l(int c, locale_t l);
int isgraph(int _c);
int isgraph_l(int c, locale_t l);
int ishexnumber(int _c);
int ishexnumber_l(int c, locale_t l);
int isideogram(int _c);
int isideogram_l(int c, locale_t l);
int islower(int _c);
int islower_l(int c, locale_t l);
int isnumber(int _c);
int isnumber_l(int c, locale_t l);
int isphonogram(int _c);
int isphonogram_l(int c, locale_t l);
int isprint(int _c);
int isprint_l(int c, locale_t l);
int ispunct(int _c);
int ispunct_l(int c, locale_t l);
int isrune(int _c);
int isrune_l(int c, locale_t l);
int issetugid(void);
int isspace(int _c);
int isspace_l(int c, locale_t l);
int isspecial(int _c);
int isspecial_l(int c, locale_t l);
int isupper(int _c);
int isupper_l(int c, locale_t l);
int iswalnum(wint_t _wc);
int iswalnum_l(wint_t _wc, locale_t _l);
int iswalpha(wint_t _wc);
int iswalpha_l(wint_t _wc, locale_t _l);
int iswascii(wint_t _wc);
int iswblank(wint_t _wc);
int iswblank_l(wint_t _wc, locale_t _l);
int iswcntrl(wint_t _wc);
int iswcntrl_l(wint_t _wc, locale_t _l);
int iswctype(wint_t _wc, wctype_t _charclass);
int iswctype_l(wint_t _wc, wctype_t _charclass, locale_t _l);
int iswdigit(wint_t _wc);
int iswdigit_l(wint_t _wc, locale_t _l);
int iswgraph(wint_t _wc);
int iswgraph_l(wint_t _wc, locale_t _l);
int iswhexnumber(wint_t _wc);
int iswhexnumber_l(wint_t _wc, locale_t _l);
int iswideogram(wint_t _wc);
int iswideogram_l(wint_t _wc, locale_t _l);
int iswlower(wint_t _wc);
int iswlower_l(wint_t _wc, locale_t _l);
int iswnumber(wint_t _wc);
int iswnumber_l(wint_t _wc, locale_t _l);
int iswphonogram(wint_t _wc);
int iswphonogram_l(wint_t _wc, locale_t _l);
int iswprint(wint_t _wc);
int iswprint_l(wint_t _wc, locale_t _l);
int iswpunct(wint_t _wc);
int iswpunct_l(wint_t _wc, locale_t _l);
int iswrune(wint_t _wc);
int iswrune_l(wint_t _wc, locale_t _l);
int iswspace(wint_t _wc);
int iswspace_l(wint_t _wc, locale_t _l);
int iswspecial(wint_t _wc);
int iswspecial_l(wint_t _wc, locale_t _l);
int iswupper(wint_t _wc);
int iswupper_l(wint_t _wc, locale_t _l);
int iswxdigit(wint_t _wc);
int iswxdigit_l(wint_t _wc, locale_t _l);
int isxdigit(int _c);
int isxdigit_l(int c, locale_t l);
long jrand48(unsigned short *);
int kevent(int kq, const struct kevent *changelist, int nchanges, struct kevent *eventlist, int nevents, const struct timespec *timeout);
int kevent64(int kq, const struct kevent64_s *changelist, int nchanges, struct kevent64_s *eventlist, int nevents, unsigned int flags, const struct timespec *timeout);
int kill(pid_t, int);
int killpg(pid_t, int);
int kqueue(void);
char *l64a(long);
long labs(long);
int lchflags(const char *, __uint32_t);
int lchmod(const char *, mode_t);
int lchown(const char *, uid_t, gid_t);
void lcong48(unsigned short *);
ldiv_t ldiv(long, long);
void *lfind(const void *, const void *, size_t *, size_t, int (*)(const void *, const void *));
int link(const char *, const char *);
int linkat(int, const char *, int, const char *, int);
int lio_listio(int mode, struct aiocb *const *aiocblist, int nent, struct sigevent *sigp);
int listen(int, int);
ssize_t listxattr(const char *path, char *namebuff, size_t size, int options);
long long llabs(long long);
lldiv_t lldiv(long long, long long);
struct lconv *localeconv(void);
struct lconv *localeconv_l(locale_t);
struct tm *localtime(const time_t *);
struct tm *localtime_r(const time_t *, struct tm *);
int lockf(int, int, off_t);
_Noreturn void longjmp(jmp_buf, int);
void longjmperror(void);
long lrand48(void);
void *lsearch(const void *, void *, size_t *, size_t, int (*)(const void *, const void *));
off_t lseek(int, off_t, int);
int lstat(const char *, struct stat *);
int lstatx_np(const char *, struct stat *, filesec_t);
int lutimes(const char *, const struct timeval *);
unsigned long long mach_absolute_time(void);
unsigned long long mach_approximate_time(void);
unsigned long long mach_continuous_approximate_time(void);
unsigned long long mach_continuous_time(void);
mach_msg_return_t mach_msg(mach_msg_header_t *msg, mach_msg_option_t option, mach_msg_size_t send_size, mach_msg_size_t rcv_size, mach_port_name_t rcv_name, mach_msg_timeout_t timeout, mach_port_name_t notify);
mach_msg_return_t mach_msg_overwrite(mach_msg_header_t *msg, mach_msg_option_t option, mach_msg_size_t send_size, mach_msg_size_t rcv_size, mach_port_name_t rcv_name, mach_msg_timeout_t timeout, mach_port_name_t notify, mach_msg_header_t *rcv_msg, mach_msg_size_t rcv_limit);
kern_return_t mach_timebase_info(mach_timebase_info_t info);
kern_return_t mach_voucher_deallocate(mach_port_name_t voucher);
kern_return_t mach_wait_until(unsigned long long deadline);
int madvise(void *, size_t, int);
void *malloc(size_t __size);
malloc_zone_t *malloc_create_zone(vm_size_t start_size, unsigned int flags);
malloc_zone_t *malloc_default_purgeable_zone(void);
malloc_zone_t *malloc_default_zone(void);
void malloc_destroy_zone(malloc_zone_t *zone);
kern_return_t malloc_get_all_zones(task_t task, memory_reader_t reader, vm_address_t **addresses, unsigned int *count);
const char *malloc_get_zone_name(malloc_zone_t *zone);
size_t malloc_good_size(size_t size);
int malloc_make_nonpurgeable(void *ptr);
void malloc_make_purgeable(void *ptr);
void malloc_printf(const char *format, ...);
void malloc_set_zone_name(malloc_zone_t *zone, const char *name);
size_t malloc_size(const void *ptr);
void *malloc_type_aligned_alloc(size_t alignment, size_t size, malloc_type_id_t type_id);
void *malloc_type_calloc(size_t count, size_t size, malloc_type_id_t type_id);
void malloc_type_free(void *ptr, malloc_type_id_t type_id);
void *malloc_type_malloc(size_t size, malloc_type_id_t type_id);
int malloc_type_posix_memalign(void **memptr, size_t alignment, size_t size, malloc_type_id_t type_id);
void *malloc_type_realloc(void *ptr, size_t size, malloc_type_id_t type_id);
void *malloc_type_valloc(size_t size, malloc_type_id_t type_id);
void *malloc_type_zone_calloc(malloc_zone_t *zone, size_t count, size_t size, malloc_type_id_t type_id);
void malloc_type_zone_free(malloc_zone_t *zone, void *ptr, malloc_type_id_t type_id);
void *malloc_type_zone_malloc(malloc_zone_t *zone, size_t size, malloc_type_id_t type_id);
void *malloc_type_zone_malloc_with_options(malloc_zone_t *zone, size_t alignment, size_t size, malloc_type_id_t type_id, malloc_zone_malloc_options_t opts);
void *malloc_type_zone_memalign(malloc_zone_t *zone, size_t alignment, size_t size, malloc_type_id_t type_id);
void *malloc_type_zone_realloc(malloc_zone_t *zone, void *ptr, size_t size, malloc_type_id_t type_id);
void *malloc_type_zone_valloc(malloc_zone_t *zone, size_t size, malloc_type_id_t type_id);
void malloc_zone_batch_free(malloc_zone_t *zone, void **to_be_freed, unsigned int num);
unsigned int malloc_zone_batch_malloc(malloc_zone_t *zone, size_t size, void **results, unsigned int num_requested);
void *malloc_zone_calloc(malloc_zone_t *zone, size_t num_items, size_t size);
boolean_t malloc_zone_check(malloc_zone_t *zone);
void malloc_zone_disable_discharge_checking(malloc_zone_t *zone);
void malloc_zone_discharge(malloc_zone_t *zone, void *memory);
boolean_t malloc_zone_enable_discharge_checking(malloc_zone_t *zone);
void malloc_zone_enumerate_discharged_pointers(malloc_zone_t *zone, void *);
void malloc_zone_free(malloc_zone_t *zone, void *ptr);
malloc_zone_t *malloc_zone_from_ptr(const void *ptr);
void malloc_zone_log(malloc_zone_t *zone, void *address);
void *malloc_zone_malloc(malloc_zone_t *zone, size_t size);
void *malloc_zone_malloc_with_options(malloc_zone_t *zone, size_t align, size_t size, malloc_zone_malloc_options_t opts);
void *malloc_zone_memalign(malloc_zone_t *zone, size_t alignment, size_t size);
size_t malloc_zone_pressure_relief(malloc_zone_t *zone, size_t goal);
void malloc_zone_print(malloc_zone_t *zone, boolean_t verbose);
void malloc_zone_print_ptr_info(void *ptr);
void *malloc_zone_realloc(malloc_zone_t *zone, void *ptr, size_t size);
void malloc_zone_register(malloc_zone_t *zone);
void malloc_zone_statistics(malloc_zone_t *zone, malloc_statistics_t *stats);
void malloc_zone_unregister(malloc_zone_t *zone);
void *malloc_zone_valloc(malloc_zone_t *zone, size_t size);
int mblen(const char *__s, size_t __n);
int mblen_l(const char *, size_t __n, locale_t);
size_t mbrlen(const char *, size_t __n, mbstate_t *);
size_t mbrlen_l(const char *, size_t __n, mbstate_t *, locale_t);
size_t mbrtowc(int *, const char *, size_t __n, mbstate_t *);
size_t mbrtowc_l(int *, const char *, size_t __n, mbstate_t *, locale_t);
int mbsinit(const mbstate_t *);
int mbsinit_l(const mbstate_t *, locale_t);
size_t mbsnrtowcs(int *, const char **, size_t, size_t __len, mbstate_t *);
size_t mbsnrtowcs_l(int *, const char **, size_t, size_t __len, mbstate_t *, locale_t);
size_t mbsrtowcs(int *, const char **, size_t __len, mbstate_t *);
size_t mbsrtowcs_l(int *, const char **, size_t __len, mbstate_t *, locale_t);
size_t mbstowcs(int *, const char *, size_t __n);
size_t mbstowcs_l(int *, const char *, size_t __n, locale_t);
int mbtowc(int *, const char *, size_t __n);
int mbtowc_l(int *, const char *, size_t __n, locale_t);
void *memccpy(void *__dst, const void *__src, int __c, size_t __n);
void *memchr(const void *__s, int __c, size_t __n);
int memcmp(const void *__s1, const void *__s2, size_t __n);
void *memcpy(void *__dst, const void *__src, size_t __n);
void *memmem(const void *__big, size_t __big_len, const void *__little, size_t __little_len);
void *memmove(void *__dst, const void *__src, size_t __len);
void *memset(void *__b, int __c, size_t __len);
void memset_pattern16(void *__b, const void *__pattern16, size_t __len);
void memset_pattern4(void *__b, const void *__pattern4, size_t __len);
void memset_pattern8(void *__b, const void *__pattern8, size_t __len);
errno_t memset_s(void *__s, rsize_t __smax, int __c, rsize_t __n);
int mergesort(void *__base, size_t __nel, size_t __width, int (*__compar)(const void *, const void *));
int mincore(const void *, size_t, char *);
int minherit(void *, size_t, int);
int mkdir(const char *, mode_t);
int mkdirat(int, const char *, mode_t);
int mkdirx_np(const char *, filesec_t);
char *mkdtemp(char *);
char *mkdtempat_np(int dfd, char *path);
int mkfifo(const char *, mode_t);
int mkfifoat(int, const char *, mode_t);
int mkfifox_np(const char *, filesec_t);
int mknod(const char *, mode_t, dev_t);
int mknodat(int, const char *, mode_t, dev_t);
int mkostemp(char *path, int oflags);
int mkostemps(char *path, int slen, int oflags);
int mkostempsat_np(int dfd, char *path, int slen, int oflags);
int mkpath_np(const char *path, mode_t omode);
int mkpathat_np(int dfd, const char *path, mode_t omode);
int mkstemp(char *);
int mkstemp_dprotected_np(char *path, int dpclass, int dpflags);
int mkstemps(char *, int);
int mkstempsat_np(int dfd, char *path, int slen);
char *mktemp(char *);
time_t mktime(struct tm *);
int mlock(const void *, size_t);
int mlockall(int);
void *mmap(void *, size_t, int, int, int, off_t);
int mount(const char *, const char *, int, void *);
int mprotect(void *, size_t, int);
long mrand48(void);
int msgctl(int, int, struct __msqid_ds_new *);
int msgget(key_t, int);
ssize_t msgrcv(int, void *, size_t, long, int);
int msgsnd(int, const void *, size_t, int);
int msgsys(int, ...);
struct mstats mstats(void);
int msync(void *, size_t, int);
int munlock(const void *, size_t);
int munlockall(void);
int munmap(void *, size_t);
int nanosleep(const struct timespec *__rqtp, struct timespec *__rmtp);
locale_t newlocale(int, const char *, locale_t);
wint_t nextwctype(wint_t, wctype_t);
wint_t nextwctype_l(wint_t, wctype_t, locale_t);
int nfssvc(int, void *);
int nftw(const char *, int (*)(const char *, const struct stat *, int, struct FTW *), int, int);
int nice(int);
char *nl_langinfo(nl_item);
char *nl_langinfo_l(nl_item, locale_t);
long nrand48(unsigned short *);
int open(const char *, int, ...);
int open_dprotected_np(const char *, int, int, int, ...);
FILE *open_memstream(char **__bufp, size_t *__sizep);
FILE *open_wmemstream(int **__bufp, size_t *__sizep);
int openat(int, const char *, int, ...);
int openat_authenticated_np(int, const char *, int, int);
int openat_dprotected_np(int, const char *, int, int, int, ...);
DIR *opendir(const char *);
void openlog(const char *, int, int);
int openx_np(const char *, int, filesec_t);
void os_unfair_lock_assert_not_owner(const os_unfair_lock *lock);
void os_unfair_lock_assert_owner(const os_unfair_lock *lock);
void os_unfair_lock_lock(os_unfair_lock_t lock);
void os_unfair_lock_lock_with_flags(os_unfair_lock_t lock, os_unfair_lock_flags_t flags);
bool os_unfair_lock_trylock(os_unfair_lock_t lock);
void os_unfair_lock_unlock(os_unfair_lock_t lock);
long pathconf(const char *, int);
int pause(void);
int pclose(FILE *);
void perror(const char *);
void pfctlinput(int, struct sockaddr *);
int pipe(int *);
int poll(struct pollfd *, nfds_t, int);
FILE *popen(const char *, const char *);
time_t posix2time(time_t);
int posix_madvise(void *, size_t, int);
int posix_memalign(void **__memptr, size_t __alignment, size_t __size);
int posix_openpt(int);
int posix_spawn(pid_t *, const char *, const posix_spawn_file_actions_t *, const posix_spawnattr_t *, char *const *__argv, char *const *__envp);
int posix_spawn_file_actions_addchdir(posix_spawn_file_actions_t *, const char *);
int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t *, const char *);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *, int);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *, int, int);
int posix_spawn_file_actions_addfchdir(posix_spawn_file_actions_t *, int);
int posix_spawn_file_actions_addfchdir_np(posix_spawn_file_actions_t *, int);
int posix_spawn_file_actions_addinherit_np(posix_spawn_file_actions_t *, int);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *, int, const char *, int, mode_t);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *);
int posix_spawn_file_actions_init(posix_spawn_file_actions_t *);
int posix_spawnattr_destroy(posix_spawnattr_t *);
int posix_spawnattr_getarchpref_np(const posix_spawnattr_t *, size_t, cpu_type_t *, cpu_subtype_t *, size_t *);
int posix_spawnattr_getbinpref_np(const posix_spawnattr_t *, size_t, cpu_type_t *, size_t *);
int posix_spawnattr_getflags(const posix_spawnattr_t *, short *);
int posix_spawnattr_getpgroup(const posix_spawnattr_t *, pid_t *);
int posix_spawnattr_getsigdefault(const posix_spawnattr_t *, sigset_t *);
int posix_spawnattr_getsigmask(const posix_spawnattr_t *, sigset_t *);
int posix_spawnattr_init(posix_spawnattr_t *);
int posix_spawnattr_set_4k_page_size_np(const posix_spawnattr_t *);
int posix_spawnattr_set_csm_np(const posix_spawnattr_t *attr, unsigned int flags);
int posix_spawnattr_setarchpref_np(posix_spawnattr_t *, size_t, cpu_type_t *, cpu_subtype_t *, size_t *);
int posix_spawnattr_setauditsessionport_np(posix_spawnattr_t *, mach_port_t);
int posix_spawnattr_setbinpref_np(posix_spawnattr_t *, size_t, cpu_type_t *, size_t *);
int posix_spawnattr_setexceptionports_np(posix_spawnattr_t *, exception_mask_t, mach_port_t, exception_behavior_t, thread_state_flavor_t);
int posix_spawnattr_setflags(posix_spawnattr_t *, short);
int posix_spawnattr_setnosmt_np(const posix_spawnattr_t *);
int posix_spawnattr_setpgroup(posix_spawnattr_t *, pid_t);
int posix_spawnattr_setsigdefault(posix_spawnattr_t *, const sigset_t *);
int posix_spawnattr_setsigmask(posix_spawnattr_t *, const sigset_t *);
int posix_spawnattr_setspecialport_np(posix_spawnattr_t *, mach_port_t, int);
int posix_spawnp(pid_t *, const char *, const posix_spawn_file_actions_t *, const posix_spawnattr_t *, char *const *__argv, char *const *__envp);
ssize_t pread(int __fd, void *__buf, size_t __nbyte, off_t __offset);
ssize_t preadv(int, const struct iovec *, int, off_t);
int printf(const char *, ...);
int printf_l(locale_t, const char *, ...);
int proc_clear_dirty(pid_t pid, unsigned int flags);
int proc_get_dirty(pid_t pid, unsigned int *flags);
int proc_kmsgbuf(void *buffer, unsigned int buffersize);
int proc_libversion(int *major, int *minor);
int proc_listallpids(void *buffer, int buffersize);
int proc_listchildpids(pid_t ppid, void *buffer, int buffersize);
int proc_listpgrppids(pid_t pgrpid, void *buffer, int buffersize);
int proc_listpids(unsigned int type, unsigned int typeinfo, void *buffer, int buffersize);
int proc_listpidspath(unsigned int type, unsigned int typeinfo, const char *path, unsigned int pathflags, void *buffer, int buffersize);
int proc_name(int pid, void *buffer, unsigned int buffersize);
int proc_pid_rusage(int pid, int flavor, rusage_info_t *buffer);
int proc_pidfdinfo(int pid, int fd, int flavor, void *buffer, int buffersize);
int proc_pidfileportinfo(int pid, unsigned int fileport, int flavor, void *buffer, int buffersize);
int proc_pidinfo(int pid, int flavor, unsigned long long arg, void *buffer, int buffersize);
int proc_pidpath(int pid, void *buffer, unsigned int buffersize);
int proc_pidpath_audittoken(audit_token_t *audittoken, void *buffer, unsigned int buffersize);
int proc_regionfilename(int pid, unsigned long long address, void *buffer, unsigned int buffersize);
int proc_set_csm(unsigned int flags);
int proc_set_dirty(pid_t pid, bool dirty);
int proc_set_no_smt(void);
int proc_setpcontrol(const int control);
int proc_setthread_csm(unsigned int flags);
int proc_setthread_no_smt(void);
int proc_signal_delegate(audit_token_t instigator, audit_token_t target, int sig);
int proc_signal_with_audittoken(audit_token_t *audittoken, int sig);
int proc_terminate(pid_t pid, int *sig);
int proc_terminate_all_rsr(int sig);
int proc_terminate_delegate(audit_token_t instigator, audit_token_t target, int *sig);
int proc_terminate_with_audittoken(audit_token_t *audittoken, int *sig);
int proc_track_dirty(pid_t pid, unsigned int flags);
int proc_udata_info(int pid, int flavor, void *buffer, int buffersize);
int profil(char *, size_t __bufsiz, unsigned long, unsigned int);
int pselect(int, fd_set *, fd_set *, fd_set *, const struct timespec *, const sigset_t *);
void psignal(int, const char *);
void psort(void *__base, size_t __nel, size_t __width, int (*__compar)(const void *, const void *));
void psort_r(void *__base, size_t __nel, size_t __width, void *, int (*__compar)(void *, const void *, const void *));
int pthread_atfork(void (*)(void), void (*)(void), void (*)(void));
int pthread_attr_destroy(pthread_attr_t *);
int pthread_attr_get_qos_class_np(pthread_attr_t *__attr, qos_class_t *__qos_class, int *__relative_priority);
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
int pthread_attr_set_qos_class_np(pthread_attr_t *__attr, qos_class_t __qos_class, int __relative_priority);
int pthread_attr_setdetachstate(pthread_attr_t *, int);
int pthread_attr_setguardsize(pthread_attr_t *, size_t);
int pthread_attr_setinheritsched(pthread_attr_t *, int);
int pthread_attr_setschedparam(pthread_attr_t *, const struct sched_param *);
int pthread_attr_setschedpolicy(pthread_attr_t *, int);
int pthread_attr_setscope(pthread_attr_t *, int);
int pthread_attr_setstack(pthread_attr_t *, void *, size_t);
int pthread_attr_setstackaddr(pthread_attr_t *, void *);
int pthread_attr_setstacksize(pthread_attr_t *, size_t);
int pthread_cancel(pthread_t);
int pthread_cond_broadcast(pthread_cond_t *);
int pthread_cond_destroy(pthread_cond_t *);
int pthread_cond_init(pthread_cond_t *, const pthread_condattr_t *);
int pthread_cond_signal(pthread_cond_t *);
int pthread_cond_signal_thread_np(pthread_cond_t *, pthread_t);
int pthread_cond_timedwait(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
int pthread_cond_timedwait_relative_np(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
int pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
int pthread_condattr_destroy(pthread_condattr_t *);
int pthread_condattr_getpshared(const pthread_condattr_t *, int *);
int pthread_condattr_init(pthread_condattr_t *);
int pthread_condattr_setpshared(pthread_condattr_t *, int);
int pthread_cpu_number_np(size_t *cpu_number_out);
int pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int pthread_create_suspended_np(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int pthread_detach(pthread_t);
int pthread_equal(pthread_t, pthread_t);
_Noreturn void pthread_exit(void *);
pthread_t pthread_from_mach_thread_np(mach_port_t);
int pthread_get_qos_class_np(pthread_t __pthread, qos_class_t *__qos_class, int *__relative_priority);
void *pthread_get_stackaddr_np(pthread_t);
size_t pthread_get_stacksize_np(pthread_t);
int pthread_getconcurrency(void);
int pthread_getname_np(pthread_t, char *, size_t);
int pthread_getschedparam(pthread_t, int *, struct sched_param *);
void *pthread_getspecific(pthread_key_t);
int pthread_getugid_np(uid_t *, gid_t *);
int pthread_is_threaded_np(void);
void pthread_jit_write_freeze_callbacks_np(void);
void pthread_jit_write_protect_np(int enabled);
int pthread_jit_write_protect_supported_np(void);
int pthread_jit_write_with_callback_np(pthread_jit_write_callback_t callback, void *ctx);
int pthread_join(pthread_t, void **);
int pthread_key_create(pthread_key_t *, void (*)(void *));
int pthread_key_delete(pthread_key_t);
int pthread_kill(pthread_t, int);
mach_port_t pthread_mach_thread_np(pthread_t);
int pthread_main_np(void);
int pthread_mutex_destroy(pthread_mutex_t *);
int pthread_mutex_getprioceiling(const pthread_mutex_t *, int *);
int pthread_mutex_init(pthread_mutex_t *, const pthread_mutexattr_t *);
int pthread_mutex_lock(pthread_mutex_t *);
int pthread_mutex_setprioceiling(pthread_mutex_t *, int, int *);
int pthread_mutex_trylock(pthread_mutex_t *);
int pthread_mutex_unlock(pthread_mutex_t *);
int pthread_mutexattr_destroy(pthread_mutexattr_t *);
int pthread_mutexattr_getpolicy_np(const pthread_mutexattr_t *, int *);
int pthread_mutexattr_getprioceiling(const pthread_mutexattr_t *, int *);
int pthread_mutexattr_getprotocol(const pthread_mutexattr_t *, int *);
int pthread_mutexattr_getpshared(const pthread_mutexattr_t *, int *);
int pthread_mutexattr_gettype(const pthread_mutexattr_t *, int *);
int pthread_mutexattr_init(pthread_mutexattr_t *);
int pthread_mutexattr_setpolicy_np(pthread_mutexattr_t *, int);
int pthread_mutexattr_setprioceiling(pthread_mutexattr_t *, int);
int pthread_mutexattr_setprotocol(pthread_mutexattr_t *, int);
int pthread_mutexattr_setpshared(pthread_mutexattr_t *, int);
int pthread_mutexattr_settype(pthread_mutexattr_t *, int);
int pthread_once(pthread_once_t *, void (*)(void));
int pthread_override_qos_class_end_np(pthread_override_t __override);
pthread_override_t pthread_override_qos_class_start_np(pthread_t __pthread, qos_class_t __qos_class, int __relative_priority);
int pthread_rwlock_destroy(pthread_rwlock_t *);
int pthread_rwlock_init(pthread_rwlock_t *, const pthread_rwlockattr_t *);
int pthread_rwlock_rdlock(pthread_rwlock_t *);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *);
int pthread_rwlock_trywrlock(pthread_rwlock_t *);
int pthread_rwlock_unlock(pthread_rwlock_t *);
int pthread_rwlock_wrlock(pthread_rwlock_t *);
int pthread_rwlockattr_destroy(pthread_rwlockattr_t *);
int pthread_rwlockattr_getpshared(const pthread_rwlockattr_t *, int *);
int pthread_rwlockattr_init(pthread_rwlockattr_t *);
int pthread_rwlockattr_setpshared(pthread_rwlockattr_t *, int);
pthread_t pthread_self(void);
int pthread_set_qos_class_self_np(qos_class_t __qos_class, int __relative_priority);
int pthread_setcancelstate(int, int *);
int pthread_setcanceltype(int, int *);
int pthread_setconcurrency(int);
int pthread_setname_np(const char *);
int pthread_setschedparam(pthread_t, int, const struct sched_param *);
int pthread_setspecific(pthread_key_t, const void *);
int pthread_setugid_np(uid_t, gid_t);
int pthread_sigmask(int, const sigset_t *, sigset_t *);
void pthread_testcancel(void);
int pthread_threadid_np(pthread_t, __uint64_t *);
void pthread_yield_np(void);
int ptrace(int _request, pid_t _pid, caddr_t _addr, int _data);
char *ptsname(int);
int ptsname_r(int fildes, char *buffer, size_t buflen);
int putc(int, FILE *);
int putc_unlocked(int, FILE *);
int putchar(int);
int putchar_unlocked(int);
int putenv(char *);
int puts(const char *);
struct utmpx *pututxline(const struct utmpx *);
int putw(int, FILE *);
wint_t putwc(int, FILE *);
wint_t putwc_l(int, FILE *, locale_t);
wint_t putwchar(int);
wint_t putwchar_l(int, locale_t);
ssize_t pwrite(int __fd, const void *__buf, size_t __nbyte, off_t __offset);
ssize_t pwritev(int, const struct iovec *, int, off_t);
qos_class_t qos_class_main(void);
qos_class_t qos_class_self(void);
void qsort(void *__base, size_t __nel, size_t __width, int (*__compar)(const void *, const void *));
void qsort_r(void *__base, size_t __nel, size_t __width, void *, int (*__compar)(void *, const void *, const void *));
const char *querylocale(int, locale_t);
_Noreturn void quick_exit(int);
int radixsort(const unsigned char **__base, int __nel, const unsigned char *__table, unsigned int __endbyte);
int raise(int);
int rand(void);
int rand_r(unsigned int *);
long random(void);
int rcmd(char **, int, const char *, const char *, const char *, int *);
int rcmd_af(char **, int, const char *, const char *, const char *, int *, int);
ssize_t read(int, void *, size_t __nbyte);
struct dirent *readdir(DIR *);
int readdir_r(DIR *, struct dirent *, struct dirent **);
ssize_t readlink(const char *, char *, size_t __bufsize);
ssize_t readlinkat(int, const char *, char *, size_t);
ssize_t readv(int, const struct iovec *, int);
void *realloc(void *__ptr, size_t __size);
void *reallocf(void *__ptr, size_t __size);
char *realpath(const char *, char *) __asm__("realpath$DARWIN_EXTSN");
int reboot(int);
ssize_t recv(int, void *, size_t, int);
ssize_t recvfrom(int, void *, size_t, int, struct sockaddr *, socklen_t *);
ssize_t recvmsg(int, struct msghdr *, int);
int regcomp(regex_t *, const char *, int);
int regcomp_l(regex_t *, const char *, int, locale_t);
size_t regerror(int, const regex_t *, char *, size_t __errbuf_size);
int regexec(const regex_t *, const char *, size_t __nmatch, regmatch_t *__pmatch, int);
void regfree(regex_t *);
int regncomp(regex_t *, const char *, size_t __len, int);
int regncomp_l(regex_t *, const char *, size_t __len, int, locale_t);
int regnexec(const regex_t *, const char *, size_t __len, size_t __nmatch, regmatch_t *__pmatch, int);
int regwcomp(regex_t *, const int *, int);
int regwcomp_l(regex_t *, const int *, int, locale_t);
int regwexec(const regex_t *, const int *, size_t __nmatch, regmatch_t *__pmatch, int);
int regwncomp(regex_t *, const int *, size_t __len, int);
int regwnexec(const regex_t *, const int *, size_t __len, size_t __nmatch, regmatch_t *__pmatch, int);
int regwnexec_l(const regex_t *, const int *, size_t __len, size_t __nmatch, regmatch_t *__pmatch, int, locale_t);
int remove(const char *);
int removexattr(const char *path, const char *name, int options);
void remque(void *);
int rename(const char *__old, const char *__new);
int renameat(int, const char *, int, const char *);
int renameatx_np(int, const char *, int, const char *, unsigned int);
int renamex_np(const char *, const char *, unsigned int);
int revoke(const char *);
void rewind(FILE *);
void rewinddir(DIR *);
char *rindex(const char *, int);
int rmdir(const char *);
int rpmatch(const char *);
int rresvport(int *);
int rresvport_af(int *, int);
int ruserok(const char *, int, const char *, const char *);
void *sbrk(int);
int scandir(const char *, struct dirent ***, int (*)(const struct dirent *), int (*)(const struct dirent **, const struct dirent **));
int scandirat(int, const char *, struct dirent ***, int (*)(const struct dirent *), int (*)(const struct dirent **, const struct dirent **));
int scanf(const char *, ...);
int scanf_l(locale_t, const char *, ...);
int sched_get_priority_max(int);
int sched_get_priority_min(int);
int sched_yield(void);
int searchfs(const char *, struct fssearchblock *, unsigned long *, unsigned int, unsigned int, struct searchstate *);
unsigned short *seed48(unsigned short *);
void seekdir(DIR *, long);
int select(int, fd_set *, fd_set *, fd_set *, struct timeval *);
int sem_close(sem_t *);
int sem_destroy(sem_t *);
int sem_getvalue(sem_t *, int *);
int sem_init(sem_t *, int, unsigned int);
sem_t *sem_open(const char *, int, ...);
int sem_post(sem_t *);
int sem_trywait(sem_t *);
int sem_unlink(const char *);
int sem_wait(sem_t *);
int semctl(int, int, int, ...);
int semget(key_t, int, int);
int semop(int, struct sembuf *, size_t);
int semsys(int, ...);
ssize_t send(int, const void *, size_t, int);
int sendfile(int, int, off_t, off_t *, struct sf_hdtr *, int);
ssize_t sendmsg(int, const struct msghdr *, int);
ssize_t sendto(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
int setattrlist(const char *, void *, void *, size_t __attrBufSize, unsigned int);
int setattrlistat(int, const char *, void *, void *, size_t, unsigned int);
int setaudit(const struct auditinfo *);
int setaudit_addr(const struct auditinfo_addr *, int);
int setauid(const au_id_t *);
void setbuf(FILE *, char *);
void setbuffer(FILE *, char *, int __size);
int setdomainname(const char *, int __namelen);
int setegid(gid_t);
int setenv(const char *__name, const char *__value, int __overwrite);
int seteuid(uid_t);
int setgid(gid_t);
void setgrent(void);
void setgrfile(const char *);
int setgroupent(int);
int setgroups(int, const gid_t *);
void sethostent(int);
void sethostid(long);
int sethostname(const char *, int __namelen);
int setiopolicy_np(int, int, int);
int setipv4sourcefilter(int, struct in_addr, struct in_addr, unsigned int, unsigned int, struct in_addr *);
int setitimer(int, const struct itimerval *, struct itimerval *);
int setjmp(jmp_buf);
void setkey(const char *);
int setlinebuf(FILE *);
char *setlocale(int, const char *);
int setlogin(const char *);
int setlogmask(int);
void *setmode(const char *);
void setnetent(int);
void setnetgrent(const char *);
int setpassent(int);
int setpgid(pid_t, pid_t);
pid_t setpgrp(void);
int setpriority(int, id_t, int);
void setprogname(const char *);
void setprotoent(int);
void setpwent(void);
int setregid(gid_t, gid_t);
int setreuid(uid_t, uid_t);
int setrgid(gid_t);
int setrlimit(int, const struct rlimit *);
void setrpcent(int stayopen);
int setruid(uid_t);
void setservent(int);
int setsgroups_np(int, const uuid_t);
pid_t setsid(void);
int setsockopt(int, int, int, const void *, socklen_t);
int setsourcefilter(int, unsigned int, struct sockaddr *, socklen_t, unsigned int, unsigned int, struct sockaddr_storage *);
char *setstate(const char *);
int settimeofday(const struct timeval *, const struct timezone *);
int setuid(uid_t);
void setusershell(void);
void setutxent(void);
void setutxent_wtmp(int);
int setvbuf(FILE *, char *, int, size_t __size);
int setwgroups_np(int, const uuid_t);
int setxattr(const char *path, const char *name, const void *value, size_t size, u_int32_t position, int options);
int shm_open(const char *, int, ...);
int shm_unlink(const char *);
void *shmat(int, const void *, int);
int shmctl(int, int, struct __shmid_ds_new *);
int shmdt(const void *);
int shmget(key_t, size_t, int);
int shmsys(int, ...);
int shutdown(int, int);
int sigaction(int, const struct sigaction *, struct sigaction *);
int sigaddset(sigset_t *, int);
int sigaltstack(const stack_t *, stack_t *);
int sigblock(int);
int sigdelset(sigset_t *, int);
int sigemptyset(sigset_t *);
int sigfillset(sigset_t *);
int sighold(int);
int sigignore(int);
int siginterrupt(int, int);
int sigismember(const sigset_t *, int);
_Noreturn void siglongjmp(sigjmp_buf, int);
void (*signal(int, void (*)(int)))(int);
int sigpause(int);
int sigpending(sigset_t *);
int sigprocmask(int, const sigset_t *, sigset_t *);
int sigrelse(int);
void (*sigset(int, void (*)(int)))(int);
int sigsetjmp(sigjmp_buf, int);
int sigsetmask(int);
int sigsuspend(const sigset_t *);
int sigvec(int, struct sigvec *, struct sigvec *);
int sigwait(const sigset_t *, int *);
unsigned int sleep(unsigned int);
int snprintf(char *__str, size_t __size, const char *__format, ...);
int snprintf_l(char *, size_t __maxlen, locale_t, const char *, ...);
int sockatmark(int);
int socket(int, int, int);
int socketpair(int, int, int, int *);
int sprintf(char *, const char *, ...);
int sprintf_l(char *, locale_t, const char *, ...);
int sradixsort(const unsigned char **__base, int __nel, const unsigned char *__table, unsigned int __endbyte);
void srand(unsigned int);
void srand48(long);
void sranddev(void);
void srandom(unsigned int);
void srandomdev(void);
int sscanf(const char *, const char *, ...);
int sscanf_l(const char *, locale_t, const char *, ...);
int stat(const char *, struct stat *);
int statfs(const char *, struct statfs *);
int statvfs(const char *, struct statvfs *);
int statx_np(const char *, struct stat *, filesec_t);
char *stpcpy(char *__dst, const char *__src);
char *stpncpy(char *__dst, const char *__src, size_t __n);
int strcasecmp(const char *, const char *);
int strcasecmp_l(const char *, const char *, locale_t);
char *strcasestr(const char *__big, const char *__little);
char *strcasestr_l(const char *, const char *, locale_t);
char *strcat(char *__s1, const char *__s2);
char *strchr(const char *__s, int __c);
char *strchrnul(const char *__s, int __c);
int strcmp(const char *__s1, const char *__s2);
int strcoll(const char *__s1, const char *__s2);
int strcoll_l(const char *, const char *, locale_t);
char *strcpy(char *__dst, const char *__src);
size_t strcspn(const char *__s, const char *__charset);
char *strdup(const char *__s1);
char *strerror(int __errnum);
int strerror_r(int __errnum, char *__strerrbuf, size_t __buflen);
size_t strftime(char *, size_t __maxsize, const char *, const struct tm *);
size_t strftime_l(char *, size_t __n, const char *, const struct tm *, locale_t);
size_t strlcat(char *__dst, const char *__source, size_t __size);
size_t strlcpy(char *__dst, const char *__source, size_t __size);
size_t strlen(const char *__s);
void strmode(int __mode, char *__bp);
int strncasecmp(const char *, const char *, size_t);
int strncasecmp_l(const char *, const char *, size_t, locale_t);
char *strncat(char *__s1, const char *__s2, size_t __n);
int strncmp(const char *__s1, const char *__s2, size_t __n);
char *strncpy(char *__dst, const char *__src, size_t __n);
char *strndup(const char *__s1, size_t __n);
size_t strnlen(const char *__s1, size_t __n);
char *strnstr(const char *__big, const char *__little, size_t __len);
char *strpbrk(const char *__s, const char *__charset);
char *strptime(const char *, const char *, struct tm *);
char *strptime_l(const char *, const char *, struct tm *, locale_t);
char *strrchr(const char *__s, int __c);
char *strsep(char **__stringp, const char *__delim);
char *strsignal(int __sig);
int strsignal_r(int __sig, char *__strsignalbuf, size_t __buflen);
size_t strspn(const char *__s, const char *__charset);
char *strstr(const char *__big, const char *__little);
double strtod(const char *, char **);
double strtod_l(const char *, char **, locale_t);
float strtof(const char *, char **);
float strtof_l(const char *, char **, locale_t);
int strtofflags(char **, unsigned long *, unsigned long *);
intmax_t strtoimax(const char *__nptr, char **__endptr, int __base);
intmax_t strtoimax_l(const char *nptr, char **endptr, int base, locale_t);
char *strtok(char *__str, const char *__sep);
char *strtok_r(char *__str, const char *__sep, char **__lasts);
long strtol(const char *__str, char **__endptr, int __base);
long strtol_l(const char *, char **, int, locale_t);
long double strtold(const char *, char **);
long double strtold_l(const char *, char **, locale_t);
long long strtoll(const char *__str, char **__endptr, int __base);
long long strtoll_l(const char *, char **, int, locale_t);
long long strtonum(const char *__numstr, long long __minval, long long __maxval, const char **__errstrp);
long long strtoq(const char *__str, char **__endptr, int __base);
long long strtoq_l(const char *, char **, int, locale_t);
unsigned long strtoul(const char *__str, char **__endptr, int __base);
unsigned long strtoul_l(const char *, char **, int, locale_t);
unsigned long long strtoull(const char *__str, char **__endptr, int __base);
unsigned long long strtoull_l(const char *, char **, int, locale_t);
uintmax_t strtoumax(const char *__nptr, char **__endptr, int __base);
uintmax_t strtoumax_l(const char *nptr, char **endptr, int base, locale_t);
unsigned long long strtouq(const char *__str, char **__endptr, int __base);
unsigned long long strtouq_l(const char *, char **, int, locale_t);
size_t strxfrm(char *__s1, const char *__s2, size_t __n);
size_t strxfrm_l(char *, const char *, size_t __n, locale_t);
void swab(const void *, void *, ssize_t __nbytes);
int swapon(const char *);
int swprintf(int *, size_t __maxlen, const int *, ...);
int swprintf_l(int *, size_t n, locale_t, const int *, ...);
int swscanf(const int *, const int *, ...);
int swscanf_l(const int *, locale_t, const int *, ...);
int symlink(const char *, const char *);
int symlinkat(const char *, int, const char *);
void sync(void);
int sync_volume_np(const char *, int);
int syscall(int, ...);
long sysconf(int);
int sysctl(int *, u_int, void *, size_t *oldlenp, void *, size_t newlen);
int sysctlbyname(const char *, void *, size_t *oldlenp, void *, size_t newlen);
int sysctlnametomib(const char *, int *, size_t *sizep);
void syslog(int, const char *, ...) __asm__("syslog$DARWIN_EXTSN");
int system(const char *);
int tcdrain(int);
int tcflow(int, int);
int tcflush(int, int);
int tcgetattr(int, struct termios *);
pid_t tcgetpgrp(int);
pid_t tcgetsid(int);
int tcsendbreak(int, int);
int tcsetattr(int, int, const struct termios *);
int tcsetpgrp(int, pid_t);
void *tdelete(const void *, void **, int (*)(const void *, const void *));
long telldir(DIR *);
char *tempnam(const char *__dir, const char *__prefix);
void *tfind(const void *, void *const *, int (*)(const void *, const void *));
time_t time(time_t *);
time_t time2posix(time_t);
time_t timegm(struct tm *const);
time_t timelocal(struct tm *const);
clock_t times(struct tms *);
int timespec_get(struct timespec *ts, int base);
int timingsafe_bcmp(const void *__b1, const void *__b2, size_t __len);
FILE *tmpfile(void);
char *tmpnam(char *);
int toascii(int _c);
int tolower(int _c);
int tolower_l(int c, locale_t l);
int toupper(int _c);
int toupper_l(int c, locale_t l);
wint_t towctrans(wint_t, wctrans_t);
wint_t towctrans_l(wint_t, wctrans_t, locale_t);
wint_t towlower(wint_t _wc);
wint_t towlower_l(wint_t _wc, locale_t _l);
wint_t towupper(wint_t _wc);
wint_t towupper_l(wint_t _wc, locale_t _l);
int truncate(const char *, off_t);
void *tsearch(const void *, void **, int (*)(const void *, const void *));
char *ttyname(int);
int ttyname_r(int, char *, size_t __len);
int ttyslot(void);
void twalk(const void *, void (*)(const void *, VISIT, int));
void tzset(void);
void tzsetwall(void);
useconds_t ualarm(useconds_t, useconds_t);
mode_t umask(mode_t);
int umaskx_np(filesec_t);
int uname(struct utsname *);
int undelete(const char *);
int ungetc(int, FILE *);
wint_t ungetwc(wint_t, FILE *);
wint_t ungetwc_l(wint_t, FILE *, locale_t);
int unlink(const char *);
int unlinkat(int, const char *, int);
int unlockpt(int);
int unmount(const char *, int);
int unsetenv(const char *);
int unwhiteout(const char *);
locale_t uselocale(locale_t);
char *user_from_uid(uid_t, int);
int usleep(useconds_t);
int utime(const char *, const struct utimbuf *);
int utimensat(int __fd, const char *__path, const struct timespec *__times, int __flag);
int utimes(const char *, const struct timeval *);
int utmpxname(const char *);
void uuid_clear(uuid_t uu);
int uuid_compare(const uuid_t uu1, const uuid_t uu2);
void uuid_copy(uuid_t dst, const uuid_t src);
void uuid_generate(uuid_t out);
void uuid_generate_random(uuid_t out);
void uuid_generate_time(uuid_t out);
int uuid_is_null(const uuid_t uu);
int uuid_parse(const uuid_string_t in, uuid_t uu);
void uuid_unparse(const uuid_t uu, uuid_string_t out);
void uuid_unparse_lower(const uuid_t uu, uuid_string_t out);
void uuid_unparse_upper(const uuid_t uu, uuid_string_t out);
void *valloc(size_t __size);
int vasprintf(char **, const char *, char *);
int vasprintf_l(char **, locale_t, const char *, char *);
int vdprintf(int, const char *, char *);
int vdprintf_l(int, locale_t, const char *, char *);
_Noreturn void verr(int, const char *, char *);
_Noreturn void verrc(int, int, const char *, char *);
_Noreturn void verrx(int, const char *, char *);
pid_t vfork(void);
int vfprintf(FILE *, const char *, char *);
int vfprintf_l(FILE *, locale_t, const char *, char *);
int vfscanf(FILE *__stream, const char *__format, char *);
int vfscanf_l(FILE *, locale_t, const char *, char *);
int vfwprintf(FILE *, const int *, char *);
int vfwprintf_l(FILE *, locale_t, const int *, char *);
int vfwscanf(FILE *, const int *, char *);
int vfwscanf_l(FILE *, locale_t, const int *, char *);
int vprintf(const char *, char *);
int vprintf_l(locale_t, const char *, char *);
int vscanf(const char *__format, char *);
int vscanf_l(locale_t, const char *, char *);
int vsnprintf(char *__str, size_t __size, const char *__format, char *);
int vsnprintf_l(char *, size_t __maxlen, locale_t, const char *, char *);
int vsprintf(char *, const char *, char *);
int vsprintf_l(char *, locale_t, const char *, char *);
int vsscanf(const char *__str, const char *__format, char *);
int vsscanf_l(const char *, locale_t, const char *, char *);
int vswprintf(int *, size_t __maxlen, const int *, char *);
int vswprintf_l(int *, size_t n, locale_t, const int *, char *);
int vswscanf(const int *, const int *, char *);
int vswscanf_l(const int *, locale_t, const int *, char *);
void vsyslog(int, const char *, char *);
void vwarn(const char *, char *);
void vwarnc(int, const char *, char *);
void vwarnx(const char *, char *);
int vwprintf(const int *, char *);
int vwprintf_l(locale_t, const int *, char *);
int vwscanf(const int *, char *);
int vwscanf_l(locale_t, const int *, char *);
pid_t wait(int *);
pid_t wait3(int *, int, struct rusage *);
pid_t wait4(pid_t, int *, int, struct rusage *);
int waitid(idtype_t, id_t, siginfo_t *, int);
pid_t waitpid(pid_t, int *, int);
void warn(const char *, ...);
void warnc(int, const char *, ...);
void warnx(const char *, ...);
int *wcpcpy(int *, const int *);
int *wcpncpy(int *, const int *, size_t __n);
size_t wcrtomb(char *, int, mbstate_t *);
size_t wcrtomb_l(char *, int, mbstate_t *, locale_t);
int wcscasecmp(const int *, const int *);
int wcscasecmp_l(const int *, const int *, locale_t);
int *wcscat(int *, const int *);
int *wcschr(const int *, int);
int wcscmp(const int *, const int *);
int wcscoll(const int *, const int *);
int wcscoll_l(const int *, const int *, locale_t);
int *wcscpy(int *, const int *);
size_t wcscspn(const int *, const int *);
int *wcsdup(const int *);
size_t wcsftime(int *, size_t __maxlen, const int *, const struct tm *);
size_t wcsftime_l(int *, size_t __n, const int *, const struct tm *, locale_t);
size_t wcslcat(int *, const int *, size_t __len);
size_t wcslcpy(int *, const int *, size_t __len);
size_t wcslen(const int *);
int wcsncasecmp(const int *, const int *, size_t n);
int wcsncasecmp_l(const int *, const int *, size_t n, locale_t);
int *wcsncat(int *, const int *, size_t __n);
int wcsncmp(const int *, const int *, size_t);
int *wcsncpy(int *, const int *, size_t __n);
size_t wcsnlen(const int *, size_t __n);
size_t wcsnrtombs(char *, const int **, size_t, size_t __len, mbstate_t *);
size_t wcsnrtombs_l(char *, const int **, size_t, size_t, mbstate_t *, locale_t);
int *wcspbrk(const int *, const int *);
int *wcsrchr(const int *, int);
size_t wcsrtombs(char *, const int **, size_t __len, mbstate_t *);
size_t wcsrtombs_l(char *, const int **, size_t __len, mbstate_t *, locale_t);
size_t wcsspn(const int *, const int *);
int *wcsstr(const int *, const int *);
double wcstod(const int *, int **);
double wcstod_l(const int *, int **, locale_t);
float wcstof(const int *, int **);
float wcstof_l(const int *, int **, locale_t);
intmax_t wcstoimax(const int *__nptr, int **__endptr, int __base);
intmax_t wcstoimax_l(const int *nptr, int **endptr, int base, locale_t);
int *wcstok(int *, const int *, int **);
long wcstol(const int *, int **, int);
long wcstol_l(const int *, int **, int, locale_t);
long double wcstold(const int *, int **);
long double wcstold_l(const int *, int **, locale_t);
long long wcstoll(const int *, int **, int);
long long wcstoll_l(const int *, int **, int, locale_t);
size_t wcstombs(char *, const int *, size_t __n);
size_t wcstombs_l(char *__restric, const int *, size_t __n, locale_t);
unsigned long wcstoul(const int *, int **, int);
unsigned long wcstoul_l(const int *, int **, int, locale_t);
unsigned long long wcstoull(const int *, int **, int);
unsigned long long wcstoull_l(const int *, int **, int, locale_t);
uintmax_t wcstoumax(const int *__nptr, int **__endptr, int __base);
uintmax_t wcstoumax_l(const int *nptr, int **endptr, int base, locale_t);
int wcswidth(const int *, size_t __n);
int wcswidth_l(const int *, size_t __n, locale_t);
size_t wcsxfrm(int *, const int *, size_t __n);
size_t wcsxfrm_l(int *, const int *, size_t __n, locale_t);
int wctob(wint_t);
int wctob_l(wint_t, locale_t);
int wctomb(char *, int);
int wctomb_l(char *, int, locale_t);
wctrans_t wctrans(const char *);
wctrans_t wctrans_l(const char *, locale_t);
wctype_t wctype(const char *);
wctype_t wctype_l(const char *, locale_t);
int wcwidth(int);
int wcwidth_l(int, locale_t);
int *wmemchr(const int *, int, size_t __n);
int wmemcmp(const int *, const int *, size_t __n);
int *wmemcpy(int *, const int *, size_t __n);
int *wmemmove(int *, const int *, size_t __n);
int *wmemset(int *, int, size_t __n);
int wordexp(const char *, wordexp_t *, int);
void wordfree(wordexp_t *);
int wprintf(const int *, ...);
int wprintf_l(locale_t, const int *, ...);
ssize_t write(int __fd, const void *__buf, size_t __nbyte);
ssize_t writev(int, const struct iovec *, int);
int wscanf(const int *, ...);
int wscanf_l(locale_t, const int *, ...);
int wtmpxname(const char *);
