// zlib's inflate(), as GCC 15.2.0 compiles the unchanged zlib 1.3.1 inflate.c
// for aarch64-linux-gnu with -O2 -fPIC. Only this function and the local data
// it reads are kept; the other functions it calls are left undefined.
//
// Its state loop dispatches through a compact halfword table whose index is
// an extended register (`ldrh w0, [x0, w1, uxtw #1]`), and every case may
// update the bit buffer `hold`, which one case compares at full width.
//
// inflate.c -- zlib decompression
// Copyright (C) 1995-2022 Mark Adler
//
// zlib.h -- interface of the 'zlib' general purpose compression library
// version 1.3.1, January 22nd, 2024
//
// Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler
//
// This software is provided 'as-is', without any express or implied
// warranty.  In no event will the authors be held liable for any damages
// arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source distribution.
//
// Jean-loup Gailly        Mark Adler
// jloup@gzip.org          madler@alumni.caltech.edu

	.arch armv8-a
	.text
	.align	2
	.p2align 5,,15
	.global	inflate
	.type	inflate, %function
inflate:
.LFB15:
	.cfi_startproc
	stp	x29, x30, [sp, -144]!
	.cfi_def_cfa_offset 144
	.cfi_offset 29, -144
	.cfi_offset 30, -136
	mov	x29, sp
	stp	x19, x20, [sp, 16]
	.cfi_offset 19, -128
	.cfi_offset 20, -120
	mov	x20, x0
	stp	x21, x22, [sp, 32]
	.cfi_offset 21, -112
	.cfi_offset 22, -104
	mov	w21, w1
	bl	inflateStateCheck
	cbnz	w0, .L97
	ldr	x10, [x20, 24]
	cbz	x10, .L97
	ldr	x8, [x20]
	mov	w22, w0
	cbz	x8, .L613
.L98:
	stp	x23, x24, [sp, 48]
	.cfi_offset 24, -88
	.cfi_offset 23, -96
	mov	w0, 16191
	ldr	x19, [x20, 56]
	ldr	w1, [x19, 8]
	cmp	w1, w0
	bne	.L99
	mov	w1, 16192
	str	w1, [x19, 8]
.L99:
	ldr	w24, [x20, 8]
	mov	w12, -16180
	ldr	w23, [x20, 32]
	ldr	x9, [x19, 80]
	mov	w11, w23
	ldr	w6, [x19, 88]
	mov	w7, w24
