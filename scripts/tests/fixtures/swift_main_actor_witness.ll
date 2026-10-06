; ModuleID = 'build-source-recovery/wmf-evidence/swift-main-actor-witness-arm64.ll'
source_filename = "build-source-recovery/wmf-evidence/swift-main-actor-witness-arm64.ll"
target datalayout = "e-m:o-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx15.0.0"

%swift.metadata_response = type { ptr, i64 }

@"$sScMScAsWP" = external global ptr, align 8
@__swift_reflection_version = linkonce_odr hidden constant i16 3
@llvm.used = appending global [2 x ptr] [ptr @__swift_reflection_version, ptr @neverd_main_actor_witness_probe], section "llvm.metadata"

define swiftcc void @neverd_main_actor_witness_probe() #0 {
entry:
  %0 = tail call swiftcc %swift.metadata_response @"$sScMMa"(i64 0) #1
  %1 = extractvalue %swift.metadata_response %0, 0
  tail call swiftcc void @neverd_actor_observer(ptr %1, ptr %1, ptr nonnull @"$sScMScAsWP")
  ret void
}

declare swiftcc %swift.metadata_response @"$sScMMa"(i64) local_unnamed_addr #0

declare swiftcc void @neverd_actor_observer(ptr, ptr, ptr) local_unnamed_addr #0

attributes #0 = { "frame-pointer"="non-leaf" "no-trapping-math"="true" "probe-stack"="__chkstk_darwin" "stack-protector-buffer-size"="8" "target-cpu"="apple-a12" "target-features"="+aes,+ccidx,+complxnum,+crc,+fp-armv8,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+ras,+rcpc,+rdm,+sha2,+v8.1a,+v8.2a,+v8.3a,+v8a,+zcm,+zcz" }
attributes #1 = { nounwind memory(none) }

!llvm.module.flags = !{!0, !1, !2, !3, !4, !5, !6, !7, !8, !9, !10, !11}
!swift.module.flags = !{!12}
!llvm.linker.options = !{!13, !14, !15, !16}

!0 = !{i32 2, !"SDK Version", [2 x i32] [i32 15, i32 5]}
!1 = !{i32 1, !"Objective-C Version", i32 2}
!2 = !{i32 1, !"Objective-C Image Info Version", i32 0}
!3 = !{i32 1, !"Objective-C Image Info Section", !"__DATA,__objc_imageinfo,regular,no_dead_strip"}
!4 = !{i32 4, !"Objective-C Garbage Collection", i32 100730624}
!5 = !{i32 1, !"Objective-C Class Properties", i32 64}
!6 = !{i32 1, !"Objective-C Enforce ClassRO Pointer Signing", i8 0}
!7 = !{i32 1, !"wchar_size", i32 4}
!8 = !{i32 8, !"PIC Level", i32 2}
!9 = !{i32 7, !"uwtable", i32 1}
!10 = !{i32 7, !"frame-pointer", i32 1}
!11 = !{i32 1, !"Swift Version", i32 7}
!12 = !{!"standard-library", i1 false}
!13 = !{!"-lswift_Concurrency"}
!14 = !{!"-lswiftCore"}
!15 = !{!"-lswift_StringProcessing"}
!16 = !{!"-lobjc"}
