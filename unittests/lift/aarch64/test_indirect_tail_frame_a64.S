// Independently constructed native tail-transfer and live-frame controls.
.arch armv8.1-a
.text
.macro BEGIN name
.p2align 2
.globl \name
.type \name,@function
\name:
.endm
.macro END name
.size \name,.-\name
.endm

BEGIN tail_frame_live
  sub sp,sp,#64
  br x0
END tail_frame_live

BEGIN tail_frame_unknown
  mov sp,x1
  br x0
END tail_frame_unknown

BEGIN tail_frame_join
  sub sp,sp,#32
  cbz w1,.Ljoined
  add sp,sp,#32
.Ljoined:
  br x0
END tail_frame_join

BEGIN tail_link_changed
  mov x30,x1
  br x0
END tail_link_changed

BEGIN tail_link_unrestored
  stp x29,x30,[sp,#-16]!
  bl tail_helper
  add sp,sp,#16
  br x0
END tail_link_unrestored

BEGIN tail_link_slot_changed
  stp x29,x30,[sp,#-16]!
  str x1,[sp,#8]
  ldp x29,x30,[sp],#16
  br x0
END tail_link_slot_changed

BEGIN tail_link_slot_partial
  stp x29,x30,[sp,#-16]!
  strb w1,[sp,#8]
  ldp x29,x30,[sp],#16
  br x0
END tail_link_slot_partial

BEGIN tail_unknown_alias
  stp x29,x30,[sp,#-16]!
  str x1,[x2]
  ldp x29,x30,[sp],#16
  br x0
END tail_unknown_alias

BEGIN tail_frame_borrowed
  stp x29,x30,[sp,#-16]!
  mov x0,sp
  bl tail_helper
  ldp x29,x30,[sp],#16
  br x1
END tail_frame_borrowed

BEGIN tail_stack_target
  mov x0,sp
  br x0
END tail_stack_target

BEGIN tail_link_target
  mov x0,x30
  br x0
END tail_link_target

BEGIN tail_frame_loop
.Lframe_loop:
  cbz w1,.Lframe_exit
  sub sp,sp,#16
  b .Lframe_loop
.Lframe_exit:
  br x0
END tail_frame_loop

BEGIN tail_link_loop
.Llink_loop:
  cbz w1,.Llink_exit
  mov x30,x2
  b .Llink_loop
.Llink_exit:
  br x0
END tail_link_loop

BEGIN tail_independent_root
  b .Lroot_join
.Laddress_root:
  add sp,sp,#16
.Lroot_join:
  br x0
END tail_independent_root

BEGIN tail_leaf
  br x0
END tail_leaf

BEGIN tail_frame_balanced
  stp x29,x30,[sp,#-16]!
  mov x29,sp
  sub sp,sp,#64
  add sp,sp,#64
  ldp x29,x30,[sp],#16
  br x0
END tail_frame_balanced

BEGIN tail_frame_diamond
  cbz w1,.Lshort
  sub sp,sp,#32
  add sp,sp,#32
  b .Ldone
.Lshort:
  sub sp,sp,#16
  add sp,sp,#16
.Ldone:
  br x0
END tail_frame_diamond

BEGIN tail_loop_balanced
.Lbalanced_loop:
  cbz w1,.Lbalanced_exit
  sub sp,sp,#16
  add sp,sp,#16
  b .Lbalanced_loop
.Lbalanced_exit:
  br x0
END tail_loop_balanced

BEGIN tail_link_restored
  stp x29,x30,[sp,#-16]!
  mov x29,sp
  bl tail_helper
  ldp x29,x30,[sp],#16
  br x0
END tail_link_restored

// libc++'s __shared_weak_count::__release_weak: an acquire load and an
// exclusive decrement loop before the virtual tail call.
BEGIN tail_exclusive_release
  add x8,x0,#16
  ldar x9,[x8]
  cbz x9,.Lrelease_call
.Lrelease_retry:
  ldaxr x9,[x8]
  sub x10,x9,#1
  stlxr w11,x10,[x8]
  cbnz w11,.Lrelease_retry
  cbz x9,.Lrelease_call
  ret
.Lrelease_call:
  ldr x8,[x0]
  ldr x1,[x8,#32]
  br x1
END tail_exclusive_release

BEGIN tail_exclusive_pair
.Lpair_retry:
  ldaxp x2,x3,[x1]
  add x2,x2,#1
  stlxp w4,x2,x3,[x1]
  cbnz w4,.Lpair_retry
  br x0
END tail_exclusive_pair

BEGIN tail_lse_atomics
  ldaddal x2,x3,[x1]
  swpal x2,x3,[x1]
  casal x2,x3,[x1]
  ldclral x2,x3,[x1]
  stadd x2,[x1]
  br x0
END tail_lse_atomics

BEGIN tail_value_intrinsic
  rbit x1,x1
  br x0
END tail_value_intrinsic

BEGIN tail_exclusive_live
  sub sp,sp,#16
.Llive_retry:
  ldaxr x2,[x1]
  stlxr w3,x2,[x1]
  cbnz w3,.Llive_retry
  br x0
END tail_exclusive_live

// An exclusive store or an atomic swap may overwrite the saved link word.
BEGIN tail_exclusive_link_slot
  stp x29,x30,[sp,#-16]!
  add x2,sp,#8
.Llink_slot_retry:
  ldaxr x3,[x2]
  stlxr w4,x1,[x2]
  cbnz w4,.Llink_slot_retry
  ldp x29,x30,[sp],#16
  br x0
END tail_exclusive_link_slot

BEGIN tail_atomic_link_slot
  stp x29,x30,[sp,#-16]!
  add x2,sp,#8
  swpal x1,x3,[x2]
  ldp x29,x30,[sp],#16
  br x0
END tail_atomic_link_slot

// A frame address published through an exclusive store or an atomic swap
// lets a later callee rewrite the saved link word, though no argument
// register carries it.
BEGIN tail_exclusive_escape
  mov x9,sp
.Lescape_retry:
  ldaxr x10,[x2]
  stlxr w11,x9,[x2]
  cbnz w11,.Lescape_retry
  stp x29,x30,[sp,#-16]!
  bl tail_helper
  ldp x29,x30,[sp],#16
  br x0
END tail_exclusive_escape

BEGIN tail_atomic_escape
  mov x9,sp
  swpal x9,x10,[x2]
  stp x29,x30,[sp,#-16]!
  bl tail_helper
  ldp x29,x30,[sp],#16
  br x0
END tail_atomic_escape

BEGIN tail_exclusive_link_load
  ldaxr x30,[x1]
  br x0
END tail_exclusive_link_load

// A supervisor call has no explicit effect contract.
BEGIN tail_unmodelled_intrinsic
  svc #0
  br x0
END tail_unmodelled_intrinsic

BEGIN tail_helper
  ret
END tail_helper
.data
.p2align 3
.quad .Laddress_root
.section .note.GNU-stack,"",@progbits
