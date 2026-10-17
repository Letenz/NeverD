# Android log readers

The Android native API-28 profile models these liblog functions over an explicit
visible snapshot: `android_logger_list_alloc`, `android_logger_list_alloc_time`,
`android_logger_open`, `android_logger_list_read`, and
`android_logger_list_free`. Direct imports and explicitly declared dynamic
providers use the same workload owner. No phone or host logging service is used.

Supply `android.log_records` as a timestamp-ordered array. Omission means that
the records are unknown; `[]` explicitly supplies an empty snapshot. Every entry
has all seven fields:

```json
{
  "log_id": 0,
  "pid": 42,
  "tid": 43,
  "seconds": 1700000000,
  "nanoseconds": 123,
  "uid": 10000,
  "payload": "0464656d6f0068656c6c6f00"
}
```

`payload` contains hexadecimal bytes, including any priority, tag, text or
binary event structure that the guest expects. The reader does not interpret
or manufacture those bytes. Fields are unsigned 32-bit numbers (decimal or
hexadecimal strings are also accepted); `pid` cannot exceed `INT32_MAX`, and
`nanoseconds` must be below one billion. Buffer IDs 0 through 5 are admitted.
Payloads are at most 4068 bytes. A snapshot contains at most 4096 entries and
1 MiB including the 28-byte headers; its size also counts toward the input
catalogue memory limit. C++ callers use `AndroidNativeOptions::LogRecords`
and share the same admission checks.

For `dlopen`/`dlsym`, separately list the exact exported names under
`android.libraries["liblog.so"]`. Having a model does not make a library present.
Closing a dynamic provider retains the existing inactive-provider refusal.

## Reader contract

- Each list has independent buffer selections and cursor. Reopening an already
  selected buffer returns its existing identity. Adding a new buffer restarts
  selection with the list's original filters, matching transport reinitialization.
- PID zero selects all visible records. A nonzero PID selects that process.
  `tail == 0` selects the whole matching snapshot; otherwise the last `tail`
  matching records are retained, in supplied order.
- `log_time` is the API-28 pair of 32-bit seconds/nanoseconds, passed together
  in an AAPCS64 integer argument register. Epoch zero selects the beginning.
  A nonzero start excludes records at that exact timestamp and earlier.
- A successful read writes a little-endian `logger_entry_v4` followed by the
  exact payload and returns the combined byte count. The whole 5124-byte
  AArch64 `log_msg` is zeroed first, including its alignment padding.
- Nonblocking mode `0x800` returns `-EAGAIN` at the end and clears the same full
  output buffer. The negative result is an `int`; the model does not convert it
  into a libc `-1`/errno response. Null or unselected lists return `-EINVAL`
  without touching the output. Invalid buffer IDs return a null logger.
- Mode zero may read already supplied records, but exhaustion stops as
  unsupported because no live producer was supplied. Missing snapshot input
  also stops without fabricating EOF. Wrap, persistent, write and unknown modes,
  negative PID filters, and security/kernel-buffer admission are unsupported.
- List and logger handles are opaque guest allocations with shared workload
  lifetime and memory accounting. Freeing a list retires its children. Null free
  is harmless; retired, unmapped or permission-tampered handles fail clearly.
  A failed output access does not advance the record cursor.

The supplied snapshot is explicitly the stream visible to this caller. This
does not reconstruct logd credentials, sockets/FDs, pruning/chatty records,
concurrent writes, alarms, persistent storage, or real-time streaming.
There is no inferred permission grant based on a record's UID.

## ABI evidence and verification

The implementation is independently written from the public Android 9 contract.
References are pinned to AOSP `android-9.0.0_r1`:

- [log_read.h](https://android.googlesource.com/platform/system/core/+/android-9.0.0_r1/liblog/include/log/log_read.h)
  defines the entry/message layouts, payload bound, reader signatures and modes.
- [logger_read.c](https://android.googlesource.com/platform/system/core/+/android-9.0.0_r1/liblog/logger_read.c)
  defines null/empty-list behavior, duplicate opens and transport reset.
- [logd_reader.c](https://android.googlesource.com/platform/system/core/+/android-9.0.0_r1/liblog/logd_reader.c)
  clears the complete message and reports nonblocking exhaustion.
- [LogBuffer.cpp](https://android.googlesource.com/platform/system/core/+/android-9.0.0_r1/logd/LogBuffer.cpp)
  and [LogTimes.cpp](https://android.googlesource.com/platform/system/core/+/android-9.0.0_r1/logd/LogTimes.cpp)
  define start positioning and PID/buffer-filtered tail selection.

`AndroidLog.*` compiles independent O0/O2 callers with ordinary, APS2 and RELR
relocations for Unicorn and HVF. Its byte oracle checks the complete output,
aggregate arguments, filters, tails, exact-time exclusion, independent cursors,
reopening, lifetime, errors and unsupported inputs. `AndroidLogOptions.*`
checks the public JSON boundary. These checks establish the finite contract
above, not equivalence to a complete Android logging daemon.

This is a versioned contract. Android 13's public
[LogdRead](https://android.googlesource.com/platform/system/logging/+/android-13.0.0_r1/liblog/logd_reader.cpp)
does not clear the whole message before receiving it, and its
[reader wrapper](https://android.googlesource.com/platform/system/logging/+/android-13.0.0_r1/liblog/logger_read.cpp)
only adds a terminator after a valid packet. Do not use this API-28 profile to
infer the untouched buffer bytes or logger-handle internals of later releases.
