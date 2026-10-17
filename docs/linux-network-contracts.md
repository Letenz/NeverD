# Offline resolver and stream contracts

NeverD can execute bounded Android address resolution and Linux TCP callers
without a host resolver or socket. These are explicit analysis inputs, not a
network stack or observations of any deployed endpoint. Unknown query, peer,
readiness or outgoing bytes stop execution instead of manufacturing success.

`android.resolver_queries` matches the nullable node and service strings and
all four hint fields exactly. Null differs from an empty string. Each query
specifies either the complete ordered address list or a positive API 28 EAI
error. `system_errno` is required only for EAI_SYSTEM (11). For example:

```json
{
  "android": {
    "resolver_queries": [{
      "node": "node.invalid", "service": "9001",
      "flags": 2, "family": 2, "socket_type": 1, "protocol": 6,
      "addresses": [{
        "sockaddr_hex": "020023297f0000010000000000000000",
        "canonical_name": "node.invalid"
      }]
    }]
  },
  "linux_files": {"files": []},
  "linux_network": {
    "connections": [{
      "peer_hex": "020023297f0000010000000000000000",
      "expected_send_hex": "000142", "receive_hex": "4f4b",
      "receive_eof": true, "max_send": 2, "max_receive": 2
    }]
  }
}
```

API 28 LP64 `addrinfo` nodes are 48 bytes: flags/family/type/protocol at 0/4/8/12,
address length at 16, canonical name at 24, address at 32, and next at 40.
`getaddrinfo` allocates separate guest-owned nodes and `freeaddrinfo` checks their
ownership and links before releasing them. Null free is valid; repeated free
or altered list links fail. A null node and service return EAI_NONAME before
reading hints or writing the result pointer. Direct imports and `dlsym` use the
same Bionic owner. `inet_ntop` and `inet_pton` support IPv4 and IPv6. Formatting
uses the first longest zero run and dotted compatible/mapped suffixes, preserves
errno on success, and leaves a short output buffer untouched with ENOSPC.
Parsing preserves the destination on malformed input and uses
API 28 decimal dotted quads, including leading zeroes; an unknown family returns
EAFNOSUPPORT before accessing pointers.

`linux_network` also requires `linux_files`. Its connections are ordered virtual
attempts with canonical Linux `sockaddr_in` (16 bytes) or `sockaddr_in6` (28
bytes). Ports and addresses use network byte order; the family uses the guest's
little-endian ABI. A peer must match the next supplied attempt. `connect_errno`
can supply ECONNREFUSED, ETIMEDOUT, ENETUNREACH, EHOSTUNREACH or EACCES instead of
success. A failed connection cannot carry stream data, and its socket must be
closed before retry. There are at most 256 attempts and 1 MiB of stream bytes.

`socket`, `connect`, `sendto` and `recvfrom` share one implementation across raw
x64/AArch64 Linux syscalls, Android named wrappers and `syscall`. Bionic `send`
and `recv` lower to that same owner. Connected TCP accepts no explicit peer
argument in send/receive. Scalar `read` and `write` share stream cursors;
`dup`/`dup3` aliases and closing/reusing descriptor numbers retain the same
open-description lifetime. Bionic alone converts raw negative errors to errno.

Every outgoing byte is compared before the cursor advances. Per-call ceilings
of 1–65536 bytes model short transfers. The receive stream ends only when
`receive_eof` explicitly says so; exhausted input without EOF remains unknown.
Bad peer or transfer bytes do not consume the connection or stream contract.
Socket seek returns ESPIPE. `setsockopt`/`getsockopt` share SO_RCVTIMEO and
SO_SNDTIMEO state across aliases, accepting whole seconds up to one day;
negative seconds retain immediate-timeout state. Fractional timeouts stop
because kernel tick rounding is not supplied. Short getters copy only the
requested prefix and report its length. These options do not create peer
readiness or advance the guest clock: stream completions remain explicit inputs. Cross-page partial faults, scatter/gather sockets,
nonblocking connect completion, readiness polling, other socket options, UDP,
shutdown and real transport are not covered by this bounded contract.

These ABI rules follow the pinned [AOSP netdb definitions](https://android.googlesource.com/platform/bionic/+/android-9.0.0_r1/libc/include/netdb.h),
[address resolver](https://android.googlesource.com/platform/bionic/+/android-9.0.0_r1/libc/dns/net/getaddrinfo.c)
and [address formatter](https://android.googlesource.com/platform/bionic/+/android-9.0.0_r1/libc/upstream-openbsd/lib/libc/net/inet_ntop.c).

Timeout option validation follows the [Android common kernel socket owner](https://android.googlesource.com/kernel/common/+/refs/heads/android12-5.10/net/core/sock.c).
