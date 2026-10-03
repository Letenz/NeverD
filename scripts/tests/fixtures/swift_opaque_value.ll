; ModuleID = 'build-source-recovery/wmf-evidence/opaque-contract-compiler-0.ll'
source_filename = "build-source-recovery/wmf-evidence/opaque-contract-compiler-0.ll"
target datalayout = "e-m:o-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx15.0.0"

%swift.type = type { i64 }
%Ts11AnyHashableV = type <{ %Ts15_AnyHashableBoxP }>
%Ts15_AnyHashableBoxP = type { [24 x i8], ptr, ptr }

@"$ss11AnyHashableVN" = external global %swift.type, align 8
@__swift_reflection_version = linkonce_odr hidden constant i16 3
@llvm.used = appending global [5 x ptr] [ptr @__swift_reflection_version, ptr @neverd_anyhashable_bounded_lifetime, ptr @neverd_anyhashable_copy, ptr @neverd_anyhashable_destroy, ptr @neverd_anyhashable_equal], section "llvm.metadata"

; Function Attrs: nounwind
define swiftcc void @neverd_anyhashable_copy(ptr %0, ptr %1) #0 {
entry:
  %2 = tail call ptr @"$ss11AnyHashableVWOc"(ptr %0, ptr %1)
  ret void
}

; Function Attrs: noinline nounwind
define linkonce_odr hidden ptr @"$ss11AnyHashableVWOc"(ptr %0, ptr %1) local_unnamed_addr #1 {
entry:
  %"$ss11AnyHashableVN.valueWitnesses" = load ptr, ptr getelementptr inbounds (i8, ptr @"$ss11AnyHashableVN", i64 -8), align 8, !invariant.load !17, !dereferenceable !18
  %2 = getelementptr inbounds i8, ptr %"$ss11AnyHashableVN.valueWitnesses", i64 16
  %InitializeWithCopy = load ptr, ptr %2, align 8, !invariant.load !17
  %3 = tail call ptr %InitializeWithCopy(ptr noalias %1, ptr noalias %0, ptr nonnull @"$ss11AnyHashableVN") #4
  ret ptr %1
}

; Function Attrs: nounwind
define swiftcc void @neverd_anyhashable_destroy(ptr %0) #0 {
entry:
  %1 = tail call ptr @"$ss11AnyHashableVWOh"(ptr %0)
  ret void
}

; Function Attrs: noinline nounwind
define linkonce_odr hidden ptr @"$ss11AnyHashableVWOh"(ptr %0) local_unnamed_addr #1 {
entry:
  %"$ss11AnyHashableVN.valueWitnesses" = load ptr, ptr getelementptr inbounds (i8, ptr @"$ss11AnyHashableVN", i64 -8), align 8, !invariant.load !17, !dereferenceable !18
  %1 = getelementptr inbounds i8, ptr %"$ss11AnyHashableVN.valueWitnesses", i64 8
  %Destroy = load ptr, ptr %1, align 8, !invariant.load !17
  tail call void %Destroy(ptr noalias %0, ptr nonnull @"$ss11AnyHashableVN") #4
  ret ptr %0
}

define swiftcc i1 @neverd_anyhashable_equal(ptr noalias nocapture dereferenceable(40) %0, ptr noalias nocapture dereferenceable(40) %1) #2 {
entry:
  %2 = tail call swiftcc i1 @"$ss11AnyHashableV2eeoiySbAB_ABtFZ"(ptr noalias nocapture nonnull dereferenceable(40) %0, ptr noalias nocapture nonnull dereferenceable(40) %1)
  ret i1 %2
}

declare swiftcc i1 @"$ss11AnyHashableV2eeoiySbAB_ABtFZ"(ptr noalias nocapture dereferenceable(40), ptr noalias nocapture dereferenceable(40)) local_unnamed_addr #2

define swiftcc i1 @neverd_anyhashable_bounded_lifetime(ptr nocapture dereferenceable(40) %0, ptr nocapture readonly %1, ptr %2) #2 {
entry:
  %3 = alloca %Ts11AnyHashableV, align 8
  call void @llvm.lifetime.start.p0(i64 40, ptr nonnull %3)
  %4 = call ptr @"$ss11AnyHashableVWOc"(ptr nonnull %0, ptr nonnull %3)
  %5 = call swiftcc i1 %1(ptr nocapture nonnull dereferenceable(40) %0, ptr noalias nocapture nonnull dereferenceable(40) %3, ptr swiftself %2)
  %6 = call ptr @"$ss11AnyHashableVWOh"(ptr nonnull %3)
  call void @llvm.lifetime.end.p0(i64 40, ptr nonnull %3)
  ret i1 %5
}

; Function Attrs: mustprogress nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.start.p0(i64 immarg, ptr nocapture) #3

; Function Attrs: mustprogress nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.end.p0(i64 immarg, ptr nocapture) #3

attributes #0 = { nounwind "frame-pointer"="non-leaf" "no-trapping-math"="true" "probe-stack"="__chkstk_darwin" "stack-protector-buffer-size"="8" "target-cpu"="apple-a12" "target-features"="+aes,+ccidx,+complxnum,+crc,+fp-armv8,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+ras,+rcpc,+rdm,+sha2,+v8.1a,+v8.2a,+v8.3a,+v8a,+zcm,+zcz" }
attributes #1 = { noinline nounwind "frame-pointer"="non-leaf" "no-trapping-math"="true" "probe-stack"="__chkstk_darwin" "stack-protector-buffer-size"="8" "target-cpu"="apple-a12" "target-features"="+aes,+ccidx,+complxnum,+crc,+fp-armv8,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+ras,+rcpc,+rdm,+sha2,+v8.1a,+v8.2a,+v8.3a,+v8a,+zcm,+zcz" }
attributes #2 = { "frame-pointer"="non-leaf" "no-trapping-math"="true" "probe-stack"="__chkstk_darwin" "stack-protector-buffer-size"="8" "target-cpu"="apple-a12" "target-features"="+aes,+ccidx,+complxnum,+crc,+fp-armv8,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+ras,+rcpc,+rdm,+sha2,+v8.1a,+v8.2a,+v8.3a,+v8a,+zcm,+zcz" }
attributes #3 = { mustprogress nocallback nofree nosync nounwind willreturn memory(argmem: readwrite) }
attributes #4 = { nounwind }

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
!17 = !{}
!18 = !{i64 88}
