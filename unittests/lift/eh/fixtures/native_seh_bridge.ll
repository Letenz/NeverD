;===- native_seh_bridge.ll - ABI body for the native bridge experiment ---===;
;
; NeverD Decompiler
;
;===--------------------------------------------------------------------===;
; Explicit source state, reached through ordinary function entries at O0/O2.
; This fixture cannot authenticate arbitrary source writes or resume descriptors.
target triple = "x86_64-pc-windows-msvc"
@neverd_native_bridge_observed = global [7 x i32] zeroinitializer, align 4

declare dllimport void @_local_unwind(ptr, ptr) noreturn

define i32 @native_resume_step(ptr %frame, i32 %digit) noinline {
entry:
  %state = getelementptr i8, ptr %frame, i64 32
  %old = load volatile i32, ptr %state, align 4
  %shifted = mul i32 %old, 10
  %next = add i32 %shifted, %digit
  store volatile i32 %next, ptr %state, align 4
  ret i32 %next
}

define void @native_resume_cleanup(i8 %abnormal, ptr %frame) noinline {
entry:
  %count.slot = getelementptr i8, ptr %frame, i64 68
  %count = load volatile i32, ptr %count.slot, align 4
  %count.next = add i32 %count, 1
  store volatile i32 %count.next, ptr %count.slot, align 4
  %recordable = icmp ult i32 %count, 2
  br i1 %recordable, label %record, label %body
record:
  ; Keep both raw bytes. Encoding [0,2] as a mask could alias [1,0].
  %actual = zext i8 %abnormal to i32
  %index = zext i32 %count to i64
  %scaled = mul i64 %index, 4
  %offset = add i64 %scaled, 72
  %slot = getelementptr i8, ptr %frame, i64 %offset
  store volatile i32 %actual, ptr %slot, align 4
  br label %body
body:
  %ignored = call i32 @native_resume_step(ptr %frame, i32 3)
  %flag = getelementptr i8, ptr %frame, i64 36
  %leave = load volatile i32, ptr %flag, align 4
  %jump = icmp ne i32 %leave, 0
  br i1 %jump, label %resume, label %return
resume:
  %sp.slot = getelementptr i8, ptr %frame, i64 56
  %target.slot = getelementptr i8, ptr %frame, i64 48
  %native.sp = load volatile ptr, ptr %sp.slot, align 8
  %target = load volatile ptr, ptr %target.slot, align 8
  call void @_local_unwind(ptr %native.sp, ptr %target)
  unreachable
return:
  ret void
}

; Observe actual exception records in event order. Unexpected events stay visible.
define i32 @native_resume_filter(ptr %pointers, ptr %frame) noinline {
entry:
  %record = load ptr, ptr %pointers, align 8
  %code = load i32, ptr %record, align 4
  %count.slot = getelementptr i8, ptr %frame, i64 88
  %count = load volatile i32, ptr %count.slot, align 4
  %next = add i32 %count, 1
  store volatile i32 %next, ptr %count.slot, align 4
  %throw.slot = getelementptr i8, ptr %frame, i64 40
  %throw = load volatile i32, ptr %throw.slot, align 4
  %throwing = icmp ne i32 %throw, 0
  %is.first = icmp eq i32 %count, 0
  %first = and i1 %throwing, %is.first
  %expected = select i1 %first, i32 -532528574, i32 -532528573
  %same = icmp eq i32 %code, %expected
  %last.index = zext i1 %throwing to i32
  %in.order = icmp ule i32 %count, %last.index
  %valid = and i1 %same, %in.order
  br i1 %valid, label %record.code, label %unexpected
record.code:
  %offset = select i1 %first, i64 80, i64 84
  %slot = getelementptr i8, ptr %frame, i64 %offset
  store volatile i32 %code, ptr %slot, align 4
  br label %return
unexpected:
  %unexpected.slot = getelementptr i8, ptr %frame, i64 92
  store volatile i32 1, ptr %unexpected.slot, align 4
  br label %return
return:
  ret i32 1
}

define i32 @native_resume_finish(ptr %frame) noinline {
entry:
  ; Copy seven independent observer fields, not inferred trace properties.
  %observation = getelementptr i8, ptr %frame, i64 68
  br label %copy
copy:
  %index = phi i64 [0, %entry], [%next, %copy]
  %source = getelementptr i32, ptr %observation, i64 %index
  %value = load volatile i32, ptr %source, align 4
  %target = getelementptr [7 x i32], ptr @neverd_native_bridge_observed, i32 0, i64 %index
  store volatile i32 %value, ptr %target, align 4
  %next = add i64 %index, 1
  %more = icmp ult i64 %next, 7
  br i1 %more, label %copy, label %return
return:
  %result = call i32 @native_resume_step(ptr %frame, i32 9)
  ret i32 %result
}
