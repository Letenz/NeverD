// Independently constructed native tail-transfer and live-frame controls.
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

BEGIN tail_helper
  ret
END tail_helper
.data
.p2align 3
.quad .Laddress_root
.section .note.GNU-stack,"",@progbits
