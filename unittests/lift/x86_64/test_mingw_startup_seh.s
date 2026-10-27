# Windows x64 SEH fixture in the shape of MinGW-w64's crtexe.c startup code.
# mingw_entry is mainCRTStartup: GCC sets its frame register before it
# allocates the stack, and __try1/__except1 guard the call with a scope that
# resumes at the `nop` the guarded call falls into. Its callee reaches an x87
# instruction (_fpreset's fninit), so the x87 state is carried across the
# call.

        .text
        .def    mingw_entry;
        .scl    2;
        .type   32;
        .endef
        .globl  mingw_entry
        .p2align 4, 0x90
mingw_entry:
.seh_proc mingw_entry
        pushq   %rbp
        .seh_pushreg %rbp
        movq    %rsp, %rbp
        .seh_setframe %rbp, 0
        subq    $32, %rsp
        .seh_stackalloc 32
        .seh_endprologue
.Lguard_begin:
        .seh_handler __C_specific_handler, @except
        .seh_handlerdata
        .long   1
        .rva    .Lguard_begin, .Lguard_end, startup_filter, .Lguard_end
        .text
        movl    $0, app_type(%rip)
        callq   startup_body
        nop
.Lguard_end:
        nop
        addq    $32, %rsp
        popq    %rbp
        retq
.seh_endproc

        .def    startup_body;
        .scl    2;
        .type   32;
        .endef
        .globl  startup_body
        .p2align 4, 0x90
startup_body:
.seh_proc startup_body
        subq    $40, %rsp
        .seh_stackalloc 40
        .seh_endprologue
        callq   fpreset
        movl    app_type(%rip), %eax
        addq    $40, %rsp
        retq
.seh_endproc

        .def    fpreset;
        .scl    2;
        .type   32;
        .endef
        .globl  fpreset
        .p2align 4, 0x90
fpreset:
        fninit
        retq

        .def    startup_filter;
        .scl    2;
        .type   32;
        .endef
        .globl  startup_filter
        .p2align 4, 0x90
startup_filter:
        movq    (%rcx), %rax
        movl    (%rax), %eax
        retq

        .def    __C_specific_handler;
        .scl    2;
        .type   32;
        .endef
        .globl  __C_specific_handler
        .p2align 4, 0x90
__C_specific_handler:
        movl    $1, %eax
        retq

        .data
        .globl  app_type
        .p2align 2
app_type:
        .long   0
