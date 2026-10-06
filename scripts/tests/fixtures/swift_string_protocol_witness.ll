; Swift 6.1.2, MacOSX15.5 SDK. Complete STRING_PROTOCOL_SOURCE compiler query.
%swift.type = type { i64 }
%swift.protocol_conformance_descriptor = type { i32, i32, i32, i32 }

@"$sSSN" = external global %swift.type, align 8
@"$sS2SSysWL" = linkonce_odr hidden local_unnamed_addr global ptr null, align 8
@"$sSSSysMc" = external global %swift.protocol_conformance_descriptor, align 4

define swiftcc void @neverd_string_protocol_witness_probe() #0 {
entry:
  %0 = tail call ptr @"$sS2SSysWl"() #3
  tail call swiftcc void @neverd_string_protocol_observer(ptr nonnull @"$sSSN", ptr nonnull @"$sSSN", ptr %0)
  ret void
}

declare swiftcc void @neverd_string_protocol_observer(ptr, ptr, ptr) local_unnamed_addr #0

; Function Attrs: nofree noinline nosync nounwind memory(none)
define linkonce_odr hidden ptr @"$sS2SSysWl"() local_unnamed_addr #1 {
entry:
  %0 = load ptr, ptr @"$sS2SSysWL", align 8
  %1 = icmp eq ptr %0, null
  br i1 %1, label %cacheIsNull, label %cont

cacheIsNull:                                      ; preds = %entry
  %2 = tail call ptr @swift_getWitnessTable(ptr nonnull @"$sSSSysMc", ptr nonnull @"$sSSN", ptr undef) #4
  store atomic ptr %2, ptr @"$sS2SSysWL" release, align 8
  br label %cont

cont:                                             ; preds = %cacheIsNull, %entry
  %3 = phi ptr [ %0, %entry ], [ %2, %cacheIsNull ]
  ret ptr %3
}

; Function Attrs: nofree nounwind memory(read)
declare ptr @swift_getWitnessTable(ptr, ptr, ptr) local_unnamed_addr #2