.L100:
	add	w1, w1, w12
	cmp	w1, 30
	bhi	.L587
	adrp	x0, .L102
	add	x0, x0, :lo12:.L102
	ldrh	w0, [x0,w1,uxtw #1]
	adr	x1, .Lrtx102
	add	x0, x1, w0, sxth #2
	br	x0
.Lrtx102:
	.section	.rodata
	.align	0
	.align	2
.L102:
	.2byte	(.L132 - .Lrtx102) / 4
	.2byte	(.L590 - .Lrtx102) / 4
	.2byte	(.L591 - .Lrtx102) / 4
	.2byte	(.L592 - .Lrtx102) / 4
	.2byte	(.L128 - .Lrtx102) / 4
	.2byte	(.L127 - .Lrtx102) / 4
	.2byte	(.L126 - .Lrtx102) / 4
	.2byte	(.L125 - .Lrtx102) / 4
	.2byte	(.L124 - .Lrtx102) / 4
	.2byte	(.L595 - .Lrtx102) / 4
	.2byte	(.L122 - .Lrtx102) / 4
	.2byte	(.L121 - .Lrtx102) / 4
	.2byte	(.L133 - .Lrtx102) / 4
	.2byte	(.L119 - .Lrtx102) / 4
	.2byte	(.L118 - .Lrtx102) / 4
	.2byte	(.L117 - .Lrtx102) / 4
	.2byte	(.L598 - .Lrtx102) / 4
	.2byte	(.L115 - .Lrtx102) / 4
	.2byte	(.L114 - .Lrtx102) / 4
	.2byte	(.L113 - .Lrtx102) / 4
	.2byte	(.L112 - .Lrtx102) / 4
	.2byte	(.L111 - .Lrtx102) / 4
	.2byte	(.L110 - .Lrtx102) / 4
	.2byte	(.L109 - .Lrtx102) / 4
	.2byte	(.L108 - .Lrtx102) / 4
	.2byte	(.L107 - .Lrtx102) / 4
	.2byte	(.L106 - .Lrtx102) / 4
	.2byte	(.L105 - .Lrtx102) / 4
	.2byte	(.L604 - .Lrtx102) / 4
	.2byte	(.L606 - .Lrtx102) / 4
	.2byte	(.L101 - .Lrtx102) / 4
	.text
	.p2align 2,,3
.L142:
	cbz	w7, .L103
	ldrb	w0, [x8], 1
	sub	w7, w7, #1
	lsl	x0, x0, x6
	add	w6, w6, 8
	add	x9, x9, x0
.L598:
	cmp	w6, 13
	bls	.L142
	and	w1, w9, 31
	ubfx	w0, w9, 5, 5
	ubfx	w2, w9, 10, 4
	add	w1, w1, 257
	add	w0, w0, 1
	add	w2, w2, 4
	stp	w2, w1, [x19, 128]
	cmp	w1, 286
	str	w0, [x19, 136]
	lsr	x9, x9, 14
	sub	w6, w6, #14
	ccmp	w0, 30, 2, ls
	bls	.L224
	adrp	x0, .LC8
	add	x0, x0, :lo12:.LC8
	str	x0, [x20, 48]
	mov	w22, -3
	mov	w0, 16209
	str	w0, [x19, 8]
	.p2align 5,,15
.L103:
	str	w7, [x20, 8]
	str	w11, [x20, 32]
	str	x8, [x20]
	ldr	w2, [x19, 60]
	str	x10, [x20, 24]
	str	x9, [x19, 80]
	str	w6, [x19, 88]
	ldr	w3, [x20, 32]
	cbnz	w2, .L306
	cmp	w23, w3
	beq	.L307
	ldr	w1, [x19, 8]
	mov	w2, 16208
	cmp	w1, w2
	bhi	.L308
	mov	w2, 16205
	cmp	w1, w2
	ccmp	w21, 4, 0, hi
	beq	.L308
.L306:
	sub	w2, w23, w3
	mov	x1, x10
	mov	x0, x20
	bl	updatewindow
	cbnz	w0, .L309
	ldr	w3, [x20, 32]
.L308:
	ldr	x0, [x20, 16]
	sub	w2, w23, w3
	ldr	w1, [x20, 8]
	sub	w3, w23, w3
	sub	w4, w24, w1
	sub	w1, w24, w1
	add	x1, x0, x1
	str	x1, [x20, 16]
	ldr	x0, [x20, 40]
	add	x0, x0, x3
	str	x0, [x20, 40]
	ldr	x0, [x19, 40]
	add	x0, x0, x3
	str	x0, [x19, 40]
	ldr	w0, [x19, 16]
	tst	x0, 4
	ccmp	w2, 0, 4, ne
	bne	.L310
.L607:
	ldr	w6, [x19, 88]
.L311:
	ldp	w1, w0, [x19, 8]
	mov	w3, 16191
	cmp	w0, 0
	cset	w0, ne
	add	w0, w6, w0, lsl 6
	cmp	w1, w3
	beq	.L614
	mov	w3, 16199
	cmp	w1, w3
	mov	w3, 16194
	ccmp	w1, w3, 4, ne
	cset	w1, eq
	orr	w4, w4, w2
	cmp	w4, 0
	add	w0, w0, w1, lsl 8
	str	w0, [x20, 88]
	ccmp	w21, 4, 4, ne
	bne	.L588
.L622:
	ldp	x23, x24, [sp, 48]
	.cfi_restore 24
	.cfi_restore 23
	cmp	w22, 0
	mov	w0, -5
	csel	w22, w22, w0, ne
.L96:
	ldp	x19, x20, [sp, 16]
	mov	w0, w22
	ldp	x21, x22, [sp, 32]
	ldp	x29, x30, [sp], 144
	.cfi_restore 30
	.cfi_restore 29
	.cfi_restore 21
	.cfi_restore 22
	.cfi_restore 19
	.cfi_restore 20
	.cfi_def_cfa_offset 0
	ret
.L163:
	.cfi_def_cfa_offset 144
	.cfi_offset 19, -128
	.cfi_offset 20, -120
	.cfi_offset 21, -112
	.cfi_offset 22, -104
	.cfi_offset 23, -96
	.cfi_offset 24, -88
	.cfi_offset 29, -144
	.cfi_offset 30, -136
	ldr	x0, [x19, 48]
	cbz	x0, .L164
	ubfx	w1, w9, 8, 1
	str	w1, [x0]
.L164:
	ldr	w0, [x19, 24]
	tbz	x0, 9, .L165
	ldr	w0, [x19, 16]
	tbnz	x0, 2, .L615
.L165:
	mov	w0, 16182
	mov	x9, 0
	mov	w6, 0
	str	w0, [x19, 8]
	.p2align 5,,15
.L168:
	cbz	w7, .L103
	ldrb	w0, [x8], 1
	sub	w7, w7, #1
	lsl	x0, x0, x6
	add	w6, w6, 8
	add	x9, x9, x0
.L591:
	cmp	w6, 31
	bls	.L168
	ldr	x0, [x19, 48]
	cbz	x0, .L169
	str	x9, [x0, 8]
.L169:
	ldr	w0, [x19, 24]
	tbz	x0, 9, .L170
	ldr	w0, [x19, 16]
	tbnz	x0, 2, .L616
.L170:
	mov	w0, 16183
	mov	x9, 0
	mov	w6, 0
	str	w0, [x19, 8]
	.p2align 5,,15
.L173:
	cbz	w7, .L103
	ldrb	w0, [x8], 1
	sub	w7, w7, #1
	lsl	x0, x0, x6
	add	w6, w6, 8
	add	x9, x9, x0
.L592:
	cmp	w6, 15
	bls	.L173
	ldr	x1, [x19, 48]
	cbz	x1, .L174
	and	w0, w9, 255
	lsr	x2, x9, 8
	stp	w0, w2, [x1, 16]
.L174:
	ldr	w2, [x19, 24]
	mov	w1, w2
	tbz	x2, 9, .L175
	ldr	w0, [x19, 16]
	tbz	x0, 2, .L175
	ldr	x0, [x19, 32]
	add	x1, sp, 136
	mov	w2, 2
	str	x10, [sp, 64]
	str	w11, [sp, 72]
	str	w7, [sp, 80]
	str	x8, [sp, 88]
	strh	w9, [sp, 136]
	bl	crc32
	ldr	w2, [x19, 24]
	ldr	x10, [sp, 64]
	mov	w1, w2
	ldr	x8, [sp, 88]
	str	x0, [x19, 32]
	ldr	w11, [sp, 72]
	ldr	w7, [sp, 80]
	.p2align 5,,15
.L175:
	mov	w0, 16184
	str	w0, [x19, 8]
	tbnz	x2, 10, .L617
	ldr	x0, [x19, 48]
	cbz	x0, .L187
	str	xzr, [x0, 24]
.L187:
	mov	w6, 0
	mov	x9, 0
	b	.L186
.L160:
	mov	w0, 16189
	mov	x9, 0
	mov	w6, 0
	str	w0, [x19, 8]
	.p2align 5,,15
.L475:
	cbz	w7, .L103
	ldrb	w0, [x8], 1
	sub	w7, w7, #1
	lsl	x0, x0, x6
	add	w6, w6, 8
	add	x9, x9, x0
.L595:
	cmp	w6, 31
	bls	.L475
	rev	x0, x9
	mov	w6, 0
	mov	x9, 0
	lsr	x0, x0, 32
	str	x0, [x19, 32]
	str	x0, [x20, 96]
	mov	w0, 16190
	str	w0, [x19, 8]
.L122:
	ldr	w0, [x19, 20]
	cbz	w0, .L618
	mov	w2, 0
	mov	x1, 0
	mov	x0, 0
	str	x10, [sp, 64]
	str	w11, [sp, 72]
	str	x9, [sp, 80]
	str	w7, [sp, 88]
	str	x8, [sp, 96]
	str	w6, [sp, 104]
	bl	adler32
.L596:
	ldr	x10, [sp, 64]
	str	x0, [x19, 32]
	ldr	x9, [sp, 80]
	str	x0, [x20, 96]
	ldr	x8, [sp, 96]
	mov	w0, 16191
	ldr	w11, [sp, 72]
	mov	w12, -16180
	ldr	w7, [sp, 88]
	ldr	w6, [sp, 104]
	str	w0, [x19, 8]
.L121:
	sub	w0, w21, #5
	cmp	w0, 1
	bls	.L103
.L133:
	ldr	w0, [x19, 12]
	cbz	w0, .L619
	and	w0, w6, 7
	and	w6, w6, -8
	lsr	x9, x9, x0
	mov	w0, 16206
	str	w0, [x19, 8]
.L106:
	ldr	w1, [x19, 16]
	cbnz	w1, .L602
.L293:
	mov	w0, 16208
	str	w0, [x19, 8]
.L604:
	mov	w22, 1
	b	.L103
.L628:
	ldr	w0, [x19, 56]
	cbnz	w0, .L152
	mov	w0, 15
	str	w0, [x19, 56]
.L152:
	mov	w2, 0
	mov	x1, 0
	mov	x0, 0
	str	x10, [sp, 64]
	str	w11, [sp, 72]
	str	w7, [sp, 80]
	str	x8, [sp, 88]
	bl	crc32
	str	x0, [x19, 32]
	mov	w1, -29921
	mov	w2, 2
	strh	w1, [sp, 136]
	add	x1, sp, 136
	bl	crc32
	ldr	x10, [sp, 64]
	mov	w6, 0
	ldr	x8, [sp, 88]
	mov	x9, 0
	ldr	w11, [sp, 72]
	ldr	w7, [sp, 80]
	str	x0, [x19, 32]
	mov	w0, 16181
	str	w0, [x19, 8]
	.p2align 5,,15
.L161:
	cbz	w7, .L103
	ldrb	w0, [x8], 1
	sub	w7, w7, #1
	lsl	x0, x0, x6
	add	w6, w6, 8
	add	x9, x9, x0
.L590:
	cmp	w6, 15
	bls	.L161
	and	w0, w9, 255
	str	w9, [x19, 24]
	cmp	w0, 8
	beq	.L620
.L320:
	adrp	x0, .LC2
	add	x0, x0, :lo12:.LC2
.L605:
	str	x0, [x20, 48]
	mov	w0, 16209
	str	w0, [x19, 8]
.L606:
	mov	w22, -3
	b	.L103
	.p2align 2,,3
.L587:
	ldp	x23, x24, [sp, 48]
	.cfi_restore 24
	.cfi_restore 23
.L97:
	mov	w22, -2
	b	.L96
	.p2align 2,,3
.L310:
	.cfi_offset 23, -96
	.cfi_offset 24, -88
	ldr	x1, [x20, 24]
	str	w2, [sp, 64]
	str	w4, [sp, 72]
	sub	x1, x1, x3
	ldr	w3, [x19, 24]
	ldr	x0, [x19, 32]
	cbnz	w3, .L621
	bl	adler32
	ldr	w2, [sp, 64]
	ldr	w4, [sp, 72]
.L313:
	str	x0, [x19, 32]
	str	x0, [x20, 96]
	b	.L607
	.p2align 2,,3
.L613:
	.cfi_restore 23
	.cfi_restore 24
	ldr	w0, [x20, 8]
	cbz	w0, .L98
	mov	w22, -2
	b	.L96
	.p2align 2,,3
.L614:
	.cfi_offset 23, -96
	.cfi_offset 24, -88
	add	w0, w0, 128
	orr	w4, w4, w2
	str	w0, [x20, 88]
	cmp	w4, 0
	ccmp	w21, 4, 4, ne
	beq	.L622
.L588:
	ldp	x23, x24, [sp, 48]
	.cfi_remember_state
	.cfi_restore 24
	.cfi_restore 23
	mov	w0, w22
	ldp	x19, x20, [sp, 16]
	ldp	x21, x22, [sp, 32]
	ldp	x29, x30, [sp], 144
	.cfi_restore 30
	.cfi_restore 29
	.cfi_restore 21
	.cfi_restore 22
	.cfi_restore 19
	.cfi_restore 20
	.cfi_def_cfa_offset 0
	ret
	.p2align 2,,3
.L307:
	.cfi_restore_state
	ldr	x1, [x20, 16]
	ldr	w0, [x20, 8]
	sub	w4, w24, w0
	sub	w0, w24, w0
	add	x0, x1, x0
	str	x0, [x20, 16]
	b	.L311
	.p2align 2,,3
.L295:
	cbz	w7, .L103
	ldrb	w0, [x8], 1
	sub	w7, w7, #1
	lsl	x0, x0, x6
	add	w6, w6, 8
	add	x9, x9, x0
.L602:
	cmp	w6, 31
	bls	.L295
	ldr	x0, [x20, 40]
	sub	w2, w23, w11
	sub	w23, w23, w11
	add	x0, x0, x23
	str	x0, [x20, 40]
	ldr	x0, [x19, 40]
	add	x0, x0, x23
	str	x0, [x19, 40]
	ands	w0, w1, 4
	ccmp	w2, 0, 4, ne
	bne	.L623
	cbz	w0, .L624
.L317:
	ldr	w0, [x19, 24]
	cbz	w0, .L300
	ldr	x0, [x19, 32]
	cmp	x0, x9
	beq	.L625
.L301:
	adrp	x0, .LC16
	add	x0, x0, :lo12:.LC16
	str	x0, [x20, 48]
	mov	w23, w11
	mov	w0, 16209
	mov	w22, -3
	str	w0, [x19, 8]
	b	.L103
	.p2align 2,,3
.L621:
	bl	crc32
	ldr	w2, [sp, 64]
	ldr	w4, [sp, 72]
	b	.L313
.L217:
	adrp	x0, .LANCHOR0
	add	x0, x0, :lo12:.LANCHOR0
	add	x1, x0, 2048
	stp	x0, x1, [x19, 104]
	adrp	x0, .LC19
	lsr	x9, x9, 3
	sub	w6, w6, #3
	ldr	d31, [x0, #:lo12:.LC19]
	mov	w0, 16199
	str	w0, [x19, 8]
	str	d31, [x19, 120]
	cmp	w21, 6
	beq	.L331
	.p2align 5,,15
.L113:
	mov	w0, 16200
	str	w0, [x19, 8]
.L112:
	cmp	w11, 257
	ccmp	w7, 5, 0, hi
	bls	.L257
	str	x8, [x20]
	mov	w1, w23
	str	w7, [x20, 8]
	mov	x0, x20
	str	x10, [x20, 24]
	str	w11, [x20, 32]
	str	x9, [x19, 80]
	str	w6, [x19, 88]
	bl	inflate_fast
	ldr	w1, [x19, 8]
	mov	w0, 16191
	ldr	x8, [x20]
	cmp	w1, w0
	ldr	x10, [x20, 24]
	mov	w12, -16180
	ldr	x9, [x19, 80]
	ldr	w6, [x19, 88]
	ldr	w7, [x20, 8]
	ldr	w11, [x20, 32]
	bne	.L100
	mov	w0, -1
	str	w0, [x19, 7148]
	b	.L121
	.p2align 2,,3
.L619:
	cmp	w6, 2
	bhi	.L214
	cbz	w7, .L331
	ldrb	w0, [x8], 1
	sub	w7, w7, #1
	lsl	x0, x0, x6
	add	w6, w6, 8
	add	x9, x9, x0
.L214:
	and	w0, w9, 1
	str	w0, [x19, 12]
	ubfx	w0, w9, 1, 2
	cmp	w0, 2
	beq	.L215
	cmp	w0, 3
	beq	.L216
	cmp	w0, 1
	beq	.L217
	lsr	x9, x9, 3
	sub	w6, w6, #3
	mov	w0, 16193
	str	w0, [x19, 8]
.L119:
	and	w0, w6, 7
	and	w6, w6, -8
	lsr	x9, x9, x0
	cmp	w6, 31
	bhi	.L219
	.p2align 5,,15
.L220:
	cbz	w7, .L103
	ldrb	w0, [x8], 1
	sub	w7, w7, #1
	lsl	x0, x0, x6
	add	w6, w6, 8
	add	x9, x9, x0
	cmp	w6, 31
	bls	.L220
	mov	w6, 32
.L219:
	lsr	x0, x9, 16
	and	x1, x9, 65535
	eor	x0, x0, 65535
	cmp	x0, w9, uxth
	beq	.L221
	adrp	x0, .LC7
	add	x0, x0, :lo12:.LC7
	str	x0, [x20, 48]
	mov	w22, -3
	mov	w0, 16209
	str	w0, [x19, 8]
	b	.L103
	.p2align 2,,3
.L109:
	ldr	w1, [x19, 100]
.L134:
	cbnz	w1, .L601
.L283:
	mov	w0, 16204
	str	w0, [x19, 8]
.L108:
	cbz	w11, .L103
	ldr	w1, [x19, 96]
	sub	w0, w23, w11
	cmp	w1, w0
	bls	.L286
	add	w1, w1, w11
	ldr	w2, [x19, 64]
	sub	w0, w1, w23
	cmp	w2, w0
	bcs	.L287
	ldr	w2, [x19, 7144]
	cbz	w2, .L287
	adrp	x0, .LC15
	add	x0, x0, :lo12:.LC15
	str	x0, [x20, 48]
	mov	w22, -3
	mov	w0, 16209
	str	w0, [x19, 8]
	b	.L103
	.p2align 2,,3
.L111:
	ldr	w1, [x19, 100]
	cbnz	w1, .L600
.L635:
	ldr	w0, [x19, 92]
.L271:
	str	w0, [x19, 7152]
	mov	w0, 16202
	str	w0, [x19, 8]
.L110:
	ldr	w0, [x19, 124]
	mov	w2, -1
	ldr	x3, [x19, 112]
	lsl	w2, w2, w0
	bic	w0, w9, w2
	lsl	x5, x0, 2
	add	x0, x3, x0, lsl 2
	ldrb	w1, [x0, 1]
	mov	w4, w1
	cmp	w6, w1
	bcs	.L274
	.p2align 5,,15
.L275:
	cbz	w7, .L103
	ldrb	w0, [x8], 1
	sub	w7, w7, #1
	lsl	x0, x0, x6
	add	x9, x9, x0
	bic	w0, w9, w2
	add	w6, w6, 8
	lsl	x5, x0, 2
	add	x0, x3, x0, lsl 2
	ldrb	w1, [x0, 1]
	mov	w4, w1
	cmp	w1, w6
	bhi	.L275
.L274:
	ldrb	w2, [x3, x5]
	mov	w5, w1
	ldrh	w13, [x0, 2]
	cmp	w2, 15
	bls	.L277
	ldr	w5, [x19, 7148]
.L278:
	sub	w6, w6, w1
	lsr	x9, x9, x1
	add	w1, w1, w5
	str	w1, [x19, 7148]
	tbz	x2, 6, .L282
	adrp	x0, .LC14
	add	x0, x0, :lo12:.LC14
	str	x0, [x20, 48]
	mov	w22, -3
	mov	w0, 16209
	str	w0, [x19, 8]
	b	.L103
	.p2align 2,,3
.L105:
	ldr	w1, [x19, 16]
	cbz	w1, .L293
	ldr	w0, [x19, 24]
	cbz	w0, .L293
	.p2align 5,,15
.L603:
	cmp	w6, 31
	bhi	.L626
.L305:
	cbz	w7, .L103
	ldrb	w0, [x8], 1
	sub	w7, w7, #1
	lsl	x0, x0, x6
	add	w6, w6, 8
	add	x9, x9, x0
	b	.L603
	.p2align 2,,3
.L626:
	tbz	x1, 2, .L334
	ldr	w0, [x19, 40]
	cmp	x0, x9
	beq	.L334
	adrp	x0, .LC17
	add	x0, x0, :lo12:.LC17
	str	x0, [x20, 48]
	mov	w22, -3
	mov	w0, 16209
	str	w0, [x19, 8]
	b	.L103
	.p2align 2,,3
.L285:
	cbz	w7, .L103
	ldrb	w0, [x8], 1
	sub	w7, w7, #1
	lsl	x0, x0, x6
	add	w6, w6, 8
	add	x9, x9, x0
.L601:
	cmp	w6, w1
	bcc	.L285
	ldr	w2, [x19, 96]
	mov	w0, -1
	lsl	w0, w0, w1
	bic	w0, w9, w0
	add	w0, w2, w0
	str	w0, [x19, 96]
	ldr	w0, [x19, 7148]
	sub	w6, w6, w1
	lsr	x9, x9, x1
	add	w0, w0, w1
	str	w0, [x19, 7148]
	b	.L283
	.p2align 2,,3
.L114:
	ldp	w2, w13, [x19, 132]
	ldr	w5, [x19, 140]
	add	w13, w2, w13
	cmp	w13, w5
	bls	.L250
.L233:
	ldr	w0, [x19, 120]
	mov	w3, -1
	ldr	x4, [x19, 104]
	lsl	w3, w3, w0
	b	.L609
	.p2align 2,,3
.L235:
	cbz	w7, .L103
	ldrb	w0, [x8], 1
	sub	w7, w7, #1
	lsl	x0, x0, x6
	add	w6, w6, 8
	add	x9, x9, x0
.L609:
	bic	w0, w9, w3
	add	x0, x4, x0, lsl 2
	ldrb	w1, [x0, 1]
	cmp	w1, w6
	bhi	.L235
	ldrh	w0, [x0, 2]
	cmp	w0, 15
	bhi	.L236
	sub	w6, w6, w1
	lsr	x9, x9, x1
	add	w1, w5, 1
	add	x5, x19, w5, uxtw 1
	str	w1, [x19, 140]
	strh	w0, [x5, 152]
	mov	w5, w1
.L237:
	cmp	w13, w5
	bhi	.L609
.L250:
	ldr	w1, [x19, 8]
	mov	w0, 16209
	cmp	w1, w0
	beq	.L606
.L234:
	ldrh	w0, [x19, 664]
	cbnz	w0, .L253
	adrp	x0, .LC10
	add	x0, x0, :lo12:.LC10
	str	x0, [x20, 48]
	mov	w22, -3
	mov	w0, 16209
	str	w0, [x19, 8]
	b	.L103
	.p2align 2,,3
.L273:
	cbz	w7, .L103
	ldrb	w0, [x8], 1
	sub	w7, w7, #1
	lsl	x0, x0, x6
	add	w6, w6, 8
	add	x9, x9, x0
.L600:
	cmp	w6, w1
	bcc	.L273
	ldr	w2, [x19, 92]
	mov	w0, -1
	lsl	w0, w0, w1
	bic	w0, w9, w0
	add	w0, w0, w2
	ldr	w2, [x19, 7148]
	sub	w6, w6, w1
	lsr	x9, x9, x1
	add	w1, w2, w1
	str	w0, [x19, 92]
	str	w1, [x19, 7148]
	b	.L271
	.p2align 2,,3
.L124:
	ldr	w1, [x19, 24]
	.p2align 5,,15
.L138:
	tbnz	x1, 9, .L594
.L206:
	ldr	x0, [x19, 48]
	cbz	x0, .L209
	ubfx	x1, x1, 9, 1
	mov	w2, 1
	stp	w1, w2, [x0, 68]
.L209:
	mov	w2, 0
	mov	x1, 0
	mov	x0, 0
	str	x10, [sp, 64]
	str	w11, [sp, 72]
	str	x9, [sp, 80]
	str	w7, [sp, 88]
	str	x8, [sp, 96]
	str	w6, [sp, 104]
	bl	crc32
	b	.L596
	.p2align 2,,3
.L208:
	cbz	w7, .L103
	ldrb	w0, [x8], 1
	sub	w7, w7, #1
	lsl	x0, x0, x6
	add	w6, w6, 8
	add	x9, x9, x0
.L594:
	cmp	w6, 15
	bls	.L208
	ldr	w0, [x19, 16]
	tbz	x0, 2, .L324
	ldrh	w0, [x19, 32]
	cmp	x0, x9
	beq	.L324
	adrp	x0, .LC5
	add	x0, x0, :lo12:.LC5
	b	.L605
	.p2align 2,,3
.L118:
	ldr	w0, [x19, 92]
.L137:
	mov	w1, 16195
	str	w1, [x19, 8]
.L136:
	cbz	w0, .L597
	cmp	w7, w0
	csel	w0, w7, w0, ls
	cmp	w0, w11
	csel	w3, w0, w11, ls
	cbz	w3, .L103
	uxtw	x2, w3
	mov	x1, x8
	mov	x0, x10
	stp	x8, x2, [sp, 64]
	str	w3, [sp, 80]
	str	w11, [sp, 88]
	str	x9, [sp, 96]
	str	w7, [sp, 104]
	str	w6, [sp, 120]
	bl	memcpy
	ldp	x8, x2, [sp, 64]
	mov	w12, -16180
	ldr	w3, [sp, 80]
	ldr	w11, [sp, 88]
	ldr	w7, [sp, 104]
	ldr	x9, [sp, 96]
	add	x10, x0, x2
	ldr	w0, [x19, 92]
	sub	w7, w7, w3
	ldr	w1, [x19, 8]
	sub	w11, w11, w3
	sub	w0, w0, w3
	ldr	w6, [sp, 120]
	add	x8, x8, x2
	str	w0, [x19, 92]
	b	.L100
	.p2align 2,,3
.L117:
	ldr	w0, [x19, 92]
	b	.L136
	.p2align 2,,3
.L115:
	ldr	w2, [x19, 128]
	ldr	w0, [x19, 140]
	cmp	w0, w2
	bcs	.L227
.L225:
	adrp	x1, .LANCHOR0
	add	x1, x1, :lo12:.LANCHOR0
	add	x1, x1, 2176
	add	w3, w0, 1
	add	x1, x1, w0, uxtw 1
	cmp	w6, 2
	bhi	.L229
.L228:
	cbz	w7, .L103
	ldrb	w0, [x8], 1
	sub	w7, w7, #1
	lsl	x0, x0, x6
	add	w6, w6, 8
	add	x9, x9, x0
.L229:
	ldrh	w0, [x1], 2
	and	w4, w9, 7
	str	w3, [x19, 140]
	lsr	x9, x9, 3
	sub	w6, w6, #3
	add	x0, x19, x0, lsl 1
	strh	w4, [x0, 152]
	cmp	w3, w2
	beq	.L627
	add	w3, w3, 1
	cmp	w6, 2
	bls	.L228
	b	.L229
	.p2align 2,,3
.L132:
	ldr	w1, [x19, 16]
	cbnz	w1, .L589
	mov	w0, 16192
	str	w0, [x19, 8]
	b	.L133
	.p2align 2,,3
.L149:
	cbz	w7, .L103
	ldrb	w0, [x8], 1
	sub	w7, w7, #1
	lsl	x0, x0, x6
	add	w6, w6, 8
	add	x9, x9, x0
.L589:
	cmp	w6, 15
	bls	.L149
	tst	x1, 2
	mov	x0, 35615
	ccmp	x9, x0, 0, ne
	beq	.L628
	ldr	x0, [x19, 48]
	cbz	x0, .L153
	mov	w1, -1
	str	w1, [x0, 72]
	ldr	w1, [x19, 16]
.L153:
	tbz	x1, 0, .L154
	ubfiz	x0, x9, 8, 8
	mov	x1, 31
	add	x0, x0, x9, lsr 8
	udiv	x1, x0, x1
	lsl	x2, x1, 5
	sub	x1, x2, x1
	cmp	x0, x1
	bne	.L154
	and	w0, w9, 15
	cmp	w0, 8
	bne	.L320
	ldr	w1, [x19, 56]
	lsr	x9, x9, 4
	and	w0, w9, 15
	add	w0, w0, 8
	cbnz	w1, .L157
	str	w0, [x19, 56]
	cmp	w0, 15
	bls	.L158
.L159:
	adrp	x0, .LC4
	add	x0, x0, :lo12:.LC4
	str	x0, [x20, 48]
	sub	w6, w6, #4
	mov	w0, 16209
	mov	w22, -3
	str	w0, [x19, 8]
	b	.L103
	.p2align 2,,3
.L128:
	ldr	w2, [x19, 24]
	mov	w1, w2
	tbnz	x2, 10, .L593
	ldr	x0, [x19, 48]
	cbz	x0, .L186
	str	xzr, [x0, 24]
	b	.L186
	.p2align 2,,3
.L127:
	ldr	w1, [x19, 24]
	and	w2, w1, 1024
.L141:
	cbz	w2, .L186
	ldr	w2, [x19, 92]
	cmp	w7, w2
	csel	w3, w7, w2, ls
	cbz	w3, .L190
	ldr	x4, [x19, 48]
	cbz	x4, .L191
	ldr	x0, [x4, 24]
	cbz	x0, .L191
	ldp	w13, w5, [x4, 32]
	sub	w4, w13, w2
	cmp	w5, w4
	bls	.L191
	add	w1, w3, w4
	add	w2, w5, w2
	cmp	w5, w1
	sub	w2, w2, w13
	mov	x1, x8
	csel	w2, w2, w3, cc
	add	x0, x0, w4, uxtw
	str	x8, [sp, 64]
	str	w3, [sp, 72]
	str	x10, [sp, 80]
	str	w11, [sp, 88]
	str	x9, [sp, 96]
	str	w7, [sp, 104]
	str	w6, [sp, 120]
	bl	memcpy
	ldr	x8, [sp, 64]
	ldr	x10, [sp, 80]
	ldr	x9, [sp, 96]
	ldr	w1, [x19, 24]
	ldr	w3, [sp, 72]
	ldr	w11, [sp, 88]
	ldr	w7, [sp, 104]
	ldr	w6, [sp, 120]
.L191:
	tbz	x1, 9, .L193
	ldr	w0, [x19, 16]
	tbnz	x0, 2, .L629
.L193:
	ldr	w2, [x19, 92]
	sub	w7, w7, w3
	add	x8, x8, w3, uxtw
	sub	w2, w2, w3
	str	w2, [x19, 92]
.L190:
	cbnz	w2, .L103
	ldr	w1, [x19, 24]
.L186:
	mov	w0, 16186
	str	w0, [x19, 8]
	str	wzr, [x19, 92]
.L140:
	tbnz	x1, 11, .L630
	ldr	x0, [x19, 48]
	cbz	x0, .L198
	str	xzr, [x0, 40]
.L198:
	mov	w0, 16187
	str	w0, [x19, 8]
	str	wzr, [x19, 92]
.L139:
	tbnz	x1, 12, .L631
	ldr	x0, [x19, 48]
	cbz	x0, .L204
	str	xzr, [x0, 56]
.L204:
	mov	w0, 16188
	str	w0, [x19, 8]
	b	.L138
	.p2align 2,,3
.L126:
	ldr	w1, [x19, 24]
	b	.L140
	.p2align 2,,3
.L125:
	ldr	w1, [x19, 24]
	b	.L139
	.p2align 2,,3
.L257:
	ldr	w0, [x19, 120]
	mov	w2, -1
	ldr	x3, [x19, 104]
	str	wzr, [x19, 7148]
	lsl	w2, w2, w0
	bic	w0, w9, w2
	lsl	x4, x0, 2
	add	x0, x3, x0, lsl 2
	ldrb	w1, [x0, 1]
	cmp	w6, w1
	bcs	.L258
	.p2align 5,,15
.L259:
	cbz	w7, .L103
	ldrb	w0, [x8], 1
	sub	w7, w7, #1
	lsl	x0, x0, x6
	add	x9, x9, x0
	bic	w0, w9, w2
	add	w6, w6, 8
	lsl	x4, x0, 2
	add	x0, x3, x0, lsl 2
	ldrb	w1, [x0, 1]
	cmp	w1, w6
	bhi	.L259
.L258:
	ldrb	w2, [x3, x4]
	ldrh	w4, [x0, 2]
	cbnz	w2, .L261
	sub	w6, w6, w1
	lsr	x9, x9, x1
	str	w4, [x19, 92]
	str	w1, [x19, 7148]
.L262:
	mov	w0, 16205
	str	w0, [x19, 8]
.L107:
	cbz	w11, .L103
	ldr	w0, [x19, 92]
	sub	w11, w11, #1
	strb	w0, [x10], 1
	mov	w0, 16200
	str	w0, [x19, 8]
	b	.L112
.L617:
	mov	w6, 0
	mov	x9, 0
	.p2align 5,,15
.L180:
	cbz	w7, .L103
	ldrb	w0, [x8], 1
	sub	w7, w7, #1
	lsl	x0, x0, x6
	add	w6, w6, 8
	add	x9, x9, x0
.L593:
	cmp	w6, 15
	bls	.L180
	ldr	x0, [x19, 48]
	str	w9, [x19, 92]
	cbz	x0, .L181
	str	w9, [x0, 32]
	ldr	w2, [x19, 24]
.L181:
	mov	w1, w2
	tbz	x1, 9, .L182
	ldr	w0, [x19, 16]
	tbnz	x0, 2, .L183
.L182:
	and	w2, w2, 1024
.L184:
	mov	w0, 16185
	mov	w6, 0
	mov	x9, 0
	str	w0, [x19, 8]
	b	.L141
	.p2align 2,,3
.L631:
	cbz	w7, .L103
	mov	x1, x8
	mov	w2, 0
	.p2align 5,,15
.L202:
	ldr	x0, [x19, 48]
	add	w2, w2, 1
	ldrb	w3, [x1]
	cbz	x0, .L201
	ldr	x4, [x0, 56]
	cbz	x4, .L201
	ldr	w0, [x0, 64]
	ldr	w5, [x19, 92]
	cmp	w5, w0
	bcs	.L201
	add	w0, w5, 1
	str	w0, [x19, 92]
	strb	w3, [x4, w5, uxtw]
.L201:
	cmp	w3, 0
	add	x1, x1, 1
	ccmp	w7, w2, 0, ne
	bhi	.L202
	ldr	w0, [x19, 24]
	tbz	x0, 9, .L203
	ldr	w0, [x19, 16]
	tbz	x0, 2, .L203
	ldr	x0, [x19, 32]
	mov	x1, x8
	str	x8, [sp, 64]
	str	w2, [sp, 72]
	str	x10, [sp, 80]
	str	w3, [sp, 88]
	str	w11, [sp, 96]
	str	x9, [sp, 104]
	str	w6, [sp, 112]
	str	w7, [sp, 120]
	bl	crc32
	ldr	x8, [sp, 64]
	str	x0, [x19, 32]
	ldr	x10, [sp, 80]
	ldr	x9, [sp, 104]
	ldr	w2, [sp, 72]
	ldr	w3, [sp, 88]
	ldr	w11, [sp, 96]
	ldr	w6, [sp, 112]
	ldr	w7, [sp, 120]
	.p2align 5,,15
.L203:
	sub	w7, w7, w2
	add	x8, x8, w2, uxtw
	cbnz	w3, .L103
	ldr	w1, [x19, 24]
	b	.L204
.L261:
	cmp	w2, 15
	bls	.L263
	sub	w6, w6, w1
	lsr	x9, x9, x1
	str	w4, [x19, 92]
	str	w1, [x19, 7148]
.L264:
	tbz	x2, 5, .L268
	mov	w0, -1
	str	w0, [x19, 7148]
	.p2align 5,,15
.L597:
	mov	w0, 16191
	str	w0, [x19, 8]
	b	.L121
	.p2align 2,,3
.L630:
	cbz	w7, .L103
	mov	x1, x8
	mov	w2, 0
	.p2align 5,,15
.L196:
	ldr	x0, [x19, 48]
	add	w2, w2, 1
	ldrb	w3, [x1]
	cbz	x0, .L195
	ldr	x4, [x0, 40]
	cbz	x4, .L195
	ldr	w0, [x0, 48]
	ldr	w5, [x19, 92]
	cmp	w5, w0
	bcs	.L195
	add	w0, w5, 1
	str	w0, [x19, 92]
	strb	w3, [x4, w5, uxtw]
.L195:
	cmp	w3, 0
	add	x1, x1, 1
	ccmp	w7, w2, 0, ne
	bhi	.L196
	ldr	w0, [x19, 24]
	tbz	x0, 9, .L197
	ldr	w0, [x19, 16]
	tbz	x0, 2, .L197
	ldr	x0, [x19, 32]
	mov	x1, x8
	str	x8, [sp, 64]
	str	w2, [sp, 72]
	str	x10, [sp, 80]
	str	w3, [sp, 88]
	str	w11, [sp, 96]
	str	x9, [sp, 104]
	str	w6, [sp, 112]
	str	w7, [sp, 120]
	bl	crc32
	ldr	x8, [sp, 64]
	str	x0, [x19, 32]
	ldr	x10, [sp, 80]
	ldr	x9, [sp, 104]
	ldr	w2, [sp, 72]
	ldr	w3, [sp, 88]
	ldr	w11, [sp, 96]
	ldr	w6, [sp, 112]
	ldr	w7, [sp, 120]
	.p2align 5,,15
.L197:
	sub	w7, w7, w2
	add	x8, x8, w2, uxtw
	cbnz	w3, .L103
	ldr	w1, [x19, 24]
	b	.L198
	.p2align 2,,3
.L287:
	ldr	w2, [x19, 68]
	sub	w1, w23, w1
	ldr	x3, [x19, 72]
	cmp	w2, w0
	bcs	.L288
	ldr	w4, [x19, 60]
	sub	w0, w0, w2
	add	w2, w2, w4
	add	w1, w2, w1
	add	x1, x3, x1
.L289:
	ldr	w2, [x19, 92]
	cmp	w2, w0
	csel	w0, w2, w0, ls
.L290:
	cmp	w0, w11
	csel	w0, w0, w11, ls
	sub	w3, w0, #1
	sub	w2, w2, w0
	add	x4, x3, 1
	sub	w11, w11, w0
	mov	x0, 0
	str	w2, [x19, 92]
	.p2align 5,,15
.L291:
	ldrb	w2, [x1, x0]
	strb	w2, [x10, x0]
	add	x0, x0, 1
	cmp	x0, x4
	bne	.L291
	ldr	w0, [x19, 92]
	add	x10, x10, 1
	add	x10, x10, x3
	cbz	w0, .L113
	ldr	w1, [x19, 8]
	b	.L100
	.p2align 2,,3
.L236:
	cmp	w0, 16
	beq	.L632
	cmp	w0, 17
	beq	.L243
	add	w14, w1, 7
	cmp	w14, w6
	bls	.L245
	.p2align 5,,15
.L244:
	cbz	w7, .L103
	ldrb	w0, [x8], 1
	sub	w7, w7, #1
	lsl	x0, x0, x6
	add	w6, w6, 8
	add	x9, x9, x0
	cmp	w6, w14
	bcc	.L244
.L245:
	lsr	x9, x9, x1
	sub	w1, w6, w1
	and	w15, w9, 127
	sub	w6, w1, #7
	add	w15, w15, 11
	lsr	x9, x9, 7
	mov	w14, 0
.L242:
	add	w5, w15, w5
	cmp	w5, w13
	bhi	.L241
	ldr	w0, [x19, 140]
	add	w5, w0, w15
	.p2align 5,,15
.L248:
	add	x1, x19, w0, uxtw 1
	add	w0, w0, 1
	strh	w14, [x1, 152]
	cmp	w0, w5
	bne	.L248
	str	w5, [x19, 140]
	b	.L237
	.p2align 2,,3
.L324:
	mov	w6, 0
	mov	x9, 0
	b	.L206
	.p2align 2,,3
.L288:
	add	w2, w2, w1
	add	x1, x3, x2
	b	.L289
.L309:
	mov	w0, 16210
	str	w0, [x19, 8]
	.p2align 5,,15
.L101:
	ldp	x23, x24, [sp, 48]
	.cfi_remember_state
	.cfi_restore 24
	.cfi_restore 23
	mov	w22, -4
	b	.L96
.L277:
	.cfi_restore_state
	add	w2, w2, w1
	mov	w14, -1
	lsl	w14, w14, w2
	bic	w0, w9, w14
	lsr	w0, w0, w1
	b	.L611
	.p2align 2,,3
.L633:
	cbz	w7, .L103
	ldrb	w0, [x8], 1
	sub	w7, w7, #1
	lsl	x0, x0, x6
	add	x9, x9, x0
	bic	w0, w9, w14
	add	w6, w6, 8
	lsr	w0, w0, w5
.L611:
	add	w0, w0, w13
	lsl	x15, x0, 2
	add	x0, x3, x0, lsl 2
	ldrb	w1, [x0, 1]
	add	w2, w1, w5
	cmp	w2, w6
	bhi	.L633
	ldrh	w13, [x0, 2]
	lsr	x9, x9, x5
	ldr	w0, [x19, 7148]
	sub	w6, w6, w4
	ldrb	w2, [x3, x15]
	add	w5, w5, w0
	b	.L278
.L215:
	mov	w0, 16196
	lsr	x9, x9, 3
	sub	w6, w6, #3
	str	w0, [x19, 8]
	b	.L598
.L183:
	ldr	x0, [x19, 32]
	add	x1, sp, 136
	mov	w2, 2
	str	x10, [sp, 64]
	str	w11, [sp, 72]
	str	w7, [sp, 80]
	str	x8, [sp, 88]
	strh	w9, [sp, 136]
	bl	crc32
	ldr	w1, [x19, 24]
	ldr	x10, [sp, 64]
	and	w2, w1, 1024
	ldr	x8, [sp, 88]
	str	x0, [x19, 32]
	ldr	w11, [sp, 72]
	ldr	w7, [sp, 80]
	b	.L184
	.p2align 2,,3
.L286:
	ldr	w0, [x19, 92]
	sub	x1, x10, w1, uxtw
	mov	w2, w0
	b	.L290
.L256:
	mov	w0, 16199
	str	w0, [x19, 8]
	cmp	w21, 6
	bne	.L113
	.p2align 5,,15
.L331:
	mov	w22, 0
	b	.L103
	.p2align 2,,3
.L282:
	and	w1, w2, 15
	mov	w0, 16203
	str	w0, [x19, 8]
	stp	w13, w1, [x19, 96]
	b	.L134
.L221:
	mov	w0, w1
	str	w1, [x19, 92]
	mov	w1, 16194
	str	w1, [x19, 8]
	mov	w6, 0
	mov	x9, 0
	cmp	w21, 6
	bne	.L137
	b	.L103
	.p2align 2,,3
.L620:
	tst	x9, 57344
	beq	.L163
	adrp	x0, .LC1
	add	x0, x0, :lo12:.LC1
	b	.L605
.L243:
	add	w14, w1, 3
	cmp	w14, w6
	bls	.L246
	.p2align 5,,15
.L247:
	cbz	w7, .L103
	ldrb	w0, [x8], 1
	sub	w7, w7, #1
	lsl	x0, x0, x6
	add	w6, w6, 8
	add	x9, x9, x0
	cmp	w14, w6
	bhi	.L247
.L246:
	lsr	x9, x9, x1
	sub	w1, w6, w1
	and	w15, w9, 7
	sub	w6, w1, #3
	add	w15, w15, 3
	lsr	x9, x9, 3
	mov	w14, 0
	b	.L242
.L216:
	adrp	x0, .LC6
	add	x0, x0, :lo12:.LC6
	str	x0, [x20, 48]
	lsr	x9, x9, 3
	mov	w0, 16209
	sub	w6, w6, #3
	mov	w22, -3
	str	w0, [x19, 8]
	b	.L103
.L627:
	mov	w0, w3
.L227:
	cmp	w0, 18
	bhi	.L230
	adrp	x2, .LANCHOR0
	add	x2, x2, :lo12:.LANCHOR0
	add	x1, x2, 2176
	add	x2, x2, 2214
	add	x0, x1, w0, uxtw 1
	.p2align 5,,15
.L231:
	ldrh	w1, [x0], 2
	add	x1, x19, x1, lsl 1
	strh	wzr, [x1, 152]
	cmp	x0, x2
	bne	.L231
	mov	w0, 19
	str	w0, [x19, 140]
.L230:
	add	x0, x19, 1368
	str	x0, [x19, 104]
	str	x0, [x19, 144]
	mov	w0, 7
	str	w0, [x19, 120]
	add	x5, x19, 792
	add	x4, x19, 120
	add	x3, x19, 144
	add	x1, x19, 152
	mov	w2, 19
	mov	w0, 0
	str	x10, [sp, 64]
	str	w11, [sp, 72]
	str	x9, [sp, 80]
	str	w7, [sp, 88]
	str	x8, [sp, 96]
	str	w6, [sp, 104]
	bl	inflate_table
	ldr	x10, [sp, 64]
	ldr	x9, [sp, 80]
	ldr	x8, [sp, 96]
	ldr	w11, [sp, 72]
	ldr	w7, [sp, 88]
	ldr	w6, [sp, 104]
	cbz	w0, .L232
	adrp	x0, .LC9
	add	x0, x0, :lo12:.LC9
	str	x0, [x20, 48]
	mov	w22, -3
	mov	w0, 16209
	str	w0, [x19, 8]
	b	.L103
.L629:
	ldr	x0, [x19, 32]
	mov	w2, w3
	mov	x1, x8
	str	x8, [sp, 64]
	str	w3, [sp, 72]
	str	x10, [sp, 80]
	str	w11, [sp, 88]
	str	x9, [sp, 96]
	str	w7, [sp, 104]
	str	w6, [sp, 120]
	bl	crc32
	ldr	x8, [sp, 64]
	str	x0, [x19, 32]
	ldr	x10, [sp, 80]
	ldr	x9, [sp, 96]
	ldr	w3, [sp, 72]
	ldr	w11, [sp, 88]
	ldr	w7, [sp, 104]
	ldr	w6, [sp, 120]
	b	.L193
.L268:
	tbz	x2, 6, .L269
	adrp	x0, .LC13
	add	x0, x0, :lo12:.LC13
	str	x0, [x20, 48]
	mov	w22, -3
	mov	w0, 16209
	str	w0, [x19, 8]
	b	.L103
.L300:
	ldr	x1, [x19, 32]
	rev	x0, x9
	cmp	x1, x0, lsr 32
	bne	.L301
.L303:
	mov	w23, w11
	mov	w6, 0
	mov	x9, 0
	b	.L293
.L616:
	ldr	x0, [x19, 32]
	add	x1, sp, 136
	mov	w2, 4
	str	x10, [sp, 64]
	str	w11, [sp, 72]
	str	w7, [sp, 80]
	str	x8, [sp, 88]
	str	w9, [sp, 136]
	bl	crc32
	ldr	x10, [sp, 64]
	str	x0, [x19, 32]
	ldr	x8, [sp, 88]
	ldr	w11, [sp, 72]
	ldr	w7, [sp, 80]
	b	.L170
.L154:
	adrp	x0, .LC3
	add	x0, x0, :lo12:.LC3
	str	x0, [x20, 48]
	mov	w22, -3
	mov	w0, 16209
	str	w0, [x19, 8]
	b	.L103
.L224:
	mov	w0, 16197
	str	w0, [x19, 8]
	mov	w0, 0
	str	wzr, [x19, 140]
	b	.L225
.L632:
	add	w14, w1, 2
	cmp	w14, w6
	bls	.L239
	.p2align 5,,15
.L240:
	cbz	w7, .L103
	ldrb	w0, [x8], 1
	sub	w7, w7, #1
	lsl	x0, x0, x6
	add	w6, w6, 8
	add	x9, x9, x0
	cmp	w14, w6
	bhi	.L240
.L239:
	sub	w6, w6, w1
	lsr	x9, x9, x1
	cbz	w5, .L241
	sub	w0, w5, #1
	and	w15, w9, 3
	add	w15, w15, 3
	lsr	x9, x9, 2
	add	x0, x19, x0, lsl 1
	sub	w6, w6, #2
	ldrh	w14, [x0, 152]
	b	.L242
.L623:
	ldr	w3, [x19, 24]
	sub	x1, x10, x23
	str	x10, [sp, 64]
	str	w11, [sp, 72]
	str	x9, [sp, 80]
	str	w7, [sp, 88]
	str	x8, [sp, 96]
	str	w6, [sp, 104]
	ldr	x0, [x19, 32]
	cbz	w3, .L297
	bl	crc32
	ldr	x10, [sp, 64]
	ldr	x9, [sp, 80]
	ldr	x8, [sp, 96]
	ldr	w11, [sp, 72]
	ldr	w7, [sp, 88]
	ldr	w6, [sp, 104]
.L298:
	ldr	w1, [x19, 16]
	str	x0, [x19, 32]
	str	x0, [x20, 96]
	tbnz	x1, 2, .L317
	mov	w0, 16207
	str	w0, [x19, 8]
	cbz	w1, .L303
.L316:
	ldr	w0, [x19, 24]
	cbz	w0, .L303
	mov	w23, w11
	mov	w6, 0
	mov	x9, 0
	b	.L305
.L263:
	add	w2, w2, w1
	mov	w0, -1
	lsl	w2, w0, w2
	b	.L610
	.p2align 2,,3
.L634:
	cbz	w7, .L103
	ldrb	w0, [x8], 1
	sub	w7, w7, #1
	lsl	x0, x0, x6
	add	w6, w6, 8
	add	x9, x9, x0
.L610:
	bic	w0, w9, w2
	lsr	w0, w0, w1
	add	w0, w0, w4
	lsl	x14, x0, 2
	add	x0, x3, x0, lsl 2
	ldrb	w5, [x0, 1]
	add	w13, w5, w1
	cmp	w13, w6
	bhi	.L634
	ldrb	w2, [x3, x14]
	lsr	x9, x9, x1
	ldrh	w0, [x0, 2]
	sub	w1, w6, w1
	str	w0, [x19, 92]
	sub	w6, w1, w5
	str	w13, [x19, 7148]
	lsr	x9, x9, x5
	cbz	w2, .L262
	b	.L264
.L624:
	mov	w0, 16207
	str	w0, [x19, 8]
	b	.L316
.L269:
	and	w1, w2, 15
	mov	w0, 16201
	str	w0, [x19, 8]
	str	w1, [x19, 100]
	cbnz	w1, .L600
	b	.L635
.L297:
	bl	adler32
	ldr	x10, [sp, 64]
	ldr	x9, [sp, 80]
	ldr	x8, [sp, 96]
	ldr	w11, [sp, 72]
	ldr	w7, [sp, 88]
	ldr	w6, [sp, 104]
	b	.L298
.L334:
	mov	w6, 0
	mov	x9, 0
	b	.L293
.L232:
	ldp	w2, w13, [x19, 132]
	mov	w0, 16198
	str	w0, [x19, 8]
	mov	w5, 0
	str	wzr, [x19, 140]
	adds	w13, w2, w13
	bne	.L233
	b	.L234
	.p2align 2,,3
.L618:
	str	x8, [x20]
	mov	w22, 2
	str	w7, [x20, 8]
	str	x10, [x20, 24]
	str	w11, [x20, 32]
	str	x9, [x19, 80]
	str	w6, [x19, 88]
	ldp	x23, x24, [sp, 48]
	.cfi_remember_state
	.cfi_restore 24
	.cfi_restore 23
	b	.L96
.L625:
	.cfi_restore_state
	mov	w0, 16207
	mov	w23, w11
	mov	w6, 0
	mov	x9, 0
	str	w0, [x19, 8]
	b	.L305
.L253:
	add	x0, x19, 1368
	str	x0, [x19, 104]
	str	x0, [x19, 144]
	mov	w0, 9
	str	w0, [x19, 120]
	add	x1, x19, 152
	add	x3, x19, 144
	add	x5, x19, 792
	add	x4, x19, 120
	mov	w0, 1
	stp	x1, x3, [sp, 64]
	stp	x5, x10, [sp, 80]
	str	w11, [sp, 96]
	stp	x9, x8, [sp, 104]
	stp	w7, w6, [sp, 120]
	bl	inflate_table
	ldp	x1, x3, [sp, 64]
	ldp	x5, x10, [sp, 80]
	ldp	x9, x8, [sp, 104]
	ldr	w11, [sp, 96]
	ldp	w7, w6, [sp, 120]
	cbz	w0, .L255
	adrp	x0, .LC11
	add	x0, x0, :lo12:.LC11
	str	x0, [x20, 48]
	mov	w22, -3
	mov	w0, 16209
	str	w0, [x19, 8]
	b	.L103
.L157:
	cmp	w0, 15
	bhi	.L159
	cmp	w1, w0
	bcc	.L159
.L158:
	mov	w1, 1
	str	x10, [sp, 64]
	str	w11, [sp, 72]
	lsl	w0, w1, w0
	str	x9, [sp, 80]
	mov	w2, 0
	str	w7, [sp, 88]
	mov	x1, 0
	str	x8, [sp, 96]
	stp	wzr, w0, [x19, 24]
	mov	x0, 0
	bl	adler32
	str	x0, [x19, 32]
	ldr	x9, [sp, 80]
	str	x0, [x20, 96]
	ldr	x10, [sp, 64]
	ands	x9, x9, 512
	ldr	x8, [sp, 96]
	mov	w12, -16180
	ldr	w11, [sp, 72]
	ldr	w7, [sp, 88]
	bne	.L160
	mov	w0, 16191
	mov	w6, 0
	str	w0, [x19, 8]
	b	.L121
.L615:
	ldr	x0, [x19, 32]
	add	x1, sp, 136
	mov	w2, 2
	str	x10, [sp, 64]
	str	w11, [sp, 72]
	str	w7, [sp, 80]
	str	x8, [sp, 88]
	strh	w9, [sp, 136]
	bl	crc32
	ldr	x10, [sp, 64]
	str	x0, [x19, 32]
	ldr	x8, [sp, 88]
	ldr	w11, [sp, 72]
	ldr	w7, [sp, 80]
	b	.L165
.L241:
	adrp	x0, .LC18
	add	x0, x0, :lo12:.LC18
	str	x0, [x20, 48]
	mov	w22, -3
	mov	w0, 16209
	str	w0, [x19, 8]
	b	.L103
.L255:
	mov	x4, x19
	ldr	x0, [x19, 144]
	str	x0, [x19, 112]
	mov	w0, 6
	str	w0, [x4, 124]!
	ldp	w0, w2, [x19, 132]
	str	x10, [sp, 64]
	str	w11, [sp, 72]
	str	x9, [sp, 80]
	add	x1, x1, x0, lsl 1
	mov	w0, 2
	str	w7, [sp, 88]
	str	x8, [sp, 96]
	str	w6, [sp, 104]
	bl	inflate_table
	ldr	x10, [sp, 64]
	mov	w12, -16180
	ldr	x9, [sp, 80]
	ldr	x8, [sp, 96]
	ldr	w11, [sp, 72]
	ldr	w7, [sp, 88]
	ldr	w6, [sp, 104]
	cbz	w0, .L256
	adrp	x0, .LC12
	add	x0, x0, :lo12:.LC12
	str	x0, [x20, 48]
	mov	w22, -3
	mov	w0, 16209
	str	w0, [x19, 8]
	b	.L103
	.cfi_endproc
.LFE15:
	.size	inflate, .-inflate
.section	.rodata.str1.8,"aMS",@progbits,1
	.align	3
.LC1:
	.string	"unknown header flags set"
	.align	3
.section	.rodata.str1.8,"aMS",@progbits,1
	.align	3
.LC2:
	.string	"unknown compression method"
	.align	3
.section	.rodata.str1.8,"aMS",@progbits,1
	.align	3
.LC3:
	.string	"incorrect header check"
	.align	3
.section	.rodata.str1.8,"aMS",@progbits,1
	.align	3
.LC4:
	.string	"invalid window size"
	.align	3
.section	.rodata.str1.8,"aMS",@progbits,1
	.align	3
.LC5:
	.string	"header crc mismatch"
	.align	3
.section	.rodata.str1.8,"aMS",@progbits,1
	.align	3
.LC6:
	.string	"invalid block type"
	.align	3
.section	.rodata.str1.8,"aMS",@progbits,1
	.align	3
.LC7:
	.string	"invalid stored block lengths"
	.align	3
.section	.rodata.str1.8,"aMS",@progbits,1
	.align	3
.LC8:
	.string	"too many length or distance symbols"
	.align	3
.section	.rodata.str1.8,"aMS",@progbits,1
	.align	3
.LC9:
	.string	"invalid code lengths set"
	.align	3
.section	.rodata.str1.8,"aMS",@progbits,1
	.align	3
.LC10:
	.string	"invalid code -- missing end-of-block"
	.align	3
.section	.rodata.str1.8,"aMS",@progbits,1
	.align	3
.LC11:
	.string	"invalid literal/lengths set"
	.align	3
.section	.rodata.str1.8,"aMS",@progbits,1
	.align	3
.LC12:
	.string	"invalid distances set"
	.align	3
.section	.rodata.str1.8,"aMS",@progbits,1
	.align	3
.LC13:
	.string	"invalid literal/length code"
	.align	3
.section	.rodata.str1.8,"aMS",@progbits,1
	.align	3
.LC14:
	.string	"invalid distance code"
	.align	3
.section	.rodata.str1.8,"aMS",@progbits,1
	.align	3
.LC15:
	.string	"invalid distance too far back"
	.align	3
.section	.rodata.str1.8,"aMS",@progbits,1
	.align	3
.LC16:
	.string	"incorrect data check"
	.align	3
.section	.rodata.str1.8,"aMS",@progbits,1
	.align	3
.LC17:
	.string	"incorrect length check"
	.align	3
.section	.rodata.str1.8,"aMS",@progbits,1
	.align	3
.LC18:
	.string	"invalid bit length repeat"
.section	.rodata.cst8,"aM",@progbits,8
	.align	3
.LC19:
	.word	9
	.word	5
	.section	.rodata
	.align	4
	.set	.LANCHOR0,. + 0
	.type	lenfix.1, %object
	.size	lenfix.1, 2048
lenfix.1:
	.byte	96, 7, 0, 0, 0, 8, 80, 0, 0, 8, 16, 0, 20, 8, 115, 0
	.byte	18, 7, 31, 0, 0, 8, 112, 0, 0, 8, 48, 0, 0, 9, 192, 0
	.byte	16, 7, 10, 0, 0, 8, 96, 0, 0, 8, 32, 0, 0, 9, 160, 0
	.byte	0, 8, 0, 0, 0, 8, 128, 0, 0, 8, 64, 0, 0, 9, 224, 0
	.byte	16, 7, 6, 0, 0, 8, 88, 0, 0, 8, 24, 0, 0, 9, 144, 0
	.byte	19, 7, 59, 0, 0, 8, 120, 0, 0, 8, 56, 0, 0, 9, 208, 0
	.byte	17, 7, 17, 0, 0, 8, 104, 0, 0, 8, 40, 0, 0, 9, 176, 0
	.byte	0, 8, 8, 0, 0, 8, 136, 0, 0, 8, 72, 0, 0, 9, 240, 0
	.byte	16, 7, 4, 0, 0, 8, 84, 0, 0, 8, 20, 0, 21, 8, 227, 0
	.byte	19, 7, 43, 0, 0, 8, 116, 0, 0, 8, 52, 0, 0, 9, 200, 0
	.byte	17, 7, 13, 0, 0, 8, 100, 0, 0, 8, 36, 0, 0, 9, 168, 0
	.byte	0, 8, 4, 0, 0, 8, 132, 0, 0, 8, 68, 0, 0, 9, 232, 0
	.byte	16, 7, 8, 0, 0, 8, 92, 0, 0, 8, 28, 0, 0, 9, 152, 0
	.byte	20, 7, 83, 0, 0, 8, 124, 0, 0, 8, 60, 0, 0, 9, 216, 0
	.byte	18, 7, 23, 0, 0, 8, 108, 0, 0, 8, 44, 0, 0, 9, 184, 0
	.byte	0, 8, 12, 0, 0, 8, 140, 0, 0, 8, 76, 0, 0, 9, 248, 0
	.byte	16, 7, 3, 0, 0, 8, 82, 0, 0, 8, 18, 0, 21, 8, 163, 0
	.byte	19, 7, 35, 0, 0, 8, 114, 0, 0, 8, 50, 0, 0, 9, 196, 0
	.byte	17, 7, 11, 0, 0, 8, 98, 0, 0, 8, 34, 0, 0, 9, 164, 0
	.byte	0, 8, 2, 0, 0, 8, 130, 0, 0, 8, 66, 0, 0, 9, 228, 0
	.byte	16, 7, 7, 0, 0, 8, 90, 0, 0, 8, 26, 0, 0, 9, 148, 0
	.byte	20, 7, 67, 0, 0, 8, 122, 0, 0, 8, 58, 0, 0, 9, 212, 0
	.byte	18, 7, 19, 0, 0, 8, 106, 0, 0, 8, 42, 0, 0, 9, 180, 0
	.byte	0, 8, 10, 0, 0, 8, 138, 0, 0, 8, 74, 0, 0, 9, 244, 0
	.byte	16, 7, 5, 0, 0, 8, 86, 0, 0, 8, 22, 0, 64, 8, 0, 0
	.byte	19, 7, 51, 0, 0, 8, 118, 0, 0, 8, 54, 0, 0, 9, 204, 0
	.byte	17, 7, 15, 0, 0, 8, 102, 0, 0, 8, 38, 0, 0, 9, 172, 0
	.byte	0, 8, 6, 0, 0, 8, 134, 0, 0, 8, 70, 0, 0, 9, 236, 0
	.byte	16, 7, 9, 0, 0, 8, 94, 0, 0, 8, 30, 0, 0, 9, 156, 0
	.byte	20, 7, 99, 0, 0, 8, 126, 0, 0, 8, 62, 0, 0, 9, 220, 0
	.byte	18, 7, 27, 0, 0, 8, 110, 0, 0, 8, 46, 0, 0, 9, 188, 0
	.byte	0, 8, 14, 0, 0, 8, 142, 0, 0, 8, 78, 0, 0, 9, 252, 0
	.byte	96, 7, 0, 0, 0, 8, 81, 0, 0, 8, 17, 0, 21, 8, 131, 0
	.byte	18, 7, 31, 0, 0, 8, 113, 0, 0, 8, 49, 0, 0, 9, 194, 0
	.byte	16, 7, 10, 0, 0, 8, 97, 0, 0, 8, 33, 0, 0, 9, 162, 0
	.byte	0, 8, 1, 0, 0, 8, 129, 0, 0, 8, 65, 0, 0, 9, 226, 0
	.byte	16, 7, 6, 0, 0, 8, 89, 0, 0, 8, 25, 0, 0, 9, 146, 0
	.byte	19, 7, 59, 0, 0, 8, 121, 0, 0, 8, 57, 0, 0, 9, 210, 0
	.byte	17, 7, 17, 0, 0, 8, 105, 0, 0, 8, 41, 0, 0, 9, 178, 0
	.byte	0, 8, 9, 0, 0, 8, 137, 0, 0, 8, 73, 0, 0, 9, 242, 0
	.byte	16, 7, 4, 0, 0, 8, 85, 0, 0, 8, 21, 0, 16, 8, 2, 1
	.byte	19, 7, 43, 0, 0, 8, 117, 0, 0, 8, 53, 0, 0, 9, 202, 0
	.byte	17, 7, 13, 0, 0, 8, 101, 0, 0, 8, 37, 0, 0, 9, 170, 0
	.byte	0, 8, 5, 0, 0, 8, 133, 0, 0, 8, 69, 0, 0, 9, 234, 0
	.byte	16, 7, 8, 0, 0, 8, 93, 0, 0, 8, 29, 0, 0, 9, 154, 0
	.byte	20, 7, 83, 0, 0, 8, 125, 0, 0, 8, 61, 0, 0, 9, 218, 0
	.byte	18, 7, 23, 0, 0, 8, 109, 0, 0, 8, 45, 0, 0, 9, 186, 0
	.byte	0, 8, 13, 0, 0, 8, 141, 0, 0, 8, 77, 0, 0, 9, 250, 0
	.byte	16, 7, 3, 0, 0, 8, 83, 0, 0, 8, 19, 0, 21, 8, 195, 0
	.byte	19, 7, 35, 0, 0, 8, 115, 0, 0, 8, 51, 0, 0, 9, 198, 0
	.byte	17, 7, 11, 0, 0, 8, 99, 0, 0, 8, 35, 0, 0, 9, 166, 0
	.byte	0, 8, 3, 0, 0, 8, 131, 0, 0, 8, 67, 0, 0, 9, 230, 0
	.byte	16, 7, 7, 0, 0, 8, 91, 0, 0, 8, 27, 0, 0, 9, 150, 0
	.byte	20, 7, 67, 0, 0, 8, 123, 0, 0, 8, 59, 0, 0, 9, 214, 0
	.byte	18, 7, 19, 0, 0, 8, 107, 0, 0, 8, 43, 0, 0, 9, 182, 0
	.byte	0, 8, 11, 0, 0, 8, 139, 0, 0, 8, 75, 0, 0, 9, 246, 0
	.byte	16, 7, 5, 0, 0, 8, 87, 0, 0, 8, 23, 0, 64, 8, 0, 0
	.byte	19, 7, 51, 0, 0, 8, 119, 0, 0, 8, 55, 0, 0, 9, 206, 0
	.byte	17, 7, 15, 0, 0, 8, 103, 0, 0, 8, 39, 0, 0, 9, 174, 0
	.byte	0, 8, 7, 0, 0, 8, 135, 0, 0, 8, 71, 0, 0, 9, 238, 0
	.byte	16, 7, 9, 0, 0, 8, 95, 0, 0, 8, 31, 0, 0, 9, 158, 0
	.byte	20, 7, 99, 0, 0, 8, 127, 0, 0, 8, 63, 0, 0, 9, 222, 0
	.byte	18, 7, 27, 0, 0, 8, 111, 0, 0, 8, 47, 0, 0, 9, 190, 0
	.byte	0, 8, 15, 0, 0, 8, 143, 0, 0, 8, 79, 0, 0, 9, 254, 0
	.byte	96, 7, 0, 0, 0, 8, 80, 0, 0, 8, 16, 0, 20, 8, 115, 0
	.byte	18, 7, 31, 0, 0, 8, 112, 0, 0, 8, 48, 0, 0, 9, 193, 0
	.byte	16, 7, 10, 0, 0, 8, 96, 0, 0, 8, 32, 0, 0, 9, 161, 0
	.byte	0, 8, 0, 0, 0, 8, 128, 0, 0, 8, 64, 0, 0, 9, 225, 0
	.byte	16, 7, 6, 0, 0, 8, 88, 0, 0, 8, 24, 0, 0, 9, 145, 0
	.byte	19, 7, 59, 0, 0, 8, 120, 0, 0, 8, 56, 0, 0, 9, 209, 0
	.byte	17, 7, 17, 0, 0, 8, 104, 0, 0, 8, 40, 0, 0, 9, 177, 0
	.byte	0, 8, 8, 0, 0, 8, 136, 0, 0, 8, 72, 0, 0, 9, 241, 0
	.byte	16, 7, 4, 0, 0, 8, 84, 0, 0, 8, 20, 0, 21, 8, 227, 0
	.byte	19, 7, 43, 0, 0, 8, 116, 0, 0, 8, 52, 0, 0, 9, 201, 0
	.byte	17, 7, 13, 0, 0, 8, 100, 0, 0, 8, 36, 0, 0, 9, 169, 0
	.byte	0, 8, 4, 0, 0, 8, 132, 0, 0, 8, 68, 0, 0, 9, 233, 0
	.byte	16, 7, 8, 0, 0, 8, 92, 0, 0, 8, 28, 0, 0, 9, 153, 0
	.byte	20, 7, 83, 0, 0, 8, 124, 0, 0, 8, 60, 0, 0, 9, 217, 0
	.byte	18, 7, 23, 0, 0, 8, 108, 0, 0, 8, 44, 0, 0, 9, 185, 0
	.byte	0, 8, 12, 0, 0, 8, 140, 0, 0, 8, 76, 0, 0, 9, 249, 0
	.byte	16, 7, 3, 0, 0, 8, 82, 0, 0, 8, 18, 0, 21, 8, 163, 0
	.byte	19, 7, 35, 0, 0, 8, 114, 0, 0, 8, 50, 0, 0, 9, 197, 0
	.byte	17, 7, 11, 0, 0, 8, 98, 0, 0, 8, 34, 0, 0, 9, 165, 0
	.byte	0, 8, 2, 0, 0, 8, 130, 0, 0, 8, 66, 0, 0, 9, 229, 0
	.byte	16, 7, 7, 0, 0, 8, 90, 0, 0, 8, 26, 0, 0, 9, 149, 0
	.byte	20, 7, 67, 0, 0, 8, 122, 0, 0, 8, 58, 0, 0, 9, 213, 0
	.byte	18, 7, 19, 0, 0, 8, 106, 0, 0, 8, 42, 0, 0, 9, 181, 0
	.byte	0, 8, 10, 0, 0, 8, 138, 0, 0, 8, 74, 0, 0, 9, 245, 0
	.byte	16, 7, 5, 0, 0, 8, 86, 0, 0, 8, 22, 0, 64, 8, 0, 0
	.byte	19, 7, 51, 0, 0, 8, 118, 0, 0, 8, 54, 0, 0, 9, 205, 0
	.byte	17, 7, 15, 0, 0, 8, 102, 0, 0, 8, 38, 0, 0, 9, 173, 0
	.byte	0, 8, 6, 0, 0, 8, 134, 0, 0, 8, 70, 0, 0, 9, 237, 0
	.byte	16, 7, 9, 0, 0, 8, 94, 0, 0, 8, 30, 0, 0, 9, 157, 0
	.byte	20, 7, 99, 0, 0, 8, 126, 0, 0, 8, 62, 0, 0, 9, 221, 0
	.byte	18, 7, 27, 0, 0, 8, 110, 0, 0, 8, 46, 0, 0, 9, 189, 0
	.byte	0, 8, 14, 0, 0, 8, 142, 0, 0, 8, 78, 0, 0, 9, 253, 0
	.byte	96, 7, 0, 0, 0, 8, 81, 0, 0, 8, 17, 0, 21, 8, 131, 0
	.byte	18, 7, 31, 0, 0, 8, 113, 0, 0, 8, 49, 0, 0, 9, 195, 0
	.byte	16, 7, 10, 0, 0, 8, 97, 0, 0, 8, 33, 0, 0, 9, 163, 0
	.byte	0, 8, 1, 0, 0, 8, 129, 0, 0, 8, 65, 0, 0, 9, 227, 0
	.byte	16, 7, 6, 0, 0, 8, 89, 0, 0, 8, 25, 0, 0, 9, 147, 0
	.byte	19, 7, 59, 0, 0, 8, 121, 0, 0, 8, 57, 0, 0, 9, 211, 0
	.byte	17, 7, 17, 0, 0, 8, 105, 0, 0, 8, 41, 0, 0, 9, 179, 0
	.byte	0, 8, 9, 0, 0, 8, 137, 0, 0, 8, 73, 0, 0, 9, 243, 0
	.byte	16, 7, 4, 0, 0, 8, 85, 0, 0, 8, 21, 0, 16, 8, 2, 1
	.byte	19, 7, 43, 0, 0, 8, 117, 0, 0, 8, 53, 0, 0, 9, 203, 0
	.byte	17, 7, 13, 0, 0, 8, 101, 0, 0, 8, 37, 0, 0, 9, 171, 0
	.byte	0, 8, 5, 0, 0, 8, 133, 0, 0, 8, 69, 0, 0, 9, 235, 0
	.byte	16, 7, 8, 0, 0, 8, 93, 0, 0, 8, 29, 0, 0, 9, 155, 0
	.byte	20, 7, 83, 0, 0, 8, 125, 0, 0, 8, 61, 0, 0, 9, 219, 0
	.byte	18, 7, 23, 0, 0, 8, 109, 0, 0, 8, 45, 0, 0, 9, 187, 0
	.byte	0, 8, 13, 0, 0, 8, 141, 0, 0, 8, 77, 0, 0, 9, 251, 0
	.byte	16, 7, 3, 0, 0, 8, 83, 0, 0, 8, 19, 0, 21, 8, 195, 0
	.byte	19, 7, 35, 0, 0, 8, 115, 0, 0, 8, 51, 0, 0, 9, 199, 0
	.byte	17, 7, 11, 0, 0, 8, 99, 0, 0, 8, 35, 0, 0, 9, 167, 0
	.byte	0, 8, 3, 0, 0, 8, 131, 0, 0, 8, 67, 0, 0, 9, 231, 0
	.byte	16, 7, 7, 0, 0, 8, 91, 0, 0, 8, 27, 0, 0, 9, 151, 0
	.byte	20, 7, 67, 0, 0, 8, 123, 0, 0, 8, 59, 0, 0, 9, 215, 0
	.byte	18, 7, 19, 0, 0, 8, 107, 0, 0, 8, 43, 0, 0, 9, 183, 0
	.byte	0, 8, 11, 0, 0, 8, 139, 0, 0, 8, 75, 0, 0, 9, 247, 0
	.byte	16, 7, 5, 0, 0, 8, 87, 0, 0, 8, 23, 0, 64, 8, 0, 0
	.byte	19, 7, 51, 0, 0, 8, 119, 0, 0, 8, 55, 0, 0, 9, 207, 0
	.byte	17, 7, 15, 0, 0, 8, 103, 0, 0, 8, 39, 0, 0, 9, 175, 0
	.byte	0, 8, 7, 0, 0, 8, 135, 0, 0, 8, 71, 0, 0, 9, 239, 0
	.byte	16, 7, 9, 0, 0, 8, 95, 0, 0, 8, 31, 0, 0, 9, 159, 0
	.byte	20, 7, 99, 0, 0, 8, 127, 0, 0, 8, 63, 0, 0, 9, 223, 0
	.byte	18, 7, 27, 0, 0, 8, 111, 0, 0, 8, 47, 0, 0, 9, 191, 0
	.byte	0, 8, 15, 0, 0, 8, 143, 0, 0, 8, 79, 0, 0, 9, 255, 0
	.type	distfix.0, %object
	.size	distfix.0, 128
distfix.0:
	.byte	16, 5, 1, 0, 23, 5, 1, 1, 19, 5, 17, 0, 27, 5, 1, 16
	.byte	17, 5, 5, 0, 25, 5, 1, 4, 21, 5, 65, 0, 29, 5, 1, 64
	.byte	16, 5, 3, 0, 24, 5, 1, 2, 20, 5, 33, 0, 28, 5, 1, 32
	.byte	18, 5, 9, 0, 26, 5, 1, 8, 22, 5, 129, 0, 64, 5, 0, 0
	.byte	16, 5, 2, 0, 23, 5, 129, 1, 19, 5, 25, 0, 27, 5, 1, 24
	.byte	17, 5, 7, 0, 25, 5, 1, 6, 21, 5, 97, 0, 29, 5, 1, 96
	.byte	16, 5, 4, 0, 24, 5, 1, 3, 20, 5, 49, 0, 28, 5, 1, 48
	.byte	18, 5, 13, 0, 26, 5, 1, 12, 22, 5, 193, 0, 64, 5, 0, 0
	.type	order.2, %object
	.size	order.2, 38
order.2:
	.byte	16, 0, 17, 0, 18, 0, 0, 0, 8, 0, 7, 0, 9, 0, 6, 0
	.byte	10, 0, 5, 0, 11, 0, 4, 0, 12, 0, 3, 0, 13, 0, 2, 0
	.byte	14, 0, 1, 0, 15, 0
	.section	.note.GNU-stack,"",@progbits
