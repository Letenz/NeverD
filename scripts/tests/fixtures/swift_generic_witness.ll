; Swift 6.1.2 arm64-apple-macosx13.0, MacOSX15.5 SDK.
; Generated from SOURCE in generate_swift_witness_contracts.py; paths omitted.
%swift.protocol_conformance_descriptor = type { i32, i32, i32, i32 }

@"$s7Combine19CurrentValueSubjectCyxq_GAA9PublisherAAMc" = external global %swift.protocol_conformance_descriptor, align 4
@"_swift_FORCE_LOAD_$_swiftDarwin_$_WitnessProbe" = weak_odr hidden constant ptr @"_swift_FORCE_LOAD_$_swiftDarwin"
@"_swift_FORCE_LOAD_$_swiftunistd_$_WitnessProbe" = weak_odr hidden constant ptr @"_swift_FORCE_LOAD_$_swiftunistd"
@"_swift_FORCE_LOAD_$_swift_time_$_WitnessProbe" = weak_odr hidden constant ptr @"_swift_FORCE_LOAD_$_swift_time"
@"_swift_FORCE_LOAD_$_swift_errno_$_WitnessProbe" = weak_odr hidden constant ptr @"_swift_FORCE_LOAD_$_swift_errno"
@"_swift_FORCE_LOAD_$_swiftsys_time_$_WitnessProbe" = weak_odr hidden constant ptr @"_swift_FORCE_LOAD_$_swiftsys_time"
@"_swift_FORCE_LOAD_$_swift_signal_$_WitnessProbe" = weak_odr hidden constant ptr @"_swift_FORCE_LOAD_$_swift_signal"
@"_swift_FORCE_LOAD_$_swift_stdio_$_WitnessProbe" = weak_odr hidden constant ptr @"_swift_FORCE_LOAD_$_swift_stdio"
@"_swift_FORCE_LOAD_$_swift_math_$_WitnessProbe" = weak_odr hidden constant ptr @"_swift_FORCE_LOAD_$_swift_math"
@"_swift_FORCE_LOAD_$_swift_Builtin_float_$_WitnessProbe" = weak_odr hidden constant ptr @"_swift_FORCE_LOAD_$_swift_Builtin_float"
@__swift_reflection_version = linkonce_odr hidden constant i16 3
@llvm.used = appending global [11 x ptr] [ptr @__swift_reflection_version, ptr @"_swift_FORCE_LOAD_$_swiftDarwin_$_WitnessProbe", ptr @"_swift_FORCE_LOAD_$_swift_Builtin_float_$_WitnessProbe", ptr @"_swift_FORCE_LOAD_$_swift_errno_$_WitnessProbe", ptr @"_swift_FORCE_LOAD_$_swift_math_$_WitnessProbe", ptr @"_swift_FORCE_LOAD_$_swift_signal_$_WitnessProbe", ptr @"_swift_FORCE_LOAD_$_swift_stdio_$_WitnessProbe", ptr @"_swift_FORCE_LOAD_$_swift_time_$_WitnessProbe", ptr @"_swift_FORCE_LOAD_$_swiftsys_time_$_WitnessProbe", ptr @"_swift_FORCE_LOAD_$_swiftunistd_$_WitnessProbe", ptr @neverd_generic_publisher_probe], section "llvm.metadata"

define swiftcc void @neverd_generic_publisher_probe(ptr %0) #0 {
entry:
  %1 = tail call ptr @swift_getWitnessTable(ptr nonnull @"$s7Combine19CurrentValueSubjectCyxq_GAA9PublisherAAMc", ptr %0, ptr undef) #2
  tail call swiftcc void @neverd_publisher_probe(ptr %0, ptr %0, ptr %1)
  ret void
}

declare swiftcc void @neverd_publisher_probe(ptr, ptr, ptr) local_unnamed_addr #0

; Function Attrs: nofree nounwind memory(read)
declare ptr @swift_getWitnessTable(ptr, ptr, ptr) local_unnamed_addr #1

declare extern_weak void @"_swift_FORCE_LOAD_$_swiftDarwin"()

declare extern_weak void @"_swift_FORCE_LOAD_$_swiftunistd"()

declare extern_weak void @"_swift_FORCE_LOAD_$_swift_time"()

declare extern_weak void @"_swift_FORCE_LOAD_$_swift_errno"()

declare extern_weak void @"_swift_FORCE_LOAD_$_swiftsys_time"()

declare extern_weak void @"_swift_FORCE_LOAD_$_swift_signal"()

declare extern_weak void @"_swift_FORCE_LOAD_$_swift_stdio"()

declare extern_weak void @"_swift_FORCE_LOAD_$_swift_math"()

declare extern_weak void @"_swift_FORCE_LOAD_$_swift_Builtin_float"()

attributes #0 = { "frame-pointer"="non-leaf" "no-trapping-math"="true" "probe-stack"="__chkstk_darwin" "stack-protector-buffer-size"="8" "target-cpu"="apple-a12" "target-features"="+aes,+ccidx,+complxnum,+crc,+fp-armv8,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+ras,+rcpc,+rdm,+sha2,+v8.1a,+v8.2a,+v8.3a,+v8a,+zcm,+zcz" }
attributes #1 = { nofree nounwind memory(read) }
attributes #2 = { nounwind memory(read) }

