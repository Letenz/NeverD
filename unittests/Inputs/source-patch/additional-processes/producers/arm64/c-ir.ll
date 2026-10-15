; ModuleID = 'replacement.c'
source_filename = "replacement.c"
target datalayout = "e-m:o-i64:64-i128:128-n32:64-S128"
target triple = "arm64-apple-macosx14.0.0"

; Function Attrs: nounwind null_pointer_is_valid ssp uwtable(sync)
define i32 @replacement(i32 noundef %x) local_unnamed_addr #0 {
entry:
  %add = shl i32 %x, 1
  %add1 = add i32 %add, 11
  ret i32 %add1
}

attributes #0 = { nounwind null_pointer_is_valid ssp uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+crc,+dotprod,+fp-armv8,+fp16fml,+fullfp16,+lse,+neon,+ras,+rcpc,+rdm,+sha2,+sha3,+v8a,+zcm,+zcz" }

!llvm.module.flags = !{!0, !1, !2}
!llvm.ident = !{!3}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 7, !"uwtable", i32 1}
!2 = !{i32 7, !"frame-pointer", i32 1}
!3 = !{!"neverc version 3389.1.5"}
