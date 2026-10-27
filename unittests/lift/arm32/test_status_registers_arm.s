// Status register access: MRS and MSR of APSR, CPSR and SPSR by their
// fields, and CPS.  The lift keeps N, Z, C, V and GE as flags; the machine
// keeps Q, the mode and the interrupt masks.

        .syntax unified
        .arch armv7-a
        .text
        .arm

// The lifted flags and GE over the machine's other bits.
        .globl read_apsr
        .type read_apsr, %function
read_apsr:
        mrs r0, apsr
        bx lr
        .size read_apsr, .-read_apsr

// The flags a comparison set, over the machine's other bits.
        .globl flags_after_compare
        .type flags_after_compare, %function
flags_after_compare:
        cmp r0, r1
        mrs r0, apsr
        bx lr
        .size flags_after_compare, .-flags_after_compare

// A mode change writes the control field, not the flags the comparison set.
        .globl flags_survive_mode_write
        .type flags_survive_mode_write, %function
flags_survive_mode_write:
        cmp r0, #0
        msr cpsr_c, #0xd3
        movne r0, #1
        moveq r0, #0
        bx lr
        .size flags_survive_mode_write, .-flags_survive_mode_write

// The flags restored from a saved word: N is its bit 31.
        .globl restore_flags
        .type restore_flags, %function
restore_flags:
        msr apsr_nzcvq, r0
        movmi r0, #1
        movpl r0, #0
        bx lr
        .size restore_flags, .-restore_flags

// GE from bits 19:16 of r0 picks each byte SEL takes.
        .globl select_by_ge
        .type select_by_ge, %function
select_by_ge:
        msr apsr_g, r0
        sel r0, r1, r2
        bx lr
        .size select_by_ge, .-select_by_ge

// An exception mode's saved status register.
        .globl read_spsr
        .type read_spsr, %function
read_spsr:
        mrs r0, spsr
        bx lr
        .size read_spsr, .-read_spsr

        .globl write_spsr
        .type write_spsr, %function
write_spsr:
        msr spsr_fsxc, r0
        bx lr
        .size write_spsr, .-write_spsr

// A kernel's local_irq_save and local_irq_restore.
        .globl irq_save
        .type irq_save, %function
irq_save:
        mrs r0, cpsr
        cpsid i
        bx lr
        .size irq_save, .-irq_save

        .globl irq_restore
        .type irq_restore, %function
irq_restore:
        msr cpsr_c, r0
        bx lr
        .size irq_restore, .-irq_restore

// Supervisor mode, entered from Thumb.
        .thumb
        .thumb_func
        .globl to_svc_thumb
        .type to_svc_thumb, %function
to_svc_thumb:
        cps #0x13
        bx lr
        .size to_svc_thumb, .-to_svc_thumb

// CPSID with no mask to change is UNPREDICTABLE: the function is refused.
        .arm
        .globl cps_without_masks
        .type cps_without_masks, %function
cps_without_masks:
        .inst 0xf10e0013
        bx lr
        .size cps_without_masks, .-cps_without_masks
