; Swift 6.1.2, MacOSX15.5 SDK, identical on all four Darwin profiles.
; Generated from STRING_INDEX_RANGE_SOURCE in generate_swift_data_declarations.py.
%swift.type_descriptor = type opaque

@"$sSS5IndexVMn" = external global %swift.type_descriptor, align 4
@"got.$sSS5IndexVMn" = private unnamed_addr constant ptr @"$sSS5IndexVMn"
@"symbolic Sny_____G SS5IndexV" = linkonce_odr hidden constant <{ [3 x i8], i8, i32, [1 x i8], i8 }> <{ [3 x i8] c"Sny", i8 2, i32 trunc (i64 sub (i64 ptrtoint (ptr @"got.$sSS5IndexVMn" to i64), i64 ptrtoint (ptr getelementptr inbounds (<{ [3 x i8], i8, i32, [1 x i8], i8 }>, ptr @"symbolic Sny_____G SS5IndexV", i32 0, i32 2) to i64)) to i32), [1 x i8] c"G", i8 0 }>, section "__TEXT,__swift5_typeref, regular", no_sanitize_address, align 2
@"$sSnySS5IndexVGMD" = linkonce_odr hidden global { i32, i32 } { i32 trunc (i64 sub (i64 ptrtoint (ptr @"symbolic Sny_____G SS5IndexV" to i64), i64 ptrtoint (ptr @"$sSnySS5IndexVGMD" to i64)) to i32), i32 -9 }, align 8

define ptr @metadata_StringIndexRange() #0 {
entry:
  %0 = tail call ptr @__swift_instantiateConcreteTypeFromMangledName(ptr nonnull @"$sSnySS5IndexVGMD") #3
  ret ptr %0
}

define linkonce_odr hidden ptr @__swift_instantiateConcreteTypeFromMangledName(ptr %0) local_unnamed_addr #1 {
entry:
  %1 = load atomic i64, ptr %0 monotonic, align 8
  %2 = icmp slt i64 %1, 0
  br i1 %2, label %6, label %3, !prof !17

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
  %13 = tail call swiftcc ptr @swift_getTypeByMangledNameInContext2(ptr %12, i64 %8, ptr null, ptr null) #4
  %14 = ptrtoint ptr %13 to i64
  store atomic i64 %14, ptr %0 monotonic, align 8
  br label %3
}

declare swiftcc ptr @swift_getTypeByMangledNameInContext2(ptr, i64, ptr, ptr) local_unnamed_addr #2
