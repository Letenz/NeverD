; Swift 6.1.2, MacOSX15.5 SDK, identical flow on all four Darwin profiles.
; Generated from GENERIC_RANGE_SOURCE in generate_swift_data_declarations.py.
%swift.protocol_conformance_descriptor = type { i32, i32, i32, i32 }
%swift.metadata_response = type { ptr, i64 }

@"$sSnyxGSXsMc" = external global %swift.protocol_conformance_descriptor, align 4

define swiftcc void @neverd_generic_range_probe(ptr %Bound, ptr %Bound.Comparable) #0 {
entry:
  %0 = tail call swiftcc %swift.metadata_response @"$sSnMa"(i64 0, ptr %Bound, ptr %Bound.Comparable) #2
  %1 = extractvalue %swift.metadata_response %0, 0
  %2 = tail call ptr @swift_getWitnessTable(ptr nonnull @"$sSnyxGSXsMc", ptr %1, ptr undef) #3
  tail call swiftcc void @neverd_range_probe(ptr %1, ptr %1, ptr %2)
  ret void
}

declare swiftcc %swift.metadata_response @"$sSnMa"(i64, ptr, ptr) local_unnamed_addr #0
declare swiftcc void @neverd_range_probe(ptr, ptr, ptr) local_unnamed_addr #0
declare ptr @swift_getWitnessTable(ptr, ptr, ptr) local_unnamed_addr #1
