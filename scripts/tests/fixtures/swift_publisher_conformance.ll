; Swift 6.1.2 ARM64 macOS compiler output for the Combine metatype probes.
; Input is generated in generate_swift_data_declarations.py; not a runtime layout fixture.
source_filename = "publisher-conformance.swift"
target datalayout = "e-m:o-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx15.0.0"

%swift.type_descriptor = type opaque
%swift.protocol_conformance_descriptor = type { i32, i32, i32, i32 }

@"$s7Combine19CurrentValueSubjectCMn" = external global %swift.type_descriptor, align 4
@"got.$s7Combine19CurrentValueSubjectCMn" = private unnamed_addr constant ptr @"$s7Combine19CurrentValueSubjectCMn"
@"$ss5NeverOMn" = external global %swift.type_descriptor, align 4
@"got.$ss5NeverOMn" = private unnamed_addr constant ptr @"$ss5NeverOMn"
@"symbolic _____ySi_____G 7Combine19CurrentValueSubjectC s5NeverO" = linkonce_odr hidden constant <{ i8, i32, [3 x i8], i8, i32, [1 x i8], i8 }> <{ i8 2, i32 trunc (i64 sub (i64 ptrtoint (ptr @"got.$s7Combine19CurrentValueSubjectCMn" to i64), i64 ptrtoint (ptr getelementptr inbounds (<{ i8, i32, [3 x i8], i8, i32, [1 x i8], i8 }>, ptr @"symbolic _____ySi_____G 7Combine19CurrentValueSubjectC s5NeverO", i32 0, i32 1) to i64)) to i32), [3 x i8] c"ySi", i8 2, i32 trunc (i64 sub (i64 ptrtoint (ptr @"got.$ss5NeverOMn" to i64), i64 ptrtoint (ptr getelementptr inbounds (<{ i8, i32, [3 x i8], i8, i32, [1 x i8], i8 }>, ptr @"symbolic _____ySi_____G 7Combine19CurrentValueSubjectC s5NeverO", i32 0, i32 4) to i64)) to i32), [1 x i8] c"G", i8 0 }>, section "__TEXT,__swift5_typeref, regular", no_sanitize_address, align 2
@"$s7Combine19CurrentValueSubjectCySis5NeverOGMD" = linkonce_odr hidden global { i32, i32 } { i32 trunc (i64 sub (i64 ptrtoint (ptr @"symbolic _____ySi_____G 7Combine19CurrentValueSubjectC s5NeverO" to i64), i64 ptrtoint (ptr @"$s7Combine19CurrentValueSubjectCySis5NeverOGMD" to i64)) to i32), i32 -14 }, align 8
@"$s7Combine19CurrentValueSubjectCySis5NeverOGACyxq_GAA9PublisherAAWL" = linkonce_odr hidden local_unnamed_addr global ptr null, align 8
@"$s7Combine19CurrentValueSubjectCyxq_GAA9PublisherAAMc" = external global %swift.protocol_conformance_descriptor, align 4
@"_swift_FORCE_LOAD_$_swiftDarwin_$_main" = weak_odr hidden constant ptr @"_swift_FORCE_LOAD_$_swiftDarwin"
@"_swift_FORCE_LOAD_$_swiftunistd_$_main" = weak_odr hidden constant ptr @"_swift_FORCE_LOAD_$_swiftunistd"
@"_swift_FORCE_LOAD_$_swift_time_$_main" = weak_odr hidden constant ptr @"_swift_FORCE_LOAD_$_swift_time"
@"_swift_FORCE_LOAD_$_swift_errno_$_main" = weak_odr hidden constant ptr @"_swift_FORCE_LOAD_$_swift_errno"
@"_swift_FORCE_LOAD_$_swiftsys_time_$_main" = weak_odr hidden constant ptr @"_swift_FORCE_LOAD_$_swiftsys_time"
@"_swift_FORCE_LOAD_$_swift_signal_$_main" = weak_odr hidden constant ptr @"_swift_FORCE_LOAD_$_swift_signal"
@"_swift_FORCE_LOAD_$_swift_stdio_$_main" = weak_odr hidden constant ptr @"_swift_FORCE_LOAD_$_swift_stdio"
@"_swift_FORCE_LOAD_$_swift_math_$_main" = weak_odr hidden constant ptr @"_swift_FORCE_LOAD_$_swift_math"
@"_swift_FORCE_LOAD_$_swift_Builtin_float_$_main" = weak_odr hidden constant ptr @"_swift_FORCE_LOAD_$_swift_Builtin_float"
@__swift_reflection_version = linkonce_odr hidden constant i16 3
@llvm.used = appending global [14 x ptr] [ptr @"$s4main28metadata_CurrentValueSubjectSVyF", ptr @"$s4main36witness_CurrentValueSubjectPublisheryyF", ptr @__swift_reflection_version, ptr @"_swift_FORCE_LOAD_$_swiftDarwin_$_main", ptr @"_swift_FORCE_LOAD_$_swift_Builtin_float_$_main", ptr @"_swift_FORCE_LOAD_$_swift_errno_$_main", ptr @"_swift_FORCE_LOAD_$_swift_math_$_main", ptr @"_swift_FORCE_LOAD_$_swift_signal_$_main", ptr @"_swift_FORCE_LOAD_$_swift_stdio_$_main", ptr @"_swift_FORCE_LOAD_$_swift_time_$_main", ptr @"_swift_FORCE_LOAD_$_swiftsys_time_$_main", ptr @"_swift_FORCE_LOAD_$_swiftunistd_$_main", ptr @metadata_CurrentValueSubject, ptr @witness_CurrentValueSubjectPublisher], section "llvm.metadata"

