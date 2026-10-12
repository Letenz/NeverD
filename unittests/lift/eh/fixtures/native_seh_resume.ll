;===- native_seh_resume.ll - x64 CRT resume feasibility ------------------===;
;
; NeverD Decompiler
;
;===--------------------------------------------------------------------===;
; Native protocol experiment with separate source cells and physical stack.
; Compiler and runtime success do not grant a NeverD reconstruction certificate.
target triple = "x86_64-pc-windows-msvc"

declare i32 @__C_specific_handler(...)
declare dllimport void @RaiseException(i32, i32, i32, ptr)
declare dllimport void @_local_unwind(ptr, ptr) noreturn
declare ptr @llvm.localaddress()
declare ptr @llvm.stacksave.p0()
declare void @llvm.localescape(...)
declare ptr @llvm.localrecover(ptr, ptr, i32 immarg)

define i32 @neverd_native_seh_resume(i32 %leave, i32 %throwing) noinline personality ptr @__C_specific_handler {
entry:
  %state = alloca [3 x i32], align 4
  %native.stack = alloca ptr, align 8
  call void (...) @llvm.localescape(ptr %state, ptr %native.stack)
  %physical = call ptr @llvm.stacksave.p0()
  store volatile ptr %physical, ptr %native.stack
  %flag = getelementptr [3 x i32], ptr %state, i32 0, i32 1
  %guard = getelementptr [3 x i32], ptr %state, i32 0, i32 2
  store volatile i32 1, ptr %state
  store i32 %leave, ptr %flag
  store volatile i32 1370025985, ptr %guard
  %throw = icmp ne i32 %throwing, 0
  br i1 %throw, label %raise, label %normal
raise:
  invoke void @RaiseException(i32 -532528574, i32 0, i32 0, ptr null)
    to label %normal unwind label %cleanup
normal:
  store volatile i32 12, ptr %state
  %normalfp = call ptr @llvm.localaddress()
  invoke void @resume_finally(i8 0, ptr %normalfp)
    to label %normal.suffix unwind label %catch.dispatch
normal.suffix:
  %n = load volatile i32, ptr %state
  %n10 = mul i32 %n, 10
  %n4 = add i32 %n10, 4
  store volatile i32 %n4, ptr %state
  br label %done
cleanup:
  %cleanup.pad = cleanuppad within none []
  %unwindfp = call ptr @llvm.localaddress()
  invoke void @resume_finally(i8 1, ptr %unwindfp) [ "funclet"(token %cleanup.pad) ]
    to label %cleanup.return unwind label %catch.dispatch
cleanup.return:
  cleanupret from %cleanup.pad unwind label %catch.dispatch
catch.dispatch:
  %switch = catchswitch within none [label %catch.pad] unwind to caller
catch.pad:
  %catch = catchpad within %switch [ptr null]
  catchret from %catch to label %handler
handler:
  %h = load volatile i32, ptr %state
  %h10 = mul i32 %h, 10
  %h6 = add i32 %h10, 6
  store volatile i32 %h6, ptr %state
  br label %done
done:
  %d = load volatile i32, ptr %state
  %d10 = mul i32 %d, 10
  %d7 = add i32 %d10, 7
  store volatile i32 %d7, ptr %state
  invoke void @RaiseException(i32 -532528573, i32 0, i32 0, ptr null)
    to label %invalid.second.return unwind label %second.dispatch
invalid.second.return:
  ret i32 -2
second.dispatch:
  %second.switch = catchswitch within none [label %second.pad] unwind to caller
second.pad:
  %second.catch = catchpad within %second.switch [ptr null]
  catchret from %second.catch to label %second.handler
second.handler:
  %g = load volatile i32, ptr %guard
  %ok = icmp eq i32 %g, 1370025985
  %v = load volatile i32, ptr %state
  %v10 = mul i32 %v, 10
  %v8 = add i32 %v10, 8
  %result = select i1 %ok, i32 %v8, i32 -1
  ret i32 %result
}

define internal void @resume_finally(i8 %abnormal, ptr %parent) noinline optnone {
entry:
  %state = call ptr @llvm.localrecover(ptr @neverd_native_seh_resume, ptr %parent, i32 0)
  %flag = getelementptr [3 x i32], ptr %state, i32 0, i32 1
  %v = load volatile i32, ptr %state
  %v10 = mul i32 %v, 10
  %v3 = add i32 %v10, 3
  store volatile i32 %v3, ptr %state
  %leave = load i32, ptr %flag
  %jump = icmp ne i32 %leave, 0
  br i1 %jump, label %resume, label %return
resume:
  %native.slot = call ptr @llvm.localrecover(ptr @neverd_native_seh_resume, ptr %parent, i32 1)
  %native.base = load volatile ptr, ptr %native.slot
  call void @_local_unwind(ptr %native.base, ptr blockaddress(@neverd_native_seh_resume, %done))
  unreachable
return:
  ret void
}
