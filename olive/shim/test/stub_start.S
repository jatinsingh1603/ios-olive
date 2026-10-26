/*
 * Stand-in "kernel" for testing the olive shim without xnu: entered by the
 * shim's plain branch on the boot core like xnu's __start (EL1, MMU and
 * caches off, x0 = boot_args PA, no stack). Position independent.
 */
	.section .text.head, "ax"
	.globl _stub_entry
_stub_entry:
	msr	daifset, #0xf
	msr	spsel, #1
	adr	x1, _stack_top
	mov	sp, x1
	adr	x1, vectors
	msr	vbar_el1, x1
	isb
	bl	stub_main
1:	wfe
	b	1b

/* Any exception: log the syndrome and stop (stub_exc never returns). */
.macro vent slot
	.balign	0x80
	mov	x0, #\slot
	b	stub_exc
.endm
	.balign	0x800
vectors:
	vent 0
	vent 1
	vent 2
	vent 3
	vent 4
	vent 5
	vent 6
	vent 7
	vent 8
	vent 9
	vent 10
	vent 11
	vent 12
	vent 13
	vent 14
	vent 15