; Function Attrs: mustprogress nofree nounwind willreturn memory(read)
define ptr @metadata_CurrentValueSubject() #0 {
entry:
  %0 = tail call ptr @__swift_instantiateConcreteTypeFromMangledName(ptr nonnull @"$s7Combine19CurrentValueSubjectCySis5NeverOGMD") #6
  ret ptr %0
}

; Function Attrs: mustprogress nofree noinline nounwind willreturn memory(read)
define linkonce_odr hidden ptr @__swift_instantiateConcreteTypeFromMangledName(ptr %0) local_unnamed_addr #1 {
entry:
  %1 = load atomic i64, ptr %0 monotonic, align 8
  %2 = icmp slt i64 %1, 0
  br i1 %2, label %6, label %3, !prof !27

3:                                                ; preds = %6, %entry
  %4 = phi i64 [ %1, %entry ], [ %14, %6 ]
  %5 = inttoptr i64 %4 to ptr
  ret ptr %5

6:                                                ; preds = %entry
  %7 = ashr i64 %1, 32
  %8 = sub nsw i64 0, %7
  %sext = shl i64 %1, 32
  %9 = ashr exact i64 %sext, 32
  %10 = ptrtoint ptr %0 to i64
  %11 = add i64 %9, %10
  %12 = inttoptr i64 %11 to ptr
  %13 = tail call swiftcc ptr @swift_getTypeByMangledNameInContext2(ptr %12, i64 %8, ptr null, ptr null) #7
  %14 = ptrtoint ptr %13 to i64
  store atomic i64 %14, ptr %0 monotonic, align 8
  br label %3
}

; Function Attrs: nounwind memory(argmem: readwrite)
declare swiftcc ptr @swift_getTypeByMangledNameInContext2(ptr, i64, ptr, ptr) local_unnamed_addr #2

; Function Attrs: mustprogress nofree nounwind willreturn memory(read)
define swiftcc ptr @"$s4main28metadata_CurrentValueSubjectSVyF"() #0 {
entry:
  %0 = tail call ptr @__swift_instantiateConcreteTypeFromMangledName(ptr nonnull @"$s7Combine19CurrentValueSubjectCySis5NeverOGMD") #6
  ret ptr %0
}

