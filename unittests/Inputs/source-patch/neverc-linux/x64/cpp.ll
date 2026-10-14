; ModuleID = 'unittests/Inputs/source-patch/neverc-linux/x64/translated.nc'
source_filename = "unittests/Inputs/source-patch/neverc-linux/x64/translated.nc"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

; Function Attrs: nounwind null_pointer_is_valid uwtable
define dso_local i32 @nct_b50f504c1df27ad7204d043c(i32 noundef %nct_f0b327f297bab_p1) local_unnamed_addr #0 {
entry:
  %mul = shl nsw i32 %nct_f0b327f297bab_p1, 1
  ret i32 %mul
}

; Function Attrs: nounwind null_pointer_is_valid uwtable
define dso_local void @nct_c6ab131685679f9335dc912d(ptr nocapture noundef writeonly %nct_f93bb80cb2b20_p1, ptr noundef %nct_f93bb80cb2b20_p2) local_unnamed_addr #0 {
entry:
  store ptr %nct_f93bb80cb2b20_p2, ptr %nct_f93bb80cb2b20_p1, align 8, !tbaa !4
  %0 = load i32, ptr %nct_f93bb80cb2b20_p2, align 4, !tbaa !9
  %add = add nsw i32 %0, 3
  store i32 %add, ptr %nct_f93bb80cb2b20_p2, align 4, !tbaa !9
  ret void
}

; Function Attrs: nounwind null_pointer_is_valid uwtable
define dso_local i32 @replacement(i32 noundef %nct_f95713e9cbdd1_p1) local_unnamed_addr #0 {
entry:
  %add.i = shl i32 %nct_f95713e9cbdd1_p1, 1
  %add.i1 = add i32 %add.i, 11
  ret i32 %add.i1
}

attributes #0 = { nounwind null_pointer_is_valid uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }

!llvm.module.flags = !{!0, !1, !2}
!llvm.ident = !{!3}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 7, !"PIE Level", i32 2}
!2 = !{i32 7, !"uwtable", i32 2}
!3 = !{!"neverc version 3389.1.5"}
!4 = !{!5, !6, i64 0}
!5 = !{!"nct_19a122b2e9ae3502d96fc970", !6, i64 0}
!6 = !{!"any pointer", !7, i64 0}
!7 = !{!"omnipotent char", !8, i64 0}
!8 = !{!"Simple C TBAA"}
!9 = !{!10, !10, i64 0}
!10 = !{!"int", !7, i64 0}