define void @witness_CurrentValueSubjectPublisher() #3 {
entry:
  %0 = tail call ptr @__swift_instantiateConcreteTypeFromMangledName(ptr nonnull @"$s7Combine19CurrentValueSubjectCySis5NeverOGMD") #6
  %1 = tail call ptr @"$s7Combine19CurrentValueSubjectCySis5NeverOGACyxq_GAA9PublisherAAWl"() #8
  tail call swiftcc void @neverd_publisher_type_probe(ptr %0, ptr %0, ptr %1) #9
  ret void
}

declare swiftcc void @neverd_publisher_type_probe(ptr, ptr, ptr) local_unnamed_addr #3

; Function Attrs: nofree noinline nosync nounwind memory(none)
define linkonce_odr hidden ptr @"$s7Combine19CurrentValueSubjectCySis5NeverOGACyxq_GAA9PublisherAAWl"() local_unnamed_addr #4 {
entry:
  %0 = load ptr, ptr @"$s7Combine19CurrentValueSubjectCySis5NeverOGACyxq_GAA9PublisherAAWL", align 8
  %1 = icmp eq ptr %0, null
  br i1 %1, label %cacheIsNull, label %cont

cacheIsNull:                                      ; preds = %entry
  %2 = tail call ptr @__swift_instantiateConcreteTypeFromMangledNameAbstract(ptr nonnull @"$s7Combine19CurrentValueSubjectCySis5NeverOGMD") #6
  %3 = tail call ptr @swift_getWitnessTable(ptr nonnull @"$s7Combine19CurrentValueSubjectCyxq_GAA9PublisherAAMc", ptr %2, ptr undef) #6
  store atomic ptr %3, ptr @"$s7Combine19CurrentValueSubjectCySis5NeverOGACyxq_GAA9PublisherAAWL" release, align 8
  br label %cont

cont:                                             ; preds = %cacheIsNull, %entry
  %4 = phi ptr [ %0, %entry ], [ %3, %cacheIsNull ]
  ret ptr %4
}

; Function Attrs: mustprogress nofree noinline nounwind willreturn memory(read)
define linkonce_odr hidden ptr @__swift_instantiateConcreteTypeFromMangledNameAbstract(ptr %0) local_unnamed_addr #1 {
entry:
  %1 = load atomic i64, ptr %0 monotonic, align 8
  %2 = icmp slt i64 %1, 0
  br i1 %2, label %6, label %3, !prof !27

3:                                                ; preds = %6, %entry
  %4 = phi i64 [ %1, %entry ], [ %14, %6 ]
  %5 = inttoptr i64 %4 to ptr
  ret ptr %5

6:                                                ; preds = %entry
  %7 = ashr i64 %1, 32
  %8 = sub nsw i64 0, %7
  %sext = shl i64 %1, 32
  %9 = ashr exact i64 %sext, 32
  %10 = ptrtoint ptr %0 to i64
  %11 = add i64 %9, %10
  %12 = inttoptr i64 %11 to ptr
  %13 = tail call swiftcc ptr @swift_getTypeByMangledNameInContextInMetadataState2(i64 255, ptr %12, i64 %8, ptr null, ptr null) #7
  %14 = ptrtoint ptr %13 to i64
  store atomic i64 %14, ptr %0 monotonic, align 8
  br label %3
}

; Function Attrs: nounwind memory(argmem: readwrite)
declare swiftcc ptr @swift_getTypeByMangledNameInContextInMetadataState2(i64, ptr, i64, ptr, ptr) local_unnamed_addr #2

; Function Attrs: nofree nounwind memory(read)
declare ptr @swift_getWitnessTable(ptr, ptr, ptr) local_unnamed_addr #5

define swiftcc void @"$s4main36witness_CurrentValueSubjectPublisheryyF"() #3 {
entry:
  %0 = tail call ptr @__swift_instantiateConcreteTypeFromMangledName(ptr nonnull @"$s7Combine19CurrentValueSubjectCySis5NeverOGMD") #6
  %1 = tail call ptr @"$s7Combine19CurrentValueSubjectCySis5NeverOGACyxq_GAA9PublisherAAWl"() #8
  tail call swiftcc void @neverd_publisher_type_probe(ptr %0, ptr %0, ptr %1)
  ret void
}

declare extern_weak void @"_swift_FORCE_LOAD_$_swiftDarwin"()

declare extern_weak void @"_swift_FORCE_LOAD_$_swiftunistd"()

declare extern_weak void @"_swift_FORCE_LOAD_$_swift_time"()

declare extern_weak void @"_swift_FORCE_LOAD_$_swift_errno"()

declare extern_weak void @"_swift_FORCE_LOAD_$_swiftsys_time"()

declare extern_weak void @"_swift_FORCE_LOAD_$_swift_signal"()

declare extern_weak void @"_swift_FORCE_LOAD_$_swift_stdio"()

declare extern_weak void @"_swift_FORCE_LOAD_$_swift_math"()

declare extern_weak void @"_swift_FORCE_LOAD_$_swift_Builtin_float"()

attributes #0 = { mustprogress nofree nounwind willreturn memory(read) "frame-pointer"="non-leaf" "no-trapping-math"="true" "probe-stack"="__chkstk_darwin" "stack-protector-buffer-size"="8" "target-cpu"="apple-a12" "target-features"="+aes,+ccidx,+complxnum,+crc,+fp-armv8,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+ras,+rcpc,+rdm,+sha2,+v8.1a,+v8.2a,+v8.3a,+v8a,+zcm,+zcz" }
attributes #1 = { mustprogress nofree noinline nounwind willreturn memory(read) "frame-pointer"="non-leaf" "no-trapping-math"="true" "probe-stack"="__chkstk_darwin" "stack-protector-buffer-size"="8" "target-cpu"="apple-a12" "target-features"="+aes,+ccidx,+complxnum,+crc,+fp-armv8,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+ras,+rcpc,+rdm,+sha2,+v8.1a,+v8.2a,+v8.3a,+v8a,+zcm,+zcz" }
attributes #2 = { nounwind memory(argmem: readwrite) }
attributes #3 = { "frame-pointer"="non-leaf" "no-trapping-math"="true" "probe-stack"="__chkstk_darwin" "stack-protector-buffer-size"="8" "target-cpu"="apple-a12" "target-features"="+aes,+ccidx,+complxnum,+crc,+fp-armv8,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+ras,+rcpc,+rdm,+sha2,+v8.1a,+v8.2a,+v8.3a,+v8a,+zcm,+zcz" }
attributes #4 = { nofree noinline nosync nounwind memory(none) "frame-pointer"="non-leaf" "no-trapping-math"="true" "probe-stack"="__chkstk_darwin" "stack-protector-buffer-size"="8" "target-cpu"="apple-a12" "target-features"="+aes,+ccidx,+complxnum,+crc,+fp-armv8,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+ras,+rcpc,+rdm,+sha2,+v8.1a,+v8.2a,+v8.3a,+v8a,+zcm,+zcz" }
attributes #5 = { nofree nounwind memory(read) }
attributes #6 = { nounwind memory(read) }
attributes #7 = { nounwind memory(argmem: read) }
attributes #8 = { nounwind memory(none) }
attributes #9 = { noinline }

!llvm.module.flags = !{!0, !1, !2, !3, !4, !5, !6, !7, !8, !9, !10, !11}
!swift.module.flags = !{!12}
!llvm.linker.options = !{!13, !14, !15, !16, !17, !18, !19, !20, !21, !22, !23, !24, !25, !26}

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
!13 = !{!"-framework", !"Combine"}
!14 = !{!"-lswiftCore"}
!15 = !{!"-lswift_StringProcessing"}
!16 = !{!"-lswift_Concurrency"}
!17 = !{!"-lswiftDarwin"}
!18 = !{!"-lswiftunistd"}
!19 = !{!"-lswift_time"}
!20 = !{!"-lswift_errno"}
!21 = !{!"-lswiftsys_time"}
!22 = !{!"-lswift_signal"}
!23 = !{!"-lswift_stdio"}
!24 = !{!"-lswift_math"}
!25 = !{!"-lswift_Builtin_float"}
!26 = !{!"-lobjc"}
!27 = !{!"branch_weights", !"expected", i32 1, i32 2000}
